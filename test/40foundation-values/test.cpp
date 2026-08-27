#include "../../src/lib/foundation_internal.hpp"

#include <catch2/catch_test_macros.hpp>

#include <drawforge/drawforge.hpp>

#include <array>
#include <cmath>
#include <cstdint>
#include <limits>
#include <span>
#include <string>
#include <string_view>
#include <type_traits>

namespace df = drawforge;

namespace {

[[nodiscard]] auto limits_with(df::ResourceLimitRequest values)
    -> df::ResourceLimits {
  const auto limits = df::ResourceLimits::create(values);
  REQUIRE(limits);
  return *limits;
}

} // namespace

TEST_CASE("identity factories reject invalid and oversized spellings",
          "[foundation][identity][failure]") {
  SECTION("empty") {
    const auto id = df::DocumentId::create("");
    REQUIRE_FALSE(id);
    REQUIRE(id.error().code == df::ValueErrorCode::empty_value);
  }

  SECTION("invalid first character") {
    const auto id = df::ObjectId::create("1object");
    REQUIRE_FALSE(id);
    REQUIRE(id.error().code == df::ValueErrorCode::invalid_identifier);
  }

  SECTION("invalid tail and non-ASCII") {
    for (const auto &spelling :
         {std::string{"bad/id"}, std::string{"bad id"},
          std::string{"caf\xC3\xA9"}, std::string{"ab\0c", 4}}) {
      const auto id = df::LayerId::create(spelling);
      REQUIRE_FALSE(id);
      REQUIRE(id.error().code == df::ValueErrorCode::invalid_identifier);
    }
  }

  SECTION("configured byte ceiling wins before syntax validation") {
    auto values = df::default_resource_limit_values;
    values.max_identifier_bytes = 3;
    const auto limits = limits_with(values);
    const auto id = df::TransactionId::create("bad/", limits);
    REQUIRE_FALSE(id);
    REQUIRE(id.error().code == df::ValueErrorCode::resource_limit);
  }
}

TEST_CASE("identity boundaries, ordering, and collision domains are explicit",
          "[foundation][identity][boundary]") {
  STATIC_REQUIRE_FALSE(std::is_same_v<df::DocumentId, df::ObjectId>);
  STATIC_REQUIRE_FALSE(std::is_convertible_v<df::DocumentId, df::ObjectId>);

  const std::string maximum(64, 'a');
  const auto accepted = df::ObjectId::create(maximum);
  REQUIRE(accepted);
  REQUIRE(accepted->value() == maximum);

  const auto over = df::ObjectId::create(maximum + "a");
  REQUIRE_FALSE(over);
  REQUIRE(over.error().code == df::ValueErrorCode::resource_limit);

  const auto first = df::ObjectId::create("alpha");
  const auto duplicate = df::ObjectId::create("alpha");
  const auto second = df::ObjectId::create("beta");
  REQUIRE(first);
  REQUIRE(duplicate);
  REQUIRE(second);
  REQUIRE(*first == *duplicate);
  REQUIRE(*first < *second);

  const std::array ids{*first, *duplicate};
  const auto unique =
      df::detail::validate_unique_ids(std::span<const df::ObjectId>{ids}, 2);
  REQUIRE_FALSE(unique);
  REQUIRE(unique.error().code == df::ValueErrorCode::duplicate_identity);

  const auto over_limit =
      df::detail::validate_unique_ids(std::span<const df::ObjectId>{ids}, 1);
  REQUIRE_FALSE(over_limit);
  REQUIRE(over_limit.error().code == df::ValueErrorCode::resource_limit);

  REQUIRE(df::DocumentId::create("shared"));
  REQUIRE(df::ObjectId::create("shared"));
}

