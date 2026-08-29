"""Strict dependency-free reference codec for the provisional wire contract."""

from __future__ import annotations

import json
import math
import re
from dataclasses import dataclass
from typing import Any, Iterable, Iterator, Mapping, Sequence

from contract import (
    DEFAULT_WIRE_LIMITS,
    HARD_WIRE_LIMITS,
    MAX_UINT64,
    PROTOCOL,
    WireLimits,
    build_schema,
    default_limits,
)


PathComponent = str | int


class WireError(Exception):
    """Bounded structured failure produced before a typed domain call."""

    def __init__(
        self,
        code: str,
        message: str,
        path: Sequence[PathComponent] = (),
    ) -> None:
        super().__init__(message)
        self.code = code
        self.message = message
        self.path = tuple(path)

    def as_error_frame(self) -> dict[str, Any]:
        error: dict[str, Any] = {
            "source": "encoding",
            "code": self.code,
            "retry_advice": "change_request",
            "operation_index": None,
            "field_path": list(self.path),
            "message": self.message,
        }
        if self.code == "unsupported_version":
            error["supported_versions"] = [PROTOCOL]
        return {"protocol": PROTOCOL, "status": "error", "error": error}


@dataclass(frozen=True)
class DecodedFrame:
    value: Mapping[str, Any]

    @property
    def category(self) -> str:
        if "request" in self.value:
            return "request"
        return "result" if self.value.get("status") == "ok" else "error"


def _fixed_error(code: str, path: Sequence[PathComponent] = ()) -> WireError:
    messages = {
        "invalid_utf8": "frame is not valid UTF-8",
        "invalid_json": "frame is not one complete JSON object",
        "duplicate_key": "JSON object contains a duplicate key",
        "unsupported_version": "frame uses an unsupported protocol version",
        "unknown_field": "object contains an unknown field",
        "missing_field": "object is missing a required field",
        "invalid_type": "value has the wrong JSON type",
        "invalid_value": "value is not accepted by the provisional contract",
        "resource_limit": "wire resource limit was exceeded",
        "nesting_limit": "JSON nesting limit was exceeded",
    }
    return WireError(code, messages[code], path)


def _decode_json(text: str, limits: WireLimits) -> Any:
    def object_pairs(pairs: list[tuple[str, Any]]) -> dict[str, Any]:
        if len(pairs) > limits.max_object_members:
            raise _fixed_error("resource_limit")
        result: dict[str, Any] = {}
        for key, value in pairs:
            if key in result:
                raise _fixed_error("duplicate_key")
            result[key] = value
        return result

    def parse_int(token: str) -> int:
        if len(token.encode("ascii")) > limits.max_number_token_bytes:
            raise _fixed_error("resource_limit")
        return int(token)

    def parse_float(token: str) -> float:
        if len(token.encode("ascii")) > limits.max_number_token_bytes:
            raise _fixed_error("resource_limit")
        value = float(token)
        if not math.isfinite(value):
            raise _fixed_error("invalid_value")
        return value

    def parse_constant(_token: str) -> None:
        raise _fixed_error("invalid_value")

    try:
        return json.loads(
            text,
            object_pairs_hook=object_pairs,
            parse_int=parse_int,
            parse_float=parse_float,
            parse_constant=parse_constant,
        )
    except WireError:
        raise
    except (json.JSONDecodeError, UnicodeError, ValueError, OverflowError) as error:
        raise _fixed_error("invalid_json") from error


def _check_container_limits(
    value: Any,
    limits: WireLimits,
    path: tuple[PathComponent, ...] = (),
    depth: int = 1,
) -> None:
    if not isinstance(value, (dict, list)):
        return
    if depth > limits.max_json_depth:
        raise _fixed_error("nesting_limit", path)
    if isinstance(value, dict):
        if len(value) > limits.max_object_members:
            raise _fixed_error("resource_limit", path)
        for key, child in value.items():
            _check_container_limits(child, limits, (*path, key), depth + 1)
    elif isinstance(value, list):
        if len(value) > limits.max_array_items:
            raise _fixed_error("resource_limit", path)
        for index, child in enumerate(value):
            _check_container_limits(child, limits, (*path, index), depth + 1)


