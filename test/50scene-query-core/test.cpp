#include "../../src/lib/scene_internal.hpp"

#include <catch2/catch_test_macros.hpp>

#include <drawforge/drawforge.hpp>

#include <array>
#include <cstdint>
#include <limits>
#include <optional>
#include <string_view>
#include <type_traits>
#include <utility>
#include <variant>
#include <vector>

namespace df = drawforge;

namespace {

template <typename Id>
[[nodiscard]] auto make_id(std::string_view value) -> Id {
  const auto result = Id::create(value);
  REQUIRE(result);
  return *result;
}

[[nodiscard]] auto make_opacity(double value) -> df::Opacity {
  const auto result = df::Opacity::create(value);
  REQUIRE(result);
  return *result;
}

[[nodiscard]] auto make_extent(std::uint64_t width, std::uint64_t height,
                               const df::ResourceLimits &limits = {})
    -> df::CanvasExtent {
  const auto result = df::CanvasExtent::create(width, height, limits);
  REQUIRE(result);
  return *result;
}

[[nodiscard]] auto make_rectangle(double x, double y, double width,
                                  double height, double radius_x = 0,
                                  double radius_y = 0) -> df::Rectangle {
  const auto result =
      df::Rectangle::create(x, y, width, height, radius_x, radius_y);
  REQUIRE(result);
  return *result;
}

[[nodiscard]] auto make_ellipse(double x, double y, double rx, double ry)
    -> df::Ellipse {
  const auto result = df::Ellipse::create(x, y, rx, ry);
  REQUIRE(result);
  return *result;
}

[[nodiscard]] auto make_document(std::vector<df::Layer> layers,
                                 std::vector<df::SceneObject> objects,
                                 std::vector<df::OpacityTrack> tracks = {},
                                 df::Revision revision = df::Revision{},
                                 const df::ResourceLimits &limits = {})
    -> df::Document {
  const auto result = df::detail::DocumentBuilder::build(
      make_id<df::DocumentId>("scene"), revision, make_extent(200, 160, limits),
      df::Color{255, 255, 255, 255}, limits, std::move(layers),
      std::move(objects), std::move(tracks));
  REQUIRE(result);
  return *result;
}

[[nodiscard]] auto translated(double x, double y) -> df::AffineTransform {
  const auto result = df::AffineTransform::create(1, 0, 0, 1, x, y);
  REQUIRE(result);
  return *result;
}

[[nodiscard]] auto filled(df::Color color) -> df::Style {
  return df::Style{color, std::nullopt};
}

} // namespace

TEST_CASE("scene values reject malformed geometry before document construction",
          "[scene][failure]") {
  REQUIRE_FALSE(df::Opacity::create(-0.01));
  REQUIRE_FALSE(df::Opacity::create(1.01));
  REQUIRE_FALSE(df::Opacity::create(std::numeric_limits<double>::quiet_NaN()));

  const auto negative = df::Rectangle::create(0, 0, -1, 2);
  REQUIRE_FALSE(negative);
  REQUIRE(negative.error().code == df::SceneErrorCode::invalid_geometry);

  const auto excessive_radius = df::Rectangle::create(0, 0, 10, 10, 6, 5);
  REQUIRE_FALSE(excessive_radius);
  REQUIRE(excessive_radius.error().code ==
          df::SceneErrorCode::invalid_geometry);

  REQUIRE_FALSE(df::Ellipse::create(0, 0, -1, 1));
  REQUIRE_FALSE(df::Stroke::create(df::Color{}, 0));
  REQUIRE_FALSE(df::Path::create({}));

  const auto point = df::Point::create(1, 1);
  REQUIRE(point);
  REQUIRE_FALSE(df::Path::create({df::LineTo{*point}}));
  REQUIRE_FALSE(df::Path::create({df::MoveTo{*point}, df::ClosePath{}}));

  const auto valid = df::Path::create(
      {df::MoveTo{*point}, df::LineTo{df::Point{}}, df::ClosePath{}});
  REQUIRE(valid);
}

