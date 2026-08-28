#include <drawforge/transaction.hpp>

#include "scene_internal.hpp"
#include "transaction_internal.hpp"

#include <algorithm>
#include <deque>
#include <limits>
#include <map>
#include <set>
#include <string>
#include <type_traits>
#include <utility>

namespace drawforge {

struct TransactionReceipt::Data {
  Data(DocumentId receipt_document_id, TransactionId receipt_transaction_id,
       Revision receipt_base_revision, Revision receipt_result_revision,
       std::vector<IdentityRef> receipt_created,
       std::vector<IdentityRef> receipt_changed,
       std::optional<Bounds> receipt_dirty_bounds,
       std::vector<TransactionWarning> receipt_warnings) noexcept
      : document_id{std::move(receipt_document_id)},
        transaction_id{std::move(receipt_transaction_id)},
        base_revision{receipt_base_revision},
        result_revision{receipt_result_revision},
        created{std::move(receipt_created)},
        changed{std::move(receipt_changed)}, dirty_bounds{receipt_dirty_bounds},
        warnings{std::move(receipt_warnings)} {}

  DocumentId document_id;
  TransactionId transaction_id;
  Revision base_revision{};
  Revision result_revision{};
  std::vector<IdentityRef> created;
  std::vector<IdentityRef> changed;
  std::optional<Bounds> dirty_bounds;
  std::vector<TransactionWarning> warnings;

  auto operator==(const Data &) const -> bool = default;
};

struct detail::TransactionReceiptAccess {
  [[nodiscard]] static auto
  make(DocumentId document_id, TransactionId transaction_id,
       Revision base_revision, Revision result_revision,
       std::vector<IdentityRef> created, std::vector<IdentityRef> changed,
       std::optional<Bounds> dirty_bounds,
       std::vector<TransactionWarning> warnings) -> TransactionReceipt {
    return TransactionReceipt{std::make_shared<TransactionReceipt::Data>(
        std::move(document_id), std::move(transaction_id), base_revision,
        result_revision, std::move(created), std::move(changed), dirty_bounds,
        std::move(warnings))};
  }
};

namespace {

thread_local detail::TransactionFaultPoint transaction_fault{
    detail::TransactionFaultPoint::none};

[[nodiscard]] auto consume_fault(const detail::TransactionFaultPoint expected)
    -> bool {
  if (transaction_fault != expected)
    return false;
  transaction_fault = detail::TransactionFaultPoint::none;
  return true;
}

constexpr ValueError invalid_storage_limit_error{
    ValueErrorCode::invalid_limit,
    "transaction storage limit exceeds the hard ceiling"};

constexpr std::string_view wrong_document_message{
    "transaction targets a different document"};
constexpr std::string_view conflict_message{
    "transaction ID was committed with different content"};
constexpr std::string_view stale_revision_message{
    "expected revision does not match committed revision"};
constexpr std::string_view empty_transaction_message{
    "operation batch is empty"};
constexpr std::string_view nothing_to_undo_message{"nothing to undo"};
constexpr std::string_view nothing_to_redo_message{"nothing to redo"};
constexpr std::string_view cancelled_message{
    "transaction was cancelled before commit"};
constexpr std::string_view resource_limit_message{"resource limit exceeded"};
constexpr std::string_view replay_limit_message{
    "replay registry cannot retain another transaction"};
constexpr std::string_view history_limit_message{
    "transaction history entry exceeds the configured limit"};
constexpr std::string_view revision_overflow_message{
    "document revision cannot advance"};
constexpr std::string_view allocation_failure_message{
    "transaction candidate allocation failed"};
constexpr std::string_view no_effect_message{
    "operation leaves the accepted value unchanged"};
constexpr std::string_view missing_identity_message{"identity does not exist"};
constexpr std::string_view duplicate_identity_message{"identity is duplicated"};
constexpr std::string_view invalid_parent_message{
    "parent is not an accepted container"};
constexpr std::string_view parent_cycle_message{
    "object parentage contains a cycle"};
constexpr std::string_view invalid_geometry_message{"geometry is invalid"};
constexpr std::string_view unsupported_node_message{
    "node kind is not supported"};
constexpr std::string_view unsupported_property_message{
    "property is not supported here"};
constexpr std::string_view number_out_of_range_message{
    "number is outside document limits"};
constexpr std::string_view arithmetic_overflow_message{
    "checked arithmetic overflowed"};
constexpr std::string_view invalid_index_message{
    "sibling index is outside the accepted range"};

[[nodiscard]] auto make_error(TransactionErrorCode code, RetryAdvice advice,
                              std::string_view message, FieldPath path = {},
                              std::optional<std::uint64_t> operation = {})
    -> TransactionError {
  return TransactionError{code, advice, operation, std::move(path), message};
}

[[nodiscard]] auto allocation_error() -> TransactionError {
  return make_error(TransactionErrorCode::allocation_failure,
                    RetryAdvice::same_request, allocation_failure_message);
}

[[nodiscard]] auto cancelled_error() -> TransactionError {
  return make_error(TransactionErrorCode::cancelled, RetryAdvice::same_request,
                    cancelled_message);
}

[[nodiscard]] auto operation_path(const std::uint64_t index,
                                  const TransactionField field) -> FieldPath {
  return FieldPath{TransactionField::body, TransactionField::operations,
                   FieldIndex{index}, field};
}

[[nodiscard]] auto map_scene_code(const SceneErrorCode code)
    -> TransactionErrorCode {
  switch (code) {
  case SceneErrorCode::missing_identity:
    return TransactionErrorCode::missing_identity;
  case SceneErrorCode::duplicate_identity:
    return TransactionErrorCode::duplicate_identity;
  case SceneErrorCode::invalid_parent:
    return TransactionErrorCode::invalid_parent;
  case SceneErrorCode::parent_cycle:
    return TransactionErrorCode::parent_cycle;
  case SceneErrorCode::invalid_geometry:
    return TransactionErrorCode::invalid_geometry;
  case SceneErrorCode::unsupported_node_kind:
    return TransactionErrorCode::unsupported_node_kind;
  case SceneErrorCode::unsupported_property:
    return TransactionErrorCode::unsupported_property;
  case SceneErrorCode::resource_limit:
    return TransactionErrorCode::resource_limit;
  case SceneErrorCode::number_out_of_range:
    return TransactionErrorCode::number_out_of_range;
  case SceneErrorCode::arithmetic_overflow:
    return TransactionErrorCode::arithmetic_overflow;
  case SceneErrorCode::allocation_failure:
    return TransactionErrorCode::allocation_failure;
  }
  return TransactionErrorCode::allocation_failure;
}

[[nodiscard]] auto message_for_scene_code(const SceneErrorCode code)
    -> std::string_view {
  switch (code) {
  case SceneErrorCode::missing_identity:
    return missing_identity_message;
  case SceneErrorCode::duplicate_identity:
    return duplicate_identity_message;
  case SceneErrorCode::invalid_parent:
    return invalid_parent_message;
  case SceneErrorCode::parent_cycle:
    return parent_cycle_message;
  case SceneErrorCode::invalid_geometry:
    return invalid_geometry_message;
  case SceneErrorCode::unsupported_node_kind:
    return unsupported_node_message;
  case SceneErrorCode::unsupported_property:
    return unsupported_property_message;
  case SceneErrorCode::resource_limit:
    return resource_limit_message;
  case SceneErrorCode::number_out_of_range:
    return number_out_of_range_message;
  case SceneErrorCode::arithmetic_overflow:
    return arithmetic_overflow_message;
  case SceneErrorCode::allocation_failure:
    return allocation_failure_message;
  }
  return allocation_failure_message;
}

[[nodiscard]] auto operation_error(const SceneError &error,
                                   const std::uint64_t operation,
                                   const TransactionField field)
    -> TransactionError {
  const auto code = map_scene_code(error.code);
  if (code == TransactionErrorCode::allocation_failure)
    return allocation_error();
  return make_error(code, RetryAdvice::change_request,
                    message_for_scene_code(error.code),
                    operation_path(operation, field), operation);
}

[[nodiscard]] auto index_error(const std::uint64_t operation)
    -> TransactionError {
  return make_error(TransactionErrorCode::number_out_of_range,
                    RetryAdvice::change_request, invalid_index_message,
                    operation_path(operation, TransactionField::index),
                    operation);
}

template <typename Id> [[nodiscard]] auto id_key(const Id &id) -> std::string {
  return std::string{id.value()};
}

[[nodiscard]] auto contains_identity(const std::vector<IdentityRef> &values,
                                     const IdentityRef &identity) -> bool {
  return std::ranges::find(values, identity) != values.end();
}

[[nodiscard]] auto contains_target(const std::vector<BoundsTarget> &values,
                                   const BoundsTarget &target) -> bool {
  return std::ranges::find(values, target) != values.end();
}

struct SceneValues {
  explicit SceneValues(const detail::DocumentState &state)
      : id{state.id}, revision{state.revision}, canvas{state.canvas},
        background{state.background}, limits{state.limits} {
    layers.reserve(state.layers.size());
    objects.reserve(state.objects.size());
    tracks = state.tracks;
    for (const auto &record : state.layers)
      layers.push_back(record.layer);
    for (const auto &record : state.objects)
      objects.push_back(record.object);
  }

