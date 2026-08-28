#include <drawforge/drawforge.hpp>

#include <catch2/catch_test_macros.hpp>

#include <algorithm>
#include <array>
#include <cmath>
#include <cstdint>
#include <limits>
#include <optional>
#include <span>
#include <utility>
#include <vector>

namespace df = drawforge;

namespace {

template <typename Id> [[nodiscard]] auto make_id(const char *value) -> Id {
  const auto id = Id::create(value);
  REQUIRE(id);
  return *id;
}

[[nodiscard]] auto make_extent(const std::uint64_t width,
                               const std::uint64_t height,
                               const df::ResourceLimits &limits = {})
    -> df::CanvasExtent {
  const auto extent = df::CanvasExtent::create(width, height, limits);
  REQUIRE(extent);
  return *extent;
}

[[nodiscard]] auto make_opacity(const double value) -> df::Opacity {
  const auto opacity = df::Opacity::create(value);
  REQUIRE(opacity);
  return *opacity;
}

[[nodiscard]] auto make_transform(const double x, const double y)
    -> df::AffineTransform {
  const auto transform = df::AffineTransform::create(1, 0, 0, 1, x, y);
  REQUIRE(transform);
  return *transform;
}

[[nodiscard]] auto make_rectangle(const double x, const double y,
                                  const double width, const double height,
                                  const double radius = 0.0) -> df::Rectangle {
  const auto rectangle =
      df::Rectangle::create(x, y, width, height, radius, radius);
  REQUIRE(rectangle);
  return *rectangle;
}

[[nodiscard]] auto make_ellipse(const double x, const double y,
                                const double radius_x, const double radius_y)
    -> df::Ellipse {
  const auto ellipse = df::Ellipse::create(x, y, radius_x, radius_y);
  REQUIRE(ellipse);
  return *ellipse;
}

[[nodiscard]] auto make_path() -> df::Path {
  const auto first = df::Point::create(14, 10);
  const auto second = df::Point::create(17, 13);
  const auto third = df::Point::create(22, 5);
  REQUIRE(first);
  REQUIRE(second);
  REQUIRE(third);
  const auto path = df::Path::create(
      {df::MoveTo{*first}, df::LineTo{*second}, df::LineTo{*third}});
  REQUIRE(path);
  return *path;
}

[[nodiscard]] auto make_closed_path() -> df::Path {
  const auto first = df::Point::create(2, 2);
  const auto second = df::Point::create(5, 2);
  const auto third = df::Point::create(3, 4);
  REQUIRE(first);
  REQUIRE(second);
  REQUIRE(third);
  const auto path = df::Path::create({df::MoveTo{*first}, df::LineTo{*second},
                                      df::LineTo{*third}, df::ClosePath{}});
  REQUIRE(path);
  return *path;
}

[[nodiscard]] auto make_stroke(const df::Color color, const double width)
    -> df::Stroke {
  const auto stroke = df::Stroke::create(color, width);
  REQUIRE(stroke);
  return *stroke;
}

[[nodiscard]] auto fnv1a(const std::span<const std::uint8_t> bytes)
    -> std::uint64_t {
  std::uint64_t hash{14695981039346656037ULL};
  for (const auto byte : bytes) {
    hash ^= byte;
    hash *= 1099511628211ULL;
  }
  return hash;
}

[[nodiscard]] auto bytes_equal(const std::span<const std::uint8_t> left,
                               const std::span<const std::uint8_t> right)
    -> bool {
  return left.size() == right.size() &&
         std::equal(left.begin(), left.end(), right.begin());
}

[[nodiscard]] auto config(const std::uint64_t time_us,
                          const std::uint64_t bytes = 1024U * 1024U)
    -> df::RenderConfig {
  const auto value = df::RenderConfig::create(time_us, bytes);
  REQUIRE(value);
  return *value;
}

[[nodiscard]] auto transaction(const char *id, const std::uint64_t revision,
                               std::vector<df::Operation> operations,
                               const char *document = "scene")
    -> df::Transaction {
  return df::Transaction{make_id<df::DocumentId>(document),
                         df::Revision{revision}, make_id<df::TransactionId>(id),
                         df::OperationBatch{std::move(operations)}};
}

[[nodiscard]] auto make_scene_dispatcher() -> df::TransactionDispatcher {
  const auto document_id = make_id<df::DocumentId>("scene");
  const auto document = df::Document::create(document_id, make_extent(24, 16),
                                             df::Color{8, 16, 24, 255});
  REQUIRE(document);
  auto dispatcher = df::TransactionDispatcher::create(*document);
  REQUIRE(dispatcher);

  const auto layer = make_id<df::LayerId>("art");
  const auto group = make_id<df::ObjectId>("group");
  const auto rectangle = make_id<df::ObjectId>("backdrop");
  const auto ellipse = make_id<df::ObjectId>("dot");
  const auto path = make_id<df::ObjectId>("mark");
  const auto triangle = make_id<df::ObjectId>("triangle");
  const auto track_id = make_id<df::TrackId>("dot-entrance");
  const auto track = df::OpacityTrack::create(track_id, ellipse, 0, 100,
                                              make_opacity(0), make_opacity(1));
  REQUIRE(track);

  const auto created = dispatcher->apply(transaction(
      "create", 0,
      {df::CreateLayer{layer, 0, true},
       df::CreateGroup{group, layer, 0, true, make_transform(2, 1)},
       df::CreateRectangle{
           rectangle,
           group,
           0,
           true,
           {},
           df::Style{df::Color{220, 40, 30, 200},
                     make_stroke(df::Color{255, 255, 255, 255}, 1.5)},
           make_opacity(1),
           make_rectangle(1, 1, 10, 8, 2)},
       df::CreateEllipse{ellipse,
                         group,
                         1,
                         true,
                         {},
                         df::Style{df::Color{20, 80, 240, 255}, std::nullopt},
                         make_opacity(0.25),
                         make_ellipse(7, 5, 3, 3)},
       df::CreatePath{path,
                      layer,
                      1,
                      true,
                      {},
                      df::Style{std::nullopt,
                                make_stroke(df::Color{40, 230, 100, 220}, 2)},
                      make_opacity(1),
                      make_path()},
       df::CreatePath{triangle,
                      group,
                      2,
                      true,
                      {},
                      df::Style{df::Color{245, 190, 30, 180}, std::nullopt},
                      make_opacity(1),
                      make_closed_path()},
       df::CreateOpacityTrack{*track}}));
  REQUIRE(created);
  return std::move(*dispatcher);
}

struct CancellationCounter {
  std::uint64_t cancel_on{};
  mutable std::uint64_t polls{};
};

[[nodiscard]] auto cancellation_token(CancellationCounter &counter)
    -> df::CancellationToken {
  return df::CancellationToken{
      &counter, [](const void *context) noexcept {
        const auto *counter = static_cast<const CancellationCounter *>(context);
        ++counter->polls;
        return counter->polls == counter->cancel_on;
      }};
}

} // namespace