def _validate_wire_limits(limits: WireLimits) -> None:
    for field_name in (
        "max_frame_bytes",
        "max_json_depth",
        "max_object_members",
        "max_array_items",
        "max_number_token_bytes",
    ):
        value = getattr(limits, field_name)
        hard_value = getattr(HARD_WIRE_LIMITS, field_name)
        if value < 0 or value > hard_value:
            raise ValueError(f"{field_name} is outside the provisional protocol ceiling")


def _json_type_matches(value: Any, expected: str) -> bool:
    if expected == "object":
        return isinstance(value, dict)
    if expected == "array":
        return isinstance(value, list)
    if expected == "string":
        return isinstance(value, str)
    if expected == "boolean":
        return isinstance(value, bool)
    if expected == "null":
        return value is None
    if expected == "integer":
        return isinstance(value, int) and not isinstance(value, bool)
    if expected == "number":
        return isinstance(value, (int, float)) and not isinstance(value, bool)
    raise AssertionError(f"unsupported schema type: {expected}")


def _canonical_key(value: Any) -> str:
    return json.dumps(value, ensure_ascii=False, sort_keys=True, separators=(",", ":"))


def _resolved_schema(schema: Mapping[str, Any], root: Mapping[str, Any]) -> Mapping[str, Any]:
    reference = schema.get("$ref")
    if reference is None:
        return schema
    prefix = "#/$defs/"
    if not isinstance(reference, str) or not reference.startswith(prefix):
        raise AssertionError("reference codec only supports local $defs references")
    return root["$defs"][reference[len(prefix) :]]