  DocumentId id;
  Revision revision{};
  CanvasExtent canvas;
  std::optional<Color> background;
  ResourceLimits limits;
  std::vector<Layer> layers;
  std::vector<SceneObject> objects;
  std::vector<OpacityTrack> tracks;
};

[[nodiscard]] auto build_document(SceneValues values, const Revision revision)
    -> std::expected<Document, SceneError> {
  return detail::DocumentBuilder::build(
      std::move(values.id), revision, values.canvas, values.background,
      values.limits, std::move(values.layers), std::move(values.objects),
      std::move(values.tracks));
}

[[nodiscard]] auto clone_with_revision(const Document &document,
                                       const Revision revision)
    -> std::expected<Document, SceneError> {
  return build_document(SceneValues{DocumentAccess::state(document)}, revision);
}

[[nodiscard]] auto document_revision(const Document &document) -> Revision {
  return DocumentAccess::state(document).revision;
}

[[nodiscard]] auto document_id(const Document &document) -> const DocumentId & {
  return DocumentAccess::state(document).id;
}

[[nodiscard]] auto find_object(const detail::DocumentState &state,
                               const ObjectId &id)
    -> const detail::ObjectRecord * {
  const auto found = state.object_index.find(id_key(id));
  return found == state.object_index.end() ? nullptr
                                           : &state.objects[found->second];
}

[[nodiscard]] auto find_layer(const detail::DocumentState &state,
                              const LayerId &id)
    -> const detail::LayerRecord * {
  const auto found = state.layer_index.find(id_key(id));
  return found == state.layer_index.end() ? nullptr
                                          : &state.layers[found->second];
}

[[nodiscard]] auto find_track(const detail::DocumentState &state,
                              const TrackId &id) -> const OpacityTrack * {
  const auto found = state.track_index.find(id_key(id));
  return found == state.track_index.end() ? nullptr
                                          : &state.tracks[found->second];
}

[[nodiscard]] auto validate_track_target(
    const detail::DocumentState &state, const OpacityTrack &track,
    const std::optional<TrackId> replaced_track, const std::uint64_t operation)
    -> std::optional<TransactionError> {
  const auto *target = find_object(state, track.target());
  if (target == nullptr) {
    return make_error(
        TransactionErrorCode::missing_identity, RetryAdvice::change_request,
        missing_identity_message,
        operation_path(operation, TransactionField::target_object_id),
        operation);
  }
  if (!std::holds_alternative<Drawable>(target->object)) {
    return make_error(
        TransactionErrorCode::unsupported_property, RetryAdvice::change_request,
        unsupported_property_message,
        operation_path(operation, TransactionField::target_object_id),
        operation);
  }
  const auto occupied = state.track_by_object.find(id_key(track.target()));
  if (occupied == state.track_by_object.end())
    return std::nullopt;
  const auto &existing = state.tracks[occupied->second].id();
  if (replaced_track && existing == *replaced_track)
    return std::nullopt;
  return make_error(
      TransactionErrorCode::unsupported_property, RetryAdvice::change_request,
      unsupported_property_message,
      operation_path(operation, TransactionField::target_object_id), operation);
}

[[nodiscard]] auto object_with_parent(const SceneObject &object,
                                      ParentRef parent) -> SceneObject {
  return std::visit(
      [&](const auto &value) -> SceneObject {
        using T = std::remove_cvref_t<decltype(value)>;
        if constexpr (std::is_same_v<T, Group>) {
          return Group{value.id(), std::move(parent), value.visible(),
                       value.transform()};
        } else {
          return Drawable{value.id(),        std::move(parent), value.visible(),
                          value.transform(), value.geometry(),  value.style(),
                          value.opacity()};
        }
      },
      object);
}

[[nodiscard]] auto object_with_visibility(const SceneObject &object,
                                          const bool visible) -> SceneObject {
  return std::visit(
      [&](const auto &value) -> SceneObject {
        using T = std::remove_cvref_t<decltype(value)>;
        if constexpr (std::is_same_v<T, Group>) {
          return Group{value.id(), value.parent(), visible, value.transform()};
        } else {
          return Drawable{value.id(),        value.parent(),   visible,
                          value.transform(), value.geometry(), value.style(),
                          value.opacity()};
        }
      },
      object);
}

[[nodiscard]] auto object_with_transform(const SceneObject &object,
                                         const AffineTransform transform)
    -> SceneObject {
  return std::visit(
      [&](const auto &value) -> SceneObject {
        using T = std::remove_cvref_t<decltype(value)>;
        if constexpr (std::is_same_v<T, Group>) {
          return Group{value.id(), value.parent(), value.visible(), transform};
        } else {
          return Drawable{value.id(),     value.parent(),   value.visible(),
                          transform,      value.geometry(), value.style(),
                          value.opacity()};
        }
      },
      object);
}

[[nodiscard]] auto drawable_with_geometry(const Drawable &value,
                                          Geometry geometry) -> SceneObject {
  return Drawable{value.id(),        value.parent(),      value.visible(),
                  value.transform(), std::move(geometry), value.style(),
                  value.opacity()};
}

[[nodiscard]] auto drawable_with_style(const Drawable &value, Style style)
    -> SceneObject {
  return Drawable{value.id(),        value.parent(),   value.visible(),
                  value.transform(), value.geometry(), std::move(style),
                  value.opacity()};
}

[[nodiscard]] auto drawable_with_opacity(const Drawable &value,
                                         const Opacity opacity) -> SceneObject {
  return Drawable{
      value.id(),       value.parent(), value.visible(), value.transform(),
      value.geometry(), value.style(),  opacity};
}

[[nodiscard]] auto object_position(const std::vector<SceneObject> &objects,
                                   const ObjectId &id) -> std::size_t {
  const auto found =
      std::ranges::find_if(objects, [&](const SceneObject &object) {
        return detail::object_id(object) == id;
      });
  return static_cast<std::size_t>(std::distance(objects.begin(), found));
}

[[nodiscard]] auto children_for_parent(const detail::DocumentState &state,
                                       const ParentRef &parent,
                                       const std::uint64_t operation)
    -> std::expected<std::vector<ObjectId>, TransactionError> {
  const std::vector<std::size_t> *children{};
  if (const auto *layer_id = std::get_if<LayerId>(&parent)) {
    const auto *layer = find_layer(state, *layer_id);
    if (layer == nullptr) {
      return std::unexpected{make_error(
          TransactionErrorCode::missing_identity, RetryAdvice::change_request,
          missing_identity_message,
          operation_path(operation, TransactionField::parent), operation)};
    }
    children = &layer->children;
  } else {
    const auto *object = find_object(state, std::get<ObjectId>(parent));
    if (object == nullptr) {
      return std::unexpected{make_error(
          TransactionErrorCode::missing_identity, RetryAdvice::change_request,
          missing_identity_message,
          operation_path(operation, TransactionField::parent), operation)};
    }
    if (!std::holds_alternative<Group>(object->object)) {
      return std::unexpected{make_error(
          TransactionErrorCode::invalid_parent, RetryAdvice::change_request,
          invalid_parent_message,
          operation_path(operation, TransactionField::parent), operation)};
    }
    children = &object->children;
  }

  std::vector<ObjectId> result;
  result.reserve(children->size());
  for (const auto child : *children)
    result.push_back(detail::object_id(state.objects[child].object));
  return result;
}

auto insert_object_at(std::vector<SceneObject> &objects, SceneObject object,
                      const std::vector<ObjectId> &siblings,
                      const std::uint64_t index) -> void {
  auto insertion = objects.end();
  if (index < siblings.size()) {
    const auto position = object_position(objects, siblings[index]);
    insertion = objects.begin() + static_cast<std::ptrdiff_t>(position);
  } else if (!siblings.empty()) {
    const auto position = object_position(objects, siblings.back());
    insertion = objects.begin() + static_cast<std::ptrdiff_t>(position + 1);
  }
  objects.insert(insertion, std::move(object));
}

[[nodiscard]] auto logical_size(const Color &) -> std::uint64_t { return 4; }
[[nodiscard]] auto logical_size(const Opacity &) -> std::uint64_t { return 8; }
[[nodiscard]] auto logical_size(const AffineTransform &) -> std::uint64_t {
  return 48;
}
[[nodiscard]] auto logical_size(const Stroke &stroke) -> std::uint64_t {
  return logical_size(stroke.color()) + 8;
}
[[nodiscard]] auto logical_size(const Style &style) -> std::uint64_t {
  return 2 + (style.fill() ? logical_size(*style.fill()) : 0) +
         (style.stroke() ? logical_size(*style.stroke()) : 0);
}
[[nodiscard]] auto logical_size(const Rectangle &) -> std::uint64_t {
  return 48;
}
[[nodiscard]] auto logical_size(const Ellipse &) -> std::uint64_t { return 32; }
[[nodiscard]] auto logical_size(const Path &path) -> std::uint64_t {
  std::uint64_t result = 8;
  for (const auto &command : path.commands())
    result += std::holds_alternative<ClosePath>(command) ? 1 : 17;
  return result;
}
[[nodiscard]] auto logical_size(const Geometry &geometry) -> std::uint64_t {
  return 1 + std::visit([](const auto &value) { return logical_size(value); },
                        geometry);
}
[[nodiscard]] auto logical_size(const ParentRef &parent) -> std::uint64_t {
  return 1 + std::visit(
                 [](const auto &id) {
                   return static_cast<std::uint64_t>(id.value().size());
                 },
                 parent);
}
[[nodiscard]] auto logical_size(const SceneObject &object) -> std::uint64_t {
  return std::visit(
      [](const auto &value) -> std::uint64_t {
        using T = std::remove_cvref_t<decltype(value)>;
        auto result = static_cast<std::uint64_t>(value.id().value().size()) +
                      logical_size(value.parent()) + 1 +
                      logical_size(value.transform());
        if constexpr (std::is_same_v<T, Drawable>) {
          result += logical_size(value.geometry()) +
                    logical_size(value.style()) + logical_size(value.opacity());
        }
        return result;
      },
      object);
}
[[nodiscard]] auto logical_size(const OpacityTrack &track) -> std::uint64_t {
  return static_cast<std::uint64_t>(track.id().value().size() +
                                    track.target().value().size()) +
         16 + logical_size(track.from()) + logical_size(track.to());
}

[[nodiscard]] auto logical_size(const Document &document) -> std::uint64_t {
  const auto &state = DocumentAccess::state(document);
  std::uint64_t result =
      static_cast<std::uint64_t>(state.id.value().size()) + 8 + 16 + 72 + 8;
  if (state.background)
    result += logical_size(*state.background);
  for (const auto &layer : state.layers)
    result += static_cast<std::uint64_t>(layer.layer.id().value().size()) + 9;
  for (const auto &object : state.objects)
    result += logical_size(object.object) + 8;
  for (const auto &track : state.tracks)
    result += logical_size(track);
  return result;
}

[[nodiscard]] auto logical_size(const IdentityRef &identity) -> std::uint64_t {
  return 1 + std::visit(
                 [](const auto &id) {
                   return static_cast<std::uint64_t>(id.value().size());
                 },
                 identity);
}

[[nodiscard]] auto logical_size(const Operation &operation) -> std::uint64_t {
  return 1 +
         std::visit(
             [](const auto &value) -> std::uint64_t {
               using T = std::remove_cvref_t<decltype(value)>;
               if constexpr (std::is_same_v<T, CreateLayer>) {
                 return value.layer_id.value().size() + 9;
               } else if constexpr (std::is_same_v<T, CreateGroup>) {
                 return value.object_id.value().size() +
                        logical_size(value.parent) + 9 +
                        logical_size(value.transform);
               } else if constexpr (std::is_same_v<T, CreateRectangle> ||
                                    std::is_same_v<T, CreateEllipse> ||
                                    std::is_same_v<T, CreatePath>) {
                 return value.object_id.value().size() +
                        logical_size(value.parent) + 9 +
                        logical_size(value.transform) +
                        logical_size(value.style) +
                        logical_size(value.opacity) +
                        logical_size(Geometry{value.geometry});
               } else if constexpr (std::is_same_v<T, CreateOpacityTrack> ||
                                    std::is_same_v<T, SetOpacityTrack>) {
                 return logical_size(value.track);
               } else if constexpr (std::is_same_v<T, SetCanvasBackground>) {
                 return 1 + (value.background ? logical_size(*value.background)
                                              : 0);
               } else if constexpr (std::is_same_v<T, SetVisibility>) {
                 return logical_size(IdentityRef{std::visit(
                            [](const auto &id) -> IdentityRef { return id; },
                            value.target)}) +
                        1;
               } else if constexpr (std::is_same_v<T, SetTransform>) {
                 return value.object_id.value().size() +
                        logical_size(value.transform);
               } else if constexpr (std::is_same_v<T, SetGeometry>) {
                 return value.object_id.value().size() +
                        logical_size(value.geometry);
               } else if constexpr (std::is_same_v<T, SetStyle>) {
                 return value.object_id.value().size() +
                        logical_size(value.style);
               } else if constexpr (std::is_same_v<T, SetOpacity>) {
                 return value.object_id.value().size() +
                        logical_size(value.opacity);
               } else if constexpr (std::is_same_v<T, ReparentObject>) {
                 return value.object_id.value().size() +
                        logical_size(value.parent) + 8;
               } else {
                 return value.object_id.value().size() + 8;
               }
             },
             operation);
}

[[nodiscard]] auto logical_size(const Transaction &transaction)
    -> std::uint64_t {
  auto result =
      static_cast<std::uint64_t>(transaction.document_id().value().size() +
                                 transaction.transaction_id().value().size()) +
      17;
  if (const auto *batch = std::get_if<OperationBatch>(&transaction.body())) {
    result += 8;
    for (const auto &operation : batch->operations)
      result += logical_size(operation);
  } else {
    result += 1;
  }
  return result;
}

[[nodiscard]] auto logical_size(const TransactionReceipt &receipt)
    -> std::uint64_t {
  auto result =
      static_cast<std::uint64_t>(receipt.document_id().value().size() +
                                 receipt.transaction_id().value().size()) +
      24;
  for (const auto &identity : receipt.created())
    result += logical_size(identity);
  for (const auto &identity : receipt.changed())
    result += logical_size(identity);
  result += receipt.dirty_bounds() ? 32 : 0;
  for (const auto &warning : receipt.warnings()) {
    result += 18 + static_cast<std::uint64_t>(warning.message.size()) +
              warning.field_path.segments().size() * 9;
  }
  return result;
}

struct ReplayEntry {
  Transaction transaction;
  TransactionReceipt receipt;
  std::uint64_t bytes{};
};

struct HistoryEntry {
  Document before;
  Document after;
  TransactionId source_transaction_id;
};

struct Aggregate {
  explicit Aggregate(Document initial) : current{std::move(initial)} {}

