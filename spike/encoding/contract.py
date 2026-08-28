"""Declarative provisional JSON contract for DrawForge issue #7.

This module is evidence, not a production adapter.  It owns the machine shape
used to generate the committed JSON Schema and examples without selecting a
C++ JSON dependency or exposing JSON values through libdrawforge.
"""

from __future__ import annotations

import argparse
import json
from dataclasses import dataclass
from pathlib import Path
from typing import Any


PROTOCOL = "drawforge.experimental/v1"
SCHEMA_DRAFT = "https://json-schema.org/draft/2020-12/schema"
MAX_UINT64 = (1 << 64) - 1
MAX_NUMERIC_MAGNITUDE = 1_048_576.0


@dataclass(frozen=True)
class WireLimits:
    max_frame_bytes: int = 8 * 1024 * 1024
    max_json_depth: int = 32
    max_object_members: int = 64
    max_array_items: int = 65_536
    max_number_token_bytes: int = 64


DEFAULT_WIRE_LIMITS = WireLimits()
HARD_MAX_FRAME_BYTES = 64 * 1024 * 1024
HARD_WIRE_LIMITS = WireLimits(max_frame_bytes=HARD_MAX_FRAME_BYTES)


def ref(name: str) -> dict[str, str]:
    return {"$ref": f"#/$defs/{name}"}


def nullable(schema: dict[str, Any]) -> dict[str, Any]:
    return {"oneOf": [schema, {"type": "null"}]}


def array_of(
    schema: dict[str, Any],
    *,
    minimum: int = 0,
    maximum: int = DEFAULT_WIRE_LIMITS.max_array_items,
    unique: bool = False,
) -> dict[str, Any]:
    result: dict[str, Any] = {
        "type": "array",
        "items": schema,
        "minItems": minimum,
        "maxItems": maximum,
    }
    if unique:
        result["uniqueItems"] = True
    return result


def object_of(
    required: dict[str, Any], optional: dict[str, Any] | None = None
) -> dict[str, Any]:
    properties = dict(required)
    properties.update(optional or {})
    return {
        "type": "object",
        "properties": properties,
        "required": list(required),
        "additionalProperties": False,
        "maxProperties": DEFAULT_WIRE_LIMITS.max_object_members,
    }


def tagged(tag: str, required: dict[str, Any], optional: dict[str, Any] | None = None) -> dict[str, Any]:
    return object_of({"kind": {"const": tag}, **required}, optional)


def operation(tag: str, required: dict[str, Any]) -> dict[str, Any]:
    return object_of({"op": {"const": tag}, **required})


