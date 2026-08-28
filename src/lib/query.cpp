#include <drawforge/query.hpp>

#include "scene_internal.hpp"

#include <algorithm>
#include <cmath>
#include <cstddef>
#include <limits>
#include <new>
#include <string>
#include <type_traits>
#include <utility>

namespace drawforge {
namespace {

constexpr SceneError missing_identity_error{SceneErrorCode::missing_identity,
                                            "identity does not exist"};
constexpr SceneError duplicate_identity_error{
    SceneErrorCode::duplicate_identity, "query contains a duplicate selector"};
constexpr SceneError invalid_geometry_error{SceneErrorCode::invalid_geometry,
                                            "bounds geometry is invalid"};
constexpr SceneError unsupported_node_kind_error{
    SceneErrorCode::unsupported_node_kind, "node kind is not supported here"};
constexpr SceneError unsupported_property_error{
    SceneErrorCode::unsupported_property, "property is not supported here"};
constexpr SceneError resource_limit_error{SceneErrorCode::resource_limit,
                                          "query resource limit exceeded"};
constexpr SceneError number_out_of_range_error{
    SceneErrorCode::number_out_of_range,
    "bounds exceed document numeric limits"};
constexpr SceneError arithmetic_overflow_error{
    SceneErrorCode::arithmetic_overflow, "bounds arithmetic overflowed"};
constexpr SceneError allocation_failure_error{
    SceneErrorCode::allocation_failure, "allocation failed"};

struct RawBounds {
  long double min_x{};
  long double min_y{};
  long double max_x{};
  long double max_y{};
  bool present{};
};

[[nodiscard]] auto object_key(const ObjectId &id) -> std::string {
  return std::string{id.value()};
}

[[nodiscard]] auto layer_key(const LayerId &id) -> std::string {
  return std::string{id.value()};
}

auto include_point(RawBounds &bounds, const long double x,
                   const long double y) noexcept -> void {
  if (!bounds.present) {
    bounds = RawBounds{x, y, x, y, true};
    return;
  }
  bounds.min_x = std::min(bounds.min_x, x);
  bounds.min_y = std::min(bounds.min_y, y);
  bounds.max_x = std::max(bounds.max_x, x);
  bounds.max_y = std::max(bounds.max_y, y);
}

auto include_bounds(RawBounds &target, const RawBounds &value) noexcept
    -> void {
  if (!value.present)
    return;
  include_point(target, value.min_x, value.min_y);
  include_point(target, value.max_x, value.max_y);
}

[[nodiscard]] auto transformed_point(const Point point,
                                     const AffineTransform transform,
                                     const ResourceLimits &limits) noexcept
    -> std::expected<Point, SceneError> {
  const auto result = transform.apply(point, limits);
  if (!result) {
    if (result.error().code == ValueErrorCode::arithmetic_overflow)
      return std::unexpected{arithmetic_overflow_error};
    return std::unexpected{number_out_of_range_error};
  }
  return *result;
}

[[nodiscard]] auto compose(const AffineTransform first,
                           const AffineTransform next,
                           const ResourceLimits &limits) noexcept
    -> std::expected<AffineTransform, SceneError> {
  const auto result = first.then(next, limits);
  if (!result) {
    if (result.error().code == ValueErrorCode::arithmetic_overflow)
      return std::unexpected{arithmetic_overflow_error};
    return std::unexpected{number_out_of_range_error};
  }
  return *result;
}

[[nodiscard]] auto checked_bounds(const RawBounds &raw,
                                  const ResourceLimits &limits) noexcept
    -> std::expected<std::optional<Bounds>, SceneError> {
  if (!raw.present)
    return std::optional<Bounds>{};
  const auto width = raw.max_x - raw.min_x;
  const auto height = raw.max_y - raw.min_y;
  for (const auto value :
       {raw.min_x, raw.min_y, raw.max_x, raw.max_y, width, height}) {
    if (!std::isfinite(value))
      return std::unexpected{arithmetic_overflow_error};
    if (std::abs(value) > limits.max_numeric_magnitude())
      return std::unexpected{number_out_of_range_error};
  }
  const auto result = Bounds::create(
      static_cast<double>(raw.min_x), static_cast<double>(raw.min_y),
      static_cast<double>(width), static_cast<double>(height), limits);
  if (!result)
    return std::unexpected{result.error()};
  return std::optional<Bounds>{*result};
}

[[nodiscard]] auto
support_bounds(const long double center_x, const long double center_y,
               const long double extent_x, const long double extent_y) noexcept
    -> RawBounds {
  return RawBounds{center_x - extent_x, center_y - extent_y,
                   center_x + extent_x, center_y + extent_y, true};
}

[[nodiscard]] auto rectangle_bounds(const Rectangle &rectangle,
                                    const AffineTransform transform,
                                    const std::optional<Stroke> &stroke,
                                    const ResourceLimits &limits) noexcept
    -> std::expected<RawBounds, SceneError> {
  const auto x = static_cast<long double>(rectangle.origin().x().value());
  const auto y = static_cast<long double>(rectangle.origin().y().value());
  const auto width = static_cast<long double>(rectangle.width().value());
  const auto height = static_cast<long double>(rectangle.height().value());
  const auto radius_x = static_cast<long double>(rectangle.radius_x().value());
  const auto radius_y = static_cast<long double>(rectangle.radius_y().value());
  const auto center =
      Point::create(static_cast<double>(x + width / 2.0L),
                    static_cast<double>(y + height / 2.0L), limits);
  if (!center)
    return std::unexpected{number_out_of_range_error};
  const auto transformed = transformed_point(*center, transform, limits);
  if (!transformed)
    return std::unexpected{transformed.error()};

  const auto a = static_cast<long double>(transform.a().value());
  const auto b = static_cast<long double>(transform.b().value());
  const auto c = static_cast<long double>(transform.c().value());
  const auto d = static_cast<long double>(transform.d().value());
  auto extent_x = std::abs(a) * (width / 2.0L - radius_x) +
                  std::abs(c) * (height / 2.0L - radius_y) +
                  std::hypot(a * radius_x, c * radius_y);
  auto extent_y = std::abs(b) * (width / 2.0L - radius_x) +
                  std::abs(d) * (height / 2.0L - radius_y) +
                  std::hypot(b * radius_x, d * radius_y);
  if (stroke) {
    const auto half = static_cast<long double>(stroke->width().value()) / 2.0L;
    if (radius_x == 0.0L && radius_y == 0.0L) {
      extent_x += half * (std::abs(a) + std::abs(c));
      extent_y += half * (std::abs(b) + std::abs(d));
    } else {
      extent_x += half * std::hypot(a, c);
      extent_y += half * std::hypot(b, d);
    }
  }
  return support_bounds(transformed->x().value(), transformed->y().value(),
                        extent_x, extent_y);
}

[[nodiscard]] auto ellipse_bounds(const Ellipse &ellipse,
                                  const AffineTransform transform,
                                  const std::optional<Stroke> &stroke,
                                  const ResourceLimits &limits) noexcept
    -> std::expected<RawBounds, SceneError> {
  const auto center = transformed_point(ellipse.center(), transform, limits);
  if (!center)
    return std::unexpected{center.error()};
  const auto a = static_cast<long double>(transform.a().value());
  const auto b = static_cast<long double>(transform.b().value());
  const auto c = static_cast<long double>(transform.c().value());
  const auto d = static_cast<long double>(transform.d().value());
  const auto rx = static_cast<long double>(ellipse.radius_x().value());
  const auto ry = static_cast<long double>(ellipse.radius_y().value());
  auto extent_x = std::hypot(a * rx, c * ry);
  auto extent_y = std::hypot(b * rx, d * ry);
  if (stroke) {
    const auto half = static_cast<long double>(stroke->width().value()) / 2.0L;
    extent_x += half * std::hypot(a, c);
    extent_y += half * std::hypot(b, d);
  }
  return support_bounds(center->x().value(), center->y().value(), extent_x,
                        extent_y);
}

struct Subpath {
  std::vector<Point> points;
  bool closed{};
};

[[nodiscard]] auto path_subpaths(const Path &path) -> std::vector<Subpath> {
  std::vector<Subpath> result;
  for (const auto &command : path.commands()) {
    if (const auto *move = std::get_if<MoveTo>(&command)) {
      result.push_back(Subpath{{move->point}, false});
    } else if (const auto *line = std::get_if<LineTo>(&command)) {
      result.back().points.push_back(line->point);
    } else {
      result.back().closed = true;
    }
  }
  return result;
}

auto include_transformed(RawBounds &bounds, const long double x,
                         const long double y, const AffineTransform transform,
                         const ResourceLimits &limits) noexcept
    -> std::expected<void, SceneError> {
  const auto point =
      Point::create(static_cast<double>(x), static_cast<double>(y), limits);
  if (!point)
    return std::unexpected{number_out_of_range_error};
  const auto transformed = transformed_point(*point, transform, limits);
  if (!transformed)
    return std::unexpected{transformed.error()};
  include_point(bounds, transformed->x().value(), transformed->y().value());
  return {};
}

[[nodiscard]] auto
path_bounds(const Path &path, const AffineTransform transform,
            const bool include_fill, const std::optional<Stroke> &stroke,
            const ResourceLimits &limits)
    -> std::expected<RawBounds, SceneError> {
  RawBounds bounds;
  const auto subpaths = path_subpaths(path);
  if (include_fill) {
    for (const auto &subpath : subpaths) {
      for (const auto point : subpath.points) {
        const auto included = include_transformed(
            bounds, point.x().value(), point.y().value(), transform, limits);
        if (!included)
          return std::unexpected{included.error()};
      }
    }
  }
  if (!stroke)
    return bounds;

  const auto half = stroke->width().value() / 2.0;
  for (const auto &subpath : subpaths) {
    const auto count = subpath.points.size();
    const auto segment_count = subpath.closed ? count : count - 1;
    for (std::size_t i = 0; i < segment_count; ++i) {
      const auto &from = subpath.points[i];
      const auto &to = subpath.points[(i + 1) % count];
      const auto dx = to.x().value() - from.x().value();
      const auto dy = to.y().value() - from.y().value();
      const auto length = std::hypot(dx, dy);
      if (length == 0.0)
        continue;
      const auto nx = -dy * half / length;
      const auto ny = dx * half / length;
      for (const auto &point : {from, to}) {
        for (const auto sign : {-1.0, 1.0}) {
          const auto included = include_transformed(
              bounds, point.x().value() + sign * nx,
              point.y().value() + sign * ny, transform, limits);
          if (!included)
            return std::unexpected{included.error()};
        }
      }
    }

    const auto first_join = subpath.closed ? std::size_t{0} : std::size_t{1};
    const auto last_join = subpath.closed ? count : count - 1;
    for (std::size_t i = first_join; i < last_join; ++i) {
      const auto &before = subpath.points[(i + count - 1) % count];
      const auto &at = subpath.points[i];
      const auto &after = subpath.points[(i + 1) % count];
      const auto dx1 = at.x().value() - before.x().value();
      const auto dy1 = at.y().value() - before.y().value();
      const auto dx2 = after.x().value() - at.x().value();
      const auto dy2 = after.y().value() - at.y().value();
      const auto len1 = std::hypot(dx1, dy1);
      const auto len2 = std::hypot(dx2, dy2);
      if (len1 == 0.0 || len2 == 0.0)
        continue;
      const auto n1x = -dy1 / len1;
      const auto n1y = dx1 / len1;
      const auto n2x = -dy2 / len2;
      const auto n2y = dx2 / len2;
      for (const auto sign : {-1.0, 1.0}) {
        auto mx = sign * (n1x + n2x);
        auto my = sign * (n1y + n2y);
        const auto mlen = std::hypot(mx, my);
        if (mlen == 0.0)
          continue;
        mx /= mlen;
        my /= mlen;
        const auto denominator = mx * (sign * n2x) + my * (sign * n2y);
        if (denominator <= 0.0)
          continue;
        const auto distance = half / denominator;
        if (distance > half * 4.0)
          continue;
        const auto included = include_transformed(
            bounds, at.x().value() + mx * distance,
            at.y().value() + my * distance, transform, limits);
        if (!included)
          return std::unexpected{included.error()};
      }
    }
  }
  return bounds;
}

[[nodiscard]] auto
geometry_bounds(const Drawable &drawable, const AffineTransform transform,
                const bool painted, const ResourceLimits &limits) noexcept
    -> std::expected<RawBounds, SceneError> {
  auto stroke = painted ? drawable.style().stroke() : std::optional<Stroke>{};
  if (stroke && stroke->color().alpha == 0)
    stroke.reset();
  const auto visible_fill =
      drawable.style().fill() && drawable.style().fill()->alpha != 0;
  return std::visit(
      [&](const auto &geometry) -> std::expected<RawBounds, SceneError> {
        using T = std::remove_cvref_t<decltype(geometry)>;
        if constexpr (std::is_same_v<T, Rectangle>) {
          if (geometry.width().value() == 0.0 ||
              geometry.height().value() == 0.0)
            return RawBounds{};
          if (painted && !visible_fill && !stroke)
            return RawBounds{};
          return rectangle_bounds(geometry, transform, stroke, limits);
        } else if constexpr (std::is_same_v<T, Ellipse>) {
          if (geometry.radius_x().value() == 0.0 ||
              geometry.radius_y().value() == 0.0)
            return RawBounds{};
          if (painted && !visible_fill && !stroke)
            return RawBounds{};
          return ellipse_bounds(geometry, transform, stroke, limits);
        } else {
          return path_bounds(geometry, transform, !painted || visible_fill,
                             stroke, limits);
        }
      },
      drawable.geometry());
}

[[nodiscard]] auto world_transform(const detail::DocumentState &state,
                                   const std::size_t index) noexcept
    -> std::expected<AffineTransform, SceneError> {
  const auto local = detail::object_transform(state.objects[index].object);
  const auto &parent = detail::object_parent(state.objects[index].object);
  if (std::holds_alternative<LayerId>(parent))
    return local;
  const auto found =
      state.object_index.find(object_key(std::get<ObjectId>(parent)));
  if (found == state.object_index.end())
    return std::unexpected{missing_identity_error};
  const auto ancestor = world_transform(state, found->second);
  if (!ancestor)
    return std::unexpected{ancestor.error()};
  return compose(local, *ancestor, state.limits);
}

[[nodiscard]] auto effectively_visible(const detail::DocumentState &state,
                                       const std::size_t index) -> bool {
  if (!detail::object_visible(state.objects[index].object))
    return false;
  const auto &parent = detail::object_parent(state.objects[index].object);
  if (const auto *layer = std::get_if<LayerId>(&parent)) {
    return state.layers[state.layer_index.at(layer_key(*layer))]
        .layer.visible();
  }
  const auto parent_index =
      state.object_index.at(object_key(std::get<ObjectId>(parent)));
  return effectively_visible(state, parent_index);
}

[[nodiscard]] auto evaluated_opacity(const detail::DocumentState &state,
                                     const Drawable &drawable,
                                     const std::uint64_t time_us) -> Opacity {
  const auto track = state.track_by_object.find(object_key(drawable.id()));
  if (track == state.track_by_object.end())
    return drawable.opacity();
  return state.tracks[track->second].evaluate(time_us, drawable.opacity());
}

[[nodiscard]] auto
subtree_bounds(const detail::DocumentState &state, const std::size_t index,
               const AffineTransform transform, const bool painted,
               const std::uint64_t time_us) noexcept
    -> std::expected<RawBounds, SceneError> {
  const auto &record = state.objects[index];
  if (const auto *drawable = std::get_if<Drawable>(&record.object)) {
    if (painted &&
        (!effectively_visible(state, index) ||
         evaluated_opacity(state, *drawable, time_us).value() == 0.0)) {
      return RawBounds{};
    }
    return geometry_bounds(*drawable, transform, painted, state.limits);
  }
  RawBounds result;
  for (const auto child : record.children) {
    const auto child_transform =
        compose(detail::object_transform(state.objects[child].object),
                transform, state.limits);
    if (!child_transform)
      return std::unexpected{child_transform.error()};
    const auto bounds =
        subtree_bounds(state, child, *child_transform, painted, time_us);
    if (!bounds)
      return std::unexpected{bounds.error()};
    include_bounds(result, *bounds);
  }
  return result;
}

[[nodiscard]] auto bounds_for_target(const detail::DocumentState &state,
                                     const BoundsTarget &target,
                                     const BoundsProjection projection,
                                     const std::uint64_t time_us) noexcept
    -> std::expected<std::optional<Bounds>, SceneError> {
  RawBounds result;
  const auto painted = projection == BoundsProjection::document_painted;
  if (const auto *layer_id = std::get_if<LayerId>(&target)) {
    const auto found = state.layer_index.find(layer_key(*layer_id));
    if (found == state.layer_index.end())
      return std::unexpected{missing_identity_error};
    for (const auto child : state.layers[found->second].children) {
      const auto transform =
          detail::object_transform(state.objects[child].object);
      const auto bounds =
          subtree_bounds(state, child, transform, painted, time_us);
      if (!bounds)
        return std::unexpected{bounds.error()};
      include_bounds(result, *bounds);
    }
  } else {
    const auto &object_id = std::get<ObjectId>(target);
    const auto found = state.object_index.find(object_key(object_id));
    if (found == state.object_index.end())
      return std::unexpected{missing_identity_error};
    AffineTransform transform;
    if (projection != BoundsProjection::local_geometry) {
      const auto world = world_transform(state, found->second);
      if (!world)
        return std::unexpected{world.error()};
      transform = *world;
    }
    const auto bounds =
        subtree_bounds(state, found->second, transform, painted, time_us);
    if (!bounds)
      return std::unexpected{bounds.error()};
    result = *bounds;
  }
  return checked_bounds(result, state.limits);
}

[[nodiscard]] auto node_kind(const detail::ObjectRecord &record) noexcept
    -> NodeKind {
  switch (detail::object_kind(record.object)) {
  case ObjectKind::group:
    return NodeKind::group;
  case ObjectKind::rectangle:
    return NodeKind::rectangle;
  case ObjectKind::ellipse:
    return NodeKind::ellipse;
  case ObjectKind::path:
    return NodeKind::path;
  }
  return NodeKind::group;
}

auto append_object_structure(const detail::DocumentState &state,
                             const std::size_t index, const std::uint32_t depth,
                             const StructureQuery &query,
                             std::vector<StructureNode> &nodes) noexcept
    -> std::expected<void, SceneError> {
  if (nodes.size() >= query.max_nodes())
    return std::unexpected{resource_limit_error};
  const auto &record = state.objects[index];
  nodes.push_back(StructureNode{
      detail::object_id(record.object), node_kind(record),
      detail::object_parent(record.object), record.sibling_index,
      detail::object_visible(record.object), record.children.size()});
  if (depth >= query.max_depth())
    return {};
  for (const auto child : record.children) {
    const auto appended =
        append_object_structure(state, child, depth + 1, query, nodes);
    if (!appended)
      return std::unexpected{appended.error()};
  }
  return {};
}

template <typename T>
[[nodiscard]] auto has_duplicates(const std::vector<T> &values) noexcept
    -> bool {
  for (std::size_t i = 0; i < values.size(); ++i) {
    for (std::size_t j = i + 1; j < values.size(); ++j) {
      if (values[i] == values[j])
        return true;
    }
  }
  return false;
}

} // namespace

auto Bounds::create(const double x, const double y, const double width,
                    const double height, const ResourceLimits &limits) noexcept
    -> std::expected<Bounds, SceneError> {
  if (!std::isfinite(width) || !std::isfinite(height) || width < 0.0 ||
      height < 0.0)
    return std::unexpected{invalid_geometry_error};
  const auto checked_x = Coordinate::create(x, limits);
  const auto checked_y = Coordinate::create(y, limits);
  const auto checked_width = Coordinate::create(width, limits);
  const auto checked_height = Coordinate::create(height, limits);
  if (!checked_x || !checked_y || !checked_width || !checked_height)
    return std::unexpected{number_out_of_range_error};
  return Bounds{*checked_x, *checked_y, *checked_width, *checked_height};
}

auto StructureQuery::create(StructureRoot root, const std::uint32_t max_depth,
                            const std::uint64_t max_nodes) noexcept
    -> std::expected<StructureQuery, SceneError> {
  if (max_nodes == 0 ||
      max_nodes > hard_resource_limit_values.max_scene_nodes ||
      max_depth > hard_resource_limit_values.max_nesting_depth + 1U)
    return std::unexpected{resource_limit_error};
  return StructureQuery{std::move(root), max_depth, max_nodes};
}

auto SelectedObjectsQuery::create(std::vector<ObjectId> object_ids,
                                  std::vector<SelectedField> fields) noexcept
    -> std::expected<SelectedObjectsQuery, SceneError> {
  if (object_ids.empty() ||
      object_ids.size() > hard_resource_limit_values.max_scene_nodes)
    return std::unexpected{resource_limit_error};
  if (fields.empty())
    return std::unexpected{unsupported_property_error};
  if (has_duplicates(object_ids) || has_duplicates(fields))
    return std::unexpected{duplicate_identity_error};
  return SelectedObjectsQuery{std::move(object_ids), std::move(fields)};
}

auto BoundsQuery::create(std::vector<BoundsTarget> targets,
                         std::vector<BoundsProjection> projections,
                         const std::uint64_t time_us) noexcept
    -> std::expected<BoundsQuery, SceneError> {
  if (targets.empty() || projections.empty() ||
      targets.size() > hard_resource_limit_values.max_scene_nodes)
    return std::unexpected{resource_limit_error};
  if (has_duplicates(targets) || has_duplicates(projections))
    return std::unexpected{duplicate_identity_error};
  return BoundsQuery{std::move(targets), std::move(projections), time_us};
}

auto inspect(const Document &document, SummaryQuery) noexcept
    -> std::expected<DocumentSummary, SceneError> {
  try {
    const auto &state = DocumentAccess::state(document);
    return DocumentSummary{state.id,
                           state.revision,
                           state.canvas,
                           state.background,
                           state.limits,
                           state.layers.size(),
                           state.objects.size(),
                           state.tracks.size()};
  } catch (...) {
    return std::unexpected{allocation_failure_error};
  }
}

auto inspect(const Document &document, const StructureQuery &query) noexcept
    -> std::expected<StructureResult, SceneError> {
  try {
    const auto &state = DocumentAccess::state(document);
    const auto depth_limit =
        std::holds_alternative<std::monostate>(query.root())
            ? state.limits.max_nesting_depth() + 1U
            : state.limits.max_nesting_depth();
    if (query.max_depth() > depth_limit ||
        query.max_nodes() > state.limits.max_scene_nodes())
      return std::unexpected{resource_limit_error};
    StructureResult result{state.id, state.revision, {}};
    if (std::holds_alternative<std::monostate>(query.root())) {
      if (query.max_depth() == 0)
        return result;
      for (std::size_t layer_index = 0; layer_index < state.layers.size();
           ++layer_index) {
        if (result.nodes.size() >= query.max_nodes())
          return std::unexpected{resource_limit_error};
        const auto &record = state.layers[layer_index];
        result.nodes.push_back(StructureNode{
            record.layer.id(), NodeKind::layer, std::nullopt, layer_index,
            record.layer.visible(), record.children.size()});
        if (query.max_depth() > 1) {
          for (const auto child : record.children) {
            const auto appended =
                append_object_structure(state, child, 2, query, result.nodes);
            if (!appended)
              return std::unexpected{appended.error()};
          }
        }
      }
      return result;
    }
    if (const auto *layer_id = std::get_if<LayerId>(&query.root())) {
      const auto found = state.layer_index.find(layer_key(*layer_id));
      if (found == state.layer_index.end())
        return std::unexpected{missing_identity_error};
      const auto &record = state.layers[found->second];
      result.nodes.push_back(StructureNode{
          record.layer.id(), NodeKind::layer, std::nullopt, found->second,
          record.layer.visible(), record.children.size()});
      if (query.max_depth() > 0) {
        for (const auto child : record.children) {
          const auto appended =
              append_object_structure(state, child, 1, query, result.nodes);
          if (!appended)
            return std::unexpected{appended.error()};
        }
      }
      return result;
    }
    const auto &object_id = std::get<ObjectId>(query.root());
    const auto found = state.object_index.find(object_key(object_id));
    if (found == state.object_index.end())
      return std::unexpected{missing_identity_error};
    if (!std::holds_alternative<Group>(state.objects[found->second].object))
      return std::unexpected{unsupported_node_kind_error};
    const auto appended =
        append_object_structure(state, found->second, 0, query, result.nodes);
    if (!appended)
      return std::unexpected{appended.error()};
    return result;
  } catch (...) {
    return std::unexpected{allocation_failure_error};
  }
}

auto inspect(const Document &document,
             const SelectedObjectsQuery &query) noexcept
    -> std::expected<SelectedObjectsResult, SceneError> {
  try {
    const auto &state = DocumentAccess::state(document);
    if (query.object_ids().size() > state.limits.max_scene_nodes())
      return std::unexpected{resource_limit_error};
    SelectedObjectsResult result{state.id, state.revision, {}};
    result.objects.reserve(query.object_ids().size());
    for (const auto &id : query.object_ids()) {
      const auto found = state.object_index.find(object_key(id));
      if (found == state.object_index.end())
        return std::unexpected{missing_identity_error};
      const auto &record = state.objects[found->second];
      SelectedObject selected{id,           std::nullopt, std::nullopt,
                              std::nullopt, std::nullopt, std::nullopt,
                              std::nullopt, std::nullopt, std::nullopt};
      for (const auto field : query.fields()) {
        if (field == SelectedField::kind) {
          selected.kind = detail::object_kind(record.object);
        } else if (field == SelectedField::parent_order) {
          selected.parent_order = ParentOrder{
              detail::object_parent(record.object), record.sibling_index};
        } else if (field == SelectedField::visibility) {
          selected.visibility = detail::object_visible(record.object);
        } else if (field == SelectedField::transform) {
          selected.transform = detail::object_transform(record.object);
        } else {
          const auto *drawable = std::get_if<Drawable>(&record.object);
          if (!drawable)
            return std::unexpected{unsupported_property_error};
          if (field == SelectedField::geometry)
            selected.geometry = drawable->geometry();
          else if (field == SelectedField::style)
            selected.style = drawable->style();
          else if (field == SelectedField::opacity)
            selected.opacity = drawable->opacity();
          else {
            const auto track = state.track_by_object.find(object_key(id));
            if (track != state.track_by_object.end()) {
              const auto &value = state.tracks[track->second];
              selected.opacity_track = OpacityTrackMetadata{
                  value.id(), value.start_time_us(), value.duration_us(),
                  value.from(), value.to()};
            }
          }
        }
      }
      result.objects.push_back(std::move(selected));
    }
    return result;
  } catch (...) {
    return std::unexpected{allocation_failure_error};
  }
}

auto inspect(const Document &document, const BoundsQuery &query) noexcept
    -> std::expected<BoundsResult, SceneError> {
  try {
    const auto &state = DocumentAccess::state(document);
    if (query.targets().size() > state.limits.max_scene_nodes())
      return std::unexpected{resource_limit_error};
    BoundsResult result{state.id, state.revision, query.time_us(), {}};
    result.items.reserve(query.targets().size());
    for (const auto &target : query.targets()) {
      BoundsItem item{target, {}};
      item.projections.reserve(query.projections().size());
      for (const auto projection : query.projections()) {
        const auto bounds =
            bounds_for_target(state, target, projection, query.time_us());
        if (!bounds)
          return std::unexpected{bounds.error()};
        item.projections.push_back(ProjectedBounds{projection, *bounds});
      }
      result.items.push_back(std::move(item));
    }
    return result;
  } catch (...) {
    return std::unexpected{allocation_failure_error};
  }
}

} // namespace drawforge