TEST_CASE("bounded text rejects malformed, embedded, and oversized data",
          "[foundation][text][failure]") {
  SECTION("embedded NUL") {
    constexpr std::string_view with_nul{"a\0b", 3};
    const auto text = df::BoundedText::create(with_nul);
    REQUIRE_FALSE(text);
    REQUIRE(text.error().code == df::ValueErrorCode::embedded_nul);
  }

  SECTION("malformed UTF-8") {
    const auto text = df::BoundedText::create(std::string{"\xC0\x80", 2});
    REQUIRE_FALSE(text);
    REQUIRE(text.error().code == df::ValueErrorCode::invalid_utf8);
  }

  SECTION("oversized input") {
    auto values = df::default_resource_limit_values;
    values.max_text_bytes = 2;
    const auto text = df::BoundedText::create("abc", limits_with(values));
    REQUIRE_FALSE(text);
    REQUIRE(text.error().code == df::ValueErrorCode::resource_limit);
  }
}

TEST_CASE("bounded text preserves exact valid UTF-8 bytes",
          "[foundation][text][boundary]") {
  auto values = df::default_resource_limit_values;
  values.max_text_bytes = 5;
  const auto limits = limits_with(values);
  const std::string value{"caf\xC3\xA9", 5};
  const auto text = df::BoundedText::create(value, limits);
  REQUIRE(text);
  REQUIRE(text->value() == value);
  REQUIRE(df::BoundedText::create("", limits));

  values.max_text_bytes = 0;
  const auto zero = limits_with(values);
  REQUIRE(df::BoundedText::create("", zero));
  REQUIRE_FALSE(df::BoundedText::create("a", zero));
}

