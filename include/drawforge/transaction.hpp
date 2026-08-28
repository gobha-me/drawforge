#pragma once

#include <drawforge/query.hpp>

#include <array>
#include <concepts>
#include <cstddef>
#include <cstdint>
#include <expected>
#include <memory>
#include <optional>
#include <span>
#include <string_view>
#include <type_traits>
#include <utility>
#include <variant>
#include <vector>

namespace drawforge {

struct TransactionStorageLimitRequest {
  // Byte ceilings use deterministic logical domain sizes rather than
  // allocator capacity or ABI-dependent sizeof values.
  std::uint64_t max_replay_records{4'096};
  std::uint64_t max_replay_bytes{64ULL * 1024ULL * 1024ULL};
  std::uint64_t max_undo_steps{256};
  std::uint64_t max_history_bytes{64ULL * 1024ULL * 1024ULL};

  auto operator==(const TransactionStorageLimitRequest &) const
      -> bool = default;
};

inline constexpr TransactionStorageLimitRequest
    default_transaction_storage_limit_values{};
inline constexpr TransactionStorageLimitRequest
    hard_transaction_storage_limit_values{
        .max_replay_records = 65'536,
        .max_replay_bytes = 256ULL * 1024ULL * 1024ULL,
        .max_undo_steps = 4'096,
        .max_history_bytes = 256ULL * 1024ULL * 1024ULL,
    };

class TransactionStorageLimits {
public:
  constexpr TransactionStorageLimits() noexcept = default;

  [[nodiscard]] static auto
  create(TransactionStorageLimitRequest values) noexcept
      -> std::expected<TransactionStorageLimits, ValueError>;

  [[nodiscard]] constexpr auto values() const noexcept
      -> const TransactionStorageLimitRequest & {
    return m_values;
  }
  [[nodiscard]] constexpr auto max_replay_records() const noexcept
      -> std::uint64_t {
    return m_values.max_replay_records;
  }
  [[nodiscard]] constexpr auto max_replay_bytes() const noexcept
      -> std::uint64_t {
    return m_values.max_replay_bytes;
  }
  [[nodiscard]] constexpr auto max_undo_steps() const noexcept
      -> std::uint64_t {
    return m_values.max_undo_steps;
  }
  [[nodiscard]] constexpr auto max_history_bytes() const noexcept
      -> std::uint64_t {
    return m_values.max_history_bytes;
  }

  auto operator==(const TransactionStorageLimits &) const -> bool = default;

private:
  explicit constexpr TransactionStorageLimits(
      TransactionStorageLimitRequest values) noexcept
      : m_values{values} {}

  TransactionStorageLimitRequest m_values{
      default_transaction_storage_limit_values};
};

struct CreateLayer {
  LayerId layer_id;
  std::uint64_t index{};
  bool visible{true};
  auto operator==(const CreateLayer &) const -> bool = default;
};

struct CreateGroup {
  ObjectId object_id;
  ParentRef parent;
  std::uint64_t index{};
  bool visible{true};
  AffineTransform transform{};
  auto operator==(const CreateGroup &) const -> bool = default;
};

struct CreateRectangle {
  ObjectId object_id;
  ParentRef parent;
  std::uint64_t index{};
  bool visible{true};
  AffineTransform transform{};
  Style style{};
  Opacity opacity;
  Rectangle geometry;
  auto operator==(const CreateRectangle &) const -> bool = default;
};

struct CreateEllipse {
  ObjectId object_id;
  ParentRef parent;
  std::uint64_t index{};
  bool visible{true};
  AffineTransform transform{};
  Style style{};
  Opacity opacity;
  Ellipse geometry;
  auto operator==(const CreateEllipse &) const -> bool = default;
};

struct CreatePath {
  ObjectId object_id;
  ParentRef parent;
  std::uint64_t index{};
  bool visible{true};
  AffineTransform transform{};
  Style style{};
  Opacity opacity;
  Path geometry;
  auto operator==(const CreatePath &) const -> bool = default;
};

struct CreateOpacityTrack {
  OpacityTrack track;
  auto operator==(const CreateOpacityTrack &) const -> bool = default;
};

struct SetCanvasBackground {
  std::optional<Color> background;
  auto operator==(const SetCanvasBackground &) const -> bool = default;
};

struct SetVisibility {
  NodeRef target;
  bool visible{true};
  auto operator==(const SetVisibility &) const -> bool = default;
};

struct SetTransform {
  ObjectId object_id;
  AffineTransform transform{};
  auto operator==(const SetTransform &) const -> bool = default;
};

struct SetGeometry {
  ObjectId object_id;
  Geometry geometry;
  auto operator==(const SetGeometry &) const -> bool = default;
};

struct SetStyle {
  ObjectId object_id;
  Style style{};
  auto operator==(const SetStyle &) const -> bool = default;
};

struct SetOpacity {
  ObjectId object_id;
  Opacity opacity;
  auto operator==(const SetOpacity &) const -> bool = default;
};

struct SetOpacityTrack {
  OpacityTrack track;
  auto operator==(const SetOpacityTrack &) const -> bool = default;
};

struct ReparentObject {
  ObjectId object_id;
  ParentRef parent;
  // The destination index is interpreted after removing this object from its
  // former parent.
  std::uint64_t index{};
  auto operator==(const ReparentObject &) const -> bool = default;
};

struct ReorderObject {
  ObjectId object_id;
  // The destination index is interpreted after removing this object from its
  // current sibling sequence.
  std::uint64_t index{};
  auto operator==(const ReorderObject &) const -> bool = default;
};

using Operation =
    std::variant<CreateLayer, CreateGroup, CreateRectangle, CreateEllipse,
                 CreatePath, CreateOpacityTrack, SetCanvasBackground,
                 SetVisibility, SetTransform, SetGeometry, SetStyle, SetOpacity,
                 SetOpacityTrack, ReparentObject, ReorderObject>;

struct OperationBatch {
  std::vector<Operation> operations;
  auto operator==(const OperationBatch &) const -> bool = default;
};

struct Undo {
  auto operator==(const Undo &) const -> bool = default;
};
struct Redo {
  auto operator==(const Redo &) const -> bool = default;
};
using TransactionBody = std::variant<OperationBatch, Undo, Redo>;

class Transaction {
public:
  Transaction(DocumentId document_id, Revision expected_revision,
              TransactionId transaction_id, TransactionBody body) noexcept
      : m_document_id{std::move(document_id)},
        m_expected_revision{expected_revision},
        m_transaction_id{std::move(transaction_id)}, m_body{std::move(body)} {}