TEST_CASE("render configuration and stable metadata fail closed",
          "[render][public][failure]") {
  REQUIRE_FALSE(df::RenderConfig::create(0, 0));
  REQUIRE_FALSE(df::RenderConfig::create(
      0, df::hard_resource_limit_values.max_output_bytes + 1));

  constexpr std::array codes{
      df::RenderErrorCode::cancelled,
      df::RenderErrorCode::resource_limit,
      df::RenderErrorCode::number_out_of_range,
      df::RenderErrorCode::arithmetic_overflow,
      df::RenderErrorCode::allocation_failure,
      df::RenderErrorCode::renderer_failure,
      df::RenderErrorCode::png_encoding_failure,
  };
  for (const auto code : codes)
    REQUIRE(df::render_error_code_name(code) != "unknown");

  const auto renderer = df::renderer_info();
  REQUIRE(renderer.name == "plutovg");
  REQUIRE(renderer.version == "1.3.3");
  REQUIRE(renderer.contract_version == 1);
  REQUIRE(df::RenderConfig::pixel_format() ==
          df::PixelFormat::rgba8_srgb_straight_alpha);
}

TEST_CASE("empty transparent and background scenes are exact and bounded",
          "[render][rgba][png][failure]") {
  const auto document_id = make_id<df::DocumentId>("empty");
  const auto extent = make_extent(2, 2);
  const auto transparent = df::Document::create(document_id, extent);
  REQUIRE(transparent);

  const auto exact_budget = config(17, extent.rgba8_bytes());
  const auto rgba = df::render_rgba(*transparent, exact_budget);
  REQUIRE(rgba);
  REQUIRE(rgba->extent() == extent);
  REQUIRE(rgba->time_us() == 17);
  REQUIRE(rgba->format() == df::PixelFormat::rgba8_srgb_straight_alpha);
  REQUIRE(rgba->renderer() == df::renderer_info());
  REQUIRE(rgba->pixels().size() == 16);
  REQUIRE(std::ranges::all_of(rgba->pixels(),
                              [](const auto byte) { return byte == 0; }));
  auto source_rgba = *rgba;
  auto moved_rgba = std::move(source_rgba);
  REQUIRE(moved_rgba.pixels().size() == 16);
  REQUIRE(source_rgba.pixels().size() == 16);

  const auto too_large =
      config(0, df::default_resource_limit_values.max_output_bytes + 1);
  const auto rejected = df::render_rgba(*transparent, too_large);
  REQUIRE_FALSE(rejected);
  REQUIRE(rejected.error().code == df::RenderErrorCode::resource_limit);

  const auto png_too_large = df::encode_png(*rgba);
  REQUIRE_FALSE(png_too_large);
  REQUIRE(png_too_large.error().code == df::RenderErrorCode::resource_limit);

  const auto background =
      df::Document::create(make_id<df::DocumentId>("background"), extent,
                           df::Color{12, 34, 56, 128});
  REQUIRE(background);
  const auto painted = df::render_rgba(*background, config(0));
  REQUIRE(painted);
  for (std::size_t offset = 0; offset < painted->pixels().size(); offset += 4) {
    REQUIRE(painted->pixels()[offset + 0] == 11);
    REQUIRE(painted->pixels()[offset + 1] == 33);
    REQUIRE(painted->pixels()[offset + 2] == 55);
    REQUIRE(painted->pixels()[offset + 3] == 128);
  }
}