def _validate_schema(
    value: Any,
    schema: Mapping[str, Any],
    root: Mapping[str, Any],
    path: tuple[PathComponent, ...] = (),
) -> None:
    if "$ref" in schema:
        prefix = "#/$defs/"
        reference = schema["$ref"]
        if not isinstance(reference, str) or not reference.startswith(prefix):
            raise AssertionError("reference codec only supports local $defs references")
        _validate_schema(value, root["$defs"][reference[len(prefix) :]], root, path)
        return

    if "oneOf" in schema:
        candidates = list(schema["oneOf"])
        if isinstance(value, dict):
            for discriminator in ("kind", "op", "status"):
                if discriminator not in value:
                    continue
                tagged_candidates = [
                    candidate
                    for candidate in candidates
                    if _resolved_schema(candidate, root)
                    .get("properties", {})
                    .get(discriminator, {})
                    .get("const")
                    == value[discriminator]
                ]
                has_discriminator = any(
                    discriminator
                    in _resolved_schema(candidate, root).get("properties", {})
                    for candidate in candidates
                )
                if tagged_candidates:
                    candidates = tagged_candidates
                elif has_discriminator:
                    raise _fixed_error("invalid_value", (*path, discriminator))
                break
            else:
                missing_counts = [
                    len(
                        set(_resolved_schema(candidate, root).get("required", []))
                        - set(value)
                    )
                    for candidate in candidates
                ]
                if missing_counts and min(missing_counts) == 0:
                    candidates = [
                        candidate
                        for candidate, count in zip(candidates, missing_counts)
                        if count == 0
                    ]
        successes = 0
        failures: list[WireError] = []
        for candidate in candidates:
            try:
                _validate_schema(value, candidate, root, path)
                successes += 1
            except WireError as error:
                failures.append(error)
        if successes == 1:
            return
        if successes > 1:
            raise _fixed_error("invalid_value", path)
        if failures:
            priority = {
                "duplicate_key": 5,
                "unknown_field": 4,
                "missing_field": 3,
                "resource_limit": 2,
                "invalid_value": 1,
                "invalid_type": 0,
            }
            best = max(failures, key=lambda error: (len(error.path), priority.get(error.code, 0)))
            raise best
        raise _fixed_error("invalid_value", path)

    if "type" in schema and not _json_type_matches(value, schema["type"]):
        raise _fixed_error("invalid_type", path)
    if "const" in schema and value != schema["const"]:
        raise _fixed_error("invalid_value", path)
    if "enum" in schema and value not in schema["enum"]:
        raise _fixed_error("invalid_value", path)

    if isinstance(value, dict):
        properties = schema.get("properties", {})
        required = schema.get("required", [])
        for name in required:
            if name not in value:
                raise _fixed_error("missing_field", (*path, name))
        if schema.get("additionalProperties") is False:
            for name in value:
                if name not in properties:
                    raise _fixed_error("unknown_field", (*path, name))
        maximum = schema.get("maxProperties")
        if maximum is not None and len(value) > maximum:
            raise _fixed_error("resource_limit", path)
        for name, child in value.items():
            child_schema = properties.get(name)
            if child_schema is not None:
                _validate_schema(child, child_schema, root, (*path, name))

    if isinstance(value, list):
        minimum = schema.get("minItems")
        maximum = schema.get("maxItems")
        if minimum is not None and len(value) < minimum:
            raise _fixed_error("invalid_value", path)
        if maximum is not None and len(value) > maximum:
            raise _fixed_error("resource_limit", path)
        if schema.get("uniqueItems"):
            keys = [_canonical_key(item) for item in value]
            if len(keys) != len(set(keys)):
                raise _fixed_error("invalid_value", path)
        item_schema = schema.get("items")
        if item_schema is not None:
            for index, child in enumerate(value):
                _validate_schema(child, item_schema, root, (*path, index))

    if isinstance(value, str):
        encoded_length = len(value.encode("utf-8"))
        if "\x00" in value:
            raise _fixed_error("invalid_value", path)
        if "minLength" in schema and encoded_length < schema["minLength"]:
            raise _fixed_error("invalid_value", path)
        if "maxLength" in schema and encoded_length > schema["maxLength"]:
            raise _fixed_error("resource_limit", path)
        if "pattern" in schema and re.fullmatch(schema["pattern"], value) is None:
            raise _fixed_error("invalid_value", path)

    if isinstance(value, (int, float)) and not isinstance(value, bool):
        numeric = float(value) if isinstance(value, float) else value
        if isinstance(numeric, float) and not math.isfinite(numeric):
            raise _fixed_error("invalid_value", path)
        if "minimum" in schema and numeric < schema["minimum"]:
            raise _fixed_error("invalid_value", path)
        if "maximum" in schema and numeric > schema["maximum"]:
            raise _fixed_error("invalid_value", path)
        if "exclusiveMinimum" in schema and numeric <= schema["exclusiveMinimum"]:
            raise _fixed_error("invalid_value", path)


def _walk(value: Any) -> Iterator[tuple[tuple[PathComponent, ...], Any]]:
    stack: list[tuple[tuple[PathComponent, ...], Any]] = [((), value)]
    while stack:
        path, current = stack.pop()
        yield path, current
        if isinstance(current, dict):
            stack.extend(((*path, key), child) for key, child in reversed(tuple(current.items())))
        elif isinstance(current, list):
            stack.extend(((*path, index), child) for index, child in reversed(tuple(enumerate(current))))


def _validate_path_geometry(geometry: Mapping[str, Any], path: tuple[PathComponent, ...]) -> None:
    commands = geometry["commands"]
    open_subpath = False
    segment_count = 0
    for index, command in enumerate(commands):
        kind = command["kind"]
        command_path = (*path, "commands", index)
        if kind == "move_to":
            if open_subpath and segment_count == 0:
                raise _fixed_error("invalid_value", command_path)
            open_subpath = True
            segment_count = 0
        elif kind == "line_to":
            if not open_subpath:
                raise _fixed_error("invalid_value", command_path)
            segment_count += 1
        elif kind == "close":
            if not open_subpath or segment_count == 0:
                raise _fixed_error("invalid_value", command_path)
            open_subpath = False
            segment_count = 0
    if open_subpath and segment_count == 0:
        raise _fixed_error("invalid_value", (*path, "commands"))


