#pragma once

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <expected>
#include <limits>
#include <span>
#include <string>
#include <string_view>
#include <vector>

namespace drawforge::renderer_spike {

inline constexpr int canvas_width = 256;
inline constexpr int canvas_height = 144;
inline constexpr std::size_t rgba_size =
    static_cast<std::size_t>(canvas_width) * canvas_height * 4U;

struct Bounds {
  double x{};
  double y{};
  double width{};
  double height{};
};

struct Error {
  std::string code;
  std::string message;
};

struct Evidence {
  std::vector<std::uint8_t> rgba;
  std::vector<std::uint8_t> png;
  Bounds fill_bounds;
  Bounds stroke_bounds;
  Bounds transformed_bounds;
  bool fill_hit{};
  bool fill_miss{};
  bool stroke_hit{};
  bool native_fill_hit_test{};
  bool native_stroke_hit_test{};
  bool native_tight_bounds{};
};

[[nodiscard]] auto backend_name() noexcept -> std::string_view;
[[nodiscard]] auto render_scene() noexcept -> std::expected<Evidence, Error>;

[[nodiscard]] inline auto
checked_rgba_size(std::size_t width, std::size_t height, std::size_t byte_limit)
    -> std::expected<std::size_t, Error> {
  if (width == 0U || height == 0U) {
    return std::unexpected(
        Error{"invalid_extent", "canvas dimensions must be nonzero"});
  }
  if (width > std::numeric_limits<std::size_t>::max() / height ||
      width * height > std::numeric_limits<std::size_t>::max() / 4U) {
    return std::unexpected(
        Error{"numeric_overflow", "RGBA size overflows size_t"});
  }
  const auto bytes = width * height * 4U;
  if (bytes > byte_limit) {
    return std::unexpected(
        Error{"resource_limit", "RGBA size exceeds the configured limit"});
  }
  return bytes;
}

[[nodiscard]] inline auto
straight_rgba_from_native_argb32(const std::uint8_t *source,
                                 std::ptrdiff_t stride, int width, int height)
    -> std::vector<std::uint8_t> {
  std::vector<std::uint8_t> rgba(static_cast<std::size_t>(width) * height * 4U);
  for (int y = 0; y < height; ++y) {
    const auto *row = source + static_cast<std::ptrdiff_t>(y) * stride;
    for (int x = 0; x < width; ++x) {
      const auto *pixel = row + static_cast<std::ptrdiff_t>(x) * 4;
      const auto alpha = pixel[3];
      const auto unpremultiply = [alpha](std::uint8_t value) -> std::uint8_t {
        if (alpha == 0U) {
          return 0U;
        }
        const auto straight =
            (static_cast<unsigned>(value) * 255U + alpha / 2U) / alpha;
        return static_cast<std::uint8_t>(std::min(straight, 255U));
      };
      const auto offset = (static_cast<std::size_t>(y) * width + x) * 4U;
      rgba[offset + 0U] = unpremultiply(pixel[2]);
      rgba[offset + 1U] = unpremultiply(pixel[1]);
      rgba[offset + 2U] = unpremultiply(pixel[0]);
      rgba[offset + 3U] = alpha;
    }
  }
  return rgba;
}

[[nodiscard]] inline auto
png_signature_is_valid(std::span<const std::uint8_t> bytes) noexcept -> bool {
  constexpr std::uint8_t signature[]{0x89U, 0x50U, 0x4eU, 0x47U,
                                     0x0dU, 0x0aU, 0x1aU, 0x0aU};
  return bytes.size() >= std::size(signature) &&
         std::equal(std::begin(signature), std::end(signature), bytes.begin());
}

} // namespace drawforge::renderer_spike