def build_schema() -> dict[str, Any]:
    identifier = {
        "type": "string",
        "pattern": "^[A-Za-z][A-Za-z0-9._-]*$",
        "minLength": 1,
        "maxLength": 128,
    }
    bounded_text = {"type": "string", "maxLength": 65_536}
    uint64 = {"type": "integer", "minimum": 0, "maximum": MAX_UINT64}
    coordinate = {
        "type": "number",
        "minimum": -MAX_NUMERIC_MAGNITUDE,
        "maximum": MAX_NUMERIC_MAGNITUDE,
    }
    nonnegative_coordinate = {
        "type": "number",
        "minimum": 0,
        "maximum": MAX_NUMERIC_MAGNITUDE,
    }
    positive_coordinate = {
        "type": "number",
        "exclusiveMinimum": 0,
        "maximum": MAX_NUMERIC_MAGNITUDE,
    }
    opacity = {"type": "number", "minimum": 0, "maximum": 1}
    color = {"type": "string", "pattern": "^#[0-9a-f]{8}$"}

    defs: dict[str, Any] = {
        "identifier": identifier,
        "bounded_text": bounded_text,
        "uint64": uint64,
        "coordinate": coordinate,
        "nonnegative_coordinate": nonnegative_coordinate,
        "positive_coordinate": positive_coordinate,
        "opacity": opacity,
        "color": color,
        "point": object_of({"x": ref("coordinate"), "y": ref("coordinate")}),
        "transform": object_of(
            {
                "a": ref("coordinate"),
                "b": ref("coordinate"),
                "c": ref("coordinate"),
                "d": ref("coordinate"),
                "e": ref("coordinate"),
                "f": ref("coordinate"),
            }
        ),
        "canvas_extent": object_of(
            {
                "width": {"type": "integer", "minimum": 1, "maximum": 16_384},
                "height": {"type": "integer", "minimum": 1, "maximum": 16_384},
            }
        ),
        "rect": object_of(
            {
                "x": ref("coordinate"),
                "y": ref("coordinate"),
                "width": ref("nonnegative_coordinate"),
                "height": ref("nonnegative_coordinate"),
            }
        ),
        "stroke": object_of(
            {"color": ref("color"), "width": ref("positive_coordinate")}
        ),
        "style": object_of(
            {
                "fill": nullable(ref("color")),
                "stroke": nullable(ref("stroke")),
            }
        ),
        "rectangle_geometry": tagged(
            "rectangle",
            {
                "x": ref("coordinate"),
                "y": ref("coordinate"),
                "width": ref("nonnegative_coordinate"),
                "height": ref("nonnegative_coordinate"),
                "radius_x": ref("nonnegative_coordinate"),
                "radius_y": ref("nonnegative_coordinate"),
            },
        ),
        "ellipse_geometry": tagged(
            "ellipse",
            {
                "center": ref("point"),
                "radius_x": ref("nonnegative_coordinate"),
                "radius_y": ref("nonnegative_coordinate"),
            },
        ),
        "move_to": tagged("move_to", {"point": ref("point")}),
        "line_to": tagged("line_to", {"point": ref("point")}),
        "close_path": tagged("close", {}),
        "path_command": {
            "oneOf": [ref("move_to"), ref("line_to"), ref("close_path")]
        },
        "path_geometry": tagged(
            "path",
            {"commands": array_of(ref("path_command"), minimum=1)},
        ),
        "geometry": {
            "oneOf": [
                ref("rectangle_geometry"),
                ref("ellipse_geometry"),
                ref("path_geometry"),
            ]
        },
        "parent_ref": {
            "oneOf": [
                tagged("layer", {"id": ref("identifier")}),
                tagged("group", {"id": ref("identifier")}),
            ]
        },
        "node_ref": {
            "oneOf": [
                tagged("layer", {"id": ref("identifier")}),
                tagged("object", {"id": ref("identifier")}),
            ]
        },
        "structure_root": {
            "oneOf": [
                tagged("document", {}),
                tagged("layer", {"id": ref("identifier")}),
                tagged("group", {"id": ref("identifier")}),
            ]
        },
        "identity_ref": {
            "oneOf": [
                tagged("document", {"id": ref("identifier")}),
                tagged("layer", {"id": ref("identifier")}),
                tagged("object", {"id": ref("identifier")}),
                tagged("track", {"id": ref("identifier")}),
            ]
        },
        "resource_limits": object_of(
            {
                "max_identifier_bytes": {"type": "integer", "minimum": 0, "maximum": 128},
                "max_text_bytes": {"type": "integer", "minimum": 0, "maximum": 65_536},
                "max_numeric_magnitude": {"type": "number", "minimum": 0, "maximum": MAX_NUMERIC_MAGNITUDE},
                "max_canvas_dimension": {"type": "integer", "minimum": 0, "maximum": 16_384},
                "max_canvas_pixels": {"type": "integer", "minimum": 0, "maximum": 67_108_864},
                "max_scene_nodes": {"type": "integer", "minimum": 0, "maximum": 65_536},
                "max_transaction_operations": {"type": "integer", "minimum": 0, "maximum": 4_096},
                "max_output_bytes": {"type": "integer", "minimum": 0, "maximum": 268_435_456},
                "max_nesting_depth": {"type": "integer", "minimum": 0, "maximum": 128},
            }
        ),
    }

    drawable_common = {
        "object_id": ref("identifier"),
        "parent": ref("parent_ref"),
        "index": ref("uint64"),
        "visible": {"type": "boolean"},
        "transform": ref("transform"),
        "style": ref("style"),
        "opacity": ref("opacity"),
    }
    defs.update(
        {
            "create_layer": operation(
                "create_layer",
                {
                    "layer_id": ref("identifier"),
                    "index": ref("uint64"),
                    "visible": {"type": "boolean"},
                },
            ),
            "create_group": operation(
                "create_group",
                {
                    "object_id": ref("identifier"),
                    "parent": ref("parent_ref"),
                    "index": ref("uint64"),
                    "visible": {"type": "boolean"},
                    "transform": ref("transform"),
                },
            ),
            "create_rectangle": operation(
                "create_rectangle",
                {**drawable_common, "geometry": ref("rectangle_geometry")},
            ),
            "create_ellipse": operation(
                "create_ellipse",
                {**drawable_common, "geometry": ref("ellipse_geometry")},
            ),
            "create_path": operation(
                "create_path", {**drawable_common, "geometry": ref("path_geometry")}
            ),
            "create_opacity_track": operation(
                "create_opacity_track",
                {
                    "track_id": ref("identifier"),
                    "target_object_id": ref("identifier"),
                    "start_time_us": ref("uint64"),
                    "duration_us": {"type": "integer", "minimum": 1, "maximum": MAX_UINT64},
                    "from_opacity": ref("opacity"),
                    "to_opacity": ref("opacity"),
                },
            ),
            "set_canvas_background": operation(
                "set_canvas_background", {"background": nullable(ref("color"))}
            ),
            "set_visibility": operation(
                "set_visibility",
                {"target": ref("node_ref"), "visible": {"type": "boolean"}},
            ),
            "set_transform": operation(
                "set_transform",
                {"object_id": ref("identifier"), "transform": ref("transform")},
            ),
            "set_geometry": operation(
                "set_geometry",
                {"object_id": ref("identifier"), "geometry": ref("geometry")},
            ),
            "set_style": operation(
                "set_style",
                {"object_id": ref("identifier"), "style": ref("style")},
            ),
            "set_opacity": operation(
                "set_opacity",
                {"object_id": ref("identifier"), "opacity": ref("opacity")},
            ),
            "set_opacity_track": operation(
                "set_opacity_track",
                {
                    "track_id": ref("identifier"),
                    "target_object_id": ref("identifier"),
                    "start_time_us": ref("uint64"),
                    "duration_us": {"type": "integer", "minimum": 1, "maximum": MAX_UINT64},
                    "from_opacity": ref("opacity"),
                    "to_opacity": ref("opacity"),
                },
            ),
            "reparent_object": operation(
                "reparent_object",
                {
                    "object_id": ref("identifier"),
                    "parent": ref("parent_ref"),
                    "index": ref("uint64"),
                },
            ),
            "reorder_object": operation(
                "reorder_object",
                {"object_id": ref("identifier"), "index": ref("uint64")},
            ),
        }
    )
    defs["operation"] = {
        "oneOf": [
            ref(name)
            for name in (
                "create_layer",
                "create_group",
                "create_rectangle",
                "create_ellipse",
                "create_path",
                "create_opacity_track",
                "set_canvas_background",
                "set_visibility",
                "set_transform",
                "set_geometry",
                "set_style",
                "set_opacity",
                "set_opacity_track",
                "reparent_object",
                "reorder_object",
            )
        ]
    }
    defs.update(
        {
            "operation_batch": tagged(
                "operations",
                {"operations": array_of(ref("operation"), minimum=1, maximum=4_096)},
            ),
            "undo_body": tagged("undo", {}),
            "redo_body": tagged("redo", {}),
            "transaction_body": {
                "oneOf": [ref("operation_batch"), ref("undo_body"), ref("redo_body")]
            },
            "transaction": object_of(
                {
                    "document_id": ref("identifier"),
                    "expected_revision": ref("uint64"),
                    "transaction_id": ref("identifier"),
                    "body": ref("transaction_body"),
                }
            ),
            "summary_query": tagged(
                "document_summary", {"document_id": ref("identifier")}
            ),
            "structure_query": tagged(
                "structure",
                {
                    "document_id": ref("identifier"),
                    "root": ref("structure_root"),
                    "max_depth": {"type": "integer", "minimum": 0, "maximum": 129},
                    "max_nodes": {"type": "integer", "minimum": 1, "maximum": 65_536},
                },
            ),
            "selected_objects_query": tagged(
                "selected_objects",
                {
                    "document_id": ref("identifier"),
                    "object_ids": array_of(ref("identifier"), minimum=1, unique=True),
                    "fields": array_of(
                        {
                            "enum": [
                                "kind",
                                "parent_order",
                                "visibility",
                                "transform",
                                "geometry",
                                "style",
                                "opacity",
                                "opacity_track",
                            ]
                        },
                        minimum=1,
                        maximum=8,
                        unique=True,
                    ),
                },
            ),
            "bounds_query": tagged(
                "bounds",
                {
                    "document_id": ref("identifier"),
                    "targets": array_of(ref("node_ref"), minimum=1, unique=True),
                    "projections": array_of(
                        {"enum": ["local_geometry", "document_geometry", "document_painted"]},
                        minimum=1,
                        maximum=3,
                        unique=True,
                    ),
                    "time_us": ref("uint64"),
                },
            ),
            "query": {
                "oneOf": [
                    ref("summary_query"),
                    ref("structure_query"),
                    ref("selected_objects_query"),
                    ref("bounds_query"),
                ]
            },
            "create_request": tagged(
                "create_document",
                {
                    "document_id": ref("identifier"),
                    "canvas": ref("canvas_extent"),
                    "background": nullable(ref("color")),
                },
                {"limits": ref("resource_limits")},
            ),
            "inspect_request": tagged("inspect", {"query": ref("query")}),
            "apply_request": tagged(
                "apply",
                {
                    "mode": {"enum": ["dry_run", "commit"]},
                    "transaction": ref("transaction"),
                },
            ),
            "render_request": tagged(
                "render",
                {
                    "document_id": ref("identifier"),
                    "expected_revision": ref("uint64"),
                    "time_us": ref("uint64"),
                    "format": {"enum": ["rgba8", "png"]},
                    "artifact_id": ref("identifier"),
                },
            ),
            "request": {
                "oneOf": [
                    ref("create_request"),
                    ref("inspect_request"),
                    ref("apply_request"),
                    ref("render_request"),
                ]
            },
        }
    )

    field_path_component = {
        "oneOf": [
            {"type": "string", "minLength": 1, "maxLength": 128},
            {"type": "integer", "minimum": 0, "maximum": MAX_UINT64},
        ]
    }
    defs.update(
        {
            "field_path": array_of(field_path_component, maximum=32),
            "warning": object_of(
                {
                    "code": {"enum": ["no_effect"]},
                    "operation_index": nullable(ref("uint64")),
                    "field_path": ref("field_path"),
                    "message": {"type": "string", "maxLength": 255},
                }
            ),
            "receipt": object_of(
                {
                    "document_id": ref("identifier"),
                    "transaction_id": ref("identifier"),
                    "base_revision": ref("uint64"),
                    "result_revision": ref("uint64"),
                    "created": array_of(ref("identity_ref"), unique=True),
                    "changed": array_of(ref("identity_ref"), unique=True),
                    "dirty_bounds": nullable(ref("rect")),
                    "warnings": array_of(ref("warning"), maximum=4_096),
                }
            ),
            "document_summary": object_of(
                {
                    "document_id": ref("identifier"),
                    "revision": ref("uint64"),
                    "canvas": ref("canvas_extent"),
                    "background": nullable(ref("color")),
                    "limits": ref("resource_limits"),
                    "layer_count": ref("uint64"),
                    "object_count": ref("uint64"),
                    "track_count": ref("uint64"),
                }
            ),
            "structure_node": object_of(
                {
                    "identity": ref("node_ref"),
                    "node_kind": {"enum": ["layer", "group", "rectangle", "ellipse", "path"]},
                    "parent": nullable(ref("parent_ref")),
                    "sibling_index": ref("uint64"),
                    "visible": {"type": "boolean"},
                    "child_count": ref("uint64"),
                }
            ),
            "opacity_track_metadata": object_of(
                {
                    "track_id": ref("identifier"),
                    "start_time_us": ref("uint64"),
                    "duration_us": {"type": "integer", "minimum": 1, "maximum": MAX_UINT64},
                    "from_opacity": ref("opacity"),
                    "to_opacity": ref("opacity"),
                }
            ),
            "selected_fields": object_of(
                {},
                {
                    "kind": {"enum": ["group", "rectangle", "ellipse", "path"]},
                    "parent": ref("parent_ref"),
                    "sibling_index": ref("uint64"),
                    "visible": {"type": "boolean"},
                    "transform": ref("transform"),
                    "geometry": ref("geometry"),
                    "style": ref("style"),
                    "opacity": ref("opacity"),
                    "opacity_track": nullable(ref("opacity_track_metadata")),
                },
            ),
            "selected_object": object_of(
                {"object_id": ref("identifier"), "fields": ref("selected_fields")}
            ),
            "projected_bounds": object_of(
                {
                    "projection": {"enum": ["local_geometry", "document_geometry", "document_painted"]},
                    "bounds": nullable(ref("rect")),
                }
            ),
            "bounds_item": object_of(
                {
                    "target": ref("node_ref"),
                    "projections": array_of(ref("projected_bounds"), minimum=1, maximum=3, unique=True),
                }
            ),
            "summary_query_result": tagged(
                "document_summary", {"summary": ref("document_summary")}
            ),
            "structure_query_result": tagged(
                "structure",
                {
                    "document_id": ref("identifier"),
                    "revision": ref("uint64"),
                    "nodes": array_of(ref("structure_node")),
                },
            ),
            "selected_objects_query_result": tagged(
                "selected_objects",
                {
                    "document_id": ref("identifier"),
                    "revision": ref("uint64"),
                    "objects": array_of(ref("selected_object")),
                },
            ),
            "bounds_query_result": tagged(
                "bounds",
                {
                    "document_id": ref("identifier"),
                    "revision": ref("uint64"),
                    "items": array_of(ref("bounds_item")),
                },
            ),
            "query_result": {
                "oneOf": [
                    ref("summary_query_result"),
                    ref("structure_query_result"),
                    ref("selected_objects_query_result"),
                    ref("bounds_query_result"),
                ]
            },
            "document_created_result": tagged(
                "document_created", {"summary": ref("document_summary")}
            ),
            "inspect_result": tagged("inspect", {"query_result": ref("query_result")}),
            "transaction_result": tagged(
                "transaction",
                {
                    "disposition": {"enum": ["dry_run", "committed", "replayed"]},
                    "receipt": ref("receipt"),
                },
            ),
            "render_result": tagged(
                "render",
                {
                    "document_id": ref("identifier"),
                    "revision": ref("uint64"),
                    "time_us": ref("uint64"),
                    "format": {"enum": ["rgba8", "png"]},
                    "artifact_id": ref("identifier"),
                    "width": {"type": "integer", "minimum": 1, "maximum": 16_384},
                    "height": {"type": "integer", "minimum": 1, "maximum": 16_384},
                    "byte_length": ref("uint64"),
                    "sha256": {"type": "string", "pattern": "^[0-9a-f]{64}$"},
                },
            ),
            "result": {
                "oneOf": [
                    ref("document_created_result"),
                    ref("inspect_result"),
                    ref("transaction_result"),
                    ref("render_result"),
                ]
            },
        }
    )

    error_codes = [
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
        "empty_value",
        "invalid_identifier",
        "embedded_nul",
        "non_finite_number",
        "invalid_extent",
        "invalid_limit",
        "wrong_document",
        "transaction_id_conflict",
        "stale_revision",
        "empty_transaction",
        "nothing_to_undo",
        "nothing_to_redo",
        "cancelled",
        "revision_overflow",
        "allocation_failure",
        "missing_identity",
        "duplicate_identity",
        "invalid_parent",
        "parent_cycle",
        "invalid_geometry",
        "unsupported_node_kind",
        "unsupported_property",
        "number_out_of_range",
        "arithmetic_overflow",
    ]
    defs.update(
        {
            "error": object_of(
                {
                    "source": {"enum": ["encoding", "domain"]},
                    "code": {"enum": error_codes},
                    "retry_advice": {"enum": ["same_request", "refresh_then_retry", "change_request", "not_retryable"]},
                    "operation_index": nullable(ref("uint64")),
                    "field_path": ref("field_path"),
                    "message": {"type": "string", "maxLength": 255},
                },
                {"supported_versions": array_of({"type": "string"}, minimum=1, maximum=8, unique=True)},
            ),
            "request_frame": object_of(
                {"protocol": {"const": PROTOCOL}, "request": ref("request")}
            ),
            "success_frame": object_of(
                {
                    "protocol": {"const": PROTOCOL},
                    "status": {"const": "ok"},
                    "result": ref("result"),
                }
            ),
            "error_frame": object_of(
                {
                    "protocol": {"const": PROTOCOL},
                    "status": {"const": "error"},
                    "error": ref("error"),
                }
            ),
        }
    )

    return {
        "$schema": SCHEMA_DRAFT,
        "title": "DrawForge provisional JSON protocol v1",
        "description": (
            "Provider-neutral Phase 0 interchange contract; compatibility is not "
            "promised before the Phase 1 gate."
        ),
        "oneOf": [ref("request_frame"), ref("success_frame"), ref("error_frame")],
        "$defs": defs,
    }


