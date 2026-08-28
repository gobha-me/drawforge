#include <drawforge/render.hpp>

#include "scene_internal.hpp"

#include <plutovg.h>

#include <algorithm>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <expected>
#include <limits>
#include <memory>
#include <new>
#include <string>
#include <type_traits>
#include <utility>
#include <variant>
#include <vector>

namespace drawforge {
namespace {

constexpr RenderError cancelled_error{RenderErrorCode::cancelled,
                                      "render operation was cancelled"};
constexpr RenderError resource_limit_error{
    RenderErrorCode::resource_limit, "render output exceeds its byte limit"};
constexpr RenderError number_out_of_range_error{
    RenderErrorCode::number_out_of_range,
    "scene values cannot be represented by the renderer"};
constexpr RenderError arithmetic_overflow_error{
    RenderErrorCode::arithmetic_overflow, "render arithmetic overflowed"};
constexpr RenderError allocation_failure_error{
    RenderErrorCode::allocation_failure, "render allocation failed"};
constexpr RenderError renderer_failure_error{RenderErrorCode::renderer_failure,
                                             "renderer operation failed"};
constexpr RenderError png_encoding_failure_error{
    RenderErrorCode::png_encoding_failure, "PNG encoding failed"};

constexpr RendererInfo phase_one_renderer{"plutovg", "1.3.3", 1};

using Surface =
    std::unique_ptr<plutovg_surface_t, decltype(&plutovg_surface_destroy)>;
using Canvas =
    std::unique_ptr<plutovg_canvas_t, decltype(&plutovg_canvas_destroy)>;

[[nodiscard]] auto map_value_error(const ValueError &error) -> RenderError {
  if (error.code == ValueErrorCode::arithmetic_overflow)
    return arithmetic_overflow_error;
  if (error.code == ValueErrorCode::number_out_of_range ||
      error.code == ValueErrorCode::non_finite_number)
    return number_out_of_range_error;
  if (error.code == ValueErrorCode::allocation_failure)
    return allocation_failure_error;
  if (error.code == ValueErrorCode::resource_limit)
    return resource_limit_error;
  return renderer_failure_error;
}

[[nodiscard]] auto checked_float(const double value)
    -> std::expected<float, RenderError> {
  if (!std::isfinite(value) ||
      std::abs(value) > std::numeric_limits<float>::max())
    return std::unexpected{number_out_of_range_error};
  return static_cast<float>(value);
}

[[nodiscard]] auto color_component(const std::uint8_t value) noexcept -> float {
  return static_cast<float>(value) / 255.0F;
}

auto set_color(plutovg_canvas_t *canvas, const Color color) noexcept -> void {
  plutovg_canvas_set_rgba(
      canvas, color_component(color.red), color_component(color.green),
      color_component(color.blue), color_component(color.alpha));
}

[[nodiscard]] auto object_key(const ObjectId &id) -> std::string {
  return std::string{id.value()};
}

class CanvasRestore {
public:
  explicit CanvasRestore(plutovg_canvas_t *canvas) noexcept : m_canvas{canvas} {
    plutovg_canvas_save(m_canvas);
  }
  CanvasRestore(const CanvasRestore &) = delete;
  auto operator=(const CanvasRestore &) -> CanvasRestore & = delete;
  ~CanvasRestore() { plutovg_canvas_restore(m_canvas); }

private:
  plutovg_canvas_t *m_canvas;
};

class SceneRenderer {
public:
  SceneRenderer(const detail::DocumentState &state, const RenderConfig &config,
                CancellationToken cancellation,
                plutovg_canvas_t *canvas) noexcept
      : m_state{state}, m_config{config}, m_cancellation{cancellation},
        m_canvas{canvas} {}

  [[nodiscard]] auto render() -> std::expected<void, RenderError> {
    const AffineTransform identity{};
    for (const auto &layer : m_state.layers) {
      if (m_cancellation.stop_requested())
        return std::unexpected{cancelled_error};
      if (!layer.layer.visible())
        continue;
      for (const auto child : layer.children) {
        const auto rendered = render_object(child, identity);
        if (!rendered)
          return std::unexpected{rendered.error()};
      }
    }
    return {};
  }

private:
  [[nodiscard]] auto compose_world(const AffineTransform local,
                                   const AffineTransform parent)
      -> std::expected<AffineTransform, RenderError> {
    const auto result = local.then(parent, m_state.limits);
    if (!result)
      return std::unexpected{map_value_error(result.error())};
    return *result;
  }

