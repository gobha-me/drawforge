#!/usr/bin/env python3
"""Failure-first conformance evidence for ADR-0005."""

from __future__ import annotations

import copy
import hashlib
import json
import sys
import unittest
from pathlib import Path
from typing import Any

from codec import WireError, decode_frame, decode_stream, encode_frame
from contract import (
    DEFAULT_WIRE_LIMITS,
    MAX_UINT64,
    PROTOCOL,
    WireLimits,
    build_schema,
    default_limits,
    example_frames,
    generated_files,
    identity_transform,
)


REPOSITORY = Path(__file__).resolve().parents[2]
EXPECTED_SUMMARY_SHA256 = "4b670621f1c03d0f30437a1eb86bc69cd30fc32b4a2373c1b557bc5a666abdf1"


def request_frame(request: dict[str, Any]) -> dict[str, Any]:
    return {"protocol": PROTOCOL, "request": request}


def apply_frame(body: dict[str, Any], *, mode: str = "commit") -> dict[str, Any]:
    return request_frame(
        {
            "kind": "apply",
            "mode": mode,
            "transaction": {
                "document_id": "scene",
                "expected_revision": 0,
                "transaction_id": "transaction-1",
                "body": body,
            },
        }
    )


def style() -> dict[str, Any]:
    return {"fill": "#112233ff", "stroke": {"color": "#ffffffff", "width": 2.0}}


def rectangle_geometry() -> dict[str, Any]:
    return {
        "kind": "rectangle",
        "x": 1.0,
        "y": 2.0,
        "width": 20.0,
        "height": 10.0,
        "radius_x": 2.0,
        "radius_y": 2.0,
    }


def ellipse_geometry() -> dict[str, Any]:
    return {
        "kind": "ellipse",
        "center": {"x": 12.0, "y": 8.0},
        "radius_x": 4.0,
        "radius_y": 3.0,
    }


def path_geometry() -> dict[str, Any]:
    return {
        "kind": "path",
        "commands": [
            {"kind": "move_to", "point": {"x": 0.0, "y": 0.0}},
            {"kind": "line_to", "point": {"x": 8.0, "y": 8.0}},
            {"kind": "close"},
        ],
    }


def all_operations() -> list[dict[str, Any]]:
    parent = {"kind": "layer", "id": "artwork"}
    drawable = {
        "parent": parent,
        "index": 0,
        "visible": True,
        "transform": identity_transform(),
        "style": style(),
        "opacity": 0.75,
    }
    return [
        {"op": "create_layer", "layer_id": "artwork", "index": 0, "visible": True},
        {
            "op": "create_group",
            "object_id": "group",
            "parent": parent,
            "index": 0,
            "visible": True,
            "transform": identity_transform(),
        },
        {"op": "create_rectangle", "object_id": "rectangle", **drawable, "geometry": rectangle_geometry()},
        {"op": "create_ellipse", "object_id": "ellipse", **drawable, "geometry": ellipse_geometry()},
        {"op": "create_path", "object_id": "path", **drawable, "geometry": path_geometry()},
        {
            "op": "create_opacity_track",
            "track_id": "fade",
            "target_object_id": "ellipse",
            "start_time_us": 0,
            "duration_us": MAX_UINT64,
            "from_opacity": 0.0,
            "to_opacity": 1.0,
        },
        {"op": "set_canvas_background", "background": "#00000000"},
        {"op": "set_visibility", "target": {"kind": "object", "id": "ellipse"}, "visible": False},
        {"op": "set_transform", "object_id": "group", "transform": identity_transform()},
        {"op": "set_geometry", "object_id": "rectangle", "geometry": ellipse_geometry()},
        {"op": "set_style", "object_id": "rectangle", "style": style()},
        {"op": "set_opacity", "object_id": "rectangle", "opacity": 1.0},
        {
            "op": "set_opacity_track",
            "track_id": "fade",
            "target_object_id": "ellipse",
            "start_time_us": MAX_UINT64,
            "duration_us": 1,
            "from_opacity": 1.0,
            "to_opacity": 0.0,
        },
        {"op": "reparent_object", "object_id": "ellipse", "parent": {"kind": "group", "id": "group"}, "index": 0},
        {"op": "reorder_object", "object_id": "rectangle", "index": MAX_UINT64},
    ]