TEST_CASE("document construction fails closed for identity and parent errors",
          "[scene][document][failure]") {
  const auto layer = make_id<df::LayerId>("artwork");
  const auto a = make_id<df::ObjectId>("a");
  const auto b = make_id<df::ObjectId>("b");

  SECTION("duplicate layer") {
    const auto result = df::detail::DocumentBuilder::build(
        make_id<df::DocumentId>("scene"), df::Revision{}, make_extent(10, 10),
        std::nullopt, {}, {df::Layer{layer, true}, df::Layer{layer, true}}, {},
        {});
    REQUIRE_FALSE(result);
    REQUIRE(result.error().code == df::SceneErrorCode::duplicate_identity);
  }

  SECTION("duplicate object in the shared group and drawable namespace") {
    const auto result = df::detail::DocumentBuilder::build(
        make_id<df::DocumentId>("scene"), df::Revision{}, make_extent(10, 10),
        std::nullopt, {}, {df::Layer{layer, true}},
        {df::Group{a, layer, true}, df::Drawable{a,
                                                 layer,
                                                 true,
                                                 {},
                                                 make_rectangle(0, 0, 1, 1),
                                                 filled({1, 2, 3, 255}),
                                                 make_opacity(1)}},
        {});
    REQUIRE_FALSE(result);
    REQUIRE(result.error().code == df::SceneErrorCode::duplicate_identity);
  }

  SECTION("missing and drawable parents") {
    const auto missing = df::detail::DocumentBuilder::build(
        make_id<df::DocumentId>("scene"), df::Revision{}, make_extent(10, 10),
        std::nullopt, {}, {df::Layer{layer, true}},
        {df::Group{a, make_id<df::LayerId>("missing"), true}}, {});
    REQUIRE_FALSE(missing);
    REQUIRE(missing.error().code == df::SceneErrorCode::missing_identity);

    const auto invalid = df::detail::DocumentBuilder::build(
        make_id<df::DocumentId>("scene"), df::Revision{}, make_extent(10, 10),
        std::nullopt, {}, {df::Layer{layer, true}},
        {df::Drawable{a,
                      layer,
                      true,
                      {},
                      make_rectangle(0, 0, 1, 1),
                      filled({1, 2, 3, 255}),
                      make_opacity(1)},
         df::Group{b, a, true}},
        {});
    REQUIRE_FALSE(invalid);
    REQUIRE(invalid.error().code == df::SceneErrorCode::invalid_parent);
  }

  SECTION("direct and indirect cycles") {
    const auto direct = df::detail::DocumentBuilder::build(
        make_id<df::DocumentId>("scene"), df::Revision{}, make_extent(10, 10),
        std::nullopt, {}, {df::Layer{layer, true}}, {df::Group{a, a, true}},
        {});
    REQUIRE_FALSE(direct);
    REQUIRE(direct.error().code == df::SceneErrorCode::parent_cycle);

    const auto indirect = df::detail::DocumentBuilder::build(
        make_id<df::DocumentId>("scene"), df::Revision{}, make_extent(10, 10),
        std::nullopt, {}, {df::Layer{layer, true}},
        {df::Group{a, b, true}, df::Group{b, a, true}}, {});
    REQUIRE_FALSE(indirect);
    REQUIRE(indirect.error().code == df::SceneErrorCode::parent_cycle);
  }
}

TEST_CASE("scene and track ceilings are enforced without partial documents",
          "[scene][limits][failure]") {
  auto values = df::default_resource_limit_values;
  values.max_scene_nodes = 1;
  values.max_canvas_dimension = 16;
  values.max_canvas_pixels = 256;
  values.max_output_bytes = 1'024;
  const auto limits = df::ResourceLimits::create(values);
  REQUIRE(limits);
  const auto layer = make_id<df::LayerId>("artwork");
  const auto object = make_id<df::ObjectId>("dot");

  const auto exhausted = df::detail::DocumentBuilder::build(
      make_id<df::DocumentId>("scene"), df::Revision{},
      make_extent(10, 10, *limits), std::nullopt, *limits,
      {df::Layer{layer, true}},
      {df::Drawable{object,
                    layer,
                    true,
                    {},
                    make_ellipse(2, 2, 1, 1),
                    filled({1, 2, 3, 255}),
                    make_opacity(1)}},
      {});
  REQUIRE_FALSE(exhausted);
  REQUIRE(exhausted.error().code == df::SceneErrorCode::resource_limit);

  const auto track_one =
      df::OpacityTrack::create(make_id<df::TrackId>("one"), object, 0, 10,
                               make_opacity(0), make_opacity(1));
  const auto track_two =
      df::OpacityTrack::create(make_id<df::TrackId>("two"), object, 0, 20,
                               make_opacity(1), make_opacity(0));
  REQUIRE(track_one);
  REQUIRE(track_two);
  const auto duplicate_target = df::detail::DocumentBuilder::build(
      make_id<df::DocumentId>("scene"), df::Revision{}, make_extent(10, 10),
      std::nullopt, {}, {df::Layer{layer, true}},
      {df::Drawable{object,
                    layer,
                    true,
                    {},
                    make_ellipse(2, 2, 1, 1),
                    filled({1, 2, 3, 255}),
                    make_opacity(1)}},
      {*track_one, *track_two});
  REQUIRE_FALSE(duplicate_target);
  REQUIRE(duplicate_target.error().code ==
          df::SceneErrorCode::unsupported_property);
}

