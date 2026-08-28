#pragma once

#include <drawforge/query.hpp>

#include <cstddef>
#include <expected>
#include <memory>
#include <string>
#include <unordered_map>
#include <vector>

namespace drawforge {

namespace detail {

struct LayerRecord {
  Layer layer;
  std::vector<std::size_t> children;
};

struct ObjectRecord {
  SceneObject object;
  std::vector<std::size_t> children;
  std::uint64_t sibling_index{};
};

struct DocumentState {
  DocumentState(DocumentId document_id, Revision document_revision,
                CanvasExtent document_canvas,
                std::optional<Color> document_background,
                ResourceLimits document_limits) noexcept
      : id{std::move(document_id)}, revision{document_revision},
        canvas{document_canvas}, background{document_background},
        limits{document_limits} {}

  DocumentId id;
  Revision revision{};
  CanvasExtent canvas;
  std::optional<Color> background;
  ResourceLimits limits;
  std::vector<LayerRecord> layers;
  std::vector<ObjectRecord> objects;
  std::vector<OpacityTrack> tracks;
  std::unordered_map<std::string, std::size_t> layer_index;
  std::unordered_map<std::string, std::size_t> object_index;
  std::unordered_map<std::string, std::size_t> track_index;
  std::unordered_map<std::string, std::size_t> track_by_object;
};

class DocumentBuilder {
public:
  [[nodiscard]] static auto
  build(DocumentId id, Revision revision, CanvasExtent canvas,
        std::optional<Color> background, ResourceLimits limits,
        std::vector<Layer> layers, std::vector<SceneObject> objects,
        std::vector<OpacityTrack> tracks) noexcept
      -> std::expected<Document, SceneError>;
};

[[nodiscard]] auto object_id(const SceneObject &object) noexcept
    -> const ObjectId &;
[[nodiscard]] auto object_parent(const SceneObject &object) noexcept
    -> const ParentRef &;
[[nodiscard]] auto object_visible(const SceneObject &object) noexcept -> bool;
[[nodiscard]] auto object_transform(const SceneObject &object) noexcept
    -> AffineTransform;
[[nodiscard]] auto object_kind(const SceneObject &object) noexcept
    -> ObjectKind;

} // namespace detail

struct Document::Impl : detail::DocumentState {
  using detail::DocumentState::DocumentState;
};

struct DocumentAccess {
  [[nodiscard]] static auto state(const Document &document) noexcept
      -> const detail::DocumentState & {
    return *document.m_impl;
  }
};

} // namespace drawforge