TEST_CASE("accepted scene rendering and encoding have deterministic goldens",
          "[render][golden][determinism]") {
  auto dispatcher = make_scene_dispatcher();
  const auto scene = dispatcher.snapshot();
  const auto at_start = df::render_rgba(scene, config(0));
  const auto at_middle = df::render_rgba(scene, config(50));
  const auto at_end = df::render_rgba(scene, config(100));
  REQUIRE(at_start);
  REQUIRE(at_middle);
  REQUIRE(at_end);
  REQUIRE_FALSE(bytes_equal(at_start->pixels(), at_middle->pixels()));
  REQUIRE_FALSE(bytes_equal(at_middle->pixels(), at_end->pixels()));

  const auto repeated = df::render_rgba(scene, config(50));
  REQUIRE(repeated);
  REQUIRE(bytes_equal(repeated->pixels(), at_middle->pixels()));

  const auto png = df::encode_png(*at_middle);
  const auto repeated_png = df::encode_png(*repeated);
  REQUIRE(png);
  REQUIRE(repeated_png);
  REQUIRE(bytes_equal(png->bytes(), repeated_png->bytes()));
  REQUIRE(png->extent() == at_middle->extent());
  REQUIRE(png->time_us() == 50);
  REQUIRE(png->renderer() == df::renderer_info());

  const auto start_hash = fnv1a(at_start->pixels());
  const auto middle_hash = fnv1a(at_middle->pixels());
  const auto end_hash = fnv1a(at_end->pixels());
  const auto png_hash = fnv1a(png->bytes());
  CAPTURE(start_hash, middle_hash, end_hash, png_hash);
  REQUIRE(start_hash == 0x439d4310df048badULL);
  REQUIRE(middle_hash == 0x04445fb63c3fbe17ULL);
  REQUIRE(end_hash == 0x8d9904ddccd0077dULL);
  REQUIRE(png_hash == 0xaeabc065e17d4638ULL);
}

TEST_CASE("canvas clipping visibility and alpha use canonical pixels",
          "[render][clipping][alpha]") {
  const auto document_id = make_id<df::DocumentId>("scene");
  const auto extent = make_extent(6, 6);
  const auto document = df::Document::create(document_id, extent);
  REQUIRE(document);
  auto dispatcher = df::TransactionDispatcher::create(*document);
  REQUIRE(dispatcher);
  const auto layer = make_id<df::LayerId>("art");
  const auto box = make_id<df::ObjectId>("box");
  REQUIRE(dispatcher->apply(transaction(
      "clip", 0,
      {df::CreateLayer{layer, 0, true},
       df::CreateRectangle{box,
                           layer,
                           0,
                           true,
                           {},
                           df::Style{df::Color{255, 20, 10, 128}, std::nullopt},
                           make_opacity(1),
                           make_rectangle(-3, -3, 7, 7)}})));
  const auto visible = df::render_rgba(dispatcher->snapshot(), config(0));
  REQUIRE(visible);
  const auto center = (1U * extent.width() + 1U) * 4U;
  REQUIRE(visible->pixels()[center + 0] == 255);
  REQUIRE(visible->pixels()[center + 1] == 19);
  REQUIRE(visible->pixels()[center + 2] == 9);
  REQUIRE(visible->pixels()[center + 3] == 128);
  const auto outside = (5U * extent.width() + 5U) * 4U;
  REQUIRE(visible->pixels()[outside + 3] == 0);

  REQUIRE(dispatcher->apply(
      transaction("hide", 1, {df::SetVisibility{df::NodeRef{layer}, false}})));
  const auto hidden = df::render_rgba(dispatcher->snapshot(), config(0));
  REQUIRE(hidden);
  REQUIRE(std::ranges::all_of(hidden->pixels(),
                              [](const auto byte) { return byte == 0; }));
}