  [[nodiscard]] auto document_id() const noexcept -> const DocumentId & {
    return m_document_id;
  }
  [[nodiscard]] constexpr auto expected_revision() const noexcept -> Revision {
    return m_expected_revision;
  }
  [[nodiscard]] auto transaction_id() const noexcept -> const TransactionId & {
    return m_transaction_id;
  }
  [[nodiscard]] auto body() const noexcept -> const TransactionBody & {
    return m_body;
  }
  auto operator==(const Transaction &) const -> bool = default;

private:
  DocumentId m_document_id;
  Revision m_expected_revision{};
  TransactionId m_transaction_id;
  TransactionBody m_body;
};

enum class ApplyMode : std::uint8_t { dry_run, commit };
enum class TransactionDisposition : std::uint8_t {
  dry_run,
  committed,
  replayed,
};
enum class RetryAdvice : std::uint8_t {
  same_request,
  refresh_then_retry,
  change_request,
  not_retryable,
};

enum class TransactionErrorCode : std::uint8_t {
  wrong_document,
  transaction_id_conflict,
  stale_revision,
  empty_transaction,
  nothing_to_undo,
  nothing_to_redo,
  cancelled,
  resource_limit,
  revision_overflow,
  allocation_failure,
  missing_identity,
  duplicate_identity,
  invalid_parent,
  parent_cycle,
  invalid_geometry,
  unsupported_node_kind,
  unsupported_property,
  number_out_of_range,
  arithmetic_overflow,
};

enum class TransactionWarningCode : std::uint8_t { no_effect };

enum class TransactionField : std::uint8_t {
  document_id,
  expected_revision,
  transaction_id,
  body,
  operations,
  layer_id,
  object_id,
  track_id,
  target_object_id,
  background,
  target,
  visible,
  transform,
  geometry,
  style,
  opacity,
  parent,
  index,
  start_time_us,
  duration_us,
  from_opacity,
  to_opacity,
};

struct FieldIndex {
  std::uint64_t value{};
  auto operator==(const FieldIndex &) const -> bool = default;
};
using FieldPathSegment = std::variant<TransactionField, FieldIndex>;

class FieldPath {
public:
  constexpr FieldPath() noexcept = default;

  template <typename... Segments>
    requires(sizeof...(Segments) <= 4 &&
             ((std::same_as<std::remove_cvref_t<Segments>, TransactionField> ||
               std::same_as<std::remove_cvref_t<Segments>, FieldIndex>) &&
              ...))
  explicit constexpr FieldPath(Segments... segments) noexcept
      : m_segments{FieldPathSegment{segments}...}, m_size{sizeof...(Segments)} {
  }