TEST_CASE("document limits revalidate complete values and nesting",
          "[scene][limits][failure]") {
  auto values = df::default_resource_limit_values;
  values.max_identifier_bytes = 4;
  values.max_numeric_magnitude = 10;
  values.max_canvas_dimension = 16;
  values.max_canvas_pixels = 256;
  values.max_output_bytes = 1'024;
  values.max_nesting_depth = 1;
  const auto limits = df::ResourceLimits::create(values);
  REQUIRE(limits);

  const auto oversized_id = df::detail::DocumentBuilder::build(
      make_id<df::DocumentId>("scene"), df::Revision{},
      make_extent(10, 10, *limits), std::nullopt, *limits, {}, {}, {});
  REQUIRE_FALSE(oversized_id);
  REQUIRE(oversized_id.error().code == df::SceneErrorCode::resource_limit);

  const auto layer = make_id<df::LayerId>("lay");
  const auto group = make_id<df::ObjectId>("grp");
  const auto child = make_id<df::ObjectId>("obj");
  const auto too_deep = df::detail::DocumentBuilder::build(
      make_id<df::DocumentId>("doc"), df::Revision{},
      make_extent(10, 10, *limits), std::nullopt, *limits,
      {df::Layer{layer, true}},
      {df::Group{group, layer, true}, df::Drawable{child,
                                                   group,
                                                   true,
                                                   {},
                                                   make_rectangle(0, 0, 1, 1),
                                                   filled({1, 2, 3, 255}),
                                                   make_opacity(1)}},
      {});
  REQUIRE_FALSE(too_deep);
  REQUIRE(too_deep.error().code == df::SceneErrorCode::resource_limit);

  const auto large = make_id<df::ObjectId>("big");
  const auto numeric = df::detail::DocumentBuilder::build(
      make_id<df::DocumentId>("doc"), df::Revision{},
      make_extent(10, 10, *limits), std::nullopt, *limits,
      {df::Layer{layer, true}},
      {df::Drawable{large,
                    layer,
                    true,
                    {},
                    make_rectangle(0, 0, 11, 1),
                    filled({1, 2, 3, 255}),
                    make_opacity(1)}},
      {});
  REQUIRE_FALSE(numeric);
  REQUIRE(numeric.error().code == df::SceneErrorCode::number_out_of_range);
}