def document_summary() -> dict[str, Any]:
    return {
        "document_id": "scene",
        "revision": 4,
        "canvas": {"width": 160, "height": 64},
        "background": "#ffffffff",
        "limits": default_limits(),
        "layer_count": 1,
        "object_count": 3,
        "track_count": 1,
    }


def success_frames() -> list[dict[str, Any]]:
    created_summary = document_summary()
    created_summary["revision"] = 0
    return [
        {
            "protocol": PROTOCOL,
            "status": "ok",
            "result": {"kind": "document_created", "summary": created_summary},
        },
        {
            "protocol": PROTOCOL,
            "status": "ok",
            "result": {
                "kind": "inspect",
                "query_result": {"kind": "document_summary", "summary": document_summary()},
            },
        },
        {
            "protocol": PROTOCOL,
            "status": "ok",
            "result": {
                "kind": "inspect",
                "query_result": {
                    "kind": "structure",
                    "document_id": "scene",
                    "revision": 4,
                    "nodes": [
                        {
                            "identity": {"kind": "layer", "id": "artwork"},
                            "node_kind": "layer",
                            "parent": None,
                            "sibling_index": 0,
                            "visible": True,
                            "child_count": 1,
                        },
                        {
                            "identity": {"kind": "object", "id": "rectangle"},
                            "node_kind": "rectangle",
                            "parent": {"kind": "layer", "id": "artwork"},
                            "sibling_index": 0,
                            "visible": True,
                            "child_count": 0,
                        },
                    ],
                },
            },
        },
        {
            "protocol": PROTOCOL,
            "status": "ok",
            "result": {
                "kind": "inspect",
                "query_result": {
                    "kind": "selected_objects",
                    "document_id": "scene",
                    "revision": 4,
                    "objects": [
                        {
                            "object_id": "rectangle",
                            "fields": {
                                "kind": "rectangle",
                                "parent": {"kind": "layer", "id": "artwork"},
                                "sibling_index": 0,
                                "visible": True,
                                "transform": identity_transform(),
                                "geometry": rectangle_geometry(),
                                "style": style(),
                                "opacity": 1.0,
                                "opacity_track": None,
                            },
                        }
                    ],
                },
            },
        },
        {
            "protocol": PROTOCOL,
            "status": "ok",
            "result": {
                "kind": "inspect",
                "query_result": {
                    "kind": "bounds",
                    "document_id": "scene",
                    "revision": 4,
                    "items": [
                        {
                            "target": {"kind": "object", "id": "rectangle"},
                            "projections": [
                                {
                                    "projection": "document_geometry",
                                    "bounds": {
                                        "x": 1.0,
                                        "y": 2.0,
                                        "width": 20.0,
                                        "height": 10.0,
                                    },
                                },
                                {"projection": "document_painted", "bounds": None},
                            ],
                        }
                    ],
                },
            },
        },
        {
            "protocol": PROTOCOL,
            "status": "ok",
            "result": {
                "kind": "render",
                "document_id": "scene",
                "revision": MAX_UINT64,
                "time_us": MAX_UINT64,
                "format": "rgba8",
                "artifact_id": "preview",
                "width": 1,
                "height": 1,
                "byte_length": 4,
                "sha256": "0" * 64,
            },
        },
    ]


def all_valid_frames() -> list[dict[str, Any]]:
    frames = example_frames()
    frames.extend(
        [
            request_frame(
                {
                    "kind": "inspect",
                    "query": {"kind": "document_summary", "document_id": "scene"},
                }
            ),
            apply_frame({"kind": "operations", "operations": all_operations()}),
            apply_frame({"kind": "undo"}),
            apply_frame({"kind": "redo"}, mode="dry_run"),
        ]
    )
    frames.extend(success_frames())
    return frames