  [[nodiscard]] constexpr auto segments() const noexcept
      -> std::span<const FieldPathSegment> {
    return {m_segments.data(), m_size};
  }
  [[nodiscard]] constexpr auto empty() const noexcept -> bool {
    return m_size == 0;
  }
  auto operator==(const FieldPath &other) const noexcept -> bool;

private:
  std::array<FieldPathSegment, 4> m_segments{};
  std::size_t m_size{};
};

using IdentityRef = std::variant<DocumentId, LayerId, ObjectId, TrackId>;

struct TransactionWarning {
  TransactionWarningCode code{};
  std::optional<std::uint64_t> operation_index;
  FieldPath field_path;
  std::string_view message;
  auto operator==(const TransactionWarning &) const -> bool = default;
};

struct TransactionError {
  TransactionErrorCode code{};
  RetryAdvice retry_advice{};
  std::optional<std::uint64_t> operation_index;
  FieldPath field_path;
  std::string_view message;
  auto operator==(const TransactionError &) const -> bool = default;
};

[[nodiscard]] auto
transaction_error_code_name(TransactionErrorCode code) noexcept
    -> std::string_view;
[[nodiscard]] auto
transaction_warning_code_name(TransactionWarningCode code) noexcept
    -> std::string_view;
[[nodiscard]] auto retry_advice_name(RetryAdvice advice) noexcept
    -> std::string_view;
[[nodiscard]] auto transaction_field_name(TransactionField field) noexcept
    -> std::string_view;

namespace detail {
struct TransactionReceiptAccess;
}

class TransactionReceipt {
public:
  TransactionReceipt(const TransactionReceipt &) noexcept = default;
  TransactionReceipt(TransactionReceipt &&) noexcept = default;
  auto operator=(const TransactionReceipt &) noexcept
      -> TransactionReceipt & = default;
  auto operator=(TransactionReceipt &&) noexcept
      -> TransactionReceipt & = default;
  ~TransactionReceipt();

  [[nodiscard]] auto document_id() const noexcept -> const DocumentId &;
  [[nodiscard]] auto transaction_id() const noexcept -> const TransactionId &;
  [[nodiscard]] auto base_revision() const noexcept -> Revision;
  [[nodiscard]] auto result_revision() const noexcept -> Revision;
  [[nodiscard]] auto created() const noexcept -> std::span<const IdentityRef>;
  [[nodiscard]] auto changed() const noexcept -> std::span<const IdentityRef>;
  [[nodiscard]] auto dirty_bounds() const noexcept
      -> const std::optional<Bounds> &;
  [[nodiscard]] auto warnings() const noexcept
      -> std::span<const TransactionWarning>;
  auto operator==(const TransactionReceipt &other) const noexcept -> bool;

private:
  struct Data;
  explicit TransactionReceipt(std::shared_ptr<const Data> data) noexcept
      : m_data{std::move(data)} {}
  std::shared_ptr<const Data> m_data;

  friend class TransactionDispatcher;
  friend struct detail::TransactionReceiptAccess;
};

struct TransactionResult {
  TransactionDisposition disposition{};
  TransactionReceipt receipt;
  auto operator==(const TransactionResult &) const -> bool = default;
};

class CancellationToken {
public:
  using Poll = bool (*)(const void *) noexcept;

  constexpr CancellationToken() noexcept = default;
  // The context is non-owning and must outlive apply(). The callback is polled
  // only at the deterministic boundaries specified by ADR-0004.
  constexpr CancellationToken(const void *context, Poll poll) noexcept
      : m_context{context}, m_poll{poll} {}

  [[nodiscard]] auto stop_requested() const noexcept -> bool {
    return m_poll != nullptr && m_poll(m_context);
  }

private:
  const void *m_context{};
  Poll m_poll{};
};

class TransactionDispatcher {
public:
  // The dispatcher defines one replay/history lifetime. Returned Documents are
  // immutable snapshots and never expose staged state.
  [[nodiscard]] static auto
  create(Document document,
         TransactionStorageLimits storage_limits = {}) noexcept
      -> std::expected<TransactionDispatcher, TransactionError>;

  TransactionDispatcher(const TransactionDispatcher &) = delete;
  TransactionDispatcher(TransactionDispatcher &&) noexcept;
  auto operator=(const TransactionDispatcher &)
      -> TransactionDispatcher & = delete;
  auto operator=(TransactionDispatcher &&) noexcept -> TransactionDispatcher &;
  ~TransactionDispatcher();

  [[nodiscard]] auto snapshot() const noexcept -> Document;
  [[nodiscard]] auto apply(const Transaction &transaction,
                           ApplyMode mode = ApplyMode::commit,
                           CancellationToken cancellation = {}) noexcept
      -> std::expected<TransactionResult, TransactionError>;

private:
  struct Impl;
  explicit TransactionDispatcher(std::unique_ptr<Impl> impl) noexcept;
  std::unique_ptr<Impl> m_impl;
};

} // namespace drawforge