TEST_CASE("bounded structure and selected queries preserve explicit order",
          "[query][structure][selected]") {
  const auto layer = make_id<df::LayerId>("artwork");
  const auto mascot = make_id<df::ObjectId>("mascot");
  const auto body = make_id<df::ObjectId>("body");
  const auto eye_left = make_id<df::ObjectId>("eye-left");
  const auto eye_right = make_id<df::ObjectId>("eye-right");
  auto document =
      make_document({df::Layer{layer, true}},
                    {df::Group{mascot, layer, true, translated(24, 12)},
                     df::Drawable{body,
                                  mascot,
                                  true,
                                  {},
                                  make_ellipse(24, 30, 18, 24),
                                  filled({40, 80, 120, 255}),
                                  make_opacity(1)},
                     df::Drawable{eye_left,
                                  mascot,
                                  true,
                                  {},
                                  make_ellipse(18, 24, 2, 3),
                                  filled({0, 0, 0, 255}),
                                  make_opacity(1)},
                     df::Drawable{eye_right,
                                  mascot,
                                  true,
                                  {},
                                  make_ellipse(30, 24, 2, 3),
                                  filled({0, 0, 0, 255}),
                                  make_opacity(1)}},
                    {}, df::Revision{4});

  const auto root = df::StructureQuery::create(mascot, 1, 4);
  REQUIRE(root);
  const auto structure = df::inspect(document, *root);
  REQUIRE(structure);
  REQUIRE(structure->revision == df::Revision{4});
  REQUIRE(structure->nodes.size() == 4);
  REQUIRE(std::get<df::ObjectId>(structure->nodes[0].identity) == mascot);
  REQUIRE(std::get<df::ObjectId>(structure->nodes[1].identity) == body);
  REQUIRE(std::get<df::ObjectId>(structure->nodes[2].identity) == eye_left);
  REQUIRE(std::get<df::ObjectId>(structure->nodes[3].identity) == eye_right);
  REQUIRE(structure->nodes[3].sibling_index == 2);

  const auto too_small = df::StructureQuery::create(mascot, 1, 3);
  REQUIRE(too_small);
  const auto failed = df::inspect(document, *too_small);
  REQUIRE_FALSE(failed);
  REQUIRE(failed.error().code == df::SceneErrorCode::resource_limit);

  const auto selection = df::SelectedObjectsQuery::create(
      {eye_right, body},
      {df::SelectedField::parent_order, df::SelectedField::geometry});
  REQUIRE(selection);
  const auto selected = df::inspect(document, *selection);
  REQUIRE(selected);
  REQUIRE(selected->objects.size() == 2);
  REQUIRE(selected->objects[0].object_id == eye_right);
  REQUIRE(selected->objects[1].object_id == body);
  REQUIRE(selected->objects[0].geometry.has_value());
  REQUIRE_FALSE(selected->objects[0].style.has_value());

  const auto inapplicable =
      df::SelectedObjectsQuery::create({mascot}, {df::SelectedField::style});
  REQUIRE(inapplicable);
  const auto rejected = df::inspect(document, *inapplicable);
  REQUIRE_FALSE(rejected);
  REQUIRE(rejected.error().code == df::SceneErrorCode::unsupported_property);
}

TEST_CASE("query factories reject duplicate selectors and invalid budgets",
          "[query][failure]") {
  const auto object = make_id<df::ObjectId>("object");
  const auto layer = make_id<df::LayerId>("layer");
  REQUIRE_FALSE(df::StructureQuery::create(std::monostate{}, 0, 0));
  REQUIRE_FALSE(df::StructureQuery::create(std::monostate{}, 130, 1));
  REQUIRE_FALSE(
      df::SelectedObjectsQuery::create({}, {df::SelectedField::kind}));
  REQUIRE_FALSE(df::SelectedObjectsQuery::create({object, object},
                                                 {df::SelectedField::kind}));
  REQUIRE_FALSE(df::SelectedObjectsQuery::create(
      {object}, {df::SelectedField::kind, df::SelectedField::kind}));
  REQUIRE_FALSE(df::BoundsQuery::create(
      {layer, layer}, {df::BoundsProjection::document_geometry}, 0));
  REQUIRE_FALSE(df::BoundsQuery::create({object},
                                        {df::BoundsProjection::local_geometry,
                                         df::BoundsProjection::local_geometry},
                                        0));
}