  Document current;
  std::map<std::string, ReplayEntry, std::less<>> replay;
  std::deque<HistoryEntry> undo;
  std::deque<HistoryEntry> redo;
  std::uint64_t replay_bytes{};
};

[[nodiscard]] auto history_bytes(const std::deque<HistoryEntry> &undo,
                                 const std::deque<HistoryEntry> &redo)
    -> std::uint64_t {
  std::map<const detail::DocumentState *, const Document *> unique;
  for (const auto &entry : undo) {
    unique.emplace(&DocumentAccess::state(entry.before), &entry.before);
    unique.emplace(&DocumentAccess::state(entry.after), &entry.after);
  }
  for (const auto &entry : redo) {
    unique.emplace(&DocumentAccess::state(entry.before), &entry.before);
    unique.emplace(&DocumentAccess::state(entry.after), &entry.after);
  }
  std::uint64_t result{};
  for (const auto &[unused, document] : unique) {
    static_cast<void>(unused);
    const auto bytes = logical_size(*document);
    if (result > std::numeric_limits<std::uint64_t>::max() - bytes)
      return std::numeric_limits<std::uint64_t>::max();
    result += bytes;
  }
  return result;
}

struct OperationEffect {
  OperationEffect(IdentityRef affected_identity, bool identity_created,
                  bool identity_changed,
                  TransactionField affected_field) noexcept
      : identity{std::move(affected_identity)}, created{identity_created},
        changed{identity_changed}, value_field{affected_field} {}

