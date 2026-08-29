#include "cli.hpp"

#include "sha256.hpp"

#include <drawforge/drawforge.hpp>
#include <yyjson.h>

#include <algorithm>
#include <array>
#include <charconv>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <exception>
#include <filesystem>
#include <fstream>
#include <initializer_list>
#include <ios>
#include <istream>
#include <limits>
#include <memory>
#include <new>
#include <optional>
#include <ostream>
#include <span>
#include <stdexcept>
#include <string>
#include <string_view>
#include <system_error>
#include <tuple>
#include <type_traits>
#include <utility>
#include <variant>
#include <vector>

static_assert(YYJSON_VERSION_HEX == 0x000C00,
              "DrawForge requires yyjson 0.12.0 exactly");

namespace drawforge::cli {
namespace {

constexpr std::string_view protocol{"drawforge.experimental/v1"};
constexpr std::size_t default_max_frame_bytes{8U * 1024U * 1024U};
constexpr std::size_t max_json_depth{32U};
constexpr std::size_t max_object_members{64U};
constexpr std::size_t max_array_items{65'536U};
constexpr std::size_t max_number_token_bytes{64U};
constexpr std::size_t artifact_write_chunk{64U * 1024U};

using PathComponent = std::variant<std::string, std::uint64_t>;
using ErrorPath = std::vector<PathComponent>;

enum class ErrorSource : std::uint8_t { encoding, domain, adapter };

struct ProtocolError {
  ErrorSource source{ErrorSource::encoding};
  std::string code;
  RetryAdvice retry_advice{RetryAdvice::change_request};
  std::optional<std::uint64_t> operation_index;
  ErrorPath path;
  std::string message;
  bool interrupted{};
};

class DecodeFailure final : public std::exception {
public:
  explicit DecodeFailure(ProtocolError error) : m_error{std::move(error)} {}
  [[nodiscard]] auto error() const noexcept -> const ProtocolError & {
    return m_error;
  }
  [[nodiscard]] auto what() const noexcept -> const char * override {
    return m_error.message.c_str();
  }

private:
  ProtocolError m_error;
};

auto child(ErrorPath path, std::string value) -> ErrorPath {
  path.emplace_back(std::move(value));
  return path;
}

auto child(ErrorPath path, const std::uint64_t value) -> ErrorPath {
  path.emplace_back(value);
  return path;
}

auto fixed_encoding_error(std::string code, ErrorPath path = {})
    -> ProtocolError {
  std::string message;
  if (code == "invalid_utf8")
    message = "frame is not valid UTF-8";
  else if (code == "invalid_json")
    message = "frame is not one complete JSON object";
  else if (code == "duplicate_key")
    message = "JSON object contains a duplicate key";
  else if (code == "unsupported_version")
    message = "frame uses an unsupported protocol version";
  else if (code == "unknown_field")
    message = "object contains an unknown field";
  else if (code == "missing_field")
    message = "object is missing a required field";
  else if (code == "invalid_type")
    message = "value has the wrong JSON type";
  else if (code == "nesting_limit")
    message = "JSON nesting limit was exceeded";
  else if (code == "resource_limit")
    message = "wire resource limit was exceeded";
  else
    message = "value is not accepted by the provisional contract";
  return ProtocolError{.source = ErrorSource::encoding,
                       .code = std::move(code),
                       .retry_advice = RetryAdvice::change_request,
                       .operation_index = std::nullopt,
                       .path = std::move(path),
                       .message = std::move(message),
                       .interrupted = false};
}

[[noreturn]] auto fail_fixed(std::string code, ErrorPath path = {}) -> void {
  throw DecodeFailure{fixed_encoding_error(std::move(code), std::move(path))};
}

auto value_string(yyjson_val *value) -> std::string_view {
  return {yyjson_get_str(value), yyjson_get_len(value)};
}

auto key_string(yyjson_val *key) -> std::string_view {
  return {yyjson_get_str(key), yyjson_get_len(key)};
}

auto require_object(yyjson_val *value, const ErrorPath &path) -> yyjson_val * {
  if (!yyjson_is_obj(value))
    fail_fixed("invalid_type", path);
  return value;
}

auto require_array(yyjson_val *value, const ErrorPath &path) -> yyjson_val * {
  if (!yyjson_is_arr(value))
    fail_fixed("invalid_type", path);
  return value;
}

auto require_member(yyjson_val *object, const std::string_view name,
                    const ErrorPath &path) -> yyjson_val * {
  auto *value = yyjson_obj_getn(object, name.data(), name.size());
  if (value == nullptr)
    fail_fixed("missing_field", child(path, std::string{name}));
  return value;
}

auto optional_member(yyjson_val *object, const std::string_view name)
    -> yyjson_val * {
  return yyjson_obj_getn(object, name.data(), name.size());
}

auto contains(const std::initializer_list<std::string_view> values,
              const std::string_view candidate) -> bool {
  return std::find(values.begin(), values.end(), candidate) != values.end();
}

auto validate_fields(yyjson_val *object,
                     const std::initializer_list<std::string_view> allowed,
                     const std::initializer_list<std::string_view> required,
                     const ErrorPath &path) -> void {
  require_object(object, path);
  yyjson_obj_iter iterator = yyjson_obj_iter_with(object);
  while (auto *key = yyjson_obj_iter_next(&iterator)) {
    const auto name = key_string(key);
    if (!contains(allowed, name))
      fail_fixed("unknown_field", child(path, std::string{name}));
  }
  for (const auto name : required) {
    if (optional_member(object, name) == nullptr)
      fail_fixed("missing_field", child(path, std::string{name}));
  }
}

auto scan_number_tokens(const std::string_view frame) -> void {
  bool string{};
  bool escape{};
  for (std::size_t index = 0; index < frame.size(); ++index) {
    const auto character = frame[index];
    if (string) {
      if (escape)
        escape = false;
      else if (character == '\\')
        escape = true;
      else if (character == '"')
        string = false;
      continue;
    }
    if (character == '"') {
      string = true;
      continue;
    }
    if (character != '-' && (character < '0' || character > '9'))
      continue;
    const auto start = index;
    while (index + 1U < frame.size()) {
      const auto next = frame[index + 1U];
      if ((next >= '0' && next <= '9') || next == '.' || next == 'e' ||
          next == 'E' || next == '+' || next == '-') {
        ++index;
      } else {
        break;
      }
    }
    if (index - start + 1U > max_number_token_bytes)
      fail_fixed("resource_limit");
  }
}

auto valid_utf8(const std::string_view text) noexcept -> bool {
  const auto *bytes = reinterpret_cast<const unsigned char *>(text.data());
  std::size_t index{};
  const auto continuation = [](const unsigned char value) {
    return value >= 0x80U && value <= 0xbfU;
  };
  while (index < text.size()) {
    const auto first = bytes[index++];
    if (first <= 0x7fU)
      continue;
    if (first >= 0xc2U && first <= 0xdfU) {
      if (index >= text.size() || !continuation(bytes[index++]))
        return false;
      continue;
    }
    if (first >= 0xe0U && first <= 0xefU) {
      if (index + 1U >= text.size())
        return false;
      const auto second = bytes[index++];
      const auto third = bytes[index++];
      if (!continuation(third))
        return false;
      if (first == 0xe0U) {
        if (second < 0xa0U || second > 0xbfU)
          return false;
      } else if (first == 0xedU) {
        if (second < 0x80U || second > 0x9fU)
          return false;
      } else if (!continuation(second)) {
        return false;
      }
      continue;
    }
    if (first >= 0xf0U && first <= 0xf4U) {
      if (index + 2U >= text.size())
        return false;
      const auto second = bytes[index++];
      const auto third = bytes[index++];
      const auto fourth = bytes[index++];
      if (!continuation(third) || !continuation(fourth))
        return false;
      if (first == 0xf0U) {
        if (second < 0x90U || second > 0xbfU)
          return false;
      } else if (first == 0xf4U) {
        if (second < 0x80U || second > 0x8fU)
          return false;
      } else if (!continuation(second)) {
        return false;
      }
      continue;
    }
    return false;
  }
  return true;
}

auto validate_tree(yyjson_val *value, ErrorPath path = {},
                   const std::size_t depth = 1U) -> void {
  if (!yyjson_is_ctn(value))
    return;
  if (depth > max_json_depth)
    fail_fixed("nesting_limit", path);
  if (yyjson_is_obj(value)) {
    if (yyjson_obj_size(value) > max_object_members)
      fail_fixed("resource_limit", path);
    std::vector<std::string_view> names;
    names.reserve(yyjson_obj_size(value));
    yyjson_obj_iter iterator = yyjson_obj_iter_with(value);
    while (auto *key = yyjson_obj_iter_next(&iterator)) {
      const auto name = key_string(key);
      if (std::find(names.begin(), names.end(), name) != names.end())
        fail_fixed("duplicate_key", child(path, std::string{name}));
      names.push_back(name);
      validate_tree(yyjson_obj_iter_get_val(key),
                    child(path, std::string{name}), depth + 1U);
    }
    return;
  }
  if (yyjson_arr_size(value) > max_array_items)
    fail_fixed("resource_limit", path);
  size_t index{};
  size_t maximum{};
  yyjson_val *item{};
  yyjson_arr_foreach(value, index, maximum, item) {
    validate_tree(item, child(path, static_cast<std::uint64_t>(index)),
                  depth + 1U);
  }
}

class ParsedDocument {
public:
  explicit ParsedDocument(yyjson_doc *document) noexcept
      : m_document{document} {}
  ParsedDocument(const ParsedDocument &) = delete;
  ParsedDocument(ParsedDocument &&other) noexcept
      : m_document{std::exchange(other.m_document, nullptr)} {}
  auto operator=(const ParsedDocument &) -> ParsedDocument & = delete;
  auto operator=(ParsedDocument &&) -> ParsedDocument & = delete;
  ~ParsedDocument() { yyjson_doc_free(m_document); }
  [[nodiscard]] auto root() const noexcept -> yyjson_val * {
    return yyjson_doc_get_root(m_document);
  }

private:
  yyjson_doc *m_document{};
};

auto parse_json(const std::string_view frame) -> ParsedDocument {
  if (frame.size() > default_max_frame_bytes)
    fail_fixed("resource_limit");
  if (frame.empty())
    fail_fixed("invalid_json");
  if (frame.starts_with(std::string_view{"\xef\xbb\xbf", 3U}) ||
      frame.find('\0') != std::string_view::npos)
    fail_fixed("invalid_utf8");
  if (!valid_utf8(frame))
    fail_fixed("invalid_utf8");
  scan_number_tokens(frame);

  yyjson_read_err error{};
  auto *document =
      yyjson_read_opts(const_cast<char *>(frame.data()), frame.size(),
                       YYJSON_READ_NOFLAG, nullptr, &error);
  if (document == nullptr)
    fail_fixed("invalid_json");
  ParsedDocument result{document};
  validate_tree(result.root());
  return result;
}

auto read_string(yyjson_val *value, const ErrorPath &path) -> std::string_view {
  if (!yyjson_is_str(value))
    fail_fixed("invalid_type", path);
  return value_string(value);
}

auto read_bool(yyjson_val *value, const ErrorPath &path) -> bool {
  if (!yyjson_is_bool(value))
    fail_fixed("invalid_type", path);
  return yyjson_get_bool(value);
}

auto read_uint(yyjson_val *value, const ErrorPath &path) -> std::uint64_t {
  if (yyjson_is_uint(value))
    return yyjson_get_uint(value);
  if (yyjson_is_sint(value) && yyjson_get_sint(value) >= 0)
    return static_cast<std::uint64_t>(yyjson_get_sint(value));
  fail_fixed(yyjson_is_num(value) ? "invalid_value" : "invalid_type", path);
}

auto read_double(yyjson_val *value, const ErrorPath &path) -> double {
  if (!yyjson_is_num(value))
    fail_fixed("invalid_type", path);
  double result{};
  if (yyjson_is_uint(value))
    result = static_cast<double>(yyjson_get_uint(value));
  else if (yyjson_is_sint(value))
    result = static_cast<double>(yyjson_get_sint(value));
  else
    result = yyjson_get_real(value);
  if (!std::isfinite(result))
    fail_fixed("invalid_value", path);
  return result == 0.0 ? 0.0 : result;
}

template <typename Identity>
auto read_id(yyjson_val *value, const ErrorPath &path,
             const ResourceLimits &limits) -> Identity {
  auto parsed = Identity::create(read_string(value, path), limits);
  if (!parsed)
    fail_fixed("invalid_value", path);
  return std::move(*parsed);
}

auto read_color(yyjson_val *value, const ErrorPath &path) -> Color {
  const auto text = read_string(value, path);
  if (text.size() != 9U || text.front() != '#')
    fail_fixed("invalid_value", path);
  auto digit = [&](const std::size_t index) -> std::uint8_t {
    const auto character = text[index];
    if (character >= '0' && character <= '9')
      return static_cast<std::uint8_t>(character - '0');
    if (character >= 'a' && character <= 'f')
      return static_cast<std::uint8_t>(character - 'a' + 10);
    fail_fixed("invalid_value", path);
  };
  auto byte = [&](const std::size_t index) -> std::uint8_t {
    return static_cast<std::uint8_t>((digit(index) << 4U) | digit(index + 1U));
  };
  return Color{byte(1), byte(3), byte(5), byte(7)};
}

auto read_nullable_color(yyjson_val *value, const ErrorPath &path)
    -> std::optional<Color> {
  if (yyjson_is_null(value))
    return std::nullopt;
  return read_color(value, path);
}

auto read_opacity(yyjson_val *value, const ErrorPath &path) -> Opacity {
  auto parsed = Opacity::create(read_double(value, path));
  if (!parsed)
    fail_fixed("invalid_value", path);
  return *parsed;
}

auto read_point(yyjson_val *value, const ErrorPath &path,
                const ResourceLimits &limits) -> Point {
  validate_fields(value, {"x", "y"}, {"x", "y"}, path);
  auto parsed = Point::create(
      read_double(require_member(value, "x", path), child(path, "x")),
      read_double(require_member(value, "y", path), child(path, "y")), limits);
  if (!parsed)
    fail_fixed("invalid_value", path);
  return *parsed;
}

auto read_transform(yyjson_val *value, const ErrorPath &path,
                    const ResourceLimits &limits) -> AffineTransform {
  validate_fields(value, {"a", "b", "c", "d", "e", "f"},
                  {"a", "b", "c", "d", "e", "f"}, path);
  auto parsed = AffineTransform::create(
      read_double(require_member(value, "a", path), child(path, "a")),
      read_double(require_member(value, "b", path), child(path, "b")),
      read_double(require_member(value, "c", path), child(path, "c")),
      read_double(require_member(value, "d", path), child(path, "d")),
      read_double(require_member(value, "e", path), child(path, "e")),
      read_double(require_member(value, "f", path), child(path, "f")), limits);
  if (!parsed)
    fail_fixed("invalid_value", path);
  return *parsed;
}

auto read_style(yyjson_val *value, const ErrorPath &path,
                const ResourceLimits &limits) -> Style {
  validate_fields(value, {"fill", "stroke"}, {"fill", "stroke"}, path);
  const auto fill_path = child(path, "fill");
  const auto stroke_path = child(path, "stroke");
  auto fill =
      read_nullable_color(require_member(value, "fill", path), fill_path);
  auto *stroke_value = require_member(value, "stroke", path);
  std::optional<Stroke> stroke;
  if (!yyjson_is_null(stroke_value)) {
    validate_fields(stroke_value, {"color", "width"}, {"color", "width"},
                    stroke_path);
    auto parsed = Stroke::create(
        read_color(require_member(stroke_value, "color", stroke_path),
                   child(stroke_path, "color")),
        read_double(require_member(stroke_value, "width", stroke_path),
                    child(stroke_path, "width")),
        limits);
    if (!parsed)
      fail_fixed("invalid_value", stroke_path);
    stroke = *parsed;
  }
  return Style{fill, stroke};
}

auto read_geometry(yyjson_val *value, const ErrorPath &path,
                   const ResourceLimits &limits) -> Geometry {
  require_object(value, path);
  const auto kind_path = child(path, "kind");
  const auto kind = read_string(require_member(value, "kind", path), kind_path);
  if (kind == "rectangle") {
    validate_fields(
        value, {"height", "kind", "radius_x", "radius_y", "width", "x", "y"},
        {"height", "kind", "radius_x", "radius_y", "width", "x", "y"}, path);
    auto parsed = Rectangle::create(
        read_double(require_member(value, "x", path), child(path, "x")),
        read_double(require_member(value, "y", path), child(path, "y")),
        read_double(require_member(value, "width", path), child(path, "width")),
        read_double(require_member(value, "height", path),
                    child(path, "height")),
        read_double(require_member(value, "radius_x", path),
                    child(path, "radius_x")),
        read_double(require_member(value, "radius_y", path),
                    child(path, "radius_y")),
        limits);
    if (!parsed)
      fail_fixed("invalid_value", path);
    return *parsed;
  }
  if (kind == "ellipse") {
    validate_fields(value, {"center", "kind", "radius_x", "radius_y"},
                    {"center", "kind", "radius_x", "radius_y"}, path);
    const auto center = read_point(require_member(value, "center", path),
                                   child(path, "center"), limits);
    auto parsed =
        Ellipse::create(center.x().value(), center.y().value(),
                        read_double(require_member(value, "radius_x", path),
                                    child(path, "radius_x")),
                        read_double(require_member(value, "radius_y", path),
                                    child(path, "radius_y")),
                        limits);
    if (!parsed)
      fail_fixed("invalid_value", path);
    return *parsed;
  }
  if (kind == "path") {
    validate_fields(value, {"commands", "kind"}, {"commands", "kind"}, path);
    auto *commands_value = require_array(
        require_member(value, "commands", path), child(path, "commands"));
    if (yyjson_arr_size(commands_value) == 0U)
      fail_fixed("invalid_value", child(path, "commands"));
    std::vector<PathCommand> commands;
    commands.reserve(yyjson_arr_size(commands_value));
    size_t index{};
    size_t maximum{};
    yyjson_val *command{};
    yyjson_arr_foreach(commands_value, index, maximum, command) {
      const auto command_path =
          child(child(path, "commands"), static_cast<std::uint64_t>(index));
      require_object(command, command_path);
      const auto command_kind =
          read_string(require_member(command, "kind", command_path),
                      child(command_path, "kind"));
      if (command_kind == "close") {
        validate_fields(command, {"kind"}, {"kind"}, command_path);
        commands.emplace_back(ClosePath{});
      } else if (command_kind == "move_to" || command_kind == "line_to") {
        validate_fields(command, {"kind", "point"}, {"kind", "point"},
                        command_path);
        auto point = read_point(require_member(command, "point", command_path),
                                child(command_path, "point"), limits);
        if (command_kind == "move_to")
          commands.emplace_back(MoveTo{point});
        else
          commands.emplace_back(LineTo{point});
      } else {
        fail_fixed("invalid_value", child(command_path, "kind"));
      }
    }
    auto parsed = Path::create(std::move(commands));
    if (!parsed)
      fail_fixed("invalid_value", path);
    return std::move(*parsed);
  }
  fail_fixed("invalid_value", kind_path);
}

auto read_parent(yyjson_val *value, const ErrorPath &path,
                 const ResourceLimits &limits) -> ParentRef {
  validate_fields(value, {"id", "kind"}, {"id", "kind"}, path);
  const auto kind =
      read_string(require_member(value, "kind", path), child(path, "kind"));
  if (kind == "layer")
    return read_id<LayerId>(require_member(value, "id", path),
                            child(path, "id"), limits);
  if (kind == "group")
    return read_id<ObjectId>(require_member(value, "id", path),
                             child(path, "id"), limits);
  fail_fixed("invalid_value", child(path, "kind"));
}

auto read_node(yyjson_val *value, const ErrorPath &path,
               const ResourceLimits &limits) -> NodeRef {
  validate_fields(value, {"id", "kind"}, {"id", "kind"}, path);
  const auto kind =
      read_string(require_member(value, "kind", path), child(path, "kind"));
  if (kind == "layer")
    return read_id<LayerId>(require_member(value, "id", path),
                            child(path, "id"), limits);
  if (kind == "object")
    return read_id<ObjectId>(require_member(value, "id", path),
                             child(path, "id"), limits);
  fail_fixed("invalid_value", child(path, "kind"));
}

auto read_track(yyjson_val *value, const ErrorPath &path,
                const ResourceLimits &limits) -> OpacityTrack {
  auto track_id = read_id<TrackId>(require_member(value, "track_id", path),
                                   child(path, "track_id"), limits);
  auto target =
      read_id<ObjectId>(require_member(value, "target_object_id", path),
                        child(path, "target_object_id"), limits);
  auto parsed = OpacityTrack::create(
      std::move(track_id), std::move(target),
      read_uint(require_member(value, "start_time_us", path),
                child(path, "start_time_us")),
      read_uint(require_member(value, "duration_us", path),
                child(path, "duration_us")),
      read_opacity(require_member(value, "from_opacity", path),
                   child(path, "from_opacity")),
      read_opacity(require_member(value, "to_opacity", path),
                   child(path, "to_opacity")));
  if (!parsed)
    fail_fixed("invalid_value", path);
  return std::move(*parsed);
}

auto read_operation(yyjson_val *value, const ErrorPath &path,
                    const ResourceLimits &limits) -> Operation {
  require_object(value, path);
  const auto op =
      read_string(require_member(value, "op", path), child(path, "op"));
  if (op == "create_layer") {
    validate_fields(value, {"index", "layer_id", "op", "visible"},
                    {"index", "layer_id", "op", "visible"}, path);
    return CreateLayer{
        read_id<LayerId>(require_member(value, "layer_id", path),
                         child(path, "layer_id"), limits),
        read_uint(require_member(value, "index", path), child(path, "index")),
        read_bool(require_member(value, "visible", path),
                  child(path, "visible"))};
  }
  if (op == "create_group") {
    validate_fields(
        value, {"index", "object_id", "op", "parent", "transform", "visible"},
        {"index", "object_id", "op", "parent", "transform", "visible"}, path);
    return CreateGroup{
        read_id<ObjectId>(require_member(value, "object_id", path),
                          child(path, "object_id"), limits),
        read_parent(require_member(value, "parent", path),
                    child(path, "parent"), limits),
        read_uint(require_member(value, "index", path), child(path, "index")),
        read_bool(require_member(value, "visible", path),
                  child(path, "visible")),
        read_transform(require_member(value, "transform", path),
                       child(path, "transform"), limits)};
  }
  const auto read_drawable = [&]() {
    return std::tuple{
        read_id<ObjectId>(require_member(value, "object_id", path),
                          child(path, "object_id"), limits),
        read_parent(require_member(value, "parent", path),
                    child(path, "parent"), limits),
        read_uint(require_member(value, "index", path), child(path, "index")),
        read_bool(require_member(value, "visible", path),
                  child(path, "visible")),
        read_transform(require_member(value, "transform", path),
                       child(path, "transform"), limits),
        read_style(require_member(value, "style", path), child(path, "style"),
                   limits),
        read_opacity(require_member(value, "opacity", path),
                     child(path, "opacity")),
        read_geometry(require_member(value, "geometry", path),
                      child(path, "geometry"), limits)};
  };
  if (op == "create_rectangle" || op == "create_ellipse" ||
      op == "create_path") {
    validate_fields(value,
                    {"geometry", "index", "object_id", "op", "opacity",
                     "parent", "style", "transform", "visible"},
                    {"geometry", "index", "object_id", "op", "opacity",
                     "parent", "style", "transform", "visible"},
                    path);
    auto [id, parent, index, visible, transform, style, opacity, geometry] =
        read_drawable();
    if (op == "create_rectangle" &&
        !std::holds_alternative<Rectangle>(geometry))
      fail_fixed("invalid_value", child(path, "geometry"));
    if (op == "create_ellipse" && !std::holds_alternative<Ellipse>(geometry))
      fail_fixed("invalid_value", child(path, "geometry"));
    if (op == "create_path" && !std::holds_alternative<Path>(geometry))
      fail_fixed("invalid_value", child(path, "geometry"));
    if (op == "create_rectangle")
      return CreateRectangle{
          std::move(id), std::move(parent),
          index,         visible,
          transform,     std::move(style),
          opacity,       std::get<Rectangle>(std::move(geometry))};
    if (op == "create_ellipse")
      return CreateEllipse{
          std::move(id), std::move(parent),
          index,         visible,
          transform,     std::move(style),
          opacity,       std::get<Ellipse>(std::move(geometry))};
    return CreatePath{std::move(id), std::move(parent),
                      index,         visible,
                      transform,     std::move(style),
                      opacity,       std::get<Path>(std::move(geometry))};
  }
  if (op == "create_opacity_track" || op == "set_opacity_track") {
    validate_fields(value,
                    {"duration_us", "from_opacity", "op", "start_time_us",
                     "target_object_id", "to_opacity", "track_id"},
                    {"duration_us", "from_opacity", "op", "start_time_us",
                     "target_object_id", "to_opacity", "track_id"},
                    path);
    auto track = read_track(value, path, limits);
    if (op == "create_opacity_track")
      return CreateOpacityTrack{std::move(track)};
    return SetOpacityTrack{std::move(track)};
  }
  if (op == "set_canvas_background") {
    validate_fields(value, {"background", "op"}, {"background", "op"}, path);
    return SetCanvasBackground{read_nullable_color(
        require_member(value, "background", path), child(path, "background"))};
  }
  if (op == "set_visibility") {
    validate_fields(value, {"op", "target", "visible"},
                    {"op", "target", "visible"}, path);
    return SetVisibility{read_node(require_member(value, "target", path),
                                   child(path, "target"), limits),
                         read_bool(require_member(value, "visible", path),
                                   child(path, "visible"))};
  }
  if (op == "set_transform") {
    validate_fields(value, {"object_id", "op", "transform"},
                    {"object_id", "op", "transform"}, path);
    return SetTransform{
        read_id<ObjectId>(require_member(value, "object_id", path),
                          child(path, "object_id"), limits),
        read_transform(require_member(value, "transform", path),
                       child(path, "transform"), limits)};
  }
  if (op == "set_geometry") {
    validate_fields(value, {"geometry", "object_id", "op"},
                    {"geometry", "object_id", "op"}, path);
    return SetGeometry{
        read_id<ObjectId>(require_member(value, "object_id", path),
                          child(path, "object_id"), limits),
        read_geometry(require_member(value, "geometry", path),
                      child(path, "geometry"), limits)};
  }
  if (op == "set_style") {
    validate_fields(value, {"object_id", "op", "style"},
                    {"object_id", "op", "style"}, path);
    return SetStyle{read_id<ObjectId>(require_member(value, "object_id", path),
                                      child(path, "object_id"), limits),
                    read_style(require_member(value, "style", path),
                               child(path, "style"), limits)};
  }
  if (op == "set_opacity") {
    validate_fields(value, {"object_id", "op", "opacity"},
                    {"object_id", "op", "opacity"}, path);
    return SetOpacity{
        read_id<ObjectId>(require_member(value, "object_id", path),
                          child(path, "object_id"), limits),
        read_opacity(require_member(value, "opacity", path),
                     child(path, "opacity"))};
  }
  if (op == "reparent_object") {
    validate_fields(value, {"index", "object_id", "op", "parent"},
                    {"index", "object_id", "op", "parent"}, path);
    return ReparentObject{
        read_id<ObjectId>(require_member(value, "object_id", path),
                          child(path, "object_id"), limits),
        read_parent(require_member(value, "parent", path),
                    child(path, "parent"), limits),
        read_uint(require_member(value, "index", path), child(path, "index"))};
  }
  if (op == "reorder_object") {
    validate_fields(value, {"index", "object_id", "op"},
                    {"index", "object_id", "op"}, path);
    return ReorderObject{
        read_id<ObjectId>(require_member(value, "object_id", path),
                          child(path, "object_id"), limits),
        read_uint(require_member(value, "index", path), child(path, "index"))};
  }
  fail_fixed("invalid_value", child(path, "op"));
}

struct CreateRequest {
  DocumentId document_id;
  CanvasExtent canvas;
  std::optional<Color> background;
  ResourceLimits limits;
};

using Query = std::variant<SummaryQuery, StructureQuery, SelectedObjectsQuery,
                           BoundsQuery>;
struct InspectRequest {
  DocumentId document_id;
  Query query;
};
struct ApplyRequest {
  ApplyMode mode{};
  Transaction transaction;
};
enum class ArtifactFormat : std::uint8_t { rgba8, png };
struct RenderRequest {
  DocumentId document_id;
  Revision expected_revision;
  std::uint64_t time_us{};
  ArtifactFormat format{};
  AssetId artifact_id;
};
using Request =
    std::variant<CreateRequest, InspectRequest, ApplyRequest, RenderRequest>;

auto read_limits(yyjson_val *value, const ErrorPath &path) -> ResourceLimits {
  validate_fields(
      value,
      {"max_canvas_dimension", "max_canvas_pixels", "max_identifier_bytes",
       "max_nesting_depth", "max_numeric_magnitude", "max_output_bytes",
       "max_scene_nodes", "max_text_bytes", "max_transaction_operations"},
      {"max_canvas_dimension", "max_canvas_pixels", "max_identifier_bytes",
       "max_nesting_depth", "max_numeric_magnitude", "max_output_bytes",
       "max_scene_nodes", "max_text_bytes", "max_transaction_operations"},
      path);
  const auto as_u32 = [&](const std::string_view name) {
    const auto number = read_uint(require_member(value, name, path),
                                  child(path, std::string{name}));
    if (number > std::numeric_limits<std::uint32_t>::max())
      fail_fixed("invalid_value", child(path, std::string{name}));
    return static_cast<std::uint32_t>(number);
  };
  ResourceLimitRequest requested{
      .max_identifier_bytes = as_u32("max_identifier_bytes"),
      .max_text_bytes = as_u32("max_text_bytes"),
      .max_numeric_magnitude =
          read_double(require_member(value, "max_numeric_magnitude", path),
                      child(path, "max_numeric_magnitude")),
      .max_canvas_dimension = as_u32("max_canvas_dimension"),
      .max_canvas_pixels =
          read_uint(require_member(value, "max_canvas_pixels", path),
                    child(path, "max_canvas_pixels")),
      .max_scene_nodes =
          read_uint(require_member(value, "max_scene_nodes", path),
                    child(path, "max_scene_nodes")),
      .max_transaction_operations =
          read_uint(require_member(value, "max_transaction_operations", path),
                    child(path, "max_transaction_operations")),
      .max_output_bytes =
          read_uint(require_member(value, "max_output_bytes", path),
                    child(path, "max_output_bytes")),
      .max_nesting_depth = as_u32("max_nesting_depth"),
  };
  auto parsed = ResourceLimits::create(requested);
  if (!parsed)
    fail_fixed("invalid_value", path);
  return *parsed;
}

auto read_query(yyjson_val *value, const ErrorPath &path,
                const ResourceLimits &limits) -> InspectRequest {
  require_object(value, path);
  const auto kind =
      read_string(require_member(value, "kind", path), child(path, "kind"));
  auto document_id =
      read_id<DocumentId>(require_member(value, "document_id", path),
                          child(path, "document_id"), limits);
  if (kind == "document_summary") {
    validate_fields(value, {"document_id", "kind"}, {"document_id", "kind"},
                    path);
    return {std::move(document_id), SummaryQuery{}};
  }
  if (kind == "structure") {
    validate_fields(
        value, {"document_id", "kind", "max_depth", "max_nodes", "root"},
        {"document_id", "kind", "max_depth", "max_nodes", "root"}, path);
    const auto root_path = child(path, "root");
    auto *root_value = require_member(value, "root", path);
    validate_fields(root_value, {"id", "kind"}, {"kind"}, root_path);
    const auto root_kind =
        read_string(require_member(root_value, "kind", root_path),
                    child(root_path, "kind"));
    StructureRoot root;
    if (root_kind == "document") {
      validate_fields(root_value, {"kind"}, {"kind"}, root_path);
      root = std::monostate{};
    } else if (root_kind == "layer") {
      validate_fields(root_value, {"id", "kind"}, {"id", "kind"}, root_path);
      root = read_id<LayerId>(require_member(root_value, "id", root_path),
                              child(root_path, "id"), limits);
    } else if (root_kind == "group") {
      validate_fields(root_value, {"id", "kind"}, {"id", "kind"}, root_path);
      root = read_id<ObjectId>(require_member(root_value, "id", root_path),
                               child(root_path, "id"), limits);
    } else {
      fail_fixed("invalid_value", child(root_path, "kind"));
    }
    const auto depth = read_uint(require_member(value, "max_depth", path),
                                 child(path, "max_depth"));
    if (depth > std::numeric_limits<std::uint32_t>::max())
      fail_fixed("invalid_value", child(path, "max_depth"));
    auto parsed = StructureQuery::create(
        std::move(root), static_cast<std::uint32_t>(depth),
        read_uint(require_member(value, "max_nodes", path),
                  child(path, "max_nodes")));
    if (!parsed)
      fail_fixed("invalid_value", path);
    return {std::move(document_id), std::move(*parsed)};
  }
  if (kind == "selected_objects") {
    validate_fields(value, {"document_id", "fields", "kind", "object_ids"},
                    {"document_id", "fields", "kind", "object_ids"}, path);
    auto *ids_value = require_array(require_member(value, "object_ids", path),
                                    child(path, "object_ids"));
    auto *fields_value = require_array(require_member(value, "fields", path),
                                       child(path, "fields"));
    if (yyjson_arr_size(ids_value) == 0U ||
        yyjson_arr_size(fields_value) == 0U ||
        yyjson_arr_size(fields_value) > 8U)
      fail_fixed("invalid_value", path);
    std::vector<ObjectId> ids;
    std::vector<SelectedField> fields;
    size_t index{};
    size_t maximum{};
    yyjson_val *item{};
    yyjson_arr_foreach(ids_value, index, maximum, item) {
      ids.push_back(read_id<ObjectId>(
          item,
          child(child(path, "object_ids"), static_cast<std::uint64_t>(index)),
          limits));
    }
    yyjson_arr_foreach(fields_value, index, maximum, item) {
      const auto field_path =
          child(child(path, "fields"), static_cast<std::uint64_t>(index));
      const auto name = read_string(item, field_path);
      if (name == "kind")
        fields.push_back(SelectedField::kind);
      else if (name == "parent_order")
        fields.push_back(SelectedField::parent_order);
      else if (name == "visibility")
        fields.push_back(SelectedField::visibility);
      else if (name == "transform")
        fields.push_back(SelectedField::transform);
      else if (name == "geometry")
        fields.push_back(SelectedField::geometry);
      else if (name == "style")
        fields.push_back(SelectedField::style);
      else if (name == "opacity")
        fields.push_back(SelectedField::opacity);
      else if (name == "opacity_track")
        fields.push_back(SelectedField::opacity_track);
      else
        fail_fixed("invalid_value", field_path);
    }
    auto parsed =
        SelectedObjectsQuery::create(std::move(ids), std::move(fields));
    if (!parsed)
      fail_fixed("invalid_value", path);
    return {std::move(document_id), std::move(*parsed)};
  }
  if (kind == "bounds") {
    validate_fields(
        value, {"document_id", "kind", "projections", "targets", "time_us"},
        {"document_id", "kind", "projections", "targets", "time_us"}, path);
    auto *targets_value = require_array(require_member(value, "targets", path),
                                        child(path, "targets"));
    auto *projections_value = require_array(
        require_member(value, "projections", path), child(path, "projections"));
    if (yyjson_arr_size(targets_value) == 0U ||
        yyjson_arr_size(projections_value) == 0U ||
        yyjson_arr_size(projections_value) > 3U)
      fail_fixed("invalid_value", path);
    std::vector<BoundsTarget> targets;
    std::vector<BoundsProjection> projections;
    size_t index{};
    size_t maximum{};
    yyjson_val *item{};
    yyjson_arr_foreach(targets_value, index, maximum, item) {
      auto node = read_node(
          item,
          child(child(path, "targets"), static_cast<std::uint64_t>(index)),
          limits);
      std::visit(
          [&](auto &&identity) { targets.emplace_back(std::move(identity)); },
          std::move(node));
    }
    yyjson_arr_foreach(projections_value, index, maximum, item) {
      const auto projection_path =
          child(child(path, "projections"), static_cast<std::uint64_t>(index));
      const auto name = read_string(item, projection_path);
      if (name == "local_geometry")
        projections.push_back(BoundsProjection::local_geometry);
      else if (name == "document_geometry")
        projections.push_back(BoundsProjection::document_geometry);
      else if (name == "document_painted")
        projections.push_back(BoundsProjection::document_painted);
      else
        fail_fixed("invalid_value", projection_path);
    }
    auto parsed =
        BoundsQuery::create(std::move(targets), std::move(projections),
                            read_uint(require_member(value, "time_us", path),
                                      child(path, "time_us")));
    if (!parsed)
      fail_fixed("invalid_value", path);
    return {std::move(document_id), std::move(*parsed)};
  }
  fail_fixed("invalid_value", child(path, "kind"));
}

auto decode_request(yyjson_val *root, const ResourceLimits &active_limits)
    -> Request {
  validate_fields(root, {"protocol", "request"}, {"protocol", "request"}, {});
  const auto version = read_string(require_member(root, "protocol", {}),
                                   {PathComponent{std::string{"protocol"}}});
  if (version != protocol)
    fail_fixed("unsupported_version", {PathComponent{std::string{"protocol"}}});
  auto *request = require_member(root, "request", {});
  const ErrorPath request_path{PathComponent{std::string{"request"}}};
  require_object(request, request_path);
  const auto kind = read_string(require_member(request, "kind", request_path),
                                child(request_path, "kind"));
  const auto hard = ResourceLimits::create(hard_resource_limit_values);
  if (!hard)
    fail_fixed("resource_limit");
  const auto hard_limits = *hard;
  if (kind == "create_document") {
    validate_fields(
        request, {"background", "canvas", "document_id", "kind", "limits"},
        {"background", "canvas", "document_id", "kind"}, request_path);
    auto limits = optional_member(request, "limits") == nullptr
                      ? ResourceLimits{}
                      : read_limits(optional_member(request, "limits"),
                                    child(request_path, "limits"));
    auto document_id = read_id<DocumentId>(
        require_member(request, "document_id", request_path),
        child(request_path, "document_id"), hard_limits);
    if (document_id.value().size() > limits.max_identifier_bytes())
      fail_fixed("resource_limit", child(request_path, "document_id"));
    auto *canvas_value = require_member(request, "canvas", request_path);
    const auto canvas_path = child(request_path, "canvas");
    validate_fields(canvas_value, {"height", "width"}, {"height", "width"},
                    canvas_path);
    auto canvas = CanvasExtent::create(
        read_uint(require_member(canvas_value, "width", canvas_path),
                  child(canvas_path, "width")),
        read_uint(require_member(canvas_value, "height", canvas_path),
                  child(canvas_path, "height")),
        limits);
    if (!canvas)
      fail_fixed("invalid_value", canvas_path);
    return CreateRequest{
        std::move(document_id), *canvas,
        read_nullable_color(require_member(request, "background", request_path),
                            child(request_path, "background")),
        limits};
  }
  if (kind == "inspect") {
    validate_fields(request, {"kind", "query"}, {"kind", "query"},
                    request_path);
    return read_query(require_member(request, "query", request_path),
                      child(request_path, "query"), active_limits);
  }
  if (kind == "apply") {
    validate_fields(request, {"kind", "mode", "transaction"},
                    {"kind", "mode", "transaction"}, request_path);
    const auto mode_name =
        read_string(require_member(request, "mode", request_path),
                    child(request_path, "mode"));
    ApplyMode mode{};
    if (mode_name == "dry_run")
      mode = ApplyMode::dry_run;
    else if (mode_name == "commit")
      mode = ApplyMode::commit;
    else
      fail_fixed("invalid_value", child(request_path, "mode"));
    auto *transaction_value =
        require_member(request, "transaction", request_path);
    const auto transaction_path = child(request_path, "transaction");
    validate_fields(
        transaction_value,
        {"body", "document_id", "expected_revision", "transaction_id"},
        {"body", "document_id", "expected_revision", "transaction_id"},
        transaction_path);
    auto *body_value =
        require_member(transaction_value, "body", transaction_path);
    const auto body_path = child(transaction_path, "body");
    require_object(body_value, body_path);
    const auto body_kind =
        read_string(require_member(body_value, "kind", body_path),
                    child(body_path, "kind"));
    TransactionBody body;
    if (body_kind == "operations") {
      validate_fields(body_value, {"kind", "operations"},
                      {"kind", "operations"}, body_path);
      auto *operations_value =
          require_array(require_member(body_value, "operations", body_path),
                        child(body_path, "operations"));
      if (yyjson_arr_size(operations_value) == 0U ||
          yyjson_arr_size(operations_value) > 4'096U)
        fail_fixed("invalid_value", child(body_path, "operations"));
      std::vector<Operation> operations;
      operations.reserve(yyjson_arr_size(operations_value));
      size_t index{};
      size_t maximum{};
      yyjson_val *operation{};
      yyjson_arr_foreach(operations_value, index, maximum, operation) {
        operations.push_back(
            read_operation(operation,
                           child(child(body_path, "operations"),
                                 static_cast<std::uint64_t>(index)),
                           active_limits));
      }
      body = OperationBatch{std::move(operations)};
    } else if (body_kind == "undo" || body_kind == "redo") {
      validate_fields(body_value, {"kind"}, {"kind"}, body_path);
      body = body_kind == "undo" ? TransactionBody{Undo{}}
                                 : TransactionBody{Redo{}};
    } else {
      fail_fixed("invalid_value", child(body_path, "kind"));
    }
    return ApplyRequest{
        mode, Transaction{
                  read_id<DocumentId>(
                      require_member(transaction_value, "document_id",
                                     transaction_path),
                      child(transaction_path, "document_id"), active_limits),
                  Revision{read_uint(
                      require_member(transaction_value, "expected_revision",
                                     transaction_path),
                      child(transaction_path, "expected_revision"))},
                  read_id<TransactionId>(
                      require_member(transaction_value, "transaction_id",
                                     transaction_path),
                      child(transaction_path, "transaction_id"), active_limits),
                  std::move(body)}};
  }
  if (kind == "render") {
    validate_fields(request,
                    {"artifact_id", "document_id", "expected_revision",
                     "format", "kind", "time_us"},
                    {"artifact_id", "document_id", "expected_revision",
                     "format", "kind", "time_us"},
                    request_path);
    const auto format_name =
        read_string(require_member(request, "format", request_path),
                    child(request_path, "format"));
    ArtifactFormat format{};
    if (format_name == "rgba8")
      format = ArtifactFormat::rgba8;
    else if (format_name == "png")
      format = ArtifactFormat::png;
    else
      fail_fixed("invalid_value", child(request_path, "format"));
    return RenderRequest{
        read_id<DocumentId>(
            require_member(request, "document_id", request_path),
            child(request_path, "document_id"), active_limits),
        Revision{read_uint(
            require_member(request, "expected_revision", request_path),
            child(request_path, "expected_revision"))},
        read_uint(require_member(request, "time_us", request_path),
                  child(request_path, "time_us")),
        format,
        read_id<AssetId>(require_member(request, "artifact_id", request_path),
                         child(request_path, "artifact_id"), active_limits)};
  }
  fail_fixed("invalid_value", child(request_path, "kind"));
}

auto json_string(const std::string_view value) -> std::string {
  std::string result;
  result.reserve(value.size() + 2U);
  result.push_back('"');
  constexpr char hex[] = "0123456789abcdef";
  for (const auto raw : value) {
    const auto character = static_cast<unsigned char>(raw);
    switch (character) {
    case '"':
      result += "\\\"";
      break;
    case '\\':
      result += "\\\\";
      break;
    case '\b':
      result += "\\b";
      break;
    case '\f':
      result += "\\f";
      break;
    case '\n':
      result += "\\n";
      break;
    case '\r':
      result += "\\r";
      break;
    case '\t':
      result += "\\t";
      break;
    default:
      if (character < 0x20U) {
        result += "\\u00";
        result.push_back(hex[character >> 4U]);
        result.push_back(hex[character & 0x0fU]);
      } else {
        result.push_back(static_cast<char>(character));
      }
      break;
    }
  }
  result.push_back('"');
  return result;
}

auto json_uint(const std::uint64_t value) -> std::string {
  std::array<char, 32> buffer{};
  const auto [end, error] =
      std::to_chars(buffer.data(), buffer.data() + buffer.size(), value);
  if (error != std::errc{})
    throw std::runtime_error{"integer encoding failed"};
  return {buffer.data(), end};
}

auto json_double(double value) -> std::string {
  if (value == 0.0)
    value = 0.0;
  std::array<char, 64> buffer{};
  const auto [end, error] =
      std::to_chars(buffer.data(), buffer.data() + buffer.size(), value,
                    std::chars_format::general);
  if (error != std::errc{})
    throw std::runtime_error{"number encoding failed"};
  std::string result{buffer.data(), end};
  if (result.find_first_of(".eE") == std::string::npos)
    result += ".0";
  return result;
}

auto json_bool(const bool value) -> std::string {
  return value ? "true" : "false";
}

using JsonMembers = std::vector<std::pair<std::string, std::string>>;

auto json_object(JsonMembers members) -> std::string {
  std::sort(members.begin(), members.end(),
            [](const auto &left, const auto &right) {
              return left.first < right.first;
            });
  std::string result{"{"};
  bool first = true;
  for (auto &[name, value] : members) {
    if (!first)
      result.push_back(',');
    first = false;
    result += json_string(name);
    result.push_back(':');
    result += value;
  }
  result.push_back('}');
  return result;
}

auto json_array(std::vector<std::string> values) -> std::string {
  std::string result{"["};
  bool first = true;
  for (auto &value : values) {
    if (!first)
      result.push_back(',');
    first = false;
    result += value;
  }
  result.push_back(']');
  return result;
}

auto encode_color(const Color color) -> std::string {
  constexpr char hex[] = "0123456789abcdef";
  std::array<char, 9> value{'#', '0', '0', '0', '0', '0', '0', '0', '0'};
  const auto put = [&](const std::size_t offset, const std::uint8_t byte) {
    value[offset] = hex[byte >> 4U];
    value[offset + 1U] = hex[byte & 0x0fU];
  };
  put(1U, color.red);
  put(3U, color.green);
  put(5U, color.blue);
  put(7U, color.alpha);
  return json_string(std::string_view{value.data(), value.size()});
}

auto encode_point(const Point point) -> std::string {
  return json_object({{"x", json_double(point.x().value())},
                      {"y", json_double(point.y().value())}});
}

auto encode_transform(const AffineTransform transform) -> std::string {
  return json_object({{"a", json_double(transform.a().value())},
                      {"b", json_double(transform.b().value())},
                      {"c", json_double(transform.c().value())},
                      {"d", json_double(transform.d().value())},
                      {"e", json_double(transform.e().value())},
                      {"f", json_double(transform.f().value())}});
}

auto encode_style(const Style &style) -> std::string {
  auto fill = style.fill() ? encode_color(*style.fill()) : std::string{"null"};
  auto stroke = std::string{"null"};
  if (style.stroke()) {
    stroke =
        json_object({{"color", encode_color(style.stroke()->color())},
                     {"width", json_double(style.stroke()->width().value())}});
  }
  return json_object(
      {{"fill", std::move(fill)}, {"stroke", std::move(stroke)}});
}

auto encode_geometry(const Geometry &geometry) -> std::string {
  return std::visit(
      [](const auto &value) -> std::string {
        using Value = std::remove_cvref_t<decltype(value)>;
        if constexpr (std::is_same_v<Value, Rectangle>) {
          return json_object(
              {{"height", json_double(value.height().value())},
               {"kind", json_string("rectangle")},
               {"radius_x", json_double(value.radius_x().value())},
               {"radius_y", json_double(value.radius_y().value())},
               {"width", json_double(value.width().value())},
               {"x", json_double(value.origin().x().value())},
               {"y", json_double(value.origin().y().value())}});
        } else if constexpr (std::is_same_v<Value, Ellipse>) {
          return json_object(
              {{"center", encode_point(value.center())},
               {"kind", json_string("ellipse")},
               {"radius_x", json_double(value.radius_x().value())},
               {"radius_y", json_double(value.radius_y().value())}});
        } else {
          std::vector<std::string> commands;
          commands.reserve(value.commands().size());
          for (const auto &command : value.commands()) {
            commands.push_back(std::visit(
                [](const auto &item) -> std::string {
                  using Command = std::remove_cvref_t<decltype(item)>;
                  if constexpr (std::is_same_v<Command, ClosePath>)
                    return json_object({{"kind", json_string("close")}});
                  else
                    return json_object(
                        {{"kind", json_string(std::is_same_v<Command, MoveTo>
                                                  ? "move_to"
                                                  : "line_to")},
                         {"point", encode_point(item.point)}});
                },
                command));
          }
          return json_object({{"commands", json_array(std::move(commands))},
                              {"kind", json_string("path")}});
        }
      },
      geometry);
}

auto encode_parent(const ParentRef &parent) -> std::string {
  return std::visit(
      [](const auto &identity) {
        using Identity = std::remove_cvref_t<decltype(identity)>;
        return json_object(
            {{"id", json_string(identity.value())},
             {"kind",
              json_string(std::is_same_v<Identity, LayerId> ? "layer"
                                                            : "group")}});
      },
      parent);
}

auto encode_node(const NodeRef &node) -> std::string {
  return std::visit(
      [](const auto &identity) {
        using Identity = std::remove_cvref_t<decltype(identity)>;
        return json_object(
            {{"id", json_string(identity.value())},
             {"kind",
              json_string(std::is_same_v<Identity, LayerId> ? "layer"
                                                            : "object")}});
      },
      node);
}

auto encode_bounds(const Bounds &bounds) -> std::string {
  return json_object({{"height", json_double(bounds.height().value())},
                      {"width", json_double(bounds.width().value())},
                      {"x", json_double(bounds.x().value())},
                      {"y", json_double(bounds.y().value())}});
}

auto encode_limits(const ResourceLimits &limits) -> std::string {
  return json_object({
      {"max_canvas_dimension", json_uint(limits.max_canvas_dimension())},
      {"max_canvas_pixels", json_uint(limits.max_canvas_pixels())},
      {"max_identifier_bytes", json_uint(limits.max_identifier_bytes())},
      {"max_nesting_depth", json_uint(limits.max_nesting_depth())},
      {"max_numeric_magnitude", json_double(limits.max_numeric_magnitude())},
      {"max_output_bytes", json_uint(limits.max_output_bytes())},
      {"max_scene_nodes", json_uint(limits.max_scene_nodes())},
      {"max_text_bytes", json_uint(limits.max_text_bytes())},
      {"max_transaction_operations",
       json_uint(limits.max_transaction_operations())},
  });
}

auto encode_summary(const DocumentSummary &summary) -> std::string {
  return json_object({
      {"background",
       summary.background ? encode_color(*summary.background) : "null"},
      {"canvas", json_object({{"height", json_uint(summary.canvas.height())},
                              {"width", json_uint(summary.canvas.width())}})},
      {"document_id", json_string(summary.document_id.value())},
      {"layer_count", json_uint(summary.layer_count)},
      {"limits", encode_limits(summary.limits)},
      {"object_count", json_uint(summary.object_count)},
      {"revision", json_uint(summary.revision.value())},
      {"track_count", json_uint(summary.track_count)},
  });
}

auto node_kind_name(const NodeKind kind) -> std::string_view {
  switch (kind) {
  case NodeKind::layer:
    return "layer";
  case NodeKind::group:
    return "group";
  case NodeKind::rectangle:
    return "rectangle";
  case NodeKind::ellipse:
    return "ellipse";
  case NodeKind::path:
    return "path";
  }
  return "path";
}

auto object_kind_name(const ObjectKind kind) -> std::string_view {
  switch (kind) {
  case ObjectKind::group:
    return "group";
  case ObjectKind::rectangle:
    return "rectangle";
  case ObjectKind::ellipse:
    return "ellipse";
  case ObjectKind::path:
    return "path";
  }
  return "path";
}

auto projection_name(const BoundsProjection projection) -> std::string_view {
  switch (projection) {
  case BoundsProjection::local_geometry:
    return "local_geometry";
  case BoundsProjection::document_geometry:
    return "document_geometry";
  case BoundsProjection::document_painted:
    return "document_painted";
  }
  return "document_painted";
}

auto encode_query_result(const DocumentSummary &result) -> std::string {
  return json_object({{"kind", json_string("document_summary")},
                      {"summary", encode_summary(result)}});
}

auto encode_query_result(const StructureResult &result) -> std::string {
  std::vector<std::string> nodes;
  nodes.reserve(result.nodes.size());
  for (const auto &node : result.nodes) {
    nodes.push_back(json_object({
        {"child_count", json_uint(node.child_count)},
        {"identity", encode_node(node.identity)},
        {"node_kind", json_string(node_kind_name(node.kind))},
        {"parent", node.parent ? encode_parent(*node.parent) : "null"},
        {"sibling_index", json_uint(node.sibling_index)},
        {"visible", json_bool(node.visible)},
    }));
  }
  return json_object({{"document_id", json_string(result.document_id.value())},
                      {"kind", json_string("structure")},
                      {"nodes", json_array(std::move(nodes))},
                      {"revision", json_uint(result.revision.value())}});
}

auto field_requested(const SelectedObjectsQuery &query,
                     const SelectedField field) -> bool {
  return std::find(query.fields().begin(), query.fields().end(), field) !=
         query.fields().end();
}

auto encode_track(const OpacityTrackMetadata &track) -> std::string {
  return json_object({{"duration_us", json_uint(track.duration_us)},
                      {"from_opacity", json_double(track.from.value())},
                      {"start_time_us", json_uint(track.start_time_us)},
                      {"to_opacity", json_double(track.to.value())},
                      {"track_id", json_string(track.id.value())}});
}

auto encode_query_result(const SelectedObjectsResult &result,
                         const SelectedObjectsQuery &query) -> std::string {
  std::vector<std::string> objects;
  objects.reserve(result.objects.size());
  for (const auto &object : result.objects) {
    JsonMembers fields;
    if (object.geometry)
      fields.emplace_back("geometry", encode_geometry(*object.geometry));
    if (object.kind)
      fields.emplace_back("kind", json_string(object_kind_name(*object.kind)));
    if (object.opacity)
      fields.emplace_back("opacity", json_double(object.opacity->value()));
    if (field_requested(query, SelectedField::opacity_track))
      fields.emplace_back(
          "opacity_track",
          object.opacity_track ? encode_track(*object.opacity_track) : "null");
    if (object.parent_order) {
      fields.emplace_back("parent", encode_parent(object.parent_order->parent));
      fields.emplace_back("sibling_index",
                          json_uint(object.parent_order->sibling_index));
    }
    if (object.style)
      fields.emplace_back("style", encode_style(*object.style));
    if (object.transform)
      fields.emplace_back("transform", encode_transform(*object.transform));
    if (object.visibility)
      fields.emplace_back("visible", json_bool(*object.visibility));
    objects.push_back(
        json_object({{"fields", json_object(std::move(fields))},
                     {"object_id", json_string(object.object_id.value())}}));
  }
  return json_object({{"document_id", json_string(result.document_id.value())},
                      {"kind", json_string("selected_objects")},
                      {"objects", json_array(std::move(objects))},
                      {"revision", json_uint(result.revision.value())}});
}

auto encode_query_result(const BoundsResult &result) -> std::string {
  std::vector<std::string> items;
  items.reserve(result.items.size());
  for (const auto &item : result.items) {
    std::vector<std::string> projections;
    projections.reserve(item.projections.size());
    for (const auto &projection : item.projections) {
      projections.push_back(json_object(
          {{"bounds",
            projection.bounds ? encode_bounds(*projection.bounds) : "null"},
           {"projection",
            json_string(projection_name(projection.projection))}}));
    }
    const NodeRef target = std::visit(
        [](const auto &value) -> NodeRef { return value; }, item.target);
    items.push_back(
        json_object({{"projections", json_array(std::move(projections))},
                     {"target", encode_node(target)}}));
  }
  return json_object({{"document_id", json_string(result.document_id.value())},
                      {"items", json_array(std::move(items))},
                      {"kind", json_string("bounds")},
                      {"revision", json_uint(result.revision.value())}});
}

auto encode_identity(const IdentityRef &identity) -> std::string {
  return std::visit(
      [](const auto &value) {
        using Identity = std::remove_cvref_t<decltype(value)>;
        std::string_view kind;
        if constexpr (std::is_same_v<Identity, DocumentId>)
          kind = "document";
        else if constexpr (std::is_same_v<Identity, LayerId>)
          kind = "layer";
        else if constexpr (std::is_same_v<Identity, ObjectId>)
          kind = "object";
        else
          kind = "track";
        return json_object(
            {{"id", json_string(value.value())}, {"kind", json_string(kind)}});
      },
      identity);
}

auto encode_field_path(const FieldPath &path) -> std::string {
  std::vector<std::string> values;
  values.reserve(path.segments().size());
  for (const auto &segment : path.segments()) {
    values.push_back(std::visit(
        [](const auto value) {
          using Value = std::remove_cvref_t<decltype(value)>;
          if constexpr (std::is_same_v<Value, TransactionField>)
            return json_string(transaction_field_name(value));
          else
            return json_uint(value.value);
        },
        segment));
  }
  return json_array(std::move(values));
}

auto disposition_name(const TransactionDisposition disposition)
    -> std::string_view {
  switch (disposition) {
  case TransactionDisposition::dry_run:
    return "dry_run";
  case TransactionDisposition::committed:
    return "committed";
  case TransactionDisposition::replayed:
    return "replayed";
  }
  return "committed";
}

auto encode_transaction_result(const TransactionResult &result) -> std::string {
  const auto &receipt = result.receipt;
  std::vector<std::string> created;
  std::vector<std::string> changed;
  std::vector<std::string> warnings;
  for (const auto &identity : receipt.created())
    created.push_back(encode_identity(identity));
  for (const auto &identity : receipt.changed())
    changed.push_back(encode_identity(identity));
  for (const auto &warning : receipt.warnings()) {
    warnings.push_back(json_object({
        {"code", json_string(transaction_warning_code_name(warning.code))},
        {"field_path", encode_field_path(warning.field_path)},
        {"message", json_string(warning.message)},
        {"operation_index", warning.operation_index
                                ? json_uint(*warning.operation_index)
                                : "null"},
    }));
  }
  auto encoded_receipt = json_object({
      {"base_revision", json_uint(receipt.base_revision().value())},
      {"changed", json_array(std::move(changed))},
      {"created", json_array(std::move(created))},
      {"dirty_bounds", receipt.dirty_bounds()
                           ? encode_bounds(*receipt.dirty_bounds())
                           : "null"},
      {"document_id", json_string(receipt.document_id().value())},
      {"result_revision", json_uint(receipt.result_revision().value())},
      {"transaction_id", json_string(receipt.transaction_id().value())},
      {"warnings", json_array(std::move(warnings))},
  });
  return json_object(
      {{"disposition", json_string(disposition_name(result.disposition))},
       {"kind", json_string("transaction")},
       {"receipt", std::move(encoded_receipt)}});
}

auto encode_path(const ErrorPath &path) -> std::string {
  std::vector<std::string> components;
  components.reserve(path.size());
  for (const auto &component : path) {
    components.push_back(std::visit(
        [](const auto &value) {
          using Value = std::remove_cvref_t<decltype(value)>;
          if constexpr (std::is_same_v<Value, std::string>)
            return json_string(value);
          else
            return json_uint(value);
        },
        component));
  }
  return json_array(std::move(components));
}

auto source_name(const ErrorSource source) -> std::string_view {
  switch (source) {
  case ErrorSource::encoding:
    return "encoding";
  case ErrorSource::domain:
    return "domain";
  case ErrorSource::adapter:
    return "adapter";
  }
  return "adapter";
}

auto encode_error_frame(const ProtocolError &error) -> std::string {
  JsonMembers members{
      {"code", json_string(error.code)},
      {"field_path", encode_path(error.path)},
      {"message", json_string(error.message)},
      {"operation_index",
       error.operation_index ? json_uint(*error.operation_index) : "null"},
      {"retry_advice", json_string(retry_advice_name(error.retry_advice))},
      {"source", json_string(source_name(error.source))},
  };
  if (error.code == "unsupported_version")
    members.emplace_back("supported_versions",
                         json_array({json_string(protocol)}));
  return json_object({{"error", json_object(std::move(members))},
                      {"protocol", json_string(protocol)},
                      {"status", json_string("error")}}) +
         "\n";
}

auto encode_success_frame(std::string result) -> std::string {
  return json_object({{"protocol", json_string(protocol)},
                      {"result", std::move(result)},
                      {"status", json_string("ok")}}) +
         "\n";
}

auto retry_for_scene(const SceneErrorCode code) -> RetryAdvice {
  if (code == SceneErrorCode::allocation_failure)
    return RetryAdvice::same_request;
  if (code == SceneErrorCode::arithmetic_overflow)
    return RetryAdvice::not_retryable;
  return RetryAdvice::change_request;
}

auto domain_error(const SceneError &error, ErrorPath path = {})
    -> ProtocolError {
  return ProtocolError{.source = ErrorSource::domain,
                       .code = std::string{scene_error_code_name(error.code)},
                       .retry_advice = retry_for_scene(error.code),
                       .operation_index = std::nullopt,
                       .path = std::move(path),
                       .message = std::string{error.message},
                       .interrupted = false};
}

auto domain_error(const TransactionError &error) -> ProtocolError {
  ErrorPath path;
  for (const auto &segment : error.field_path.segments()) {
    std::visit(
        [&](const auto value) {
          using Value = std::remove_cvref_t<decltype(value)>;
          if constexpr (std::is_same_v<Value, TransactionField>)
            path.emplace_back(std::string{transaction_field_name(value)});
          else
            path.emplace_back(value.value);
        },
        segment);
  }
  return ProtocolError{
      .source = ErrorSource::domain,
      .code = std::string{transaction_error_code_name(error.code)},
      .retry_advice = error.retry_advice,
      .operation_index = error.operation_index,
      .path = std::move(path),
      .message = std::string{error.message},
      .interrupted = error.code == TransactionErrorCode::cancelled};
}

auto domain_error(const RenderError &error) -> ProtocolError {
  auto advice = RetryAdvice::not_retryable;
  if (error.code == RenderErrorCode::cancelled ||
      error.code == RenderErrorCode::allocation_failure)
    advice = RetryAdvice::same_request;
  else if (error.code == RenderErrorCode::resource_limit ||
           error.code == RenderErrorCode::number_out_of_range)
    advice = RetryAdvice::change_request;
  return ProtocolError{.source = ErrorSource::domain,
                       .code = std::string{render_error_code_name(error.code)},
                       .retry_advice = advice,
                       .operation_index = std::nullopt,
                       .path = {},
                       .message = std::string{error.message},
                       .interrupted = error.code == RenderErrorCode::cancelled};
}

auto wrong_document_error() -> ProtocolError {
  return ProtocolError{.source = ErrorSource::domain,
                       .code = "wrong_document",
                       .retry_advice = RetryAdvice::change_request,
                       .operation_index = std::nullopt,
                       .path = {PathComponent{std::string{"document_id"}}},
                       .message = "transaction targets a different document",
                       .interrupted = false};
}

auto missing_document_error() -> ProtocolError {
  return ProtocolError{.source = ErrorSource::domain,
                       .code = "missing_identity",
                       .retry_advice = RetryAdvice::change_request,
                       .operation_index = std::nullopt,
                       .path = {PathComponent{std::string{"document_id"}}},
                       .message = "identity does not exist",
                       .interrupted = false};
}

auto stale_revision_error() -> ProtocolError {
  return ProtocolError{
      .source = ErrorSource::domain,
      .code = "stale_revision",
      .retry_advice = RetryAdvice::refresh_then_retry,
      .operation_index = std::nullopt,
      .path = {PathComponent{std::string{"expected_revision"}}},
      .message = "expected revision does not match committed revision",
      .interrupted = false};
}

auto hard_limits() -> ResourceLimits {
  auto result = ResourceLimits::create(hard_resource_limit_values);
  if (!result)
    std::terminate();
  return *result;
}

struct RenderArtifact {
  DocumentId document_id;
  Revision revision;
  std::uint64_t time_us{};
  ArtifactFormat format{};
  AssetId artifact_id;
  CanvasExtent extent;
  std::uint64_t byte_length{};
  std::string sha256;
};

auto encode_render_result(const RenderArtifact &artifact) -> std::string {
  return json_object({
      {"artifact_id", json_string(artifact.artifact_id.value())},
      {"byte_length", json_uint(artifact.byte_length)},
      {"document_id", json_string(artifact.document_id.value())},
      {"format",
       json_string(artifact.format == ArtifactFormat::rgba8 ? "rgba8" : "png")},
      {"height", json_uint(artifact.extent.height())},
      {"kind", json_string("render")},
      {"revision", json_uint(artifact.revision.value())},
      {"sha256", json_string(artifact.sha256)},
      {"time_us", json_uint(artifact.time_us)},
      {"width", json_uint(artifact.extent.width())},
  });
}

auto artifact_error(std::string code, std::string message,
                    const RetryAdvice advice, const bool interrupted = false)
    -> ProtocolError {
  return ProtocolError{.source = ErrorSource::adapter,
                       .code = std::move(code),
                       .retry_advice = advice,
                       .operation_index = std::nullopt,
                       .path = {PathComponent{std::string{"artifact_id"}}},
                       .message = std::move(message),
                       .interrupted = interrupted};
}

auto write_artifact(const std::filesystem::path &directory,
                    const AssetId &artifact_id, const ArtifactFormat format,
                    const std::span<const std::uint8_t> bytes,
                    const CancellationToken cancellation)
    -> std::expected<void, ProtocolError> {
  const auto extension = format == ArtifactFormat::rgba8 ? ".rgba8" : ".png";
  const auto target =
      directory / (std::string{artifact_id.value()} + extension);
  std::ofstream stream{target,
                       std::ios::binary | std::ios::out | std::ios::noreplace};
  if (!stream) {
    std::error_code error;
    if (std::filesystem::exists(target, error) && !error)
      return std::unexpected{artifact_error("artifact_exists",
                                            "artifact target already exists",
                                            RetryAdvice::change_request)};
    return std::unexpected{artifact_error("artifact_io_failure",
                                          "artifact could not be written",
                                          RetryAdvice::same_request)};
  }

  auto cleanup = [&]() {
    stream.close();
    std::error_code ignored;
    std::filesystem::remove(target, ignored);
  };
  std::size_t offset{};
  while (offset < bytes.size()) {
    if (cancellation.stop_requested()) {
      cleanup();
      return std::unexpected{artifact_error("cancelled",
                                            "artifact write was cancelled",
                                            RetryAdvice::same_request, true)};
    }
    const auto count = std::min(artifact_write_chunk, bytes.size() - offset);
    stream.write(reinterpret_cast<const char *>(bytes.data() + offset),
                 static_cast<std::streamsize>(count));
    if (!stream) {
      cleanup();
      return std::unexpected{artifact_error("artifact_io_failure",
                                            "artifact could not be written",
                                            RetryAdvice::same_request)};
    }
    offset += count;
  }
  stream.flush();
  if (!stream) {
    cleanup();
    return std::unexpected{artifact_error("artifact_io_failure",
                                          "artifact could not be written",
                                          RetryAdvice::same_request)};
  }
  stream.close();
  if (!stream) {
    std::error_code ignored;
    std::filesystem::remove(target, ignored);
    return std::unexpected{artifact_error("artifact_io_failure",
                                          "artifact could not be written",
                                          RetryAdvice::same_request)};
  }
  return {};
}

class Session {
public:
  explicit Session(std::filesystem::path artifact_directory)
      : m_artifact_directory{std::move(artifact_directory)},
        m_decode_limits{hard_limits()} {}

  [[nodiscard]] auto decode_limits() const noexcept -> const ResourceLimits & {
    return m_decode_limits;
  }

  auto process(Request request, const CancellationToken cancellation)
      -> std::expected<std::string, ProtocolError> {
    return std::visit(
        [&](auto &&value) {
          return process_request(std::move(value), cancellation);
        },
        std::move(request));
  }

private:
  auto current_summary() const
      -> std::expected<DocumentSummary, ProtocolError> {
    if (!m_dispatcher)
      return std::unexpected{missing_document_error()};
    auto result = inspect(m_dispatcher->snapshot(), SummaryQuery{});
    if (!result)
      return std::unexpected{domain_error(result.error())};
    return *result;
  }

  auto require_document(const DocumentId &document_id) const
      -> std::expected<DocumentSummary, ProtocolError> {
    auto summary = current_summary();
    if (!summary)
      return std::unexpected{summary.error()};
    if (summary->document_id != document_id)
      return std::unexpected{wrong_document_error()};
    return *summary;
  }

  auto process_request(CreateRequest request, CancellationToken)
      -> std::expected<std::string, ProtocolError> {
    if (m_dispatcher) {
      auto summary = current_summary();
      if (!summary)
        return std::unexpected{summary.error()};
      if (summary->document_id != request.document_id)
        return std::unexpected{wrong_document_error()};
      return std::unexpected{
          ProtocolError{.source = ErrorSource::domain,
                        .code = "duplicate_identity",
                        .retry_advice = RetryAdvice::change_request,
                        .operation_index = std::nullopt,
                        .path = {PathComponent{std::string{"document_id"}}},
                        .message = "identity is duplicated",
                        .interrupted = false}};
    }
    auto document = Document::create(request.document_id, request.canvas,
                                     request.background, request.limits);
    if (!document)
      return std::unexpected{domain_error(document.error())};
    auto dispatcher = TransactionDispatcher::create(std::move(*document));
    if (!dispatcher)
      return std::unexpected{domain_error(dispatcher.error())};
    m_dispatcher = std::move(*dispatcher);
    m_decode_limits = request.limits;
    auto summary = current_summary();
    if (!summary)
      return std::unexpected{summary.error()};
    return json_object({{"kind", json_string("document_created")},
                        {"summary", encode_summary(*summary)}});
  }

  auto process_request(InspectRequest request, CancellationToken)
      -> std::expected<std::string, ProtocolError> {
    auto summary = require_document(request.document_id);
    if (!summary)
      return std::unexpected{summary.error()};
    auto query_result = std::visit(
        [&](const auto &query) -> std::expected<std::string, ProtocolError> {
          auto result = inspect(m_dispatcher->snapshot(), query);
          if (!result)
            return std::unexpected{domain_error(result.error())};
          if constexpr (std::is_same_v<std::remove_cvref_t<decltype(query)>,
                                       SelectedObjectsQuery>)
            return encode_query_result(*result, query);
          else
            return encode_query_result(*result);
        },
        request.query);
    if (!query_result)
      return std::unexpected{query_result.error()};
    return json_object({{"kind", json_string("inspect")},
                        {"query_result", std::move(*query_result)}});
  }

  auto process_request(ApplyRequest request,
                       const CancellationToken cancellation)
      -> std::expected<std::string, ProtocolError> {
    if (!m_dispatcher)
      return std::unexpected{missing_document_error()};
    auto result =
        m_dispatcher->apply(request.transaction, request.mode, cancellation);
    if (!result)
      return std::unexpected{domain_error(result.error())};
    return encode_transaction_result(*result);
  }

  auto process_request(RenderRequest request,
                       const CancellationToken cancellation)
      -> std::expected<std::string, ProtocolError> {
    auto summary = require_document(request.document_id);
    if (!summary)
      return std::unexpected{summary.error()};
    if (summary->revision != request.expected_revision)
      return std::unexpected{stale_revision_error()};
    auto config = RenderConfig::create(request.time_us,
                                       summary->limits.max_output_bytes());
    if (!config)
      return std::unexpected{domain_error(config.error())};
    auto rgba = render_rgba(m_dispatcher->snapshot(), *config, cancellation);
    if (!rgba)
      return std::unexpected{domain_error(rgba.error())};

    std::optional<PngImage> png;
    std::span<const std::uint8_t> bytes = rgba->pixels();
    if (request.format == ArtifactFormat::png) {
      auto encoded = encode_png(*rgba, cancellation);
      if (!encoded)
        return std::unexpected{domain_error(encoded.error())};
      png = std::move(*encoded);
      bytes = png->bytes();
    }
    auto digest = sha256_hex(bytes);
    auto written = write_artifact(m_artifact_directory, request.artifact_id,
                                  request.format, bytes, cancellation);
    if (!written)
      return std::unexpected{written.error()};
    return encode_render_result(RenderArtifact{
        std::move(request.document_id), request.expected_revision,
        request.time_us, request.format, std::move(request.artifact_id),
        rgba->extent(), static_cast<std::uint64_t>(bytes.size()),
        std::move(digest)});
  }

  std::filesystem::path m_artifact_directory;
  ResourceLimits m_decode_limits;
  std::optional<TransactionDispatcher> m_dispatcher;
};

auto emit(std::ostream &output, std::ostream &diagnostics,
          const std::string &frame) -> bool {
  output.write(frame.data(), static_cast<std::streamsize>(frame.size()));
  output.flush();
  if (output)
    return true;
  diagnostics << "drawforge: structured stdout write failed\n";
  return false;
}

auto category_for(const ProtocolError &error) -> int {
  if (error.interrupted)
    return exit_cancelled;
  switch (error.source) {
  case ErrorSource::encoding:
    return exit_encoding;
  case ErrorSource::domain:
    return exit_domain;
  case ErrorSource::adapter:
    return exit_adapter;
  }
  return exit_adapter;
}

auto record_category(int current, const int candidate) -> int {
  if (candidate == exit_cancelled)
    return candidate;
  if (current == exit_cancelled)
    return current;
  return std::max(current, candidate);
}

auto process_frame(Session &session, const std::string_view frame,
                   std::ostream &output, std::ostream &diagnostics,
                   const CancellationToken cancellation, int &exit_category)
    -> bool {
  try {
    if (cancellation.stop_requested()) {
      const ProtocolError error{.source = ErrorSource::adapter,
                                .code = "cancelled",
                                .retry_advice = RetryAdvice::same_request,
                                .operation_index = std::nullopt,
                                .path = {},
                                .message = "adapter processing was cancelled",
                                .interrupted = true};
      exit_category = exit_cancelled;
      static_cast<void>(emit(output, diagnostics, encode_error_frame(error)));
      return false;
    }
    auto document = parse_json(frame);
    auto request = decode_request(document.root(), session.decode_limits());
    auto result = session.process(std::move(request), cancellation);
    if (result)
      return emit(output, diagnostics,
                  encode_success_frame(std::move(*result)));
    exit_category =
        record_category(exit_category, category_for(result.error()));
    const auto interrupted = result.error().interrupted;
    if (!emit(output, diagnostics, encode_error_frame(result.error())))
      return false;
    return !interrupted;
  } catch (const DecodeFailure &failure) {
    exit_category = record_category(exit_category, exit_encoding);
    return emit(output, diagnostics, encode_error_frame(failure.error()));
  } catch (const std::bad_alloc &) {
    const ProtocolError error{.source = ErrorSource::adapter,
                              .code = "allocation_failure",
                              .retry_advice = RetryAdvice::same_request,
                              .operation_index = std::nullopt,
                              .path = {},
                              .message = "adapter allocation failed",
                              .interrupted = false};
    exit_category = record_category(exit_category, exit_adapter);
    return emit(output, diagnostics, encode_error_frame(error));
  }
}

} // namespace

auto run_jsonl(std::istream &input, std::ostream &output,
               std::ostream &diagnostics,
               const std::filesystem::path &artifact_directory,
               const CancellationToken cancellation) -> int {
  std::error_code filesystem_error;
  const auto status =
      std::filesystem::status(artifact_directory, filesystem_error);
  if (filesystem_error || !std::filesystem::exists(status) ||
      !std::filesystem::is_directory(status)) {
    diagnostics
        << "drawforge: --artifact-dir must name an existing directory\n";
    return exit_invocation;
  }

  Session session{artifact_directory};
  std::string frame;
  frame.reserve(64U * 1024U);
  bool overlong{};
  bool saw_input{};
  int exit_category{exit_success};

  auto finish_frame = [&](const bool terminated) -> bool {
    saw_input = true;
    if (overlong ||
        frame.size() + (terminated ? 1U : 0U) > default_max_frame_bytes) {
      const auto error = fixed_encoding_error("resource_limit");
      exit_category = record_category(exit_category, exit_encoding);
      if (!emit(output, diagnostics, encode_error_frame(error)))
        return false;
    } else {
      std::string_view value{frame};
      if (terminated && value.ends_with('\r'))
        value.remove_suffix(1U);
      if (!process_frame(session, value, output, diagnostics, cancellation,
                         exit_category))
        return false;
    }
    frame.clear();
    overlong = false;
    return true;
  };

  char character{};
  while (input.get(character)) {
    if (character == '\n') {
      if (!finish_frame(true))
        return exit_category == exit_cancelled ? exit_cancelled : exit_adapter;
      if (exit_category == exit_cancelled)
        return exit_cancelled;
      continue;
    }
    if (!overlong) {
      if (frame.size() >= default_max_frame_bytes)
        overlong = true;
      else
        frame.push_back(character);
    }
  }

  if (cancellation.stop_requested())
    return exit_cancelled;
  if (!input.eof()) {
    diagnostics << "drawforge: structured stdin read failed\n";
    return exit_adapter;
  }
  if (overlong || !frame.empty()) {
    if (!finish_frame(false))
      return exit_category == exit_cancelled ? exit_cancelled : exit_adapter;
  }
  if (!saw_input && cancellation.stop_requested())
    return exit_cancelled;
  return exit_category;
}

} // namespace drawforge::cli