def _validate_geometry(geometry: Mapping[str, Any], path: tuple[PathComponent, ...]) -> None:
    kind = geometry["kind"]
    if kind == "rectangle":
        if geometry["radius_x"] > geometry["width"] / 2:
            raise _fixed_error("invalid_value", (*path, "radius_x"))
        if geometry["radius_y"] > geometry["height"] / 2:
            raise _fixed_error("invalid_value", (*path, "radius_y"))
    elif kind == "path":
        _validate_path_geometry(geometry, path)


def _validate_create_request(request: Mapping[str, Any], path: tuple[PathComponent, ...]) -> None:
    limits = request.get("limits", default_limits())
    canvas = request["canvas"]
    width = canvas["width"]
    height = canvas["height"]
    if len(request["document_id"].encode("utf-8")) > limits["max_identifier_bytes"]:
        raise _fixed_error("resource_limit", (*path, "document_id"))
    if width > limits["max_canvas_dimension"] or height > limits["max_canvas_dimension"]:
        raise _fixed_error("resource_limit", (*path, "canvas"))
    pixels = width * height
    if pixels > limits["max_canvas_pixels"]:
        raise _fixed_error("resource_limit", (*path, "canvas"))
    if pixels * 4 > limits["max_output_bytes"]:
        raise _fixed_error("resource_limit", (*path, "canvas"))


def _validate_receipt(receipt: Mapping[str, Any], path: tuple[PathComponent, ...]) -> None:
    created = {_canonical_key(item) for item in receipt["created"]}
    changed = {_canonical_key(item) for item in receipt["changed"]}
    if created & changed:
        raise _fixed_error("invalid_value", path)
    base = receipt["base_revision"]
    if base == MAX_UINT64 or receipt["result_revision"] != base + 1:
        raise _fixed_error("invalid_value", (*path, "result_revision"))


def _semantic_validate(value: Mapping[str, Any]) -> None:
    if "request" in value:
        request = value["request"]
        if request["kind"] == "create_document":
            _validate_create_request(request, ("request",))
    for path, current in _walk(value):
        if isinstance(current, dict) and current.get("kind") in ("rectangle", "ellipse", "path"):
            if "commands" in current or "center" in current or "radius_x" in current and "width" in current:
                _validate_geometry(current, path)
    if value.get("status") == "ok" and value["result"]["kind"] == "transaction":
        _validate_receipt(value["result"]["receipt"], ("result", "receipt"))
    if value.get("status") == "ok" and value["result"]["kind"] == "document_created":
        if value["result"]["summary"]["revision"] != 0:
            raise _fixed_error("invalid_value", ("result", "summary", "revision"))
    if value.get("status") == "ok" and value["result"]["kind"] == "inspect":
        query_result = value["result"]["query_result"]
        if query_result["kind"] == "structure":
            for index, node in enumerate(query_result["nodes"]):
                identity_kind = node["identity"]["kind"]
                if (node["node_kind"] == "layer") != (identity_kind == "layer"):
                    raise _fixed_error(
                        "invalid_value",
                        ("result", "query_result", "nodes", index, "node_kind"),
                    )
        if query_result["kind"] == "bounds":
            for index, item in enumerate(query_result["items"]):
                projections = [entry["projection"] for entry in item["projections"]]
                if len(projections) != len(set(projections)):
                    raise _fixed_error(
                        "invalid_value",
                        ("result", "query_result", "items", index, "projections"),
                    )
    if value.get("status") == "error":
        error = value["error"]
        encoding_codes = {
            "invalid_utf8",
            "invalid_json",
            "duplicate_key",
            "unsupported_version",
            "unknown_field",
            "missing_field",
            "invalid_type",
            "invalid_value",
            "resource_limit",
            "nesting_limit",
        }
        adapter_codes = {
            "allocation_failure",
            "artifact_exists",
            "artifact_io_failure",
            "cancelled",
        }
        if error["source"] == "encoding":
            if error["code"] not in encoding_codes:
                raise _fixed_error("invalid_value", ("error", "code"))
            if error["operation_index"] is not None:
                raise _fixed_error("invalid_value", ("error", "operation_index"))
            if error["retry_advice"] != "change_request":
                raise _fixed_error("invalid_value", ("error", "retry_advice"))
        elif error["source"] == "adapter":
            if error["code"] not in adapter_codes:
                raise _fixed_error("invalid_value", ("error", "code"))
            if error["operation_index"] is not None:
                raise _fixed_error("invalid_value", ("error", "operation_index"))
        elif error["code"] in encoding_codes | {
            "artifact_exists",
            "artifact_io_failure",
        }:
            raise _fixed_error("invalid_value", ("error", "code"))
        has_versions = "supported_versions" in error
        if error["code"] == "unsupported_version":
            if error.get("supported_versions") != [PROTOCOL]:
                raise _fixed_error("invalid_value", ("error", "supported_versions"))
        elif has_versions:
            raise _fixed_error("unknown_field", ("error", "supported_versions"))


