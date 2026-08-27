#pragma once

#include <compare>
#include <cstdint>
#include <expected>
#include <string>
#include <string_view>
#include <utility>

namespace drawforge {

enum class ValueErrorCode : std::uint8_t {
  empty_value = 0,
  invalid_identifier = 1,
  invalid_utf8 = 2,
  embedded_nul = 3,
  non_finite_number = 4,
  number_out_of_range = 5,
  invalid_extent = 6,
  invalid_limit = 7,
  resource_limit = 8,
  arithmetic_overflow = 9,
  duplicate_identity = 10,
  revision_overflow = 11,
  allocation_failure = 12,
};

struct ValueError {
  ValueErrorCode code{};
  std::string_view message{};

  auto operator==(const ValueError &) const -> bool = default;
};

[[nodiscard]] auto value_error_code_name(ValueErrorCode code) noexcept
    -> std::string_view;

struct ResourceLimitRequest {
  std::uint32_t max_identifier_bytes{64};
  std::uint32_t max_text_bytes{4U * 1024U};
  double max_numeric_magnitude{65'536.0};
  std::uint32_t max_canvas_dimension{4'096};
  std::uint64_t max_canvas_pixels{16ULL * 1024ULL * 1024ULL};
  std::uint64_t max_scene_nodes{4'096};
  std::uint64_t max_transaction_operations{256};
  std::uint64_t max_output_bytes{64ULL * 1024ULL * 1024ULL};
  std::uint32_t max_nesting_depth{32};

  auto operator==(const ResourceLimitRequest &) const -> bool = default;
};

inline constexpr ResourceLimitRequest default_resource_limit_values{};
inline constexpr ResourceLimitRequest hard_resource_limit_values{
    .max_identifier_bytes = 128,
    .max_text_bytes = 64U * 1024U,
    .max_numeric_magnitude = 1'048'576.0,
    .max_canvas_dimension = 16'384,
    .max_canvas_pixels = 64ULL * 1024ULL * 1024ULL,
    .max_scene_nodes = 65'536,
    .max_transaction_operations = 4'096,
    .max_output_bytes = 256ULL * 1024ULL * 1024ULL,
    .max_nesting_depth = 128,
};

class ResourceLimits {
public:
  constexpr ResourceLimits() noexcept = default;

  [[nodiscard]] static auto create(ResourceLimitRequest values) noexcept
      -> std::expected<ResourceLimits, ValueError>;

  [[nodiscard]] constexpr auto values() const noexcept
      -> const ResourceLimitRequest & {
    return m_values;
  }
  [[nodiscard]] constexpr auto max_identifier_bytes() const noexcept
      -> std::uint32_t {
    return m_values.max_identifier_bytes;
  }
  [[nodiscard]] constexpr auto max_text_bytes() const noexcept
      -> std::uint32_t {
    return m_values.max_text_bytes;
  }
  [[nodiscard]] constexpr auto max_numeric_magnitude() const noexcept
      -> double {
    return m_values.max_numeric_magnitude;
  }
  [[nodiscard]] constexpr auto max_canvas_dimension() const noexcept
      -> std::uint32_t {
    return m_values.max_canvas_dimension;
  }
  [[nodiscard]] constexpr auto max_canvas_pixels() const noexcept
      -> std::uint64_t {
    return m_values.max_canvas_pixels;
  }
  [[nodiscard]] constexpr auto max_scene_nodes() const noexcept
      -> std::uint64_t {
    return m_values.max_scene_nodes;
  }
  [[nodiscard]] constexpr auto max_transaction_operations() const noexcept
      -> std::uint64_t {
    return m_values.max_transaction_operations;
  }
  [[nodiscard]] constexpr auto max_output_bytes() const noexcept
      -> std::uint64_t {
    return m_values.max_output_bytes;
  }
  [[nodiscard]] constexpr auto max_nesting_depth() const noexcept
      -> std::uint32_t {
    return m_values.max_nesting_depth;
  }

  auto operator==(const ResourceLimits &) const -> bool = default;

private:
  explicit constexpr ResourceLimits(ResourceLimitRequest values) noexcept
      : m_values{values} {}

  ResourceLimitRequest m_values{default_resource_limit_values};
};

#define DRAWFORGE_DECLARE_ID_TYPE(Name)                                        \
  class Name {                                                                 \
  public:                                                                      \
    [[nodiscard]] static auto                                                  \
    create(std::string_view value, const ResourceLimits &limits = {}) noexcept \
        -> std::expected<Name, ValueError>;                                    \
    [[nodiscard]] auto value() const noexcept -> std::string_view {            \
      return m_value;                                                          \
    }                                                                          \
    auto operator<=>(const Name &) const = default;                            \
                                                                               \
  private:                                                                     \
    explicit Name(std::string value) noexcept : m_value{std::move(value)} {}   \
    std::string m_value;                                                       \
  }

// IDs preserve an ASCII spelling matching [A-Za-z][A-Za-z0-9._-]*. Each
// concrete type is a separate collision domain and has no cross-domain
// conversion.
DRAWFORGE_DECLARE_ID_TYPE(DocumentId);
DRAWFORGE_DECLARE_ID_TYPE(LayerId);
DRAWFORGE_DECLARE_ID_TYPE(ObjectId);
DRAWFORGE_DECLARE_ID_TYPE(TransactionId);
DRAWFORGE_DECLARE_ID_TYPE(AssetId);
DRAWFORGE_DECLARE_ID_TYPE(TrackId);

#undef DRAWFORGE_DECLARE_ID_TYPE

class BoundedText {
public:
  [[nodiscard]] static auto create(std::string_view value,
                                   const ResourceLimits &limits = {}) noexcept
      -> std::expected<BoundedText, ValueError>;

  [[nodiscard]] auto value() const noexcept -> std::string_view {
    return m_value;
  }
  auto operator<=>(const BoundedText &) const = default;

private:
  explicit BoundedText(std::string value) noexcept
      : m_value{std::move(value)} {}
  std::string m_value;
};

class Revision {
public:
  explicit constexpr Revision(std::uint64_t value = 0) noexcept
      : m_value{value} {}

  [[nodiscard]] constexpr auto value() const noexcept -> std::uint64_t {
    return m_value;
  }
  [[nodiscard]] auto next() const noexcept
      -> std::expected<Revision, ValueError>;
  auto operator<=>(const Revision &) const = default;

private:
  std::uint64_t m_value{};
};

class Coordinate {
public:
  constexpr Coordinate() noexcept = default;

  [[nodiscard]] static auto create(double value,
                                   const ResourceLimits &limits = {}) noexcept
      -> std::expected<Coordinate, ValueError>;

  [[nodiscard]] constexpr auto value() const noexcept -> double {
    return m_value;
  }
  auto operator==(const Coordinate &) const -> bool = default;

private:
  friend class AffineTransform;
  explicit constexpr Coordinate(double value) noexcept : m_value{value} {}
  double m_value{};
};

class Point {
public:
  constexpr Point() noexcept = default;
  constexpr Point(Coordinate x, Coordinate y) noexcept : m_x{x}, m_y{y} {}

  [[nodiscard]] static auto create(double x, double y,
                                   const ResourceLimits &limits = {}) noexcept
      -> std::expected<Point, ValueError>;

  [[nodiscard]] constexpr auto x() const noexcept -> Coordinate { return m_x; }
  [[nodiscard]] constexpr auto y() const noexcept -> Coordinate { return m_y; }
  auto operator==(const Point &) const -> bool = default;

private:
  Coordinate m_x{};
  Coordinate m_y{};
};

class AffineTransform {
public:
  constexpr AffineTransform() noexcept = default;

  [[nodiscard]] static auto create(double a, double b, double c, double d,
                                   double e, double f,
                                   const ResourceLimits &limits = {}) noexcept
      -> std::expected<AffineTransform, ValueError>;

  [[nodiscard]] constexpr auto a() const noexcept -> Coordinate { return m_a; }
  [[nodiscard]] constexpr auto b() const noexcept -> Coordinate { return m_b; }
  [[nodiscard]] constexpr auto c() const noexcept -> Coordinate { return m_c; }
  [[nodiscard]] constexpr auto d() const noexcept -> Coordinate { return m_d; }
  [[nodiscard]] constexpr auto e() const noexcept -> Coordinate { return m_e; }
  [[nodiscard]] constexpr auto f() const noexcept -> Coordinate { return m_f; }

  [[nodiscard]] auto apply(Point point,
                           const ResourceLimits &limits = {}) const noexcept
      -> std::expected<Point, ValueError>;

  // Returns a transform that applies this transform and then `next`.
  [[nodiscard]] auto then(const AffineTransform &next,
                          const ResourceLimits &limits = {}) const noexcept
      -> std::expected<AffineTransform, ValueError>;

  auto operator==(const AffineTransform &) const -> bool = default;

private:
  constexpr AffineTransform(Coordinate a, Coordinate b, Coordinate c,
                            Coordinate d, Coordinate e, Coordinate f) noexcept
      : m_a{a}, m_b{b}, m_c{c}, m_d{d}, m_e{e}, m_f{f} {}

  Coordinate m_a{Coordinate{1.0}};
  Coordinate m_b{Coordinate{}};
  Coordinate m_c{Coordinate{}};
  Coordinate m_d{Coordinate{1.0}};
  Coordinate m_e{};
  Coordinate m_f{};
};

class CanvasExtent {
public:
  [[nodiscard]] static auto create(std::uint64_t width, std::uint64_t height,
                                   const ResourceLimits &limits = {}) noexcept
      -> std::expected<CanvasExtent, ValueError>;

  [[nodiscard]] constexpr auto width() const noexcept -> std::uint32_t {
    return m_width;
  }
  [[nodiscard]] constexpr auto height() const noexcept -> std::uint32_t {
    return m_height;
  }
  [[nodiscard]] constexpr auto pixel_count() const noexcept -> std::uint64_t {
    return m_pixel_count;
  }
  [[nodiscard]] constexpr auto rgba8_bytes() const noexcept -> std::uint64_t {
    return m_rgba8_bytes;
  }
  auto operator==(const CanvasExtent &) const -> bool = default;

private:
  constexpr CanvasExtent(std::uint32_t width, std::uint32_t height,
                         std::uint64_t pixel_count,
                         std::uint64_t rgba8_bytes) noexcept
      : m_width{width}, m_height{height}, m_pixel_count{pixel_count},
        m_rgba8_bytes{rgba8_bytes} {}

  std::uint32_t m_width{};
  std::uint32_t m_height{};
  std::uint64_t m_pixel_count{};
  std::uint64_t m_rgba8_bytes{};
};

} // namespace drawforge
