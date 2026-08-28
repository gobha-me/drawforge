#include <drawforge/scene.hpp>

#include "scene_internal.hpp"

#include <algorithm>
#include <cmath>
#include <limits>
#include <new>
#include <string>
#include <type_traits>
#include <unordered_set>
#include <utility>

namespace drawforge {
namespace {

constexpr SceneError missing_identity_error{SceneErrorCode::missing_identity,
                                            "identity does not exist"};
constexpr SceneError duplicate_identity_error{
    SceneErrorCode::duplicate_identity, "identity is duplicated"};
constexpr SceneError invalid_parent_error{
    SceneErrorCode::invalid_parent, "parent is not an accepted container"};
constexpr SceneError parent_cycle_error{SceneErrorCode::parent_cycle,
                                        "object parentage contains a cycle"};
constexpr SceneError invalid_geometry_error{SceneErrorCode::invalid_geometry,
                                            "geometry is invalid"};
constexpr SceneError unsupported_property_error{
    SceneErrorCode::unsupported_property, "property is not supported here"};
constexpr SceneError resource_limit_error{SceneErrorCode::resource_limit,
                                          "resource limit exceeded"};
constexpr SceneError number_out_of_range_error{
    SceneErrorCode::number_out_of_range, "number is outside document limits"};
constexpr SceneError arithmetic_overflow_error{
    SceneErrorCode::arithmetic_overflow, "checked arithmetic overflowed"};
constexpr SceneError allocation_failure_error{
    SceneErrorCode::allocation_failure, "allocation failed"};

[[nodiscard]] auto map_value_error(const ValueError &error) noexcept
    -> SceneError {
  switch (error.code) {
  case ValueErrorCode::resource_limit:
  case ValueErrorCode::invalid_limit:
    return resource_limit_error;
  case ValueErrorCode::arithmetic_overflow:
    return arithmetic_overflow_error;
  case ValueErrorCode::allocation_failure:
    return allocation_failure_error;
  default:
    return number_out_of_range_error;
  }
}

[[nodiscard]] auto checked_coordinate(double value,
                                      const ResourceLimits &limits) noexcept
    -> std::expected<Coordinate, SceneError> {
  const auto coordinate = Coordinate::create(value, limits);
  if (!coordinate) {
    return std::unexpected{map_value_error(coordinate.error())};
  }
  return *coordinate;
}

template <typename Id>
[[nodiscard]] auto validate_id(const Id &id,
                               const ResourceLimits &limits) noexcept
    -> std::expected<void, SceneError> {
  const auto checked = Id::create(id.value(), limits);
  if (!checked)
    return std::unexpected{map_value_error(checked.error())};
  return {};
}

[[nodiscard]] auto validate_transform(const AffineTransform transform,
                                      const ResourceLimits &limits) noexcept
    -> std::expected<void, SceneError> {
  const auto checked = AffineTransform::create(
      transform.a().value(), transform.b().value(), transform.c().value(),
      transform.d().value(), transform.e().value(), transform.f().value(),
      limits);
  if (!checked)
    return std::unexpected{map_value_error(checked.error())};
  return {};
}

[[nodiscard]] auto validate_geometry(const Geometry &geometry,
                                     const ResourceLimits &limits) noexcept
    -> std::expected<void, SceneError> {
  return std::visit(
      [&](const auto &value) -> std::expected<void, SceneError> {
        using T = std::remove_cvref_t<decltype(value)>;
        if constexpr (std::is_same_v<T, Rectangle>) {
          const auto checked = Rectangle::create(
              value.origin().x().value(), value.origin().y().value(),
              value.width().value(), value.height().value(),
              value.radius_x().value(), value.radius_y().value(), limits);
          if (!checked)
            return std::unexpected{checked.error()};
        } else if constexpr (std::is_same_v<T, Ellipse>) {
          const auto checked = Ellipse::create(
              value.center().x().value(), value.center().y().value(),
              value.radius_x().value(), value.radius_y().value(), limits);
          if (!checked)
            return std::unexpected{checked.error()};
        } else {
          for (const auto &command : value.commands()) {
            const auto point = std::visit(
                [](const auto &item) -> std::optional<Point> {
                  using Command = std::remove_cvref_t<decltype(item)>;
                  if constexpr (std::is_same_v<Command, ClosePath>)
                    return std::nullopt;
                  else
                    return item.point;
                },
                command);
            if (point) {
              const auto checked =
                  Point::create(point->x().value(), point->y().value(), limits);
              if (!checked)
                return std::unexpected{map_value_error(checked.error())};
            }
          }
        }
        return {};
      },
      geometry);
}

[[nodiscard]] auto
validate_document_values(const DocumentId &id, const CanvasExtent &canvas,
                         const ResourceLimits &limits,
                         const std::vector<Layer> &layers,
                         const std::vector<SceneObject> &objects,
                         const std::vector<OpacityTrack> &tracks) noexcept
    -> std::expected<void, SceneError> {
  const auto checked_id = validate_id(id, limits);
  if (!checked_id)
    return std::unexpected{checked_id.error()};
  const auto checked_canvas =
      CanvasExtent::create(canvas.width(), canvas.height(), limits);
  if (!checked_canvas)
    return std::unexpected{map_value_error(checked_canvas.error())};
  for (const auto &layer : layers) {
    const auto checked = validate_id(layer.id(), limits);
    if (!checked)
      return std::unexpected{checked.error()};
  }
  for (const auto &object : objects) {
    const auto checked_object_id =
        validate_id(detail::object_id(object), limits);
    if (!checked_object_id)
      return std::unexpected{checked_object_id.error()};
    const auto checked_parent = std::visit(
        [&](const auto &parent) { return validate_id(parent, limits); },
        detail::object_parent(object));
    if (!checked_parent)
      return std::unexpected{checked_parent.error()};
    const auto checked_transform =
        validate_transform(detail::object_transform(object), limits);
    if (!checked_transform)
      return std::unexpected{checked_transform.error()};
    if (const auto *drawable = std::get_if<Drawable>(&object)) {
      const auto checked_geometry =
          validate_geometry(drawable->geometry(), limits);
      if (!checked_geometry)
        return std::unexpected{checked_geometry.error()};
      if (drawable->style().stroke()) {
        const auto checked_stroke =
            Stroke::create(drawable->style().stroke()->color(),
                           drawable->style().stroke()->width().value(), limits);
        if (!checked_stroke)
          return std::unexpected{checked_stroke.error()};
      }
    }
  }
  for (const auto &track : tracks) {
    const auto checked_track = validate_id(track.id(), limits);
    if (!checked_track)
      return std::unexpected{checked_track.error()};
    const auto checked_target = validate_id(track.target(), limits);
    if (!checked_target)
      return std::unexpected{checked_target.error()};
  }
  return {};
}

[[nodiscard]] auto validate_depth(std::size_t index,
                                  detail::DocumentState &state,
                                  std::vector<std::uint8_t> &marks,
                                  std::vector<std::uint32_t> &depths) noexcept
    -> std::expected<std::uint32_t, SceneError> {
  if (marks[index] == 2) {
    return depths[index];
  }
  if (marks[index] == 1) {
    return std::unexpected{parent_cycle_error};
  }
  marks[index] = 1;
  const auto &parent = detail::object_parent(state.objects[index].object);
  std::uint64_t depth = 1;
  if (const auto *object_parent = std::get_if<ObjectId>(&parent)) {
    const auto found =
        state.object_index.find(std::string{object_parent->value()});
    if (found == state.object_index.end()) {
      return std::unexpected{missing_identity_error};
    }
    if (!std::holds_alternative<Group>(state.objects[found->second].object)) {
      return std::unexpected{invalid_parent_error};
    }
    const auto parent_depth =
        validate_depth(found->second, state, marks, depths);
    if (!parent_depth) {
      return std::unexpected{parent_depth.error()};
    }
    depth = static_cast<std::uint64_t>(*parent_depth) + 1;
  } else {
    const auto &layer = std::get<LayerId>(parent);
    if (!state.layer_index.contains(std::string{layer.value()})) {
      return std::unexpected{missing_identity_error};
    }
  }
  if (depth > state.limits.max_nesting_depth()) {
    return std::unexpected{resource_limit_error};
  }
  depths[index] = static_cast<std::uint32_t>(depth);
  marks[index] = 2;
  return depths[index];
}

} // namespace

auto scene_error_code_name(const SceneErrorCode code) noexcept
    -> std::string_view {
  switch (code) {
  case SceneErrorCode::missing_identity:
    return "missing_identity";
  case SceneErrorCode::duplicate_identity:
    return "duplicate_identity";
  case SceneErrorCode::invalid_parent:
    return "invalid_parent";
  case SceneErrorCode::parent_cycle:
    return "parent_cycle";
  case SceneErrorCode::invalid_geometry:
    return "invalid_geometry";
  case SceneErrorCode::unsupported_node_kind:
    return "unsupported_node_kind";
  case SceneErrorCode::unsupported_property:
    return "unsupported_property";
  case SceneErrorCode::resource_limit:
    return "resource_limit";
  case SceneErrorCode::number_out_of_range:
    return "number_out_of_range";
  case SceneErrorCode::arithmetic_overflow:
    return "arithmetic_overflow";
  case SceneErrorCode::allocation_failure:
    return "allocation_failure";
  }
  return "unknown";
}

auto Opacity::create(const double value) noexcept
    -> std::expected<Opacity, SceneError> {
  if (!std::isfinite(value) || value < 0.0 || value > 1.0) {
    return std::unexpected{invalid_geometry_error};
  }
  return Opacity{value == 0.0 ? 0.0 : value};
}

auto Stroke::create(const Color color, const double width,
                    const ResourceLimits &limits) noexcept
    -> std::expected<Stroke, SceneError> {
  if (!std::isfinite(width) || width <= 0.0) {
    return std::unexpected{invalid_geometry_error};
  }
  const auto checked = checked_coordinate(width, limits);
  if (!checked) {
    return std::unexpected{checked.error()};
  }
  return Stroke{color, *checked};
}

auto Rectangle::create(const double x, const double y, const double width,
                       const double height, const double radius_x,
                       const double radius_y,
                       const ResourceLimits &limits) noexcept
    -> std::expected<Rectangle, SceneError> {
  if (!std::isfinite(width) || !std::isfinite(height) ||
      !std::isfinite(radius_x) || !std::isfinite(radius_y) || width < 0.0 ||
      height < 0.0 || radius_x < 0.0 || radius_y < 0.0 ||
      radius_x > width / 2.0 || radius_y > height / 2.0) {
    return std::unexpected{invalid_geometry_error};
  }
  const auto origin = Point::create(x, y, limits);
  const auto checked_width = checked_coordinate(width, limits);
  const auto checked_height = checked_coordinate(height, limits);
  const auto checked_radius_x = checked_coordinate(radius_x, limits);
  const auto checked_radius_y = checked_coordinate(radius_y, limits);
  if (!origin || !checked_width || !checked_height || !checked_radius_x ||
      !checked_radius_y) {
    if (!origin)
      return std::unexpected{map_value_error(origin.error())};
    if (!checked_width)
      return std::unexpected{checked_width.error()};
    if (!checked_height)
      return std::unexpected{checked_height.error()};
    if (!checked_radius_x)
      return std::unexpected{checked_radius_x.error()};
    return std::unexpected{checked_radius_y.error()};
  }
  return Rectangle{*origin, *checked_width, *checked_height, *checked_radius_x,
                   *checked_radius_y};
}

auto Ellipse::create(const double center_x, const double center_y,
                     const double radius_x, const double radius_y,
                     const ResourceLimits &limits) noexcept
    -> std::expected<Ellipse, SceneError> {
  if (!std::isfinite(radius_x) || !std::isfinite(radius_y) || radius_x < 0.0 ||
      radius_y < 0.0) {
    return std::unexpected{invalid_geometry_error};
  }
  const auto center = Point::create(center_x, center_y, limits);
  const auto checked_radius_x = checked_coordinate(radius_x, limits);
  const auto checked_radius_y = checked_coordinate(radius_y, limits);
  if (!center)
    return std::unexpected{map_value_error(center.error())};
  if (!checked_radius_x)
    return std::unexpected{checked_radius_x.error()};
  if (!checked_radius_y)
    return std::unexpected{checked_radius_y.error()};
  return Ellipse{*center, *checked_radius_x, *checked_radius_y};
}

auto Path::create(std::vector<PathCommand> commands) noexcept
    -> std::expected<Path, SceneError> {
  if (commands.empty()) {
    return std::unexpected{invalid_geometry_error};
  }
  bool open = false;
  bool has_segment = false;
  for (const auto &command : commands) {
    if (std::holds_alternative<MoveTo>(command)) {
      if (open && !has_segment) {
        return std::unexpected{invalid_geometry_error};
      }
      open = true;
      has_segment = false;
    } else if (std::holds_alternative<LineTo>(command)) {
      if (!open) {
        return std::unexpected{invalid_geometry_error};
      }
      has_segment = true;
    } else {
      if (!open || !has_segment) {
        return std::unexpected{invalid_geometry_error};
      }
      open = false;
      has_segment = false;
    }
  }
  if (open && !has_segment) {
    return std::unexpected{invalid_geometry_error};
  }
  try {
    return Path{std::move(commands)};
  } catch (...) {
    return std::unexpected{allocation_failure_error};
  }
}

auto Drawable::kind() const noexcept -> ObjectKind {
  if (std::holds_alternative<Rectangle>(m_geometry))
    return ObjectKind::rectangle;
  if (std::holds_alternative<Ellipse>(m_geometry))
    return ObjectKind::ellipse;
  return ObjectKind::path;
}

auto OpacityTrack::create(TrackId id, ObjectId target,
                          const std::uint64_t start_time_us,
                          const std::uint64_t duration_us, const Opacity from,
                          const Opacity to) noexcept
    -> std::expected<OpacityTrack, SceneError> {
  if (duration_us == 0) {
    return std::unexpected{invalid_geometry_error};
  }
  if (start_time_us > std::numeric_limits<std::uint64_t>::max() - duration_us)
    return std::unexpected{arithmetic_overflow_error};
  return OpacityTrack{
      std::move(id), std::move(target), start_time_us, duration_us, from, to};
}

auto OpacityTrack::evaluate(const std::uint64_t time_us,
                            const Opacity authored) const noexcept -> Opacity {
  if (time_us < m_start_time_us) {
    return authored;
  }
  const auto elapsed = time_us - m_start_time_us;
  if (elapsed >= m_duration_us) {
    return m_to;
  }
  const auto ratio =
      static_cast<double>(elapsed) / static_cast<double>(m_duration_us);
  return *Opacity::create(m_from.value() +
                          (m_to.value() - m_from.value()) * ratio);
}

namespace detail {

auto object_id(const SceneObject &object) noexcept -> const ObjectId & {
  return std::visit(
      [](const auto &value) -> const ObjectId & { return value.id(); }, object);
}

auto object_parent(const SceneObject &object) noexcept -> const ParentRef & {
  return std::visit(
      [](const auto &value) -> const ParentRef & { return value.parent(); },
      object);
}

auto object_visible(const SceneObject &object) noexcept -> bool {
  return std::visit([](const auto &value) { return value.visible(); }, object);
}

auto object_transform(const SceneObject &object) noexcept -> AffineTransform {
  return std::visit([](const auto &value) { return value.transform(); },
                    object);
}

auto object_kind(const SceneObject &object) noexcept -> ObjectKind {
  return std::visit(
      [](const auto &value) {
        using T = std::remove_cvref_t<decltype(value)>;
        if constexpr (std::is_same_v<T, Group>)
          return ObjectKind::group;
        else
          return value.kind();
      },
      object);
}

auto DocumentBuilder::build(DocumentId id, const Revision revision,
                            CanvasExtent canvas,
                            std::optional<Color> background,
                            ResourceLimits limits, std::vector<Layer> layers,
                            std::vector<SceneObject> objects,
                            std::vector<OpacityTrack> tracks) noexcept
    -> std::expected<Document, SceneError> {
  try {
    const auto checked_values =
        validate_document_values(id, canvas, limits, layers, objects, tracks);
    if (!checked_values)
      return std::unexpected{checked_values.error()};
    const auto layer_count = static_cast<std::uint64_t>(layers.size());
    const auto object_count = static_cast<std::uint64_t>(objects.size());
    if (layer_count > limits.max_scene_nodes() ||
        object_count > limits.max_scene_nodes() - layer_count) {
      return std::unexpected{resource_limit_error};
    }
    auto impl = std::make_shared<Document::Impl>(std::move(id), revision,
                                                 canvas, background, limits);
    impl->layers.reserve(layers.size());
    impl->objects.reserve(objects.size());
    impl->tracks = std::move(tracks);

    for (auto &layer : layers) {
      const auto key = std::string{layer.id().value()};
      if (impl->layer_index.contains(key)) {
        return std::unexpected{duplicate_identity_error};
      }
      impl->layer_index.emplace(key, impl->layers.size());
      impl->layers.push_back(LayerRecord{std::move(layer), {}});
    }
    for (auto &object : objects) {
      const auto key = std::string{object_id(object).value()};
      if (impl->object_index.contains(key)) {
        return std::unexpected{duplicate_identity_error};
      }
      impl->object_index.emplace(key, impl->objects.size());
      impl->objects.push_back(ObjectRecord{std::move(object), {}, 0});
    }

    std::vector<std::uint8_t> marks(impl->objects.size());
    std::vector<std::uint32_t> depths(impl->objects.size());
    for (std::size_t index = 0; index < impl->objects.size(); ++index) {
      const auto depth = validate_depth(index, *impl, marks, depths);
      if (!depth) {
        return std::unexpected{depth.error()};
      }
    }

    for (std::size_t index = 0; index < impl->objects.size(); ++index) {
      auto &record = impl->objects[index];
      const auto &parent = object_parent(record.object);
      if (const auto *layer_id = std::get_if<LayerId>(&parent)) {
        auto &children =
            impl->layers
                .at(impl->layer_index.at(std::string{layer_id->value()}))
                .children;
        record.sibling_index = children.size();
        children.push_back(index);
      } else {
        const auto &parent_id = std::get<ObjectId>(parent);
        auto &children =
            impl->objects
                .at(impl->object_index.at(std::string{parent_id.value()}))
                .children;
        record.sibling_index = children.size();
        children.push_back(index);
      }
    }

    for (std::size_t index = 0; index < impl->tracks.size(); ++index) {
      const auto &track = impl->tracks[index];
      const auto track_key = std::string{track.id().value()};
      if (impl->track_index.contains(track_key)) {
        return std::unexpected{duplicate_identity_error};
      }
      const auto object_key = std::string{track.target().value()};
      const auto target = impl->object_index.find(object_key);
      if (target == impl->object_index.end()) {
        return std::unexpected{missing_identity_error};
      }
      if (!std::holds_alternative<Drawable>(
              impl->objects[target->second].object)) {
        return std::unexpected{unsupported_property_error};
      }
      if (impl->track_by_object.contains(object_key)) {
        return std::unexpected{unsupported_property_error};
      }
      impl->track_index.emplace(track_key, index);
      impl->track_by_object.emplace(object_key, index);
    }
    return Document{std::move(impl)};
  } catch (...) {
    return std::unexpected{allocation_failure_error};
  }
}

} // namespace detail

auto Document::create(const DocumentId &id, CanvasExtent canvas,
                      std::optional<Color> background,
                      ResourceLimits limits) noexcept
    -> std::expected<Document, SceneError> {
  auto checked_id = DocumentId::create(id.value(), limits);
  if (!checked_id)
    return std::unexpected{map_value_error(checked_id.error())};
  auto owned_id = std::move(checked_id).value();
  return detail::DocumentBuilder::build(std::move(owned_id), Revision{}, canvas,
                                        background, limits, {}, {}, {});
}

Document::~Document() = default;

} // namespace drawforge
