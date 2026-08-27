#include "backend.hpp"

#include <plutovg.h>

#include <exception>
#include <memory>

namespace drawforge::renderer_spike {
namespace {

using Surface =
    std::unique_ptr<plutovg_surface_t, decltype(&plutovg_surface_destroy)>;
using Canvas =
    std::unique_ptr<plutovg_canvas_t, decltype(&plutovg_canvas_destroy)>;

struct PngSink {
  std::vector<std::uint8_t> *bytes;
  bool failed{};
};

auto append_png(void *closure, void *data, int size) -> void {
  auto &sink = *static_cast<PngSink *>(closure);
  if (sink.failed) {
    return;
  }
  const auto *first = static_cast<const std::uint8_t *>(data);
  try {
    sink.bytes->insert(sink.bytes->end(), first, first + size);
  } catch (...) {
    // PlutoVG's callback returns void, so contain allocation failures here and
    // report them after the encoder returns instead of unwinding through C.
    sink.failed = true;
  }
}

[[nodiscard]] auto to_bounds(const plutovg_rect_t &rect) noexcept -> Bounds {
  return Bounds{rect.x, rect.y, rect.w, rect.h};
}

} // namespace

auto backend_name() noexcept -> std::string_view { return "plutovg"; }

auto render_scene() noexcept -> std::expected<Evidence, Error> {
  try {
    Surface surface(plutovg_surface_create(canvas_width, canvas_height),
                    &plutovg_surface_destroy);
    if (!surface) {
      return std::unexpected(
          Error{"allocation_failure", "PlutoVG surface creation failed"});
    }
    Canvas canvas(plutovg_canvas_create(surface.get()),
                  &plutovg_canvas_destroy);
    if (!canvas) {
      return std::unexpected(
          Error{"allocation_failure", "PlutoVG canvas creation failed"});
    }

    plutovg_canvas_set_rgba(canvas.get(), 248.0F / 255.0F, 250.0F / 255.0F,
                            252.0F / 255.0F, 1.0F);
    plutovg_canvas_fill_rect(canvas.get(), 0.0F, 0.0F, canvas_width,
                             canvas_height);
    plutovg_canvas_set_rgba(canvas.get(), 23.0F / 255.0F, 32.0F / 255.0F,
                            51.0F / 255.0F, 1.0F);
    plutovg_canvas_round_rect(canvas.get(), 8.0F, 8.0F, 240.0F, 128.0F, 16.0F,
                              16.0F);
    plutovg_canvas_fill(canvas.get());
    plutovg_canvas_set_rgba(canvas.get(), 53.0F / 255.0F, 196.0F / 255.0F,
                            106.0F / 255.0F, 1.0F);
    plutovg_canvas_ellipse(canvas.get(), 52.0F, 52.0F, 18.0F, 12.0F);
    plutovg_canvas_fill(canvas.get());

    plutovg_canvas_move_to(canvas.get(), 96.0F, 104.0F);
    plutovg_canvas_line_to(canvas.get(), 128.0F, 34.0F);
    plutovg_canvas_line_to(canvas.get(), 164.0F, 104.0F);
    plutovg_canvas_close_path(canvas.get());
    plutovg_rect_t fill_extents{};
    plutovg_canvas_fill_extents(canvas.get(), &fill_extents);
    const bool fill_hit =
        plutovg_canvas_fill_contains(canvas.get(), 128.0F, 70.0F);
    const bool fill_miss =
        !plutovg_canvas_fill_contains(canvas.get(), 80.0F, 70.0F);
    plutovg_canvas_set_rgba(canvas.get(), 167.0F / 255.0F, 139.0F / 255.0F,
                            250.0F / 255.0F, 0.72F);
    plutovg_canvas_fill(canvas.get());

    plutovg_canvas_move_to(canvas.get(), 184.0F, 58.0F);
    plutovg_canvas_line_to(canvas.get(), 197.0F, 72.0F);
    plutovg_canvas_line_to(canvas.get(), 224.0F, 38.0F);
    plutovg_canvas_set_line_width(canvas.get(), 6.0F);
    plutovg_rect_t stroke_extents{};
    plutovg_canvas_stroke_extents(canvas.get(), &stroke_extents);
    const bool stroke_hit =
        plutovg_canvas_stroke_contains(canvas.get(), 190.5F, 65.0F);
    plutovg_canvas_set_rgba(canvas.get(), 1.0F, 1.0F, 1.0F, 1.0F);
    plutovg_canvas_stroke(canvas.get());

    plutovg_canvas_save(canvas.get());
    plutovg_canvas_translate(canvas.get(), 195.0F, 102.0F);
    plutovg_canvas_rotate(canvas.get(), 0.22F);
    plutovg_canvas_translate(canvas.get(), -195.0F, -102.0F);
    plutovg_canvas_rect(canvas.get(), 174.0F, 92.0F, 42.0F, 20.0F);
    plutovg_rect_t transformed_extents{};
    plutovg_canvas_fill_extents(canvas.get(), &transformed_extents);
    plutovg_canvas_set_rgba(canvas.get(), 249.0F / 255.0F, 115.0F / 255.0F,
                            22.0F / 255.0F, 1.0F);
    plutovg_canvas_fill(canvas.get());
    plutovg_canvas_restore(canvas.get());

    Evidence evidence;
    evidence.rgba = straight_rgba_from_native_argb32(
        plutovg_surface_get_data(surface.get()),
        plutovg_surface_get_stride(surface.get()), canvas_width, canvas_height);
    PngSink sink{&evidence.png};
    if (!plutovg_surface_write_to_png_stream(surface.get(), &append_png,
                                             &sink) ||
        sink.failed) {
      return std::unexpected(
          Error{"png_failure", "PlutoVG PNG stream encoding failed"});
    }
    evidence.fill_bounds = to_bounds(fill_extents);
    evidence.stroke_bounds = to_bounds(stroke_extents);
    evidence.transformed_bounds = to_bounds(transformed_extents);
    evidence.fill_hit = fill_hit;
    evidence.fill_miss = fill_miss;
    evidence.stroke_hit = stroke_hit;
    evidence.native_fill_hit_test = true;
    evidence.native_stroke_hit_test = true;
    // Canvas extents are raster-span bounds rounded to device pixels. They are
    // excellent dirty bounds, but not exact geometric bounds.
    evidence.native_tight_bounds = false;
    return evidence;
  } catch (const std::exception &exception) {
    return std::unexpected(Error{"exception_contained", exception.what()});
  } catch (...) {
    return std::unexpected(
        Error{"exception_contained", "unknown PlutoVG adapter exception"});
  }
}

} // namespace drawforge::renderer_spike