class EncodingContractTests(unittest.TestCase):
    def assert_wire_error(self, code: str, raw: bytes | str, limits: WireLimits = DEFAULT_WIRE_LIMITS) -> WireError:
        with self.assertRaises(WireError) as context:
            decode_frame(raw, limits)
        self.assertEqual(context.exception.code, code)
        self.assertLessEqual(len(context.exception.message.encode("utf-8")), 255)
        return context.exception

    def encoded(self, frame: dict[str, Any]) -> bytes:
        return json.dumps(frame, ensure_ascii=False, separators=(",", ":"), allow_nan=False).encode("utf-8")

    def test_failure_matrix_rejects_bad_frames_before_schema(self) -> None:
        self.assert_wire_error("invalid_utf8", b"\xff")
        self.assert_wire_error("invalid_utf8", b"\xef\xbb\xbf{}")
        self.assert_wire_error("invalid_utf8", b'{"protocol":"drawforge.experimental/v1","x":"\x00"}')
        self.assert_wire_error("invalid_json", b"")
        self.assert_wire_error("invalid_json", b"\n")
        self.assert_wire_error("invalid_json", b"{} {}")
        self.assert_wire_error("invalid_json", b"{}\n{}")
        self.assert_wire_error("invalid_json", b'{"protocol":')
        self.assert_wire_error("invalid_value", b'{"protocol":NaN}')
        self.assert_wire_error("invalid_value", b'{"protocol":1e10000}')
        self.assert_wire_error(
            "duplicate_key",
            b'{"protocol":"drawforge.experimental/v1","protocol":"drawforge.experimental/v1"}',
        )

    def test_failure_matrix_rejects_schema_drift_and_ambiguity(self) -> None:
        base = request_frame({"kind": "inspect", "query": {"kind": "document_summary", "document_id": "scene"}})
        future = copy.deepcopy(base)
        future["protocol"] = "drawforge.experimental/v2"
        error = self.assert_wire_error("unsupported_version", self.encoded(future))
        self.assertEqual(error.as_error_frame()["error"]["supported_versions"], [PROTOCOL])

        unknown = copy.deepcopy(base)
        unknown["request"]["surprise"] = True
        self.assert_wire_error("unknown_field", self.encoded(unknown))
        missing = copy.deepcopy(base)
        del missing["request"]["query"]
        self.assert_wire_error("missing_field", self.encoded(missing))
        wrong_type = copy.deepcopy(base)
        wrong_type["request"]["query"]["document_id"] = 7
        self.assert_wire_error("invalid_type", self.encoded(wrong_type))
        future_kind = copy.deepcopy(base)
        future_kind["request"]["query"]["kind"] = "everything"
        self.assert_wire_error("invalid_value", self.encoded(future_kind))
        inline_binary = copy.deepcopy(base)
        inline_binary["request"]["bytes"] = "AAAA"
        self.assert_wire_error("unknown_field", self.encoded(inline_binary))

    def test_failure_matrix_rejects_invalid_semantic_values(self) -> None:
        create = example_frames()[0]
        uppercase = copy.deepcopy(create)
        uppercase["request"]["background"] = "#FFFFFFFF"
        self.assert_wire_error("invalid_value", self.encoded(uppercase))
        bad_id = copy.deepcopy(create)
        bad_id["request"]["document_id"] = "1-scene"
        self.assert_wire_error("invalid_value", self.encoded(bad_id))
        too_small = copy.deepcopy(create)
        too_small["request"]["limits"] = default_limits()
        too_small["request"]["limits"]["max_canvas_pixels"] = 1
        self.assert_wire_error("resource_limit", self.encoded(too_small))

        selected = example_frames()[2]
        duplicate = copy.deepcopy(selected)
        duplicate["request"]["query"]["object_ids"] = ["badge", "badge"]
        self.assert_wire_error("invalid_value", self.encoded(duplicate))

        operations = all_operations()
        radius = copy.deepcopy(operations[2])
        radius["geometry"]["radius_x"] = 11.0
        self.assert_wire_error(
            "invalid_value",
            self.encoded(apply_frame({"kind": "operations", "operations": [radius]})),
        )
        for commands in (
            [{"kind": "line_to", "point": {"x": 0.0, "y": 0.0}}],
            [{"kind": "move_to", "point": {"x": 0.0, "y": 0.0}}],
            [
                {"kind": "move_to", "point": {"x": 0.0, "y": 0.0}},
                {"kind": "close"},
            ],
        ):
            path = copy.deepcopy(operations[4])
            path["geometry"]["commands"] = commands
            self.assert_wire_error(
                "invalid_value",
                self.encoded(apply_frame({"kind": "operations", "operations": [path]})),
            )

    def test_failure_matrix_rejects_invalid_receipts(self) -> None:
        receipt_frame = example_frames()[6]
        overlap = copy.deepcopy(receipt_frame)
        overlap["result"]["receipt"]["changed"] = [{"kind": "object", "id": "badge"}]
        self.assert_wire_error("invalid_value", self.encoded(overlap))
        revision = copy.deepcopy(receipt_frame)
        revision["result"]["receipt"]["result_revision"] = 2
        self.assert_wire_error("invalid_value", self.encoded(revision))

    def test_failure_matrix_rejects_inconsistent_results_and_errors(self) -> None:
        created = success_frames()[0]
        wrong_created_revision = copy.deepcopy(created)
        wrong_created_revision["result"]["summary"]["revision"] = 1
        self.assert_wire_error("invalid_value", self.encoded(wrong_created_revision))

        structure = success_frames()[2]
        wrong_identity_domain = copy.deepcopy(structure)
        wrong_identity_domain["result"]["query_result"]["nodes"][0]["identity"] = {
            "kind": "object",
            "id": "artwork",
        }
        self.assert_wire_error("invalid_value", self.encoded(wrong_identity_domain))

        bounds = success_frames()[4]
        duplicate_projection = copy.deepcopy(bounds)
        duplicate_projection["result"]["query_result"]["items"][0]["projections"][1][
            "projection"
        ] = "document_geometry"
        self.assert_wire_error("invalid_value", self.encoded(duplicate_projection))

        domain_error = example_frames()[7]
        wrong_source = copy.deepcopy(domain_error)
        wrong_source["error"]["source"] = "encoding"
        self.assert_wire_error("invalid_value", self.encoded(wrong_source))
        adapter_error = example_frames()[8]
        wrong_adapter_source = copy.deepcopy(adapter_error)
        wrong_adapter_source["error"]["source"] = "domain"
        self.assert_wire_error("invalid_value", self.encoded(wrong_adapter_source))
        adapter_operation = copy.deepcopy(adapter_error)
        adapter_operation["error"]["operation_index"] = 0
        self.assert_wire_error("invalid_value", self.encoded(adapter_operation))
        extra_versions = copy.deepcopy(domain_error)
        extra_versions["error"]["supported_versions"] = [PROTOCOL]
        self.assert_wire_error("unknown_field", self.encoded(extra_versions))

        unsupported = WireError(
            "unsupported_version",
            "frame uses an unsupported protocol version",
            ("protocol",),
        ).as_error_frame()
        missing_versions = copy.deepcopy(unsupported)
        del missing_versions["error"]["supported_versions"]
        self.assert_wire_error("invalid_value", self.encoded(missing_versions))

    def test_failure_matrix_preserves_integer_contract(self) -> None:
        render = example_frames()[5]
        fractional = copy.deepcopy(render)
        fractional["request"]["expected_revision"] = 1.5
        self.assert_wire_error("invalid_type", self.encoded(fractional))
        overflow = copy.deepcopy(render)
        overflow["request"]["expected_revision"] = MAX_UINT64 + 1
        self.assert_wire_error("invalid_value", self.encoded(overflow))

    def test_failure_matrix_enforces_injected_wire_limits(self) -> None:
        raw = encode_frame(example_frames()[0])
        frame_limit = WireLimits(max_frame_bytes=len(raw) - 1)
        self.assert_wire_error("resource_limit", raw, frame_limit)
        depth_limit = WireLimits(max_json_depth=2)
        self.assert_wire_error("nesting_limit", raw, depth_limit)
        member_limit = WireLimits(max_object_members=2)
        self.assert_wire_error("resource_limit", raw, member_limit)
        array_frame = self.encoded(apply_frame({"kind": "operations", "operations": all_operations()[:2]}))
        array_limit = WireLimits(max_array_items=1)
        self.assert_wire_error("resource_limit", array_frame, array_limit)
        number_limit = WireLimits(max_number_token_bytes=1)
        self.assert_wire_error("resource_limit", raw, number_limit)
        with self.assertRaises(ValueError):
            decode_frame(raw, WireLimits(max_json_depth=33))

    def test_every_variant_round_trips_deterministically(self) -> None:
        digest = hashlib.sha256()
        categories: set[str] = set()
        for frame in all_valid_frames():
            encoded = encode_frame(frame)
            first = decode_frame(encoded)
            second = decode_frame(encode_frame(first))
            self.assertEqual(first, second)
            self.assertEqual(encoded, encode_frame(second))
            categories.add(first.category)
            digest.update(encoded)
        self.assertEqual(categories, {"request", "result", "error"})
        self.assertEqual(digest.hexdigest(), EXPECTED_SUMMARY_SHA256)

    def test_stream_framing_accepts_lf_crlf_and_complete_eof(self) -> None:
        first = encode_frame(example_frames()[0])
        second = encode_frame(example_frames()[1]).replace(b"\n", b"\r\n")
        third = encode_frame(example_frames()[2]).rstrip(b"\n")
        decoded = list(decode_stream(first + second + third))
        self.assertEqual([item.value for item in decoded], example_frames()[:3])
        with self.assertRaises(WireError):
            list(decode_stream(first + b"\n" + second))

    def test_negative_zero_is_canonicalized_and_full_uint64_is_exact(self) -> None:
        frame = request_frame(
            {
                "kind": "render",
                "document_id": "scene",
                "expected_revision": MAX_UINT64,
                "time_us": MAX_UINT64,
                "format": "png",
                "artifact_id": "preview",
            }
        )
        transform_frame = apply_frame(
            {
                "kind": "operations",
                "operations": [
                    {
                        "op": "set_transform",
                        "object_id": "object",
                        "transform": {"a": 1.0, "b": -0.0, "c": 0.0, "d": 1.0, "e": -0.0, "f": 0.0},
                    }
                ],
            }
        )
        self.assertEqual(decode_frame(encode_frame(frame)).value, frame)
        encoded = encode_frame(transform_frame)
        self.assertNotIn(b"-0.0", encoded)

    def test_generated_schema_and_examples_are_current(self) -> None:
        for path, expected in generated_files(REPOSITORY).items():
            self.assertEqual(path.read_text(encoding="utf-8"), expected)
        schema = build_schema()
        self.assertEqual(schema["$schema"], "https://json-schema.org/draft/2020-12/schema")
        self.assertFalse(schema["$defs"]["request_frame"]["additionalProperties"])
        examples = (REPOSITORY / "schema/experimental/v1/examples.jsonl").read_bytes()
        self.assertEqual(len(list(decode_stream(examples))), len(example_frames()))


def main() -> int:
    suite = unittest.defaultTestLoader.loadTestsFromTestCase(EncodingContractTests)
    result = unittest.TextTestRunner(verbosity=2).run(suite)
    if result.wasSuccessful():
        print(f"encoding contract summary sha256: {EXPECTED_SUMMARY_SHA256}")
        print(f"validated frames: {len(all_valid_frames())}")
    return 0 if result.wasSuccessful() else 1


if __name__ == "__main__":
    sys.exit(main())