def _normalize(value: Any) -> Any:
    if isinstance(value, float) and value == 0.0:
        return 0.0
    if isinstance(value, dict):
        return {key: _normalize(child) for key, child in value.items()}
    if isinstance(value, list):
        return [_normalize(child) for child in value]
    return value


def validate_value(value: Any, limits: WireLimits = DEFAULT_WIRE_LIMITS) -> Mapping[str, Any]:
    _validate_wire_limits(limits)
    _check_container_limits(value, limits)
    if not isinstance(value, dict):
        raise _fixed_error("invalid_type")
    protocol = value.get("protocol")
    if protocol != PROTOCOL:
        raise _fixed_error("unsupported_version", ("protocol",))
    schema = build_schema()
    _validate_schema(value, schema, schema)
    _semantic_validate(value)
    return _normalize(value)


def decode_frame(raw: bytes | str, limits: WireLimits = DEFAULT_WIRE_LIMITS) -> DecodedFrame:
    _validate_wire_limits(limits)
    if isinstance(raw, str):
        try:
            encoded = raw.encode("utf-8")
        except UnicodeEncodeError as error:
            raise _fixed_error("invalid_utf8") from error
    else:
        encoded = raw
    if len(encoded) > limits.max_frame_bytes:
        raise _fixed_error("resource_limit")
    if encoded.startswith(b"\xef\xbb\xbf") or b"\x00" in encoded:
        raise _fixed_error("invalid_utf8")
    if encoded.endswith(b"\n"):
        encoded = encoded[:-1]
        if encoded.endswith(b"\r"):
            encoded = encoded[:-1]
    if not encoded or b"\n" in encoded or b"\r" in encoded:
        raise _fixed_error("invalid_json")
    try:
        text = encoded.decode("utf-8", errors="strict")
    except UnicodeDecodeError as error:
        raise _fixed_error("invalid_utf8") from error
    value = _decode_json(text, limits)
    normalized = validate_value(value, limits)
    return DecodedFrame(normalized)


def encode_frame(value: Mapping[str, Any] | DecodedFrame, limits: WireLimits = DEFAULT_WIRE_LIMITS) -> bytes:
    payload = value.value if isinstance(value, DecodedFrame) else value
    normalized = validate_value(payload, limits)
    try:
        encoded = (
            json.dumps(
                normalized,
                ensure_ascii=False,
                sort_keys=True,
                separators=(",", ":"),
                allow_nan=False,
            )
            + "\n"
        ).encode("utf-8")
    except (TypeError, ValueError, UnicodeEncodeError) as error:
        raise _fixed_error("invalid_value") from error
    if len(encoded) > limits.max_frame_bytes:
        raise _fixed_error("resource_limit")
    return encoded


def decode_stream(raw: bytes, limits: WireLimits = DEFAULT_WIRE_LIMITS) -> Iterable[DecodedFrame]:
    for line in raw.splitlines(keepends=True):
        yield decode_frame(line, limits)