  [[nodiscard]] auto set_matrix(const AffineTransform transform)
      -> std::expected<void, RenderError> {
    const auto a = checked_float(transform.a().value());
    const auto b = checked_float(transform.b().value());
    const auto c = checked_float(transform.c().value());
    const auto d = checked_float(transform.d().value());
    const auto e = checked_float(transform.e().value());
    const auto f = checked_float(transform.f().value());
    if (!a || !b || !c || !d || !e || !f)
      return std::unexpected{number_out_of_range_error};
    const plutovg_matrix_t matrix{*a, *b, *c, *d, *e, *f};
    plutovg_canvas_set_matrix(m_canvas, &matrix);
    return {};
  }

  [[nodiscard]] auto append_geometry(const Geometry &geometry)
      -> std::expected<void, RenderError> {
    plutovg_canvas_new_path(m_canvas);
    return std::visit(
        [&](const auto &value) -> std::expected<void, RenderError> {
          using T = std::remove_cvref_t<decltype(value)>;
          if constexpr (std::is_same_v<T, Rectangle>) {
            const auto x = checked_float(value.origin().x().value());
            const auto y = checked_float(value.origin().y().value());
            const auto width = checked_float(value.width().value());
            const auto height = checked_float(value.height().value());
            const auto radius_x = checked_float(value.radius_x().value());
            const auto radius_y = checked_float(value.radius_y().value());
            if (!x || !y || !width || !height || !radius_x || !radius_y)
              return std::unexpected{number_out_of_range_error};
            plutovg_canvas_round_rect(m_canvas, *x, *y, *width, *height,
                                      *radius_x, *radius_y);
          } else if constexpr (std::is_same_v<T, Ellipse>) {
            const auto center_x = checked_float(value.center().x().value());
            const auto center_y = checked_float(value.center().y().value());
            const auto radius_x = checked_float(value.radius_x().value());
            const auto radius_y = checked_float(value.radius_y().value());
            if (!center_x || !center_y || !radius_x || !radius_y)
              return std::unexpected{number_out_of_range_error};
            plutovg_canvas_ellipse(m_canvas, *center_x, *center_y, *radius_x,
                                   *radius_y);
          } else {
            for (const auto &command : value.commands()) {
              if (m_cancellation.stop_requested())
                return std::unexpected{cancelled_error};
              const auto appended = std::visit(
                  [&](const auto &path_command)
                      -> std::expected<void, RenderError> {
                    using Command = std::remove_cvref_t<decltype(path_command)>;
                    if constexpr (std::is_same_v<Command, ClosePath>) {
                      plutovg_canvas_close_path(m_canvas);
                    } else {
                      const auto x =
                          checked_float(path_command.point.x().value());
                      const auto y =
                          checked_float(path_command.point.y().value());
                      if (!x || !y)
                        return std::unexpected{number_out_of_range_error};
                      if constexpr (std::is_same_v<Command, MoveTo>)
                        plutovg_canvas_move_to(m_canvas, *x, *y);
                      else
                        plutovg_canvas_line_to(m_canvas, *x, *y);
                    }
                    return {};
                  },
                  command);
              if (!appended)
                return std::unexpected{appended.error()};
            }
          }
          return {};
        },
        geometry);
  }

  [[nodiscard]] auto evaluated_opacity(const Drawable &drawable) -> Opacity {
    const auto track = m_state.track_by_object.find(object_key(drawable.id()));
    if (track == m_state.track_by_object.end())
      return drawable.opacity();
    return m_state.tracks[track->second].evaluate(m_config.time_us(),
                                                  drawable.opacity());
  }