def identity_transform() -> dict[str, float]:
    return {"a": 1.0, "b": 0.0, "c": 0.0, "d": 1.0, "e": 0.0, "f": 0.0}


def default_limits() -> dict[str, int | float]:
    return {
        "max_identifier_bytes": 64,
        "max_text_bytes": 4_096,
        "max_numeric_magnitude": 65_536.0,
        "max_canvas_dimension": 4_096,
        "max_canvas_pixels": 16_777_216,
        "max_scene_nodes": 4_096,
        "max_transaction_operations": 256,
        "max_output_bytes": 67_108_864,
        "max_nesting_depth": 32,
    }


def example_frames() -> list[dict[str, Any]]:
    protocol = PROTOCOL
    return [
        {
            "protocol": protocol,
            "request": {
                "kind": "create_document",
                "document_id": "status-badge",
                "canvas": {"width": 160, "height": 64},
                "background": None,
            },
        },
        {
            "protocol": protocol,
            "request": {
                "kind": "inspect",
                "query": {
                    "kind": "structure",
                    "document_id": "status-badge",
                    "root": {"kind": "document"},
                    "max_depth": 2,
                    "max_nodes": 8,
                },
            },
        },
        {
            "protocol": protocol,
            "request": {
                "kind": "inspect",
                "query": {
                    "kind": "selected_objects",
                    "document_id": "status-badge",
                    "object_ids": ["indicator", "badge"],
                    "fields": ["parent_order", "geometry", "style", "opacity"],
                },
            },
        },
        {
            "protocol": protocol,
            "request": {
                "kind": "inspect",
                "query": {
                    "kind": "bounds",
                    "document_id": "status-badge",
                    "targets": [{"kind": "object", "id": "badge"}],
                    "projections": ["document_geometry", "document_painted"],
                    "time_us": 0,
                },
            },
        },
        {
            "protocol": protocol,
            "request": {
                "kind": "apply",
                "mode": "dry_run",
                "transaction": {
                    "document_id": "status-badge",
                    "expected_revision": 0,
                    "transaction_id": "create-status-badge-v1",
                    "body": {
                        "kind": "operations",
                        "operations": [
                            {"op": "create_layer", "layer_id": "artwork", "index": 0, "visible": True},
                            {
                                "op": "create_rectangle",
                                "object_id": "badge",
                                "parent": {"kind": "layer", "id": "artwork"},
                                "index": 0,
                                "visible": True,
                                "transform": identity_transform(),
                                "style": {"fill": "#172033ff", "stroke": None},
                                "opacity": 1.0,
                                "geometry": {
                                    "kind": "rectangle",
                                    "x": 0.0,
                                    "y": 0.0,
                                    "width": 160.0,
                                    "height": 64.0,
                                    "radius_x": 16.0,
                                    "radius_y": 16.0,
                                },
                            },
                        ],
                    },
                },
            },
        },
        {
            "protocol": protocol,
            "request": {
                "kind": "render",
                "document_id": "status-badge",
                "expected_revision": 1,
                "time_us": 0,
                "format": "png",
                "artifact_id": "preview-1",
            },
        },
        {
            "protocol": protocol,
            "status": "ok",
            "result": {
                "kind": "transaction",
                "disposition": "committed",
                "receipt": {
                    "document_id": "status-badge",
                    "transaction_id": "create-status-badge-v1",
                    "base_revision": 0,
                    "result_revision": 1,
                    "created": [
                        {"kind": "layer", "id": "artwork"},
                        {"kind": "object", "id": "badge"},
                    ],
                    "changed": [],
                    "dirty_bounds": {"x": 0.0, "y": 0.0, "width": 160.0, "height": 64.0},
                    "warnings": [],
                },
            },
        },
        {
            "protocol": protocol,
            "status": "error",
            "error": {
                "source": "domain",
                "code": "stale_revision",
                "retry_advice": "refresh_then_retry",
                "operation_index": None,
                "field_path": ["expected_revision"],
                "message": "expected revision does not match committed revision",
            },
        },
    ]


