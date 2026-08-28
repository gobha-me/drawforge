#pragma once

#include <drawforge/scene.hpp>

#include <cstddef>
#include <cstdint>
#include <expected>
#include <optional>
#include <utility>
#include <variant>
#include <vector>

namespace drawforge {

enum class NodeKind : std::uint8_t {
  layer,
  group,
  rectangle,
  ellipse,
  path,
};
using NodeRef = std::variant<LayerId, ObjectId>;

class Bounds {
public:
  [[nodiscard]] static auto create(double x, double y, double width,
                                   double height,
                                   const ResourceLimits &limits = {}) noexcept
      -> std::expected<Bounds, SceneError>;
  [[nodiscard]] constexpr auto x() const noexcept -> Coordinate { return m_x; }
  [[nodiscard]] constexpr auto y() const noexcept -> Coordinate { return m_y; }
  [[nodiscard]] constexpr auto width() const noexcept -> Coordinate {
    return m_width;
  }
  [[nodiscard]] constexpr auto height() const noexcept -> Coordinate {
    return m_height;
  }
  auto operator==(const Bounds &) const -> bool = default;

private:
  constexpr Bounds(Coordinate x, Coordinate y, Coordinate width,
                   Coordinate height) noexcept
      : m_x{x}, m_y{y}, m_width{width}, m_height{height} {}
  Coordinate m_x{};
  Coordinate m_y{};
  Coordinate m_width{};
  Coordinate m_height{};
};

struct SummaryQuery {
  auto operator==(const SummaryQuery &) const -> bool = default;
};

struct DocumentSummary {
  DocumentId document_id;
  Revision revision{};
  CanvasExtent canvas;
  std::optional<Color> background;
  ResourceLimits limits;
  std::uint64_t layer_count{};
  std::uint64_t object_count{};
  std::uint64_t track_count{};
  auto operator==(const DocumentSummary &) const -> bool = default;
};

using StructureRoot = std::variant<std::monostate, LayerId, ObjectId>;

class StructureQuery {
public:
  [[nodiscard]] static auto create(StructureRoot root, std::uint32_t max_depth,
                                   std::uint64_t max_nodes) noexcept
      -> std::expected<StructureQuery, SceneError>;
  [[nodiscard]] auto root() const noexcept -> const StructureRoot & {
    return m_root;
  }
  [[nodiscard]] constexpr auto max_depth() const noexcept -> std::uint32_t {
    return m_max_depth;
  }
  [[nodiscard]] constexpr auto max_nodes() const noexcept -> std::uint64_t {
    return m_max_nodes;
  }

private:
  StructureQuery(StructureRoot root, std::uint32_t max_depth,
                 std::uint64_t max_nodes) noexcept
      : m_root{std::move(root)}, m_max_depth{max_depth},
        m_max_nodes{max_nodes} {}
  StructureRoot m_root;
  std::uint32_t m_max_depth{};
  std::uint64_t m_max_nodes{};
};

struct StructureNode {
  NodeRef identity;
  NodeKind kind{};
  std::optional<ParentRef> parent;
  std::uint64_t sibling_index{};
  bool visible{};
  std::uint64_t child_count{};
  auto operator==(const StructureNode &) const -> bool = default;
};

struct StructureResult {
  DocumentId document_id;
  Revision revision{};
  std::vector<StructureNode> nodes;
  auto operator==(const StructureResult &) const -> bool = default;
};

enum class SelectedField : std::uint8_t {
  kind,
  parent_order,
  visibility,
  transform,
  geometry,
  style,
  opacity,
  opacity_track,
};

class SelectedObjectsQuery {
public:
  [[nodiscard]] static auto create(std::vector<ObjectId> object_ids,
                                   std::vector<SelectedField> fields) noexcept
      -> std::expected<SelectedObjectsQuery, SceneError>;
  [[nodiscard]] auto object_ids() const noexcept
      -> const std::vector<ObjectId> & {
    return m_object_ids;
  }
  [[nodiscard]] auto fields() const noexcept
      -> const std::vector<SelectedField> & {
    return m_fields;
  }

private:
  SelectedObjectsQuery(std::vector<ObjectId> object_ids,
                       std::vector<SelectedField> fields) noexcept
      : m_object_ids{std::move(object_ids)}, m_fields{std::move(fields)} {}
  std::vector<ObjectId> m_object_ids;
  std::vector<SelectedField> m_fields;
};

struct ParentOrder {
  ParentRef parent;
  std::uint64_t sibling_index{};
  auto operator==(const ParentOrder &) const -> bool = default;
};

struct OpacityTrackMetadata {
  TrackId id;
  std::uint64_t start_time_us{};
  std::uint64_t duration_us{};
  Opacity from;
  Opacity to;
  auto operator==(const OpacityTrackMetadata &) const -> bool = default;
};

struct SelectedObject {
  ObjectId object_id;
  std::optional<ObjectKind> kind;
  std::optional<ParentOrder> parent_order;
  std::optional<bool> visibility;
  std::optional<AffineTransform> transform;
  std::optional<Geometry> geometry;
  std::optional<Style> style;
  std::optional<Opacity> opacity;
  std::optional<OpacityTrackMetadata> opacity_track;
  auto operator==(const SelectedObject &) const -> bool = default;
};

struct SelectedObjectsResult {
  DocumentId document_id;
  Revision revision{};
  std::vector<SelectedObject> objects;
  auto operator==(const SelectedObjectsResult &) const -> bool = default;
};

enum class BoundsProjection : std::uint8_t {
  local_geometry,
  document_geometry,
  document_painted,
};
using BoundsTarget = std::variant<LayerId, ObjectId>;

class BoundsQuery {
public:
  [[nodiscard]] static auto create(std::vector<BoundsTarget> targets,
                                   std::vector<BoundsProjection> projections,
                                   std::uint64_t time_us) noexcept
      -> std::expected<BoundsQuery, SceneError>;
  [[nodiscard]] auto targets() const noexcept
      -> const std::vector<BoundsTarget> & {
    return m_targets;
  }
  [[nodiscard]] auto projections() const noexcept
      -> const std::vector<BoundsProjection> & {
    return m_projections;
  }
  [[nodiscard]] constexpr auto time_us() const noexcept -> std::uint64_t {
    return m_time_us;
  }

private:
  BoundsQuery(std::vector<BoundsTarget> targets,
              std::vector<BoundsProjection> projections,
              std::uint64_t time_us) noexcept
      : m_targets{std::move(targets)}, m_projections{std::move(projections)},
        m_time_us{time_us} {}
  std::vector<BoundsTarget> m_targets;
  std::vector<BoundsProjection> m_projections;
  std::uint64_t m_time_us{};
};

struct ProjectedBounds {
  BoundsProjection projection{};
  std::optional<Bounds> bounds;
  auto operator==(const ProjectedBounds &) const -> bool = default;
};

struct BoundsItem {
  BoundsTarget target;
  std::vector<ProjectedBounds> projections;
  auto operator==(const BoundsItem &) const -> bool = default;
};

struct BoundsResult {
  DocumentId document_id;
  Revision revision{};
  std::uint64_t time_us{};
  std::vector<BoundsItem> items;
  auto operator==(const BoundsResult &) const -> bool = default;
};

[[nodiscard]] auto inspect(const Document &document, SummaryQuery) noexcept
    -> std::expected<DocumentSummary, SceneError>;
[[nodiscard]] auto inspect(const Document &document,
                           const StructureQuery &query) noexcept
    -> std::expected<StructureResult, SceneError>;
[[nodiscard]] auto inspect(const Document &document,
                           const SelectedObjectsQuery &query) noexcept
    -> std::expected<SelectedObjectsResult, SceneError>;
[[nodiscard]] auto inspect(const Document &document,
                           const BoundsQuery &query) noexcept
    -> std::expected<BoundsResult, SceneError>;

} // namespace drawforge