TEST_CASE("semantic bounds compose transforms, paint, and explicit time",
          "[query][bounds][animation]") {
  const auto layer = make_id<df::LayerId>("artwork");
  const auto group = make_id<df::ObjectId>("group");
  const auto dot = make_id<df::ObjectId>("dot");
  const auto track =
      df::OpacityTrack::create(make_id<df::TrackId>("entrance"), dot, 100, 600,
                               make_opacity(0), make_opacity(1));
  REQUIRE(track);
  auto document = make_document(
      {df::Layer{layer, true}},
      {df::Group{group, layer, true, translated(24, 12)},
       df::Drawable{dot, group, true, translated(2, 3),
                    make_ellipse(10, 10, 5, 3), filled({53, 196, 106, 255}),
                    make_opacity(0)}},
      {*track});

  const auto query =
      df::BoundsQuery::create({dot},
                              {df::BoundsProjection::local_geometry,
                               df::BoundsProjection::document_geometry,
                               df::BoundsProjection::document_painted},
                              400);
  REQUIRE(query);
  const auto result = df::inspect(document, *query);
  REQUIRE(result);
  REQUIRE(result->items.size() == 1);
  REQUIRE(result->items[0].projections.size() == 3);
  const auto &local = result->items[0].projections[0].bounds;
  const auto &world = result->items[0].projections[1].bounds;
  const auto &painted = result->items[0].projections[2].bounds;
  REQUIRE(local);
  REQUIRE(world);
  REQUIRE(painted);
  REQUIRE(local->x().value() == 5.0);
  REQUIRE(local->y().value() == 7.0);
  REQUIRE(world->x().value() == 31.0);
  REQUIRE(world->y().value() == 22.0);
  REQUIRE(world->width().value() == 10.0);
  REQUIRE(world->height().value() == 6.0);
  REQUIRE(*painted == *world);

  const auto before_query = df::BoundsQuery::create(
      {dot}, {df::BoundsProjection::document_painted}, 99);
  REQUIRE(before_query);
  const auto before = df::inspect(document, *before_query);
  REQUIRE(before);
  REQUIRE_FALSE(before->items[0].projections[0].bounds.has_value());
  REQUIRE(track->evaluate(99, make_opacity(0.25)).value() == 0.25);
  REQUIRE(track->evaluate(100, make_opacity(0.25)).value() == 0.0);
  REQUIRE(track->evaluate(400, make_opacity(0.25)).value() == 0.5);
  REQUIRE(track->evaluate(700, make_opacity(0.25)).value() == 1.0);
  REQUIRE(track->evaluate(900, make_opacity(0.25)).value() == 1.0);

  const auto repeated = df::inspect(document, *query);
  REQUIRE(repeated);
  REQUIRE(*repeated == *result);
}

TEST_CASE("stroke and affine ellipse bounds remain semantic and exact",
          "[query][bounds][stroke]") {
  const auto layer = make_id<df::LayerId>("artwork");
  const auto rectangle = make_id<df::ObjectId>("rectangle");
  const auto ellipse = make_id<df::ObjectId>("ellipse");
  const auto stroke = df::Stroke::create(df::Color{255, 255, 255, 255}, 2);
  const auto rotate = df::AffineTransform::create(0, 1, -1, 0, 0, 0);
  REQUIRE(stroke);
  REQUIRE(rotate);
  auto document = make_document(
      {df::Layer{layer, true}},
      {df::Drawable{rectangle,
                    layer,
                    true,
                    {},
                    make_rectangle(0, 0, 10, 4),
                    df::Style{std::nullopt, *stroke},
                    make_opacity(1)},
       df::Drawable{ellipse, layer, true, *rotate, make_ellipse(10, 20, 5, 3),
                    filled({1, 2, 3, 255}), make_opacity(1)}});
  const auto query = df::BoundsQuery::create(
      {rectangle, ellipse}, {df::BoundsProjection::document_painted}, 0);
  REQUIRE(query);
  const auto result = df::inspect(document, *query);
  REQUIRE(result);
  const auto &rectangle_bounds = result->items[0].projections[0].bounds;
  const auto &ellipse_bounds = result->items[1].projections[0].bounds;
  REQUIRE(rectangle_bounds);
  REQUIRE(ellipse_bounds);
  REQUIRE(rectangle_bounds->x().value() == -1.0);
  REQUIRE(rectangle_bounds->y().value() == -1.0);
  REQUIRE(rectangle_bounds->width().value() == 12.0);
  REQUIRE(rectangle_bounds->height().value() == 6.0);
  REQUIRE(ellipse_bounds->x().value() == -23.0);
  REQUIRE(ellipse_bounds->y().value() == 5.0);
  REQUIRE(ellipse_bounds->width().value() == 6.0);
  REQUIRE(ellipse_bounds->height().value() == 10.0);
}

TEST_CASE("empty and transparent paint produce explicit empty bounds",
          "[query][bounds][empty]") {
  const auto layer = make_id<df::LayerId>("artwork");
  const auto empty = make_id<df::ObjectId>("empty");
  const auto transparent = make_id<df::ObjectId>("transparent");
  const auto transparent_stroke = df::Stroke::create(df::Color{1, 2, 3, 0}, 2);
  REQUIRE(transparent_stroke);
  auto document = make_document(
      {df::Layer{layer, true}},
      {df::Drawable{empty,
                    layer,
                    true,
                    {},
                    make_rectangle(0, 0, 0, 4),
                    filled({1, 2, 3, 255}),
                    make_opacity(1)},
       df::Drawable{transparent,
                    layer,
                    true,
                    {},
                    make_ellipse(5, 5, 2, 2),
                    df::Style{df::Color{1, 2, 3, 0}, *transparent_stroke},
                    make_opacity(1)}});
  const auto query = df::BoundsQuery::create(
      {empty, transparent}, {df::BoundsProjection::document_painted}, 0);
  REQUIRE(query);
  const auto result = df::inspect(document, *query);
  REQUIRE(result);
  REQUIRE_FALSE(result->items[0].projections[0].bounds.has_value());
  REQUIRE_FALSE(result->items[1].projections[0].bounds.has_value());
}

