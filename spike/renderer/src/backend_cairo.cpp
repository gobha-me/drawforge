#include "backend.hpp"

#include <cairo.h>

#include <exception>
#include <memory>
#include <numbers>

namespace drawforge::renderer_spike {
namespace {

using Surface =
    std::unique_ptr<cairo_surface_t, decltype(&cairo_surface_destroy)>;
using Context = std::unique_ptr<cairo_t, decltype(&cairo_destroy)>;

auto png_write(void *closure, const unsigned char *data, unsigned int length)
    -> cairo_status_t {
  auto &bytes = *static_cast<std::vector<std::uint8_t> *>(closure);
  try {
    bytes.insert(bytes.end(), data, data + length);
    return CAIRO_STATUS_SUCCESS;
  } catch (...) {
    return CAIRO_STATUS_NO_MEMORY;
  }
}

auto round_rect(cairo_t *context, double x, double y, double width,
                double height, double radius) -> void {
  constexpr auto quarter_turn = std::numbers::pi / 2.0;
  cairo_new_sub_path(context);
  cairo_arc(context, x + width - radius, y + radius, radius, -quarter_turn,
            0.0);
  cairo_arc(context, x + width - radius, y + height - radius, radius, 0.0,
            quarter_turn);
  cairo_arc(context, x + radius, y + height - radius, radius, quarter_turn,
            std::numbers::pi);
  cairo_arc(context, x + radius, y + radius, radius, std::numbers::pi,
            3.0 * quarter_turn);
  cairo_close_path(context);
}

[[nodiscard]] auto bounds(double x1, double y1, double x2, double y2) noexcept
    -> Bounds {
  return Bounds{x1, y1, x2 - x1, y2 - y1};
}

} // namespace

auto backend_name() noexcept -> std::string_view { return "cairo"; }

auto render_scene() noexcept -> std::expected<Evidence, Error> {
  try {
    Surface surface(cairo_image_surface_create(CAIRO_FORMAT_ARGB32,
                                               canvas_width, canvas_height),
                    &cairo_surface_destroy);
    if (cairo_surface_status(surface.get()) != CAIRO_STATUS_SUCCESS) {
      return std::unexpected(
          Error{"allocation_failure", "Cairo surface creation failed"});
    }
    Context context(cairo_create(surface.get()), &cairo_destroy);
    if (cairo_status(context.get()) != CAIRO_STATUS_SUCCESS) {
      return std::unexpected(
          Error{"allocation_failure", "Cairo context creation failed"});
    }

    cairo_set_source_rgb(context.get(), 248.0 / 255.0, 250.0 / 255.0,
                         252.0 / 255.0);
    cairo_paint(context.get());
    round_rect(context.get(), 8.0, 8.0, 240.0, 128.0, 16.0);
    cairo_set_source_rgb(context.get(), 23.0 / 255.0, 32.0 / 255.0,
                         51.0 / 255.0);
    cairo_fill(context.get());

    cairo_save(context.get());
    cairo_translate(context.get(), 52.0, 52.0);
    cairo_scale(context.get(), 18.0, 12.0);
    cairo_arc(context.get(), 0.0, 0.0, 1.0, 0.0, 2.0 * std::numbers::pi);
    cairo_restore(context.get());
    cairo_set_source_rgb(context.get(), 53.0 / 255.0, 196.0 / 255.0,
                         106.0 / 255.0);
    cairo_fill(context.get());

    cairo_move_to(context.get(), 96.0, 104.0);
    cairo_line_to(context.get(), 128.0, 34.0);
    cairo_line_to(context.get(), 164.0, 104.0);
    cairo_close_path(context.get());
    double fill_x1{};
    double fill_y1{};
    double fill_x2{};
    double fill_y2{};
    cairo_fill_extents(context.get(), &fill_x1, &fill_y1, &fill_x2, &fill_y2);
    const bool fill_hit = cairo_in_fill(context.get(), 128.0, 70.0) != 0;
    const bool fill_miss = cairo_in_fill(context.get(), 80.0, 70.0) == 0;
    cairo_set_source_rgba(context.get(), 167.0 / 255.0, 139.0 / 255.0,
                          250.0 / 255.0, 0.72);
    cairo_fill(context.get());

    cairo_move_to(context.get(), 184.0, 58.0);
    cairo_line_to(context.get(), 197.0, 72.0);
    cairo_line_to(context.get(), 224.0, 38.0);
    cairo_set_line_width(context.get(), 6.0);
    double stroke_x1{};
    double stroke_y1{};
    double stroke_x2{};
    double stroke_y2{};
    cairo_stroke_extents(context.get(), &stroke_x1, &stroke_y1, &stroke_x2,
                         &stroke_y2);
    const bool stroke_hit = cairo_in_stroke(context.get(), 190.5, 65.0) != 0;
    cairo_set_source_rgb(context.get(), 1.0, 1.0, 1.0);
    cairo_stroke(context.get());

    cairo_save(context.get());
    cairo_translate(context.get(), 195.0, 102.0);
    cairo_rotate(context.get(), 0.22);
    cairo_translate(context.get(), -195.0, -102.0);
    cairo_rectangle(context.get(), 174.0, 92.0, 42.0, 20.0);
    double transformed_x1{};
    double transformed_y1{};
    double transformed_x2{};
    double transformed_y2{};
    cairo_fill_extents(context.get(), &transformed_x1, &transformed_y1,
                       &transformed_x2, &transformed_y2);
    cairo_set_source_rgb(context.get(), 249.0 / 255.0, 115.0 / 255.0,
                         22.0 / 255.0);
    cairo_fill(context.get());
    cairo_restore(context.get());

    if (cairo_status(context.get()) != CAIRO_STATUS_SUCCESS) {
      return std::unexpected(
          Error{"backend_failure",
                cairo_status_to_string(cairo_status(context.get()))});
    }
    cairo_surface_flush(surface.get());

    Evidence evidence;
    evidence.rgba = straight_rgba_from_native_argb32(
        cairo_image_surface_get_data(surface.get()),
        cairo_image_surface_get_stride(surface.get()), canvas_width,
        canvas_height);
    const auto png_status = cairo_surface_write_to_png_stream(
        surface.get(), &png_write, &evidence.png);
    if (png_status != CAIRO_STATUS_SUCCESS) {
      return std::unexpected(
          Error{"png_failure", cairo_status_to_string(png_status)});
    }
    evidence.fill_bounds = bounds(fill_x1, fill_y1, fill_x2, fill_y2);
    evidence.stroke_bounds = bounds(stroke_x1, stroke_y1, stroke_x2, stroke_y2);
    evidence.transformed_bounds =
        bounds(transformed_x1, transformed_y1, transformed_x2, transformed_y2);
    evidence.fill_hit = fill_hit;
    evidence.fill_miss = fill_miss;
    evidence.stroke_hit = stroke_hit;
    evidence.native_fill_hit_test = true;
    evidence.native_stroke_hit_test = true;
    // Extents under a rotated CTM are conservative when expressed back in user
    // space, so the adapter cannot advertise exact transformed geometry bounds.
    evidence.native_tight_bounds = false;
    return evidence;
  } catch (const std::exception &exception) {
    return std::unexpected(Error{"exception_contained", exception.what()});
  } catch (...) {
    return std::unexpected(
        Error{"exception_contained", "unknown Cairo adapter exception"});
  }
}

} // namespace drawforge::renderer_spike
