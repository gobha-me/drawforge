#pragma once

#include <drawforge/foundation.hpp>

#include <cstdint>
#include <expected>
#include <memory>
#include <optional>
#include <string_view>
#include <utility>
#include <variant>
#include <vector>

namespace drawforge {

enum class SceneErrorCode : std::uint8_t {
  missing_identity = 0,
  duplicate_identity = 1,
  invalid_parent = 2,
  parent_cycle = 3,
  invalid_geometry = 4,
  unsupported_node_kind = 5,
  unsupported_property = 6,
  resource_limit = 7,
  number_out_of_range = 8,
  arithmetic_overflow = 9,
  allocation_failure = 10,
};

struct SceneError {
  SceneErrorCode code{};
  std::string_view message{};
  auto operator==(const SceneError &) const -> bool = default;
};

[[nodiscard]] auto scene_error_code_name(SceneErrorCode code) noexcept
    -> std::string_view;

struct Color {
  std::uint8_t red{};
  std::uint8_t green{};
  std::uint8_t blue{};
  std::uint8_t alpha{};
  auto operator==(const Color &) const -> bool = default;
};

class Opacity {
public:
  [[nodiscard]] static auto create(double value) noexcept
      -> std::expected<Opacity, SceneError>;
  [[nodiscard]] constexpr auto value() const noexcept -> double {
    return m_value;
  }
  auto operator==(const Opacity &) const -> bool = default;

private:
  explicit constexpr Opacity(double value) noexcept : m_value{value} {}
  double m_value{1.0};
};

class Stroke {
public:
  [[nodiscard]] static auto create(Color color, double width,
                                   const ResourceLimits &limits = {}) noexcept
      -> std::expected<Stroke, SceneError>;
  [[nodiscard]] constexpr auto color() const noexcept -> Color {
    return m_color;
  }
  [[nodiscard]] constexpr auto width() const noexcept -> Coordinate {
    return m_width;
  }
  auto operator==(const Stroke &) const -> bool = default;

private:
  constexpr Stroke(Color color, Coordinate width) noexcept
      : m_color{color}, m_width{width} {}
  Color m_color{};
  Coordinate m_width{};
};

class Style {
public:
  constexpr Style() noexcept = default;
  constexpr Style(std::optional<Color> fill,
                  std::optional<Stroke> stroke) noexcept
      : m_fill{fill}, m_stroke{stroke} {}
  [[nodiscard]] constexpr auto fill() const noexcept
      -> const std::optional<Color> & {
    return m_fill;
  }
  [[nodiscard]] constexpr auto stroke() const noexcept
      -> const std::optional<Stroke> & {
    return m_stroke;
  }
  auto operator==(const Style &) const -> bool = default;

private:
  std::optional<Color> m_fill{};
  std::optional<Stroke> m_stroke{};
};

class Rectangle {
public:
  [[nodiscard]] static auto
  create(double x, double y, double width, double height, double radius_x = 0.0,
         double radius_y = 0.0, const ResourceLimits &limits = {}) noexcept
      -> std::expected<Rectangle, SceneError>;
  [[nodiscard]] constexpr auto origin() const noexcept -> Point {
    return m_origin;
  }
  [[nodiscard]] constexpr auto width() const noexcept -> Coordinate {
    return m_width;
  }
  [[nodiscard]] constexpr auto height() const noexcept -> Coordinate {
    return m_height;
  }
  [[nodiscard]] constexpr auto radius_x() const noexcept -> Coordinate {
    return m_radius_x;
  }
  [[nodiscard]] constexpr auto radius_y() const noexcept -> Coordinate {
    return m_radius_y;
  }
  auto operator==(const Rectangle &) const -> bool = default;

private:
  constexpr Rectangle(Point origin, Coordinate width, Coordinate height,
                      Coordinate radius_x, Coordinate radius_y) noexcept
      : m_origin{origin}, m_width{width}, m_height{height},
        m_radius_x{radius_x}, m_radius_y{radius_y} {}
  Point m_origin{};
  Coordinate m_width{};
  Coordinate m_height{};
  Coordinate m_radius_x{};
  Coordinate m_radius_y{};
};

class Ellipse {
public:
  [[nodiscard]] static auto create(double center_x, double center_y,
                                   double radius_x, double radius_y,
                                   const ResourceLimits &limits = {}) noexcept
      -> std::expected<Ellipse, SceneError>;
  [[nodiscard]] constexpr auto center() const noexcept -> Point {
    return m_center;
  }
  [[nodiscard]] constexpr auto radius_x() const noexcept -> Coordinate {
    return m_radius_x;
  }
  [[nodiscard]] constexpr auto radius_y() const noexcept -> Coordinate {
    return m_radius_y;
  }
  auto operator==(const Ellipse &) const -> bool = default;

private:
  constexpr Ellipse(Point center, Coordinate radius_x,
                    Coordinate radius_y) noexcept
      : m_center{center}, m_radius_x{radius_x}, m_radius_y{radius_y} {}
  Point m_center{};
  Coordinate m_radius_x{};
  Coordinate m_radius_y{};
};

struct MoveTo {
  Point point{};
  auto operator==(const MoveTo &) const -> bool = default;
};
struct LineTo {
  Point point{};
  auto operator==(const LineTo &) const -> bool = default;
};
struct ClosePath {
  auto operator==(const ClosePath &) const -> bool = default;
};
using PathCommand = std::variant<MoveTo, LineTo, ClosePath>;

class Path {
public:
  [[nodiscard]] static auto create(std::vector<PathCommand> commands) noexcept
      -> std::expected<Path, SceneError>;
  [[nodiscard]] auto commands() const noexcept
      -> const std::vector<PathCommand> & {
    return m_commands;
  }
  auto operator==(const Path &) const -> bool = default;

private:
  explicit Path(std::vector<PathCommand> commands) noexcept
      : m_commands{std::move(commands)} {}
  std::vector<PathCommand> m_commands;
};

using Geometry = std::variant<Rectangle, Ellipse, Path>;
using ParentRef = std::variant<LayerId, ObjectId>;

enum class ObjectKind : std::uint8_t { group, rectangle, ellipse, path };

class Layer {
public:
  constexpr Layer(LayerId id, bool visible) noexcept
      : m_id{std::move(id)}, m_visible{visible} {}
  [[nodiscard]] auto id() const noexcept -> const LayerId & { return m_id; }
  [[nodiscard]] constexpr auto visible() const noexcept -> bool {
    return m_visible;
  }
  auto operator==(const Layer &) const -> bool = default;

private:
  LayerId m_id;
  bool m_visible{true};
};

class Group {
public:
  Group(ObjectId id, ParentRef parent, bool visible,
        AffineTransform transform = {}) noexcept
      : m_id{std::move(id)}, m_parent{std::move(parent)}, m_visible{visible},
        m_transform{transform} {}
  [[nodiscard]] auto id() const noexcept -> const ObjectId & { return m_id; }
  [[nodiscard]] auto parent() const noexcept -> const ParentRef & {
    return m_parent;
  }
  [[nodiscard]] constexpr auto visible() const noexcept -> bool {
    return m_visible;
  }
  [[nodiscard]] constexpr auto transform() const noexcept -> AffineTransform {
    return m_transform;
  }
  auto operator==(const Group &) const -> bool = default;

private:
  ObjectId m_id;
  ParentRef m_parent;
  bool m_visible{true};
  AffineTransform m_transform{};
};

class Drawable {
public:
  Drawable(ObjectId id, ParentRef parent, bool visible,
           AffineTransform transform, Geometry geometry, Style style,
           Opacity opacity) noexcept
      : m_id{std::move(id)}, m_parent{std::move(parent)}, m_visible{visible},
        m_transform{transform}, m_geometry{std::move(geometry)},
        m_style{std::move(style)}, m_opacity{opacity} {}
  [[nodiscard]] auto id() const noexcept -> const ObjectId & { return m_id; }
  [[nodiscard]] auto parent() const noexcept -> const ParentRef & {
    return m_parent;
  }
  [[nodiscard]] constexpr auto visible() const noexcept -> bool {
    return m_visible;
  }
  [[nodiscard]] constexpr auto transform() const noexcept -> AffineTransform {
    return m_transform;
  }
  [[nodiscard]] auto geometry() const noexcept -> const Geometry & {
    return m_geometry;
  }
  [[nodiscard]] auto style() const noexcept -> const Style & { return m_style; }
  [[nodiscard]] constexpr auto opacity() const noexcept -> Opacity {
    return m_opacity;
  }
  [[nodiscard]] auto kind() const noexcept -> ObjectKind;
  auto operator==(const Drawable &) const -> bool = default;

private:
  ObjectId m_id;
  ParentRef m_parent;
  bool m_visible{true};
  AffineTransform m_transform{};
  Geometry m_geometry;
  Style m_style{};
  Opacity m_opacity;
};

using SceneObject = std::variant<Group, Drawable>;

class OpacityTrack {
public:
  [[nodiscard]] static auto
  create(TrackId id, ObjectId target, std::uint64_t start_time_us,
         std::uint64_t duration_us, Opacity from, Opacity to) noexcept
      -> std::expected<OpacityTrack, SceneError>;
  [[nodiscard]] auto id() const noexcept -> const TrackId & { return m_id; }
  [[nodiscard]] auto target() const noexcept -> const ObjectId & {
    return m_target;
  }
  [[nodiscard]] constexpr auto start_time_us() const noexcept -> std::uint64_t {
    return m_start_time_us;
  }
  [[nodiscard]] constexpr auto duration_us() const noexcept -> std::uint64_t {
    return m_duration_us;
  }
  [[nodiscard]] constexpr auto from() const noexcept -> Opacity {
    return m_from;
  }
  [[nodiscard]] constexpr auto to() const noexcept -> Opacity { return m_to; }
  [[nodiscard]] auto evaluate(std::uint64_t time_us,
                              Opacity authored) const noexcept -> Opacity;
  auto operator==(const OpacityTrack &) const -> bool = default;

private:
  OpacityTrack(TrackId id, ObjectId target, std::uint64_t start_time_us,
               std::uint64_t duration_us, Opacity from, Opacity to) noexcept
      : m_id{std::move(id)}, m_target{std::move(target)},
        m_start_time_us{start_time_us}, m_duration_us{duration_us},
        m_from{from}, m_to{to} {}
  TrackId m_id;
  ObjectId m_target;
  std::uint64_t m_start_time_us{};
  std::uint64_t m_duration_us{};
  Opacity m_from;
  Opacity m_to;
};

namespace detail {
class DocumentBuilder;
}

class Document {
public:
  [[nodiscard]] static auto create(const DocumentId &id, CanvasExtent canvas,
                                   std::optional<Color> background = {},
                                   ResourceLimits limits = {}) noexcept
      -> std::expected<Document, SceneError>;
  Document(const Document &) noexcept = default;
  Document(Document &&other) noexcept : m_impl{other.m_impl} {}
  auto operator=(const Document &) noexcept -> Document & = default;
  auto operator=(Document &&other) noexcept -> Document & {
    m_impl = other.m_impl;
    return *this;
  }
  ~Document();

private:
  struct Impl;
  explicit Document(std::shared_ptr<const Impl> impl) noexcept
      : m_impl{std::move(impl)} {}
  std::shared_ptr<const Impl> m_impl;

  friend class detail::DocumentBuilder;
  friend struct DocumentAccess;
};

} // namespace drawforge