  IdentityRef identity;
  bool created{};
  bool changed{};
  TransactionField value_field{};
};

struct OperationOutcome {
  Document document;
  OperationEffect effect;
};

} // namespace

namespace {

[[nodiscard]] auto apply_operation(const Document &document,
                                   const Operation &operation,
                                   std::uint64_t operation_index)
    -> std::expected<OperationOutcome, TransactionError>;
[[nodiscard]] auto
compute_dirty_bounds(const Document &before, const Document &after,
                     const std::vector<BoundsTarget> &targets,
                     bool dirty_canvas)
    -> std::expected<std::optional<Bounds>, SceneError>;

[[nodiscard]] auto identity_less(const IdentityRef &left,
                                 const IdentityRef &right) -> bool {
  if (left.index() != right.index())
    return left.index() < right.index();
  return std::visit(
      [](const auto &left_id, const auto &right_id) -> bool {
        using Left = std::remove_cvref_t<decltype(left_id)>;
        using Right = std::remove_cvref_t<decltype(right_id)>;
        if constexpr (std::is_same_v<Left, Right>)
          return left_id.value() < right_id.value();
        return false;
      },
      left, right);
}

[[nodiscard]] auto child_ids(const detail::DocumentState &state,
                             const std::vector<std::size_t> &children)
    -> std::vector<ObjectId> {
  std::vector<ObjectId> result;
  result.reserve(children.size());
  for (const auto child : children)
    result.push_back(detail::object_id(state.objects[child].object));
  return result;
}

struct DocumentDifference {
  std::vector<IdentityRef> changed;
  std::vector<BoundsTarget> dirty_targets;
  bool dirty_canvas{};
};

[[nodiscard]] auto document_difference(const Document &before,
                                       const Document &after)
    -> DocumentDifference {
  const auto &old_state = DocumentAccess::state(before);
  const auto &new_state = DocumentAccess::state(after);
  DocumentDifference result;

  if (old_state.background != new_state.background) {
    result.changed.push_back(IdentityRef{new_state.id});
    result.dirty_canvas = true;
  }

  std::map<std::string_view, std::size_t> old_layer_positions;
  std::map<std::string_view, std::size_t> new_layer_positions;
  for (std::size_t index = 0; index < old_state.layers.size(); ++index)
    old_layer_positions.emplace(old_state.layers[index].layer.id().value(),
                                index);
  for (std::size_t index = 0; index < new_state.layers.size(); ++index)
    new_layer_positions.emplace(new_state.layers[index].layer.id().value(),
                                index);
  std::set<std::string_view> layer_ids;
  for (const auto &[id, unused] : old_layer_positions) {
    static_cast<void>(unused);
    layer_ids.insert(id);
  }
  for (const auto &[id, unused] : new_layer_positions) {
    static_cast<void>(unused);
    layer_ids.insert(id);
  }
  for (const auto id : layer_ids) {
    const auto old_position = old_layer_positions.find(id);
    const auto new_position = new_layer_positions.find(id);
    bool changed = old_position == old_layer_positions.end() ||
                   new_position == new_layer_positions.end();
    const LayerId *identity{};
    if (old_position != old_layer_positions.end())
      identity = &old_state.layers[old_position->second].layer.id();
    else
      identity = &new_state.layers[new_position->second].layer.id();
    if (!changed) {
      const auto &old_record = old_state.layers[old_position->second];
      const auto &new_record = new_state.layers[new_position->second];
      changed = old_position->second != new_position->second ||
                old_record.layer != new_record.layer ||
                child_ids(old_state, old_record.children) !=
                    child_ids(new_state, new_record.children);
    }
    if (changed) {
      result.changed.push_back(IdentityRef{*identity});
      result.dirty_targets.push_back(BoundsTarget{*identity});
    }
  }

  std::map<std::string_view, std::size_t> old_object_positions;
  std::map<std::string_view, std::size_t> new_object_positions;
  for (std::size_t index = 0; index < old_state.objects.size(); ++index)
    old_object_positions.emplace(
        detail::object_id(old_state.objects[index].object).value(), index);
  for (std::size_t index = 0; index < new_state.objects.size(); ++index)
    new_object_positions.emplace(
        detail::object_id(new_state.objects[index].object).value(), index);
  std::set<std::string_view> object_ids;
  for (const auto &[id, unused] : old_object_positions) {
    static_cast<void>(unused);
    object_ids.insert(id);
  }
  for (const auto &[id, unused] : new_object_positions) {
    static_cast<void>(unused);
    object_ids.insert(id);
  }
  for (const auto id : object_ids) {
    const auto old_position = old_object_positions.find(id);
    const auto new_position = new_object_positions.find(id);
    bool changed = old_position == old_object_positions.end() ||
                   new_position == new_object_positions.end();
    const ObjectId *identity{};
    if (old_position != old_object_positions.end())
      identity =
          &detail::object_id(old_state.objects[old_position->second].object);
    else
      identity =
          &detail::object_id(new_state.objects[new_position->second].object);
    if (!changed) {
      const auto &old_record = old_state.objects[old_position->second];
      const auto &new_record = new_state.objects[new_position->second];
      changed = old_record.object != new_record.object ||
                old_record.sibling_index != new_record.sibling_index ||
                child_ids(old_state, old_record.children) !=
                    child_ids(new_state, new_record.children);
    }
    if (changed) {
      result.changed.push_back(IdentityRef{*identity});
      result.dirty_targets.push_back(BoundsTarget{*identity});
    }
  }

  std::map<std::string_view, const OpacityTrack *> old_tracks;
  std::map<std::string_view, const OpacityTrack *> new_tracks;
  for (const auto &track : old_state.tracks)
    old_tracks.emplace(track.id().value(), &track);
  for (const auto &track : new_state.tracks)
    new_tracks.emplace(track.id().value(), &track);
  std::set<std::string_view> track_ids;
  for (const auto &[id, unused] : old_tracks) {
    static_cast<void>(unused);
    track_ids.insert(id);
  }
  for (const auto &[id, unused] : new_tracks) {
    static_cast<void>(unused);
    track_ids.insert(id);
  }
  for (const auto id : track_ids) {
    const auto old_track = old_tracks.find(id);
    const auto new_track = new_tracks.find(id);
    const auto changed = old_track == old_tracks.end() ||
                         new_track == new_tracks.end() ||
                         *old_track->second != *new_track->second;
    if (!changed)
      continue;
    const auto *identity = old_track != old_tracks.end()
                               ? &old_track->second->id()
                               : &new_track->second->id();
    result.changed.push_back(IdentityRef{*identity});
    if (old_track != old_tracks.end())
      result.dirty_targets.push_back(BoundsTarget{old_track->second->target()});
    if (new_track != new_tracks.end())
      result.dirty_targets.push_back(BoundsTarget{new_track->second->target()});
  }

  std::ranges::sort(result.changed, identity_less);
  result.changed.erase(std::ranges::unique(result.changed).begin(),
                       result.changed.end());
  std::vector<BoundsTarget> unique_targets;
  for (const auto &target : result.dirty_targets) {
    if (!contains_target(unique_targets, target))
      unique_targets.push_back(target);
  }
  result.dirty_targets = std::move(unique_targets);
  return result;
}

[[nodiscard]] auto create_receipt(
    const Transaction &transaction, const Revision base_revision,
    const Revision result_revision, std::vector<IdentityRef> created,
    std::vector<IdentityRef> changed, const Document &before,
    const Document &after, const std::vector<BoundsTarget> &dirty_targets,
    const bool dirty_canvas, std::vector<TransactionWarning> warnings)
    -> std::expected<TransactionReceipt, TransactionError> {
  if (consume_fault(detail::TransactionFaultPoint::receipt_allocation))
    return std::unexpected{allocation_error()};
  const auto dirty =
      compute_dirty_bounds(before, after, dirty_targets, dirty_canvas);
  if (!dirty) {
    const auto code = map_scene_code(dirty.error().code);
    return std::unexpected{
        make_error(code,
                   code == TransactionErrorCode::allocation_failure
                       ? RetryAdvice::same_request
                       : RetryAdvice::change_request,
                   message_for_scene_code(dirty.error().code))};
  }
  return detail::TransactionReceiptAccess::make(
      transaction.document_id(), transaction.transaction_id(), base_revision,
      result_revision, std::move(created), std::move(changed), *dirty,
      std::move(warnings));
}

[[nodiscard]] auto replay_capacity_error(const Aggregate &state,
                                         const TransactionStorageLimits &limits,
                                         const std::uint64_t record_bytes)
    -> std::optional<TransactionError> {
  if (state.replay.size() >= limits.max_replay_records() ||
      record_bytes > limits.max_replay_bytes() ||
      state.replay_bytes > limits.max_replay_bytes() - record_bytes) {
    return make_error(TransactionErrorCode::resource_limit,
                      RetryAdvice::change_request, replay_limit_message);
  }
  return std::nullopt;
}

[[nodiscard]] auto prepare_state(Aggregate candidate,
                                 const CancellationToken cancellation)
    -> std::expected<std::shared_ptr<const Aggregate>, TransactionError> {
  auto prepared = std::make_shared<Aggregate>(std::move(candidate));
  if (cancellation.stop_requested())
    return std::unexpected{cancelled_error()};
  return prepared;
}

[[nodiscard]] auto
apply_batch(const Aggregate &state,
            const TransactionStorageLimits &storage_limits,
            const Transaction &transaction, const OperationBatch &batch,
            const ApplyMode mode, const CancellationToken cancellation)
    -> std::expected<
        std::pair<TransactionResult, std::shared_ptr<const Aggregate>>,
        TransactionError> {
  const auto &limits = DocumentAccess::state(state.current).limits;
  if (batch.operations.empty()) {
    return std::unexpected{make_error(
        TransactionErrorCode::empty_transaction, RetryAdvice::change_request,
        empty_transaction_message,
        FieldPath{TransactionField::body, TransactionField::operations})};
  }
  if (batch.operations.size() > limits.max_transaction_operations()) {
    return std::unexpected{make_error(
        TransactionErrorCode::resource_limit, RetryAdvice::change_request,
        resource_limit_message,
        FieldPath{TransactionField::body, TransactionField::operations})};
  }
  if (consume_fault(detail::TransactionFaultPoint::stage_allocation))
    return std::unexpected{allocation_error()};

  auto staged = state.current;
  std::vector<IdentityRef> created;
  std::vector<IdentityRef> changed;
  std::vector<TransactionWarning> warnings;

  created.reserve(batch.operations.size());
  changed.reserve(batch.operations.size());
  warnings.reserve(batch.operations.size());

  for (std::size_t index = 0; index < batch.operations.size(); ++index) {
    if (cancellation.stop_requested())
      return std::unexpected{cancelled_error()};
    auto outcome = apply_operation(staged, batch.operations[index], index);
    if (!outcome)
      return std::unexpected{outcome.error()};
    const auto &effect = outcome->effect;
    if (effect.created && !contains_identity(created, effect.identity))
      created.push_back(effect.identity);
    if (effect.changed && !effect.created &&
        !contains_identity(created, effect.identity) &&
        !contains_identity(changed, effect.identity)) {
      changed.push_back(effect.identity);
    }
    if (!effect.changed) {
      warnings.push_back(TransactionWarning{
          TransactionWarningCode::no_effect, index,
          operation_path(index, effect.value_field), no_effect_message});
    }
    staged = std::move(outcome->document);
  }

  const auto next_revision = document_revision(state.current).next();
  if (!next_revision) {
    return std::unexpected{make_error(TransactionErrorCode::revision_overflow,
                                      RetryAdvice::not_retryable,
                                      revision_overflow_message)};
  }
  auto committed_document = clone_with_revision(staged, *next_revision);
  if (!committed_document)
    return std::unexpected{operation_error(committed_document.error(),
                                           batch.operations.size() - 1,
                                           TransactionField::body)};
  const auto final_difference =
      document_difference(state.current, *committed_document);
  std::erase_if(changed, [&](const IdentityRef &identity) {
    return !contains_identity(final_difference.changed, identity);
  });
  auto receipt = create_receipt(
      transaction, document_revision(state.current), *next_revision,
      std::move(created), std::move(changed), state.current,
      *committed_document, final_difference.dirty_targets,
      final_difference.dirty_canvas, std::move(warnings));
  if (!receipt)
    return std::unexpected{receipt.error()};

  const auto record_bytes = logical_size(transaction) + logical_size(*receipt);
  if (const auto capacity =
          replay_capacity_error(state, storage_limits, record_bytes)) {
    return std::unexpected{*capacity};
  }

  auto candidate = state;
  candidate.current = *committed_document;
  candidate.redo.clear();
  candidate.undo.push_back(HistoryEntry{state.current, *committed_document,
                                        transaction.transaction_id()});
  while (candidate.undo.size() > storage_limits.max_undo_steps())
    candidate.undo.pop_front();
  while (!candidate.undo.empty() &&
         history_bytes(candidate.undo, candidate.redo) >
             storage_limits.max_history_bytes() &&
         candidate.undo.size() > 1) {
    candidate.undo.pop_front();
  }
  if (candidate.undo.empty() || history_bytes(candidate.undo, candidate.redo) >
                                    storage_limits.max_history_bytes()) {
    return std::unexpected{make_error(TransactionErrorCode::resource_limit,
                                      RetryAdvice::change_request,
                                      history_limit_message)};
  }
  candidate.replay.emplace(id_key(transaction.transaction_id()),
                           ReplayEntry{transaction, *receipt, record_bytes});
  candidate.replay_bytes += record_bytes;

  auto prepared = prepare_state(std::move(candidate), cancellation);
  if (!prepared)
    return std::unexpected{prepared.error()};
  return std::pair{TransactionResult{mode == ApplyMode::dry_run
                                         ? TransactionDisposition::dry_run
                                         : TransactionDisposition::committed,
                                     *receipt},
                   std::move(*prepared)};
}

[[nodiscard]] auto apply_history(const Aggregate &state,
                                 const TransactionStorageLimits &storage_limits,
                                 const Transaction &transaction,
                                 const bool undo, const ApplyMode mode,
                                 const CancellationToken cancellation)
    -> std::expected<
        std::pair<TransactionResult, std::shared_ptr<const Aggregate>>,
        TransactionError> {
  const auto &source = undo ? state.undo : state.redo;
  if (source.empty()) {
    return std::unexpected{
        make_error(undo ? TransactionErrorCode::nothing_to_undo
                        : TransactionErrorCode::nothing_to_redo,
                   RetryAdvice::change_request,
                   undo ? nothing_to_undo_message : nothing_to_redo_message)};
  }
  const auto next_revision = document_revision(state.current).next();
  if (!next_revision) {
    return std::unexpected{make_error(TransactionErrorCode::revision_overflow,
                                      RetryAdvice::not_retryable,
                                      revision_overflow_message)};
  }
  const auto entry = source.back();
  const auto &historical = undo ? entry.before : entry.after;
  auto target = clone_with_revision(historical, *next_revision);
  if (!target)
    return std::unexpected{make_error(
        map_scene_code(target.error().code), RetryAdvice::same_request,
        message_for_scene_code(target.error().code))};

  auto difference = document_difference(state.current, *target);
  auto receipt = create_receipt(
      transaction, document_revision(state.current), *next_revision, {},
      std::move(difference.changed), state.current, *target,
      difference.dirty_targets, difference.dirty_canvas, {});
  if (!receipt)
    return std::unexpected{receipt.error()};

  const auto record_bytes = logical_size(transaction) + logical_size(*receipt);
  if (const auto capacity =
          replay_capacity_error(state, storage_limits, record_bytes)) {
    return std::unexpected{*capacity};
  }

  auto candidate = state;
  candidate.current = *target;
  if (undo) {
    candidate.undo.pop_back();
    candidate.redo.push_back(entry);
  } else {
    candidate.redo.pop_back();
    candidate.undo.push_back(entry);
  }
  candidate.replay.emplace(id_key(transaction.transaction_id()),
                           ReplayEntry{transaction, *receipt, record_bytes});
  candidate.replay_bytes += record_bytes;
  auto prepared = prepare_state(std::move(candidate), cancellation);
  if (!prepared)
    return std::unexpected{prepared.error()};
  return std::pair{TransactionResult{mode == ApplyMode::dry_run
                                         ? TransactionDisposition::dry_run
                                         : TransactionDisposition::committed,
                                     *receipt},
                   std::move(*prepared)};
}

} // namespace

auto detail::set_transaction_fault(const TransactionFaultPoint fault) noexcept
    -> void {
  transaction_fault = fault;
}

struct TransactionDispatcher::Impl {
  Impl(TransactionStorageLimits dispatcher_storage_limits,
       std::shared_ptr<const Aggregate> dispatcher_state) noexcept
      : storage_limits{dispatcher_storage_limits},
        state{std::move(dispatcher_state)} {}