TEST_CASE("render and PNG cancellation return no partial artifact",
          "[render][cancellation][failure]") {
  auto dispatcher = make_scene_dispatcher();

  CancellationCounter before{1};
  const auto cancelled_before = df::render_rgba(
      dispatcher.snapshot(), config(0), cancellation_token(before));
  REQUIRE_FALSE(cancelled_before);
  REQUIRE(cancelled_before.error().code == df::RenderErrorCode::cancelled);

  CancellationCounter during{3};
  const auto cancelled_during = df::render_rgba(
      dispatcher.snapshot(), config(0), cancellation_token(during));
  REQUIRE_FALSE(cancelled_during);
  REQUIRE(cancelled_during.error().code == df::RenderErrorCode::cancelled);

  const auto rgba = df::render_rgba(dispatcher.snapshot(), config(0));
  REQUIRE(rgba);
  CancellationCounter encoding{2};
  const auto cancelled_png =
      df::encode_png(*rgba, cancellation_token(encoding));
  REQUIRE_FALSE(cancelled_png);
  REQUIRE(cancelled_png.error().code == df::RenderErrorCode::cancelled);
}

TEST_CASE("semantic receipt dirty bounds enclose every changed pixel",
          "[render][transaction][dirty-bounds]") {
  const auto document_id = make_id<df::DocumentId>("scene");
  const auto extent = make_extent(16, 8);
  const auto document = df::Document::create(document_id, extent);
  REQUIRE(document);
  auto dispatcher = df::TransactionDispatcher::create(*document);
  REQUIRE(dispatcher);
  const auto layer = make_id<df::LayerId>("art");
  const auto box = make_id<df::ObjectId>("box");
  REQUIRE(dispatcher->apply(transaction(
      "create-dirty", 0,
      {df::CreateLayer{layer, 0, true},
       df::CreateRectangle{box,
                           layer,
                           0,
                           true,
                           {},
                           df::Style{df::Color{240, 30, 20, 255}, std::nullopt},
                           make_opacity(1),
                           make_rectangle(2, 2, 4, 3)}})));
  const auto before = df::render_rgba(dispatcher->snapshot(), config(0));
  REQUIRE(before);
  const auto moved = dispatcher->apply(transaction(
      "move-dirty", 1, {df::SetTransform{box, make_transform(5, 0)}}));
  REQUIRE(moved);
  REQUIRE(moved->receipt.dirty_bounds());
  const auto after = df::render_rgba(dispatcher->snapshot(), config(0));
  REQUIRE(after);

  const auto &dirty = *moved->receipt.dirty_bounds();
  const auto min_x = static_cast<std::int64_t>(std::floor(dirty.x().value()));
  const auto min_y = static_cast<std::int64_t>(std::floor(dirty.y().value()));
  const auto max_x = static_cast<std::int64_t>(
      std::ceil(dirty.x().value() + dirty.width().value()));
  const auto max_y = static_cast<std::int64_t>(
      std::ceil(dirty.y().value() + dirty.height().value()));
  std::uint64_t changed_pixels{};
  for (std::uint32_t y = 0; y < extent.height(); ++y) {
    for (std::uint32_t x = 0; x < extent.width(); ++x) {
      const auto offset =
          (static_cast<std::size_t>(y) * extent.width() + x) * 4U;
      if (std::equal(before->pixels().begin() + offset,
                     before->pixels().begin() + offset + 4,
                     after->pixels().begin() + offset))
        continue;
      ++changed_pixels;
      REQUIRE(static_cast<std::int64_t>(x) >= min_x);
      REQUIRE(static_cast<std::int64_t>(x) < max_x);
      REQUIRE(static_cast<std::int64_t>(y) >= min_y);
      REQUIRE(static_cast<std::int64_t>(y) < max_y);
    }
  }
  REQUIRE(changed_pixels > 0);

  const auto no_effect = dispatcher->apply(transaction(
      "same-transform", 2, {df::SetTransform{box, make_transform(5, 0)}}));
  REQUIRE(no_effect);
  REQUIRE_FALSE(no_effect->receipt.dirty_bounds());
  const auto unchanged = df::render_rgba(dispatcher->snapshot(), config(0));
  REQUIRE(unchanged);
  REQUIRE(bytes_equal(after->pixels(), unchanged->pixels()));
}