def generated_files(repository: Path) -> dict[Path, str]:
    schema_path = repository / "schema/experimental/v1/protocol.schema.json"
    examples_path = repository / "schema/experimental/v1/examples.jsonl"
    schema_text = json.dumps(build_schema(), ensure_ascii=False, indent=2) + "\n"
    examples_text = "".join(
        json.dumps(frame, ensure_ascii=False, sort_keys=True, separators=(",", ":")) + "\n"
        for frame in example_frames()
    )
    return {schema_path: schema_text, examples_path: examples_text}


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    mode = parser.add_mutually_exclusive_group(required=True)
    mode.add_argument("--write", action="store_true", help="write generated contract files")
    mode.add_argument("--check", action="store_true", help="verify generated contract files")
    args = parser.parse_args()
    repository = Path(__file__).resolve().parents[2]
    failures: list[str] = []
    for path, expected in generated_files(repository).items():
        if args.write:
            path.parent.mkdir(parents=True, exist_ok=True)
            path.write_text(expected, encoding="utf-8")
            continue
        try:
            actual = path.read_text(encoding="utf-8")
        except FileNotFoundError:
            failures.append(f"missing generated file: {path.relative_to(repository)}")
            continue
        if actual != expected:
            failures.append(f"stale generated file: {path.relative_to(repository)}")
    if failures:
        for failure in failures:
            print(failure)
        return 1
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