  TransactionStorageLimits storage_limits;
  std::shared_ptr<const Aggregate> state;
};

TransactionDispatcher::TransactionDispatcher(
    std::unique_ptr<Impl> impl) noexcept
    : m_impl{std::move(impl)} {}

auto TransactionDispatcher::create(
    Document document, TransactionStorageLimits storage_limits) noexcept
    -> std::expected<TransactionDispatcher, TransactionError> {
  try {
    auto aggregate = std::make_shared<Aggregate>(std::move(document));
    auto impl = std::make_unique<Impl>(storage_limits, std::move(aggregate));
    return TransactionDispatcher{std::move(impl)};
  } catch (...) {
    return std::unexpected{allocation_error()};
  }
}

TransactionDispatcher::TransactionDispatcher(
    TransactionDispatcher &&) noexcept = default;
auto TransactionDispatcher::operator=(TransactionDispatcher &&) noexcept
    -> TransactionDispatcher & = default;
TransactionDispatcher::~TransactionDispatcher() = default;

auto TransactionDispatcher::snapshot() const noexcept -> Document {
  return m_impl->state->current;
}

auto TransactionDispatcher::apply(const Transaction &transaction,
                                  const ApplyMode mode,
                                  const CancellationToken cancellation) noexcept
    -> std::expected<TransactionResult, TransactionError> {
  try {
    const auto &state = *m_impl->state;
    if (transaction.document_id() != document_id(state.current)) {
      return std::unexpected{make_error(
          TransactionErrorCode::wrong_document, RetryAdvice::change_request,
          wrong_document_message, FieldPath{TransactionField::document_id})};
    }

    const auto established =
        state.replay.find(transaction.transaction_id().value());
    if (established != state.replay.end()) {
      if (established->second.transaction == transaction) {
        return TransactionResult{TransactionDisposition::replayed,
                                 established->second.receipt};
      }
      return std::unexpected{
          make_error(TransactionErrorCode::transaction_id_conflict,
                     RetryAdvice::change_request, conflict_message,
                     FieldPath{TransactionField::transaction_id})};
    }

    if (cancellation.stop_requested())
      return std::unexpected{cancelled_error()};

    if (transaction.expected_revision() != document_revision(state.current)) {
      return std::unexpected{
          make_error(TransactionErrorCode::stale_revision,
                     RetryAdvice::refresh_then_retry, stale_revision_message,
                     FieldPath{TransactionField::expected_revision})};
    }

    const auto checked_transaction_id =
        TransactionId::create(transaction.transaction_id().value(),
                              DocumentAccess::state(state.current).limits);
    if (!checked_transaction_id) {
      if (checked_transaction_id.error().code ==
          ValueErrorCode::allocation_failure) {
        return std::unexpected{allocation_error()};
      }
      return std::unexpected{make_error(
          TransactionErrorCode::resource_limit, RetryAdvice::change_request,
          resource_limit_message, FieldPath{TransactionField::transaction_id})};
    }

    std::expected<
        std::pair<TransactionResult, std::shared_ptr<const Aggregate>>,
        TransactionError>
        result = std::visit(
            [&](const auto &body) {
              using T = std::remove_cvref_t<decltype(body)>;
              if constexpr (std::is_same_v<T, OperationBatch>) {
                return apply_batch(state, m_impl->storage_limits, transaction,
                                   body, mode, cancellation);
              } else {
                return apply_history(state, m_impl->storage_limits, transaction,
                                     std::is_same_v<T, Undo>, mode,
                                     cancellation);
              }
            },
            transaction.body());
    if (!result)
      return std::unexpected{result.error()};
    if (mode == ApplyMode::commit)
      m_impl->state = std::move(result->second);
    return std::move(result->first);
  } catch (...) {
    return std::unexpected{allocation_error()};
  }
}

} // namespace drawforge

