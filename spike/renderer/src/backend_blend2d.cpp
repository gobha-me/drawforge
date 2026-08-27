#include "backend.hpp"

#include <blend2d/blend2d.h>

#include <exception>

namespace drawforge::renderer_spike {
namespace {

[[nodiscard]] auto failed(BLResult result) noexcept -> bool {
  return result != BL_SUCCESS;
}

[[nodiscard]] auto make_error(std::string_view operation, BLResult result)
    -> Error {
  return Error{"backend_failure", std::string(operation) +
                                      " failed with Blend2D status " +
                                      std::to_string(result)};
}

} // namespace

auto backend_name() noexcept -> std::string_view { return "blend2d"; }

auto render_scene() noexcept -> std::expected<Evidence, Error> {
  try {
    BLImage image;
    if (const auto result =
            image.create(canvas_width, canvas_height, BL_FORMAT_PRGB32);
        failed(result)) {
      return std::unexpected(make_error("image.create", result));
    }

    BLContext context;
    if (const auto result = context.begin(image); failed(result)) {
      return std::unexpected(make_error("context.begin", result));
    }
    if (failed(context.set_comp_op(BL_COMP_OP_SRC_COPY)) ||
        failed(context.fill_all(BLRgba32(0xfff8fafcU))) ||
        failed(context.set_comp_op(BL_COMP_OP_SRC_OVER)) ||
        failed(context.fill_round_rect(8.0, 8.0, 240.0, 128.0, 16.0, 16.0,
                                       BLRgba32(0xff172033U))) ||
        failed(context.fill_ellipse(52.0, 52.0, 18.0, 12.0,
                                    BLRgba32(0xff35c46aU)))) {
      context.end();
      return std::unexpected(
          Error{"backend_failure", "Blend2D primitive drawing failed"});
    }

    BLPath filled_path;
    if (failed(filled_path.move_to(96.0, 104.0)) ||
        failed(filled_path.line_to(128.0, 34.0)) ||
        failed(filled_path.line_to(164.0, 104.0)) ||
        failed(filled_path.close())) {
      context.end();
      return std::unexpected(
          Error{"backend_failure", "Blend2D fill path construction failed"});
    }
    if (failed(context.save()) || failed(context.set_global_alpha(0.72)) ||
        failed(context.fill_path(filled_path, BLRgba32(0xffa78bfaU))) ||
        failed(context.restore())) {
      context.end();
      return std::unexpected(
          Error{"backend_failure", "Blend2D fill path rendering failed"});
    }

    BLPath stroke_path;
    if (failed(stroke_path.move_to(184.0, 58.0)) ||
        failed(stroke_path.line_to(197.0, 72.0)) ||
        failed(stroke_path.line_to(224.0, 38.0)) ||
        failed(context.set_stroke_width(6.0)) ||
        failed(context.stroke_path(stroke_path, BLRgba32(0xffffffffU)))) {
      context.end();
      return std::unexpected(
          Error{"backend_failure", "Blend2D stroke rendering failed"});
    }

    BLPath transformed_path;
    if (failed(transformed_path.add_rect(174.0, 92.0, 42.0, 20.0))) {
      context.end();
      return std::unexpected(
          Error{"backend_failure", "Blend2D transformed path creation failed"});
    }
    BLMatrix2D transform;
    transform.reset();
    if (failed(transform.rotate(0.22, 195.0, 102.0)) ||
        failed(transformed_path.transform(transform)) ||
        failed(context.fill_path(transformed_path, BLRgba32(0xfff97316U))) ||
        failed(context.end())) {
      return std::unexpected(
          Error{"backend_failure", "Blend2D transformed rendering failed"});
    }

    BLBox fill_box;
    BLBox stroke_box;
    BLBox transformed_box;
    if (failed(filled_path.get_bounding_box(&fill_box)) ||
        failed(stroke_path.get_bounding_box(&stroke_box)) ||
        failed(transformed_path.get_bounding_box(&transformed_box))) {
      return std::unexpected(
          Error{"backend_failure", "Blend2D path bounds failed"});
    }

    BLImageData data;
    if (const auto result = image.get_data(&data); failed(result)) {
      return std::unexpected(make_error("image.get_data", result));
    }
    auto rgba = straight_rgba_from_native_argb32(
        static_cast<const std::uint8_t *>(data.pixel_data), data.stride,
        canvas_width, canvas_height);

    BLImageCodec codec;
    if (const auto result = codec.find_by_name("PNG"); failed(result)) {
      return std::unexpected(make_error("codec.find_by_name", result));
    }
    BLArray<std::uint8_t> encoded;
    if (const auto result = image.write_to_data(encoded, codec);
        failed(result)) {
      return std::unexpected(make_error("image.write_to_data", result));
    }

    Evidence evidence;
    evidence.rgba = std::move(rgba);
    evidence.png.assign(encoded.data(), encoded.data() + encoded.size());
    evidence.fill_bounds =
        Bounds{fill_box.x0, fill_box.y0, fill_box.x1 - fill_box.x0,
               fill_box.y1 - fill_box.y0};
    evidence.stroke_bounds = Bounds{stroke_box.x0 - 3.0, stroke_box.y0 - 3.0,
                                    stroke_box.x1 - stroke_box.x0 + 6.0,
                                    stroke_box.y1 - stroke_box.y0 + 6.0};
    evidence.transformed_bounds =
        Bounds{transformed_box.x0, transformed_box.y0,
               transformed_box.x1 - transformed_box.x0,
               transformed_box.y1 - transformed_box.y0};
    evidence.fill_hit =
        filled_path.hit_test(BLPoint(128.0, 70.0), BL_FILL_RULE_NON_ZERO) ==
        BL_HIT_TEST_IN;
    evidence.fill_miss =
        filled_path.hit_test(BLPoint(80.0, 70.0), BL_FILL_RULE_NON_ZERO) ==
        BL_HIT_TEST_OUT;
    evidence.stroke_hit = false;
    evidence.native_fill_hit_test = true;
    evidence.native_stroke_hit_test = false;
    evidence.native_tight_bounds = true;
    return evidence;
  } catch (const std::exception &exception) {
    return std::unexpected(Error{"exception_contained", exception.what()});
  } catch (...) {
    return std::unexpected(
        Error{"exception_contained", "unknown Blend2D adapter exception"});
  }
}

} // namespace drawforge::renderer_spike