  [[nodiscard]] auto draw(const Drawable &drawable, const AffineTransform world)
      -> std::expected<void, RenderError> {
    const auto opacity = evaluated_opacity(drawable);
    if (opacity.value() == 0.0)
      return {};

    CanvasRestore restore{m_canvas};
    const auto matrix = set_matrix(world);
    if (!matrix)
      return std::unexpected{matrix.error()};

    plutovg_canvas_set_operator(m_canvas, PLUTOVG_OPERATOR_SRC_OVER);
    plutovg_canvas_set_fill_rule(m_canvas, PLUTOVG_FILL_RULE_NON_ZERO);
    plutovg_canvas_set_line_cap(m_canvas, PLUTOVG_LINE_CAP_BUTT);
    plutovg_canvas_set_line_join(m_canvas, PLUTOVG_LINE_JOIN_MITER);
    plutovg_canvas_set_miter_limit(m_canvas, 4.0F);
    plutovg_canvas_set_opacity(m_canvas, static_cast<float>(opacity.value()));

    const auto &style = drawable.style();
    if (style.fill() && style.fill()->alpha != 0) {
      const auto geometry = append_geometry(drawable.geometry());
      if (!geometry)
        return std::unexpected{geometry.error()};
      set_color(m_canvas, *style.fill());
      plutovg_canvas_fill(m_canvas);
    }
    if (style.stroke() && style.stroke()->color().alpha != 0 &&
        style.stroke()->width().value() > 0.0) {
      const auto geometry = append_geometry(drawable.geometry());
      if (!geometry)
        return std::unexpected{geometry.error()};
      const auto width = checked_float(style.stroke()->width().value());
      if (!width)
        return std::unexpected{width.error()};
      set_color(m_canvas, style.stroke()->color());
      plutovg_canvas_set_line_width(m_canvas, *width);
      plutovg_canvas_stroke(m_canvas);
    }
    return {};
  }

  [[nodiscard]] auto render_object(const std::size_t index,
                                   const AffineTransform parent)
      -> std::expected<void, RenderError> {
    if (m_cancellation.stop_requested())
      return std::unexpected{cancelled_error};

    const auto &record = m_state.objects[index];
    if (!detail::object_visible(record.object))
      return {};
    const auto world =
        compose_world(detail::object_transform(record.object), parent);
    if (!world)
      return std::unexpected{world.error()};

    if (const auto *drawable = std::get_if<Drawable>(&record.object))
      return draw(*drawable, *world);

    for (const auto child : record.children) {
      const auto rendered = render_object(child, *world);
      if (!rendered)
        return std::unexpected{rendered.error()};
    }
    return {};
  }

  const detail::DocumentState &m_state;
  const RenderConfig &m_config;
  CancellationToken m_cancellation;
  plutovg_canvas_t *m_canvas;
};

struct PngSink {
  std::vector<std::uint8_t> bytes;
  std::uint64_t limit{};
  CancellationToken cancellation;
  bool cancelled{};
  bool limit_exceeded{};
  bool allocation_failed{};
  bool invalid_chunk{};
};

auto append_png(void *closure, void *data, const int size) -> void {
  auto &sink = *static_cast<PngSink *>(closure);
  if (sink.cancelled || sink.limit_exceeded || sink.allocation_failed ||
      sink.invalid_chunk)
    return;
  if (sink.cancellation.stop_requested()) {
    sink.cancelled = true;
    return;
  }
  if (size < 0 || (size > 0 && data == nullptr)) {
    sink.invalid_chunk = true;
    return;
  }
  if (size == 0)
    return;
  const auto chunk_size = static_cast<std::uint64_t>(size);
  if (chunk_size > sink.limit || sink.bytes.size() > sink.limit - chunk_size) {
    sink.limit_exceeded = true;
    return;
  }
  const auto *first = static_cast<const std::uint8_t *>(data);
  try {
    sink.bytes.insert(sink.bytes.end(), first, first + size);
  } catch (...) {
    sink.allocation_failed = true;
  }
}

[[nodiscard]] auto
png_signature_valid(const std::span<const std::uint8_t> bytes) noexcept
    -> bool {
  constexpr std::uint8_t signature[]{0x89U, 0x50U, 0x4eU, 0x47U,
                                     0x0dU, 0x0aU, 0x1aU, 0x0aU};
  return bytes.size() >= std::size(signature) &&
         std::equal(std::begin(signature), std::end(signature), bytes.begin());
}

} // namespace

struct RgbaImage::Data {
  CanvasExtent extent;
  std::uint64_t time_us{};
  std::uint64_t max_output_bytes{};
  RendererInfo renderer;
  std::vector<std::uint8_t> pixels;
  std::vector<std::uint8_t> native_argb;
};

struct PngImage::Data {
  CanvasExtent extent;
  std::uint64_t time_us{};
  RendererInfo renderer;
  std::vector<std::uint8_t> bytes;
};

namespace detail {

struct RenderAccess {
  [[nodiscard]] static auto
  make_rgba(CanvasExtent extent, const std::uint64_t time_us,
            const std::uint64_t max_output_bytes, RendererInfo renderer,
            std::vector<std::uint8_t> pixels,
            std::vector<std::uint8_t> native_argb) -> RgbaImage {
    return RgbaImage{std::make_shared<const RgbaImage::Data>(
        RgbaImage::Data{extent, time_us, max_output_bytes, renderer,
                        std::move(pixels), std::move(native_argb)})};
  }

