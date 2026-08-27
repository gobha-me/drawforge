#include <drawforge/foundation.hpp>

#include "foundation_internal.hpp"

#include <cmath>
#include <cstddef>
#include <limits>
#include <new>
#include <string>
#include <utility>

namespace drawforge {
namespace {

constexpr ValueError empty_value_error{ValueErrorCode::empty_value,
                                       "value must not be empty"};
constexpr ValueError invalid_identifier_error{
    ValueErrorCode::invalid_identifier,
    "identifier must match [A-Za-z][A-Za-z0-9._-]*"};
constexpr ValueError invalid_utf8_error{ValueErrorCode::invalid_utf8,
                                        "text must be valid UTF-8"};
constexpr ValueError embedded_nul_error{ValueErrorCode::embedded_nul,
                                        "text must not contain NUL"};
constexpr ValueError non_finite_number_error{ValueErrorCode::non_finite_number,
                                             "number must be finite"};
constexpr ValueError number_out_of_range_error{
    ValueErrorCode::number_out_of_range,
    "number exceeds the configured magnitude"};
constexpr ValueError invalid_extent_error{ValueErrorCode::invalid_extent,
                                          "canvas dimensions must be non-zero"};
constexpr ValueError invalid_limit_error{
    ValueErrorCode::invalid_limit,
    "resource limit is invalid or exceeds the hard ceiling"};
constexpr ValueError resource_limit_error{ValueErrorCode::resource_limit,
                                          "value exceeds the configured limit"};
constexpr ValueError revision_overflow_error{ValueErrorCode::revision_overflow,
                                             "revision cannot be advanced"};
constexpr ValueError allocation_failure_error{
    ValueErrorCode::allocation_failure, "value storage allocation failed"};

[[nodiscard]] constexpr auto ascii_alpha(unsigned char value) noexcept -> bool {
  return (value >= static_cast<unsigned char>('A') &&
          value <= static_cast<unsigned char>('Z')) ||
         (value >= static_cast<unsigned char>('a') &&
          value <= static_cast<unsigned char>('z'));
}

[[nodiscard]] constexpr auto identifier_tail(unsigned char value) noexcept
    -> bool {
  return ascii_alpha(value) ||
         (value >= static_cast<unsigned char>('0') &&
          value <= static_cast<unsigned char>('9')) ||
         value == static_cast<unsigned char>('.') ||
         value == static_cast<unsigned char>('_') ||
         value == static_cast<unsigned char>('-');
}

[[nodiscard]] auto validate_identifier(std::string_view value,
                                       const ResourceLimits &limits) noexcept
    -> std::expected<std::string, ValueError> {
  if (value.empty()) {
    return std::unexpected{empty_value_error};
  }
  if (value.size() > limits.max_identifier_bytes()) {
    return std::unexpected{resource_limit_error};
  }
  if (!ascii_alpha(static_cast<unsigned char>(value.front()))) {
    return std::unexpected{invalid_identifier_error};
  }
  for (const char character : value.substr(1)) {
    if (!identifier_tail(static_cast<unsigned char>(character))) {
      return std::unexpected{invalid_identifier_error};
    }
  }

  try {
    return std::string{value};
  } catch (const std::bad_alloc &) {
    return std::unexpected{allocation_failure_error};
  } catch (...) {
    return std::unexpected{allocation_failure_error};
  }
}

enum class Utf8Status : std::uint8_t {
  valid,
  embedded_nul,
  malformed,
};

[[nodiscard]] auto continuation(std::string_view value,
                                std::size_t index) noexcept -> int {
  if (index >= value.size()) {
    return -1;
  }
  const auto byte = static_cast<unsigned char>(value[index]);
  return (byte >= 0x80U && byte <= 0xBFU) ? static_cast<int>(byte) : -1;
}

[[nodiscard]] auto validate_utf8(std::string_view value) noexcept
    -> Utf8Status {
  for (std::size_t index = 0; index < value.size();) {
    const auto first = static_cast<unsigned char>(value[index]);
    if (first == 0) {
      return Utf8Status::embedded_nul;
    }
    if (first <= 0x7FU) {
      ++index;
      continue;
    }

    const auto second = continuation(value, index + 1);
    if (first >= 0xC2U && first <= 0xDFU && second >= 0) {
      index += 2;
      continue;
    }

    const auto third = continuation(value, index + 2);
    if (first >= 0xE0U && first <= 0xEFU && second >= 0 && third >= 0) {
      const auto valid_second = (first != 0xE0U || second >= 0xA0) &&
                                (first != 0xEDU || second <= 0x9F);
      if (valid_second) {
        index += 3;
        continue;
      }
    }

    const auto fourth = continuation(value, index + 3);
    if (first >= 0xF0U && first <= 0xF4U && second >= 0 && third >= 0 &&
        fourth >= 0) {
      const auto valid_second = (first != 0xF0U || second >= 0x90) &&
                                (first != 0xF4U || second <= 0x8F);
      if (valid_second) {
        index += 4;
        continue;
      }
    }

    return Utf8Status::malformed;
  }
  return Utf8Status::valid;
}

[[nodiscard]] auto checked_component(long double value,
                                     const ResourceLimits &limits) noexcept
    -> std::expected<double, ValueError> {
  const auto narrowed = static_cast<double>(value);
  const auto coordinate = Coordinate::create(narrowed, limits);
  if (!coordinate) {
    return std::unexpected{coordinate.error()};
  }
  return coordinate->value();
}

} // namespace

auto value_error_code_name(const ValueErrorCode code) noexcept
    -> std::string_view {
  switch (code) {
  case ValueErrorCode::empty_value:
    return "empty_value";
  case ValueErrorCode::invalid_identifier:
    return "invalid_identifier";
  case ValueErrorCode::invalid_utf8:
    return "invalid_utf8";
  case ValueErrorCode::embedded_nul:
    return "embedded_nul";
  case ValueErrorCode::non_finite_number:
    return "non_finite_number";
  case ValueErrorCode::number_out_of_range:
    return "number_out_of_range";
  case ValueErrorCode::invalid_extent:
    return "invalid_extent";
  case ValueErrorCode::invalid_limit:
    return "invalid_limit";
  case ValueErrorCode::resource_limit:
    return "resource_limit";
  case ValueErrorCode::arithmetic_overflow:
    return "arithmetic_overflow";
  case ValueErrorCode::duplicate_identity:
    return "duplicate_identity";
  case ValueErrorCode::revision_overflow:
    return "revision_overflow";
  case ValueErrorCode::allocation_failure:
    return "allocation_failure";
  }
  return "unknown";
}

auto ResourceLimits::create(const ResourceLimitRequest values) noexcept
    -> std::expected<ResourceLimits, ValueError> {
  const auto &hard = hard_resource_limit_values;
  if (values.max_identifier_bytes > hard.max_identifier_bytes ||
      values.max_text_bytes > hard.max_text_bytes ||
      values.max_canvas_dimension > hard.max_canvas_dimension ||
      values.max_canvas_pixels > hard.max_canvas_pixels ||
      values.max_scene_nodes > hard.max_scene_nodes ||
      values.max_transaction_operations > hard.max_transaction_operations ||
      values.max_output_bytes > hard.max_output_bytes ||
      values.max_nesting_depth > hard.max_nesting_depth) {
    return std::unexpected{invalid_limit_error};
  }
  if (!std::isfinite(values.max_numeric_magnitude)) {
    return std::unexpected{non_finite_number_error};
  }
  if (values.max_numeric_magnitude < 0.0 ||
      values.max_numeric_magnitude > hard.max_numeric_magnitude) {
    return std::unexpected{invalid_limit_error};
  }
  return ResourceLimits{values};
}

#define DRAWFORGE_IMPLEMENT_ID_TYPE(Name)                                      \
  auto Name::create(std::string_view value,                                    \
                    const ResourceLimits &limits) noexcept                     \
      -> std::expected<Name, ValueError> {                                     \
    auto validated = validate_identifier(value, limits);                       \
    if (!validated) {                                                          \
      return std::unexpected{validated.error()};                               \
    }                                                                          \
    return Name{std::move(*validated)};                                        \
  }

DRAWFORGE_IMPLEMENT_ID_TYPE(DocumentId)
DRAWFORGE_IMPLEMENT_ID_TYPE(LayerId)
DRAWFORGE_IMPLEMENT_ID_TYPE(ObjectId)
DRAWFORGE_IMPLEMENT_ID_TYPE(TransactionId)
DRAWFORGE_IMPLEMENT_ID_TYPE(AssetId)
DRAWFORGE_IMPLEMENT_ID_TYPE(TrackId)

#undef DRAWFORGE_IMPLEMENT_ID_TYPE

auto BoundedText::create(std::string_view value,
                         const ResourceLimits &limits) noexcept
    -> std::expected<BoundedText, ValueError> {
  if (value.size() > limits.max_text_bytes()) {
    return std::unexpected{resource_limit_error};
  }
  switch (validate_utf8(value)) {
  case Utf8Status::embedded_nul:
    return std::unexpected{embedded_nul_error};
  case Utf8Status::malformed:
    return std::unexpected{invalid_utf8_error};
  case Utf8Status::valid:
    break;
  }

  try {
    return BoundedText{std::string{value}};
  } catch (const std::bad_alloc &) {
    return std::unexpected{allocation_failure_error};
  } catch (...) {
    return std::unexpected{allocation_failure_error};
  }
}

auto Revision::next() const noexcept -> std::expected<Revision, ValueError> {
  if (m_value == std::numeric_limits<std::uint64_t>::max()) {
    return std::unexpected{revision_overflow_error};
  }
  return Revision{m_value + 1};
}

auto Coordinate::create(const double value,
                        const ResourceLimits &limits) noexcept
    -> std::expected<Coordinate, ValueError> {
  if (!std::isfinite(value)) {
    return std::unexpected{non_finite_number_error};
  }
  if (std::abs(value) > limits.max_numeric_magnitude()) {
    return std::unexpected{number_out_of_range_error};
  }
  return Coordinate{value == 0.0 ? 0.0 : value};
}

auto Point::create(const double x, const double y,
                   const ResourceLimits &limits) noexcept
    -> std::expected<Point, ValueError> {
  const auto checked_x = Coordinate::create(x, limits);
  if (!checked_x) {
    return std::unexpected{checked_x.error()};
  }
  const auto checked_y = Coordinate::create(y, limits);
  if (!checked_y) {
    return std::unexpected{checked_y.error()};
  }
  return Point{*checked_x, *checked_y};
}

auto AffineTransform::create(const double a, const double b, const double c,
                             const double d, const double e, const double f,
                             const ResourceLimits &limits) noexcept
    -> std::expected<AffineTransform, ValueError> {
  const auto checked_a = Coordinate::create(a, limits);
  if (!checked_a) {
    return std::unexpected{checked_a.error()};
  }
  const auto checked_b = Coordinate::create(b, limits);
  if (!checked_b) {
    return std::unexpected{checked_b.error()};
  }
  const auto checked_c = Coordinate::create(c, limits);
  if (!checked_c) {
    return std::unexpected{checked_c.error()};
  }
  const auto checked_d = Coordinate::create(d, limits);
  if (!checked_d) {
    return std::unexpected{checked_d.error()};
  }
  const auto checked_e = Coordinate::create(e, limits);
  if (!checked_e) {
    return std::unexpected{checked_e.error()};
  }
  const auto checked_f = Coordinate::create(f, limits);
  if (!checked_f) {
    return std::unexpected{checked_f.error()};
  }
  return AffineTransform{*checked_a, *checked_b, *checked_c,
                         *checked_d, *checked_e, *checked_f};
}

auto AffineTransform::apply(const Point point,
                            const ResourceLimits &limits) const noexcept
    -> std::expected<Point, ValueError> {
  const auto x = checked_component(
      static_cast<long double>(m_a.value()) * point.x().value() +
          static_cast<long double>(m_c.value()) * point.y().value() +
          m_e.value(),
      limits);
  if (!x) {
    return std::unexpected{x.error()};
  }
  const auto y = checked_component(
      static_cast<long double>(m_b.value()) * point.x().value() +
          static_cast<long double>(m_d.value()) * point.y().value() +
          m_f.value(),
      limits);
  if (!y) {
    return std::unexpected{y.error()};
  }
  return Point::create(*x, *y, limits);
}

auto AffineTransform::then(const AffineTransform &next,
                           const ResourceLimits &limits) const noexcept
    -> std::expected<AffineTransform, ValueError> {
  const auto component =
      [&limits](long double value) -> std::expected<double, ValueError> {
    return checked_component(value, limits);
  };

  const auto a =
      component(static_cast<long double>(next.m_a.value()) * m_a.value() +
                static_cast<long double>(next.m_c.value()) * m_b.value());
  if (!a)
    return std::unexpected{a.error()};
  const auto b =
      component(static_cast<long double>(next.m_b.value()) * m_a.value() +
                static_cast<long double>(next.m_d.value()) * m_b.value());
  if (!b)
    return std::unexpected{b.error()};
  const auto c =
      component(static_cast<long double>(next.m_a.value()) * m_c.value() +
                static_cast<long double>(next.m_c.value()) * m_d.value());
  if (!c)
    return std::unexpected{c.error()};
  const auto d =
      component(static_cast<long double>(next.m_b.value()) * m_c.value() +
                static_cast<long double>(next.m_d.value()) * m_d.value());
  if (!d)
    return std::unexpected{d.error()};
  const auto e =
      component(static_cast<long double>(next.m_a.value()) * m_e.value() +
                static_cast<long double>(next.m_c.value()) * m_f.value() +
                next.m_e.value());
  if (!e)
    return std::unexpected{e.error()};
  const auto f =
      component(static_cast<long double>(next.m_b.value()) * m_e.value() +
                static_cast<long double>(next.m_d.value()) * m_f.value() +
                next.m_f.value());
  if (!f)
    return std::unexpected{f.error()};
  return AffineTransform::create(*a, *b, *c, *d, *e, *f, limits);
}

auto CanvasExtent::create(const std::uint64_t width, const std::uint64_t height,
                          const ResourceLimits &limits) noexcept
    -> std::expected<CanvasExtent, ValueError> {
  if (width == 0 || height == 0) {
    return std::unexpected{invalid_extent_error};
  }
  if (width > limits.max_canvas_dimension() ||
      height > limits.max_canvas_dimension()) {
    return std::unexpected{resource_limit_error};
  }
  const auto pixels = detail::checked_multiply(width, height);
  if (!pixels) {
    return std::unexpected{pixels.error()};
  }
  if (*pixels > limits.max_canvas_pixels()) {
    return std::unexpected{resource_limit_error};
  }
  const auto bytes = detail::checked_multiply(*pixels, 4);
  if (!bytes) {
    return std::unexpected{bytes.error()};
  }
  if (*bytes > limits.max_output_bytes()) {
    return std::unexpected{resource_limit_error};
  }
  return CanvasExtent{static_cast<std::uint32_t>(width),
                      static_cast<std::uint32_t>(height), *pixels, *bytes};
}

} // namespace drawforge