namespace drawforge {

auto TransactionStorageLimits::create(
    const TransactionStorageLimitRequest values) noexcept
    -> std::expected<TransactionStorageLimits, ValueError> {
  const auto &hard = hard_transaction_storage_limit_values;
  if (values.max_replay_records > hard.max_replay_records ||
      values.max_replay_bytes > hard.max_replay_bytes ||
      values.max_undo_steps > hard.max_undo_steps ||
      values.max_history_bytes > hard.max_history_bytes) {
    return std::unexpected{invalid_storage_limit_error};
  }
  return TransactionStorageLimits{values};
}

auto FieldPath::operator==(const FieldPath &other) const noexcept -> bool {
  return segments().size() == other.segments().size() &&
         std::ranges::equal(segments(), other.segments());
}

auto transaction_error_code_name(const TransactionErrorCode code) noexcept
    -> std::string_view {
  switch (code) {
  case TransactionErrorCode::wrong_document:
    return "wrong_document";
  case TransactionErrorCode::transaction_id_conflict:
    return "transaction_id_conflict";
  case TransactionErrorCode::stale_revision:
    return "stale_revision";
  case TransactionErrorCode::empty_transaction:
    return "empty_transaction";
  case TransactionErrorCode::nothing_to_undo:
    return "nothing_to_undo";
  case TransactionErrorCode::nothing_to_redo:
    return "nothing_to_redo";
  case TransactionErrorCode::cancelled:
    return "cancelled";
  case TransactionErrorCode::resource_limit:
    return "resource_limit";
  case TransactionErrorCode::revision_overflow:
    return "revision_overflow";
  case TransactionErrorCode::allocation_failure:
    return "allocation_failure";
  case TransactionErrorCode::missing_identity:
    return "missing_identity";
  case TransactionErrorCode::duplicate_identity:
    return "duplicate_identity";
  case TransactionErrorCode::invalid_parent:
    return "invalid_parent";
  case TransactionErrorCode::parent_cycle:
    return "parent_cycle";
  case TransactionErrorCode::invalid_geometry:
    return "invalid_geometry";
  case TransactionErrorCode::unsupported_node_kind:
    return "unsupported_node_kind";
  case TransactionErrorCode::unsupported_property:
    return "unsupported_property";
  case TransactionErrorCode::number_out_of_range:
    return "number_out_of_range";
  case TransactionErrorCode::arithmetic_overflow:
    return "arithmetic_overflow";
  }
  return "unknown";
}

auto transaction_warning_code_name(const TransactionWarningCode code) noexcept
    -> std::string_view {
  switch (code) {
  case TransactionWarningCode::no_effect:
    return "no_effect";
  }
  return "unknown";
}

auto retry_advice_name(const RetryAdvice advice) noexcept -> std::string_view {
  switch (advice) {
  case RetryAdvice::same_request:
    return "same_request";
  case RetryAdvice::refresh_then_retry:
    return "refresh_then_retry";
  case RetryAdvice::change_request:
    return "change_request";
  case RetryAdvice::not_retryable:
    return "not_retryable";
  }
  return "unknown";
}

auto transaction_field_name(const TransactionField field) noexcept
    -> std::string_view {
  switch (field) {
  case TransactionField::document_id:
    return "document_id";
  case TransactionField::expected_revision:
    return "expected_revision";
  case TransactionField::transaction_id:
    return "transaction_id";
  case TransactionField::body:
    return "body";
  case TransactionField::operations:
    return "operations";
  case TransactionField::layer_id:
    return "layer_id";
  case TransactionField::object_id:
    return "object_id";
  case TransactionField::track_id:
    return "track_id";
  case TransactionField::target_object_id:
    return "target_object_id";
  case TransactionField::background:
    return "background";
  case TransactionField::target:
    return "target";
  case TransactionField::visible:
    return "visible";
  case TransactionField::transform:
    return "transform";
  case TransactionField::geometry:
    return "geometry";
  case TransactionField::style:
    return "style";
  case TransactionField::opacity:
    return "opacity";
  case TransactionField::parent:
    return "parent";
  case TransactionField::index:
    return "index";
  case TransactionField::start_time_us:
    return "start_time_us";
  case TransactionField::duration_us:
    return "duration_us";
  case TransactionField::from_opacity:
    return "from_opacity";
  case TransactionField::to_opacity:
    return "to_opacity";
  }
  return "unknown";
}

TransactionReceipt::~TransactionReceipt() = default;

auto TransactionReceipt::document_id() const noexcept -> const DocumentId & {
  return m_data->document_id;
}
auto TransactionReceipt::transaction_id() const noexcept
    -> const TransactionId & {
  return m_data->transaction_id;
}
auto TransactionReceipt::base_revision() const noexcept -> Revision {
  return m_data->base_revision;
}
auto TransactionReceipt::result_revision() const noexcept -> Revision {
  return m_data->result_revision;
}
auto TransactionReceipt::created() const noexcept
    -> std::span<const IdentityRef> {
  return m_data->created;
}
auto TransactionReceipt::changed() const noexcept
    -> std::span<const IdentityRef> {
  return m_data->changed;
}
auto TransactionReceipt::dirty_bounds() const noexcept
    -> const std::optional<Bounds> & {
  return m_data->dirty_bounds;
}
auto TransactionReceipt::warnings() const noexcept
    -> std::span<const TransactionWarning> {
  return m_data->warnings;
}
auto TransactionReceipt::operator==(
    const TransactionReceipt &other) const noexcept -> bool {
  return *m_data == *other.m_data;
}

namespace {

[[nodiscard]] auto finish_operation(SceneValues values, OperationEffect effect,
                                    const std::uint64_t operation,
                                    const TransactionField field)
    -> std::expected<OperationOutcome, TransactionError> {
  const auto revision = values.revision;
  auto document = build_document(std::move(values), revision);
  if (!document)
    return std::unexpected{operation_error(document.error(), operation, field)};
  return OperationOutcome{std::move(*document), std::move(effect)};
}

[[nodiscard]] auto missing_operation_error(const std::uint64_t operation,
                                           const TransactionField field)
    -> TransactionError {
  return make_error(TransactionErrorCode::missing_identity,
                    RetryAdvice::change_request, missing_identity_message,
                    operation_path(operation, field), operation);
}

[[nodiscard]] auto duplicate_operation_error(const std::uint64_t operation,
                                             const TransactionField field)
    -> TransactionError {
  return make_error(TransactionErrorCode::duplicate_identity,
                    RetryAdvice::change_request, duplicate_identity_message,
                    operation_path(operation, field), operation);
}

[[nodiscard]] auto unsupported_operation_error(const std::uint64_t operation,
                                               const TransactionField field)
    -> TransactionError {
  return make_error(TransactionErrorCode::unsupported_property,
                    RetryAdvice::change_request, unsupported_property_message,
                    operation_path(operation, field), operation);
}

[[nodiscard]] auto apply_operation(const Document &document,
                                   const Operation &operation,
                                   const std::uint64_t operation_index)
    -> std::expected<OperationOutcome, TransactionError> {
  const auto &state = DocumentAccess::state(document);

  return std::visit(
      [&](const auto &value)
          -> std::expected<OperationOutcome, TransactionError> {
        using T = std::remove_cvref_t<decltype(value)>;

        if constexpr (std::is_same_v<T, CreateLayer>) {
          if (find_layer(state, value.layer_id) != nullptr) {
            return std::unexpected{duplicate_operation_error(
                operation_index, TransactionField::layer_id)};
          }
          if (value.index > state.layers.size())
            return std::unexpected{index_error(operation_index)};
          auto values = SceneValues{state};
          values.layers.insert(values.layers.begin() +
                                   static_cast<std::ptrdiff_t>(value.index),
                               Layer{value.layer_id, value.visible});
          OperationEffect effect{IdentityRef{value.layer_id}, true, true,
                                 TransactionField::layer_id};
          return finish_operation(std::move(values), std::move(effect),
                                  operation_index, TransactionField::layer_id);
        } else if constexpr (std::is_same_v<T, CreateGroup> ||
                             std::is_same_v<T, CreateRectangle> ||
                             std::is_same_v<T, CreateEllipse> ||
                             std::is_same_v<T, CreatePath>) {
          if (find_object(state, value.object_id) != nullptr) {
            return std::unexpected{duplicate_operation_error(
                operation_index, TransactionField::object_id)};
          }
          auto siblings =
              children_for_parent(state, value.parent, operation_index);
          if (!siblings)
            return std::unexpected{siblings.error()};
          if (value.index > siblings->size())
            return std::unexpected{index_error(operation_index)};

          SceneObject object = [&]() -> SceneObject {
            if constexpr (std::is_same_v<T, CreateGroup>) {
              return Group{value.object_id, value.parent, value.visible,
                           value.transform};
            } else {
              return Drawable{value.object_id,
                              value.parent,
                              value.visible,
                              value.transform,
                              Geometry{value.geometry},
                              value.style,
                              value.opacity};
            }
          }();
          auto values = SceneValues{state};
          insert_object_at(values.objects, std::move(object), *siblings,
                           value.index);
          OperationEffect effect{IdentityRef{value.object_id}, true, true,
                                 TransactionField::object_id};
          return finish_operation(std::move(values), std::move(effect),
                                  operation_index, TransactionField::object_id);
        } else if constexpr (std::is_same_v<T, CreateOpacityTrack>) {
          if (find_track(state, value.track.id()) != nullptr) {
            return std::unexpected{duplicate_operation_error(
                operation_index, TransactionField::track_id)};
          }
          if (const auto target_error = validate_track_target(
                  state, value.track, std::nullopt, operation_index)) {
            return std::unexpected{*target_error};
          }
          auto values = SceneValues{state};
          values.tracks.push_back(value.track);
          OperationEffect effect{IdentityRef{value.track.id()}, true, true,
                                 TransactionField::track_id};
          return finish_operation(std::move(values), std::move(effect),
                                  operation_index, TransactionField::track_id);
        } else if constexpr (std::is_same_v<T, SetCanvasBackground>) {
          const auto changed = state.background != value.background;
          OperationEffect effect{IdentityRef{state.id}, false, changed,
                                 TransactionField::background};
          if (!changed)
            return OperationOutcome{document, std::move(effect)};
          auto values = SceneValues{state};
          values.background = value.background;
          return finish_operation(std::move(values), std::move(effect),
                                  operation_index,
                                  TransactionField::background);
        } else if constexpr (std::is_same_v<T, SetVisibility>) {
          if (const auto *layer_id = std::get_if<LayerId>(&value.target)) {
            const auto found = state.layer_index.find(id_key(*layer_id));
            if (found == state.layer_index.end()) {
              return std::unexpected{missing_operation_error(
                  operation_index, TransactionField::target)};
            }
            const auto changed =
                state.layers[found->second].layer.visible() != value.visible;
            OperationEffect effect{IdentityRef{*layer_id}, false, changed,
                                   TransactionField::visible};
            if (!changed)
              return OperationOutcome{document, std::move(effect)};
            auto values = SceneValues{state};
            values.layers[found->second] = Layer{*layer_id, value.visible};
            return finish_operation(std::move(values), std::move(effect),
                                    operation_index, TransactionField::visible);
          }

          const auto &object_id = std::get<ObjectId>(value.target);
          const auto found = state.object_index.find(id_key(object_id));
          if (found == state.object_index.end()) {
            return std::unexpected{missing_operation_error(
                operation_index, TransactionField::target)};
          }
          const auto changed =
              detail::object_visible(state.objects[found->second].object) !=
              value.visible;
          OperationEffect effect{IdentityRef{object_id}, false, changed,
                                 TransactionField::visible};
          if (!changed)
            return OperationOutcome{document, std::move(effect)};
          auto values = SceneValues{state};
          values.objects[found->second] = object_with_visibility(
              values.objects[found->second], value.visible);
          return finish_operation(std::move(values), std::move(effect),
                                  operation_index, TransactionField::visible);
        } else if constexpr (std::is_same_v<T, SetTransform>) {
          const auto found = state.object_index.find(id_key(value.object_id));
          if (found == state.object_index.end()) {
            return std::unexpected{missing_operation_error(
                operation_index, TransactionField::object_id)};
          }
          const auto changed =
              detail::object_transform(state.objects[found->second].object) !=
              value.transform;
          OperationEffect effect{IdentityRef{value.object_id}, false, changed,
                                 TransactionField::transform};
          if (!changed)
            return OperationOutcome{document, std::move(effect)};
          auto values = SceneValues{state};
          values.objects[found->second] = object_with_transform(
              values.objects[found->second], value.transform);
          return finish_operation(std::move(values), std::move(effect),
                                  operation_index, TransactionField::transform);
        } else if constexpr (std::is_same_v<T, SetGeometry> ||
                             std::is_same_v<T, SetStyle> ||
                             std::is_same_v<T, SetOpacity>) {
          const auto found = state.object_index.find(id_key(value.object_id));
          if (found == state.object_index.end()) {
            return std::unexpected{missing_operation_error(
                operation_index, TransactionField::object_id)};
          }
          const auto *drawable =
              std::get_if<Drawable>(&state.objects[found->second].object);
          constexpr auto field = [] {
            if constexpr (std::is_same_v<T, SetGeometry>)
              return TransactionField::geometry;
            else if constexpr (std::is_same_v<T, SetStyle>)
              return TransactionField::style;
            else
              return TransactionField::opacity;
          }();
          if (drawable == nullptr) {
            return std::unexpected{
                unsupported_operation_error(operation_index, field)};
          }
          const auto changed = [&] {
            if constexpr (std::is_same_v<T, SetGeometry>)
              return drawable->geometry() != value.geometry;
            else if constexpr (std::is_same_v<T, SetStyle>)
              return drawable->style() != value.style;
            else
              return drawable->opacity() != value.opacity;
          }();
          OperationEffect effect{IdentityRef{value.object_id}, false, changed,
                                 field};
          if (!changed)
            return OperationOutcome{document, std::move(effect)};
          auto values = SceneValues{state};
          if constexpr (std::is_same_v<T, SetGeometry>) {
            values.objects[found->second] =
                drawable_with_geometry(*drawable, value.geometry);
          } else if constexpr (std::is_same_v<T, SetStyle>) {
            values.objects[found->second] =
                drawable_with_style(*drawable, value.style);
          } else {
            values.objects[found->second] =
                drawable_with_opacity(*drawable, value.opacity);
          }
          return finish_operation(std::move(values), std::move(effect),
                                  operation_index, field);
        } else if constexpr (std::is_same_v<T, SetOpacityTrack>) {
          const auto found = state.track_index.find(id_key(value.track.id()));
          if (found == state.track_index.end()) {
            return std::unexpected{missing_operation_error(
                operation_index, TransactionField::track_id)};
          }
          const auto &existing = state.tracks[found->second];
          const auto changed = existing != value.track;
          OperationEffect effect{IdentityRef{value.track.id()}, false, changed,
                                 TransactionField::track_id};
          if (!changed)
            return OperationOutcome{document, std::move(effect)};
          if (const auto target_error = validate_track_target(
                  state, value.track, value.track.id(), operation_index)) {
            return std::unexpected{*target_error};
          }
          auto values = SceneValues{state};
          values.tracks[found->second] = value.track;
          return finish_operation(std::move(values), std::move(effect),
                                  operation_index, TransactionField::track_id);
        } else if constexpr (std::is_same_v<T, ReparentObject> ||
                             std::is_same_v<T, ReorderObject>) {
          const auto found = state.object_index.find(id_key(value.object_id));
          if (found == state.object_index.end()) {
            return std::unexpected{missing_operation_error(
                operation_index, TransactionField::object_id)};
          }
          const auto &existing = state.objects[found->second].object;
          const auto old_parent = detail::object_parent(existing);
          const ParentRef destination_parent = [&]() -> ParentRef {
            if constexpr (std::is_same_v<T, ReparentObject>)
              return value.parent;
            else
              return old_parent;
          }();
          auto destination_siblings =
              children_for_parent(state, destination_parent, operation_index);
          if (!destination_siblings)
            return std::unexpected{destination_siblings.error()};
          std::erase(*destination_siblings, value.object_id);
          if (value.index > destination_siblings->size())
            return std::unexpected{index_error(operation_index)};

          const auto current_index = state.objects[found->second].sibling_index;
          const auto changed =
              old_parent != destination_parent || current_index != value.index;
          const auto field = std::is_same_v<T, ReparentObject>
                                 ? TransactionField::parent
                                 : TransactionField::index;
          OperationEffect effect{IdentityRef{value.object_id}, false, changed,
                                 field};
          if (!changed)
            return OperationOutcome{document, std::move(effect)};

          auto values = SceneValues{state};
          auto moved = object_with_parent(existing, destination_parent);
          const auto position =
              object_position(values.objects, value.object_id);
          values.objects.erase(values.objects.begin() +
                               static_cast<std::ptrdiff_t>(position));
          insert_object_at(values.objects, std::move(moved),
                           *destination_siblings, value.index);
          return finish_operation(std::move(values), std::move(effect),
                                  operation_index, field);
        }
      },
      operation);
}

[[nodiscard]] auto normalized_paint_document(const Document &document)
    -> std::expected<Document, SceneError> {
  const auto &state = DocumentAccess::state(document);
  auto values = SceneValues{state};
  for (auto &layer : values.layers)
    layer = Layer{layer.id(), true};
  const auto full_opacity = Opacity::create(1.0);
  if (!full_opacity)
    return std::unexpected{full_opacity.error()};
  for (auto &object : values.objects) {
    object = std::visit(
        [&](const auto &value) -> SceneObject {
          using T = std::remove_cvref_t<decltype(value)>;
          if constexpr (std::is_same_v<T, Group>) {
            return Group{value.id(), value.parent(), true, value.transform()};
          } else {
            return Drawable{value.id(),        value.parent(),   true,
                            value.transform(), value.geometry(), value.style(),
                            *full_opacity};
          }
        },
        object);
  }
  values.tracks.clear();
  return build_document(std::move(values), state.revision);
}

[[nodiscard]] auto unite_bounds(const Bounds &first, const Bounds &second,
                                const ResourceLimits &limits)
    -> std::expected<Bounds, SceneError> {
  const auto left = std::min(first.x().value(), second.x().value());
  const auto top = std::min(first.y().value(), second.y().value());
  const auto right = std::max(
      static_cast<long double>(first.x().value()) + first.width().value(),
      static_cast<long double>(second.x().value()) + second.width().value());
  const auto bottom = std::max(
      static_cast<long double>(first.y().value()) + first.height().value(),
      static_cast<long double>(second.y().value()) + second.height().value());
  const auto width = right - static_cast<long double>(left);
  const auto height = bottom - static_cast<long double>(top);
  if (width > std::numeric_limits<double>::max() ||
      height > std::numeric_limits<double>::max()) {
    return std::unexpected{SceneError{SceneErrorCode::arithmetic_overflow,
                                      arithmetic_overflow_message}};
  }
  return Bounds::create(left, top, static_cast<double>(width),
                        static_cast<double>(height), limits);
}

[[nodiscard]] auto append_target_bounds(const Document &document,
                                        const BoundsTarget &target,
                                        std::optional<Bounds> &dirty)
    -> std::expected<void, SceneError> {
  const auto normalized = normalized_paint_document(document);
  if (!normalized)
    return std::unexpected{normalized.error()};
  const auto query =
      BoundsQuery::create({target}, {BoundsProjection::document_painted}, 0);
  if (!query)
    return std::unexpected{query.error()};
  const auto result = inspect(*normalized, *query);
  if (!result)
    return std::unexpected{result.error()};
  const auto &candidate = result->items.front().projections.front().bounds;
  if (!candidate)
    return {};
  if (!dirty) {
    dirty = *candidate;
    return {};
  }
  const auto combined =
      unite_bounds(*dirty, *candidate, DocumentAccess::state(document).limits);
  if (!combined)
    return std::unexpected{combined.error()};
  dirty = *combined;
  return {};
}

[[nodiscard]] auto canvas_bounds(const Document &document)
    -> std::expected<Bounds, SceneError> {
  const auto &state = DocumentAccess::state(document);
  return Bounds::create(0, 0, state.canvas.width(), state.canvas.height(),
                        state.limits);
}

[[nodiscard]] auto
compute_dirty_bounds(const Document &before, const Document &after,
                     const std::vector<BoundsTarget> &targets,
                     const bool dirty_canvas)
    -> std::expected<std::optional<Bounds>, SceneError> {
  std::optional<Bounds> dirty;
  if (dirty_canvas) {
    const auto canvas = canvas_bounds(after);
    if (!canvas)
      return std::unexpected{canvas.error()};
    dirty = *canvas;
  }
  for (const auto &target : targets) {
    const auto before_exists = std::visit(
        [&](const auto &id) {
          using Id = std::remove_cvref_t<decltype(id)>;
          if constexpr (std::is_same_v<Id, LayerId>)
            return find_layer(DocumentAccess::state(before), id) != nullptr;
          else
            return find_object(DocumentAccess::state(before), id) != nullptr;
        },
        target);
    if (before_exists) {
      const auto appended = append_target_bounds(before, target, dirty);
      if (!appended)
        return std::unexpected{appended.error()};
    }
    const auto after_exists = std::visit(
        [&](const auto &id) {
          using Id = std::remove_cvref_t<decltype(id)>;
          if constexpr (std::is_same_v<Id, LayerId>)
            return find_layer(DocumentAccess::state(after), id) != nullptr;
          else
            return find_object(DocumentAccess::state(after), id) != nullptr;
        },
        target);
    if (after_exists) {
      const auto appended = append_target_bounds(after, target, dirty);
      if (!appended)
        return std::unexpected{appended.error()};
    }
  }
  return dirty;
}

} // namespace

} // namespace drawforge