  [[nodiscard]] static auto rgba(const RgbaImage &image) noexcept
      -> const RgbaImage::Data & {
    return *image.m_data;
  }

  [[nodiscard]] static auto
  make_png(CanvasExtent extent, const std::uint64_t time_us,
           RendererInfo renderer, std::vector<std::uint8_t> bytes) -> PngImage {
    return PngImage{std::make_shared<const PngImage::Data>(
        PngImage::Data{extent, time_us, renderer, std::move(bytes)})};
  }
};

} // namespace detail

RgbaImage::~RgbaImage() = default;
PngImage::~PngImage() = default;

auto RgbaImage::extent() const noexcept -> CanvasExtent {
  return m_data->extent;
}
auto RgbaImage::time_us() const noexcept -> std::uint64_t {
  return m_data->time_us;
}
auto RgbaImage::format() const noexcept -> PixelFormat {
  return PixelFormat::rgba8_srgb_straight_alpha;
}
auto RgbaImage::renderer() const noexcept -> RendererInfo {
  return m_data->renderer;
}
auto RgbaImage::pixels() const noexcept -> std::span<const std::uint8_t> {
  return m_data->pixels;
}

auto PngImage::extent() const noexcept -> CanvasExtent {
  return m_data->extent;
}
auto PngImage::time_us() const noexcept -> std::uint64_t {
  return m_data->time_us;
}
auto PngImage::renderer() const noexcept -> RendererInfo {
  return m_data->renderer;
}
auto PngImage::bytes() const noexcept -> std::span<const std::uint8_t> {
  return m_data->bytes;
}

auto render_error_code_name(const RenderErrorCode code) noexcept
    -> std::string_view {
  switch (code) {
  case RenderErrorCode::cancelled:
    return "cancelled";
  case RenderErrorCode::resource_limit:
    return "resource_limit";
  case RenderErrorCode::number_out_of_range:
    return "number_out_of_range";
  case RenderErrorCode::arithmetic_overflow:
    return "arithmetic_overflow";
  case RenderErrorCode::allocation_failure:
    return "allocation_failure";
  case RenderErrorCode::renderer_failure:
    return "renderer_failure";
  case RenderErrorCode::png_encoding_failure:
    return "png_encoding_failure";
  }
  return "renderer_failure";
}

auto renderer_info() noexcept -> RendererInfo { return phase_one_renderer; }

auto RenderConfig::create(const std::uint64_t time_us,
                          const std::uint64_t max_output_bytes) noexcept
    -> std::expected<RenderConfig, RenderError> {
  if (max_output_bytes == 0 ||
      max_output_bytes > hard_resource_limit_values.max_output_bytes)
    return std::unexpected{resource_limit_error};
  return RenderConfig{time_us, max_output_bytes};
}

auto render_rgba(const Document &document, const RenderConfig &config,
                 const CancellationToken cancellation) noexcept
    -> std::expected<RgbaImage, RenderError> {
  try {
    if (cancellation.stop_requested())
      return std::unexpected{cancelled_error};

    const auto &state = DocumentAccess::state(document);
    const auto bytes = state.canvas.rgba8_bytes();
    if (config.max_output_bytes() > state.limits.max_output_bytes() ||
        bytes > config.max_output_bytes())
      return std::unexpected{resource_limit_error};
    if (state.canvas.width() >
            static_cast<std::uint32_t>(std::numeric_limits<int>::max()) ||
        state.canvas.height() >
            static_cast<std::uint32_t>(std::numeric_limits<int>::max()) ||
        bytes > std::numeric_limits<std::size_t>::max())
      return std::unexpected{arithmetic_overflow_error};

    const auto width = static_cast<int>(state.canvas.width());
    const auto height = static_cast<int>(state.canvas.height());
    Surface surface(plutovg_surface_create(width, height),
                    &plutovg_surface_destroy);
    if (!surface)
      return std::unexpected{allocation_failure_error};
    Canvas canvas(plutovg_canvas_create(surface.get()),
                  &plutovg_canvas_destroy);
    if (!canvas)
      return std::unexpected{allocation_failure_error};

    if (state.background) {
      const plutovg_color_t background{
          color_component(state.background->red),
          color_component(state.background->green),
          color_component(state.background->blue),
          color_component(state.background->alpha)};
      plutovg_surface_clear(surface.get(), &background);
    }

    SceneRenderer renderer{state, config, cancellation, canvas.get()};
    const auto rendered = renderer.render();
    if (!rendered)
      return std::unexpected{rendered.error()};

    const auto stride = plutovg_surface_get_stride(surface.get());
    if (stride <= 0 ||
        static_cast<std::uint64_t>(stride) * state.canvas.height() != bytes)
      return std::unexpected{renderer_failure_error};

    std::vector<std::uint8_t> native_argb(static_cast<std::size_t>(bytes));
    std::memcpy(native_argb.data(), plutovg_surface_get_data(surface.get()),
                native_argb.size());
    std::vector<std::uint8_t> pixels(static_cast<std::size_t>(bytes));
    for (std::uint32_t y = 0; y < state.canvas.height(); ++y) {
      if (cancellation.stop_requested())
        return std::unexpected{cancelled_error};
      for (std::uint32_t x = 0; x < state.canvas.width(); ++x) {
        const auto offset =
            (static_cast<std::size_t>(y) * state.canvas.width() + x) * 4U;
        std::uint32_t pixel{};
        std::memcpy(&pixel, native_argb.data() + offset, sizeof(pixel));
        const auto alpha = (pixel >> 24U) & 0xffU;
        const auto red = (pixel >> 16U) & 0xffU;
        const auto green = (pixel >> 8U) & 0xffU;
        const auto blue = pixel & 0xffU;
        if (alpha == 0U) {
          pixels[offset + 0U] = 0U;
          pixels[offset + 1U] = 0U;
          pixels[offset + 2U] = 0U;
          pixels[offset + 3U] = 0U;
        } else {
          pixels[offset + 0U] = static_cast<std::uint8_t>(red * 255U / alpha);
          pixels[offset + 1U] = static_cast<std::uint8_t>(green * 255U / alpha);
          pixels[offset + 2U] = static_cast<std::uint8_t>(blue * 255U / alpha);
          pixels[offset + 3U] = static_cast<std::uint8_t>(alpha);
        }
      }
    }

    if (cancellation.stop_requested())
      return std::unexpected{cancelled_error};
    return detail::RenderAccess::make_rgba(
        state.canvas, config.time_us(), config.max_output_bytes(),
        phase_one_renderer, std::move(pixels), std::move(native_argb));
  } catch (const std::bad_alloc &) {
    return std::unexpected{allocation_failure_error};
  } catch (...) {
    return std::unexpected{renderer_failure_error};
  }
}

auto encode_png(const RgbaImage &image,
                const CancellationToken cancellation) noexcept
    -> std::expected<PngImage, RenderError> {
  try {
    if (cancellation.stop_requested())
      return std::unexpected{cancelled_error};
    const auto &data = detail::RenderAccess::rgba(image);
    const auto width = static_cast<int>(data.extent.width());
    const auto height = static_cast<int>(data.extent.height());
    Surface surface(plutovg_surface_create(width, height),
                    &plutovg_surface_destroy);
    if (!surface)
      return std::unexpected{allocation_failure_error};
    const auto stride = plutovg_surface_get_stride(surface.get());
    if (stride <= 0 ||
        static_cast<std::uint64_t>(stride) * data.extent.height() !=
            data.native_argb.size())
      return std::unexpected{renderer_failure_error};
    std::memcpy(plutovg_surface_get_data(surface.get()),
                data.native_argb.data(), data.native_argb.size());

    PngSink sink{{}, data.max_output_bytes, cancellation};
    const auto encoded =
        plutovg_surface_write_to_png_stream(surface.get(), &append_png, &sink);
    if (sink.cancelled || cancellation.stop_requested())
      return std::unexpected{cancelled_error};
    if (sink.limit_exceeded)
      return std::unexpected{resource_limit_error};
    if (sink.allocation_failed)
      return std::unexpected{allocation_failure_error};
    if (sink.invalid_chunk || !encoded || !png_signature_valid(sink.bytes))
      return std::unexpected{png_encoding_failure_error};

    return detail::RenderAccess::make_png(data.extent, data.time_us,
                                          data.renderer, std::move(sink.bytes));
  } catch (const std::bad_alloc &) {
    return std::unexpected{allocation_failure_error};
  } catch (...) {
    return std::unexpected{png_encoding_failure_error};
  }
}

} // namespace drawforge
