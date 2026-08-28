#pragma once

#include <drawforge/cancellation.hpp>
#include <drawforge/scene.hpp>

#include <cstdint>
#include <expected>
#include <memory>
#include <span>
#include <string_view>

namespace drawforge {

namespace detail {
struct RenderAccess;
}

enum class RenderErrorCode : std::uint8_t {
  cancelled = 0,
  resource_limit = 1,
  number_out_of_range = 2,
  arithmetic_overflow = 3,
  allocation_failure = 4,
  renderer_failure = 5,
  png_encoding_failure = 6,
};

struct RenderError {
  RenderErrorCode code{};
  std::string_view message{};
  auto operator==(const RenderError &) const -> bool = default;
};

[[nodiscard]] auto render_error_code_name(RenderErrorCode code) noexcept
    -> std::string_view;

enum class PixelFormat : std::uint8_t { rgba8_srgb_straight_alpha = 0 };

struct RendererInfo {
  std::string_view name{};
  std::string_view version{};
  std::uint32_t contract_version{};
  auto operator==(const RendererInfo &) const -> bool = default;
};

[[nodiscard]] auto renderer_info() noexcept -> RendererInfo;

class RenderConfig {
public:
  [[nodiscard]] static auto create(std::uint64_t time_us,
                                   std::uint64_t max_output_bytes) noexcept
      -> std::expected<RenderConfig, RenderError>;

  [[nodiscard]] constexpr auto time_us() const noexcept -> std::uint64_t {
    return m_time_us;
  }
  [[nodiscard]] constexpr auto max_output_bytes() const noexcept
      -> std::uint64_t {
    return m_max_output_bytes;
  }
  [[nodiscard]] static constexpr auto pixel_format() noexcept -> PixelFormat {
    return PixelFormat::rgba8_srgb_straight_alpha;
  }
  auto operator==(const RenderConfig &) const -> bool = default;

private:
  constexpr RenderConfig(std::uint64_t time_us,
                         std::uint64_t max_output_bytes) noexcept
      : m_time_us{time_us}, m_max_output_bytes{max_output_bytes} {}

  std::uint64_t m_time_us{};
  std::uint64_t m_max_output_bytes{};
};

class RgbaImage {
public:
  RgbaImage(const RgbaImage &) noexcept = default;
  RgbaImage(RgbaImage &&other) noexcept : m_data{other.m_data} {}
  auto operator=(const RgbaImage &) noexcept -> RgbaImage & = default;
  auto operator=(RgbaImage &&other) noexcept -> RgbaImage & {
    m_data = other.m_data;
    return *this;
  }
  ~RgbaImage();

  [[nodiscard]] auto extent() const noexcept -> CanvasExtent;
  [[nodiscard]] auto time_us() const noexcept -> std::uint64_t;
  [[nodiscard]] auto format() const noexcept -> PixelFormat;
  [[nodiscard]] auto renderer() const noexcept -> RendererInfo;
  [[nodiscard]] auto pixels() const noexcept -> std::span<const std::uint8_t>;

private:
  struct Data;
  explicit RgbaImage(std::shared_ptr<const Data> data) noexcept
      : m_data{std::move(data)} {}
  std::shared_ptr<const Data> m_data;

  friend struct detail::RenderAccess;
};

class PngImage {
public:
  PngImage(const PngImage &) noexcept = default;
  PngImage(PngImage &&other) noexcept : m_data{other.m_data} {}
  auto operator=(const PngImage &) noexcept -> PngImage & = default;
  auto operator=(PngImage &&other) noexcept -> PngImage & {
    m_data = other.m_data;
    return *this;
  }
  ~PngImage();

  [[nodiscard]] auto extent() const noexcept -> CanvasExtent;
  [[nodiscard]] auto time_us() const noexcept -> std::uint64_t;
  [[nodiscard]] auto renderer() const noexcept -> RendererInfo;
  [[nodiscard]] auto bytes() const noexcept -> std::span<const std::uint8_t>;

private:
  struct Data;
  explicit PngImage(std::shared_ptr<const Data> data) noexcept
      : m_data{std::move(data)} {}
  std::shared_ptr<const Data> m_data;

  friend struct detail::RenderAccess;
};

[[nodiscard]] auto render_rgba(const Document &document,
                               const RenderConfig &config,
                               CancellationToken cancellation = {}) noexcept
    -> std::expected<RgbaImage, RenderError>;

[[nodiscard]] auto encode_png(const RgbaImage &image,
                              CancellationToken cancellation = {}) noexcept
    -> std::expected<PngImage, RenderError>;

} // namespace drawforge