TEST_CASE("open path strokes use butt caps and checked transform chains",
          "[query][bounds][path][failure]") {
  const auto layer = make_id<df::LayerId>("artwork");
  const auto line = make_id<df::ObjectId>("line");
  const auto group = make_id<df::ObjectId>("group");
  const auto large = make_id<df::ObjectId>("large");
  const auto stroke = df::Stroke::create(df::Color{255, 255, 255, 255}, 2);
  const auto start = df::Point::create(0, 0);
  const auto finish = df::Point::create(10, 0);
  const auto scale = df::AffineTransform::create(1'000, 0, 0, 1, 0, 0);
  const auto offset = df::AffineTransform::create(1, 0, 0, 1, 1'000, 0);
  REQUIRE(stroke);
  REQUIRE(start);
  REQUIRE(finish);
  const auto path = df::Path::create({df::MoveTo{*start}, df::LineTo{*finish}});
  REQUIRE(path);
  REQUIRE(scale);
  REQUIRE(offset);
  auto document = make_document(
      {df::Layer{layer, true}},
      {df::Drawable{line,
                    layer,
                    true,
                    {},
                    *path,
                    df::Style{std::nullopt, *stroke},
                    make_opacity(1)},
       df::Group{group, layer, true, *scale},
       df::Drawable{large, group, true, *offset, make_rectangle(0, 0, 1, 1),
                    filled({1, 2, 3, 255}), make_opacity(1)}});

  const auto line_query = df::BoundsQuery::create(
      {line}, {df::BoundsProjection::document_painted}, 0);
  REQUIRE(line_query);
  const auto line_result = df::inspect(document, *line_query);
  REQUIRE(line_result);
  const auto &line_bounds = line_result->items[0].projections[0].bounds;
  REQUIRE(line_bounds);
  REQUIRE(line_bounds->x().value() == 0.0);
  REQUIRE(line_bounds->y().value() == -1.0);
  REQUIRE(line_bounds->width().value() == 10.0);
  REQUIRE(line_bounds->height().value() == 2.0);

  const auto overflow_query = df::BoundsQuery::create(
      {large}, {df::BoundsProjection::document_geometry}, 0);
  REQUIRE(overflow_query);
  const auto overflow = df::inspect(document, *overflow_query);
  REQUIRE_FALSE(overflow);
  REQUIRE(overflow.error().code == df::SceneErrorCode::number_out_of_range);
}

TEST_CASE("empty public documents expose only owning read-only queries",
          "[scene][public][happy]") {
  STATIC_REQUIRE(std::is_copy_constructible_v<df::Document>);
  STATIC_REQUIRE_FALSE(std::is_default_constructible_v<df::Document>);
  const auto document = df::Document::create(make_id<df::DocumentId>("empty"),
                                             make_extent(64, 64));
  REQUIRE(document);
  const auto summary = df::inspect(*document, df::SummaryQuery{});
  REQUIRE(summary);
  REQUIRE(summary->revision == df::Revision{});
  REQUIRE(summary->layer_count == 0);
  REQUIRE(summary->object_count == 0);
  REQUIRE(summary->track_count == 0);
  REQUIRE(summary->document_id.value() == "empty");

  constexpr std::array codes{
      df::SceneErrorCode::missing_identity,
      df::SceneErrorCode::duplicate_identity,
      df::SceneErrorCode::invalid_parent,
      df::SceneErrorCode::parent_cycle,
      df::SceneErrorCode::invalid_geometry,
      df::SceneErrorCode::unsupported_node_kind,
      df::SceneErrorCode::unsupported_property,
      df::SceneErrorCode::resource_limit,
      df::SceneErrorCode::number_out_of_range,
      df::SceneErrorCode::arithmetic_overflow,
      df::SceneErrorCode::allocation_failure,
  };
  for (const auto code : codes) {
    REQUIRE(df::scene_error_code_name(code) != "unknown");
  }
}