TEST_CASE("numeric values reject non-finite and out-of-range inputs",
          "[foundation][numeric][failure]") {
  for (const auto value : {std::numeric_limits<double>::infinity(),
                           -std::numeric_limits<double>::infinity(),
                           std::numeric_limits<double>::quiet_NaN()}) {
    const auto coordinate = df::Coordinate::create(value);
    REQUIRE_FALSE(coordinate);
    REQUIRE(coordinate.error().code == df::ValueErrorCode::non_finite_number);
  }

  const auto over = df::Coordinate::create(65'536.0001);
  REQUIRE_FALSE(over);
  REQUIRE(over.error().code == df::ValueErrorCode::number_out_of_range);

  const auto transform = df::AffineTransform::create(
      1, 0, 0, 1, 0, std::numeric_limits<double>::infinity());
  REQUIRE_FALSE(transform);
  REQUIRE(transform.error().code == df::ValueErrorCode::non_finite_number);
}

TEST_CASE("coordinates normalize negative zero and accept exact boundaries",
          "[foundation][numeric][boundary]") {
  const auto negative_zero = df::Coordinate::create(-0.0);
  REQUIRE(negative_zero);
  REQUIRE(negative_zero->value() == 0.0);
  REQUIRE_FALSE(std::signbit(negative_zero->value()));

  REQUIRE(df::Coordinate::create(65'536.0));
  REQUIRE(df::Coordinate::create(-65'536.0));
}

TEST_CASE("affine operations are ordered and checked",
          "[foundation][transform]") {
  const auto translate = df::AffineTransform::create(1, 0, 0, 1, 2, 3);
  const auto scale = df::AffineTransform::create(2, 0, 0, 2, 0, 0);
  const auto point = df::Point::create(1, 1);
  REQUIRE(translate);
  REQUIRE(scale);
  REQUIRE(point);

  const auto composed = translate->then(*scale);
  REQUIRE(composed);
  const auto applied = composed->apply(*point);
  REQUIRE(applied);
  REQUIRE(applied->x().value() == 6.0);
  REQUIRE(applied->y().value() == 8.0);

  const auto singular = df::AffineTransform::create(0, 0, 0, 0, 0, 0);
  REQUIRE(singular);

  const auto large = df::AffineTransform::create(65'536, 0, 0, 1, 0, 0);
  const auto two = df::Point::create(2, 0);
  REQUIRE(large);
  REQUIRE(two);
  const auto overflowed = large->apply(*two);
  REQUIRE_FALSE(overflowed);
  REQUIRE(overflowed.error().code == df::ValueErrorCode::number_out_of_range);
}

TEST_CASE("resource limit requests reject unsafe hard-ceiling violations",
          "[foundation][limits][failure]") {
  SECTION("integral ceiling") {
    auto values = df::hard_resource_limit_values;
    ++values.max_nesting_depth;
    const auto limits = df::ResourceLimits::create(values);
    REQUIRE_FALSE(limits);
    REQUIRE(limits.error().code == df::ValueErrorCode::invalid_limit);
  }

  SECTION("numeric ceiling") {
    auto values = df::hard_resource_limit_values;
    values.max_numeric_magnitude = std::numeric_limits<double>::infinity();
    const auto limits = df::ResourceLimits::create(values);
    REQUIRE_FALSE(limits);
    REQUIRE(limits.error().code == df::ValueErrorCode::non_finite_number);
  }

  SECTION("negative numeric magnitude") {
    auto values = df::default_resource_limit_values;
    values.max_numeric_magnitude = -1.0;
    const auto limits = df::ResourceLimits::create(values);
    REQUIRE_FALSE(limits);
    REQUIRE(limits.error().code == df::ValueErrorCode::invalid_limit);
  }
}

TEST_CASE("resource limits accept hard ceilings and zero fail-closed budgets",
          "[foundation][limits][boundary]") {
  STATIC_REQUIRE(df::default_resource_limit_values.max_identifier_bytes == 64);
  STATIC_REQUIRE(df::default_resource_limit_values.max_text_bytes == 4'096);
  STATIC_REQUIRE(df::default_resource_limit_values.max_numeric_magnitude ==
                 65'536.0);
  STATIC_REQUIRE(df::default_resource_limit_values.max_canvas_dimension ==
                 4'096);
  STATIC_REQUIRE(df::default_resource_limit_values.max_canvas_pixels ==
                 16ULL * 1024ULL * 1024ULL);
  STATIC_REQUIRE(df::default_resource_limit_values.max_scene_nodes == 4'096);
  STATIC_REQUIRE(df::default_resource_limit_values.max_transaction_operations ==
                 256);
  STATIC_REQUIRE(df::default_resource_limit_values.max_output_bytes ==
                 64ULL * 1024ULL * 1024ULL);
  STATIC_REQUIRE(df::default_resource_limit_values.max_nesting_depth == 32);

  const auto hard = df::ResourceLimits::create(df::hard_resource_limit_values);
  REQUIRE(hard);
  REQUIRE(hard->values() == df::hard_resource_limit_values);

  df::ResourceLimitRequest zero{};
  zero.max_identifier_bytes = 0;
  zero.max_text_bytes = 0;
  zero.max_numeric_magnitude = 0;
  zero.max_canvas_dimension = 0;
  zero.max_canvas_pixels = 0;
  zero.max_scene_nodes = 0;
  zero.max_transaction_operations = 0;
  zero.max_output_bytes = 0;
  zero.max_nesting_depth = 0;
  const auto limits = df::ResourceLimits::create(zero);
  REQUIRE(limits);
  REQUIRE_FALSE(df::DocumentId::create("scene", *limits));
  REQUIRE_FALSE(df::CanvasExtent::create(1, 1, *limits));
}

TEST_CASE("canvas extents reject invalid, oversized, and over-budget layouts",
          "[foundation][extent][failure]") {
  const auto empty = df::CanvasExtent::create(0, 1);
  REQUIRE_FALSE(empty);
  REQUIRE(empty.error().code == df::ValueErrorCode::invalid_extent);

  const auto wide = df::CanvasExtent::create(4'097, 1);
  REQUIRE_FALSE(wide);
  REQUIRE(wide.error().code == df::ValueErrorCode::resource_limit);

  const auto hard = limits_with(df::hard_resource_limit_values);
  const auto too_many_pixels = df::CanvasExtent::create(8'193, 8'192, hard);
  REQUIRE_FALSE(too_many_pixels);
  REQUIRE(too_many_pixels.error().code == df::ValueErrorCode::resource_limit);
}

TEST_CASE("canvas extent accounting is exact at configured boundaries",
          "[foundation][extent][boundary]") {
  const auto ordinary = df::CanvasExtent::create(4'096, 4'096);
  REQUIRE(ordinary);
  REQUIRE(ordinary->pixel_count() == 16ULL * 1024ULL * 1024ULL);
  REQUIRE(ordinary->rgba8_bytes() == 64ULL * 1024ULL * 1024ULL);

  const auto hard = limits_with(df::hard_resource_limit_values);
  const auto maximum = df::CanvasExtent::create(8'192, 8'192, hard);
  REQUIRE(maximum);
  REQUIRE(maximum->rgba8_bytes() == 256ULL * 1024ULL * 1024ULL);
}

TEST_CASE("checked size arithmetic rejects overflow before limits",
          "[foundation][arithmetic][failure]") {
  const auto maximum = std::numeric_limits<std::uint64_t>::max();
  const auto sum = df::detail::checked_add(maximum, 1);
  REQUIRE_FALSE(sum);
  REQUIRE(sum.error().code == df::ValueErrorCode::arithmetic_overflow);

  const auto product = df::detail::checked_multiply(maximum, 2);
  REQUIRE_FALSE(product);
  REQUIRE(product.error().code == df::ValueErrorCode::arithmetic_overflow);

  REQUIRE(df::detail::checked_add(maximum, 0) == maximum);
  REQUIRE(df::detail::checked_multiply(maximum, 1) == maximum);
}

TEST_CASE("revision advancement is checked at the final value",
          "[foundation][revision]") {
  const df::Revision initial;
  REQUIRE(initial.value() == 0);
  REQUIRE(initial.next() == df::Revision{1});

  const df::Revision maximum{std::numeric_limits<std::uint64_t>::max()};
  const auto next = maximum.next();
  REQUIRE_FALSE(next);
  REQUIRE(next.error().code == df::ValueErrorCode::revision_overflow);
}

TEST_CASE("stable error code names and ordinary values complete the contract",
          "[foundation][happy]") {
  constexpr std::array codes{
      df::ValueErrorCode::empty_value,
      df::ValueErrorCode::invalid_identifier,
      df::ValueErrorCode::invalid_utf8,
      df::ValueErrorCode::embedded_nul,
      df::ValueErrorCode::non_finite_number,
      df::ValueErrorCode::number_out_of_range,
      df::ValueErrorCode::invalid_extent,
      df::ValueErrorCode::invalid_limit,
      df::ValueErrorCode::resource_limit,
      df::ValueErrorCode::arithmetic_overflow,
      df::ValueErrorCode::duplicate_identity,
      df::ValueErrorCode::revision_overflow,
      df::ValueErrorCode::allocation_failure,
  };
  for (const auto code : codes) {
    const auto name = df::value_error_code_name(code);
    REQUIRE_FALSE(name.empty());
    REQUIRE(name != "unknown");
    REQUIRE(name.size() <= 32);
  }

  const auto document = df::DocumentId::create("scene.v1");
  const auto asset = df::AssetId::create("palette.main");
  const auto track = df::TrackId::create("entrance-opacity");
  const auto label = df::BoundedText::create("Status badge");
  const auto point = df::AffineTransform{}.apply(df::Point{});
  REQUIRE(document);
  REQUIRE(asset);
  REQUIRE(track);
  REQUIRE(label);
  REQUIRE(point);
  REQUIRE(document->value() == "scene.v1");
  REQUIRE(label->value() == "Status badge");
  REQUIRE(point->x().value() == 0.0);
  REQUIRE(point->y().value() == 0.0);
}
