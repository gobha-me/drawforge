#!/usr/bin/env python3
"""Actual-process checks for the headless JSONL command."""

from __future__ import annotations

import hashlib
import json
import subprocess
import sys
import tempfile
from pathlib import Path


sys.dont_write_bytecode = True
REPOSITORY = Path(__file__).resolve().parents[2]
sys.path.insert(0, str(REPOSITORY / "spike" / "encoding"))

from codec import decode_frame  # noqa: E402
from verify import all_operations  # noqa: E402


PROTOCOL = "drawforge.experimental/v1"


def frame(request: dict[str, object]) -> bytes:
    return (json.dumps({"protocol": PROTOCOL, "request": request}, separators=(",", ":")) + "\n").encode()


def create() -> dict[str, object]:
    return {
        "kind": "create_document",
        "document_id": "integration",
        "canvas": {"width": 8, "height": 8},
        "background": "#123456ff",
    }


def render() -> dict[str, object]:
    return {
        "kind": "render",
        "document_id": "integration",
        "expected_revision": 0,
        "time_us": 0,
        "format": "rgba8",
        "artifact_id": "preview",
    }


def all_operation_variants() -> dict[str, object]:
    operations = all_operations()
    operations[12]["start_time_us"] = 0
    operations[14]["index"] = 999
    return {
        "kind": "apply",
        "mode": "commit",
        "transaction": {
            "document_id": "integration",
            "expected_revision": 0,
            "transaction_id": "all-operations-v1",
            "body": {"kind": "operations", "operations": operations},
        },
    }


def query_pipeline() -> bytes:
    apply = {
        "kind": "apply",
        "mode": "commit",
        "transaction": {
            "document_id": "integration",
            "expected_revision": 0,
            "transaction_id": "query-scene-v1",
            "body": {"kind": "operations", "operations": all_operations()[:6]},
        },
    }
    queries = [
        {"kind": "document_summary", "document_id": "integration"},
        {
            "kind": "structure",
            "document_id": "integration",
            "root": {"kind": "document"},
            "max_depth": 4,
            "max_nodes": 16,
        },
        {
            "kind": "selected_objects",
            "document_id": "integration",
            "object_ids": ["rectangle", "ellipse", "path"],
            "fields": [
                "kind",
                "parent_order",
                "visibility",
                "transform",
                "geometry",
                "style",
                "opacity",
                "opacity_track",
            ],
        },
        {
            "kind": "bounds",
            "document_id": "integration",
            "targets": [{"kind": "object", "id": "rectangle"}],
            "projections": ["local_geometry", "document_geometry", "document_painted"],
            "time_us": 0,
        },
    ]
    payload = frame(create()) + frame(apply)
    return payload + b"".join(frame({"kind": "inspect", "query": query}) for query in queries)


def invoke(binary: Path, directory: Path, payload: bytes) -> subprocess.CompletedProcess[bytes]:
    return subprocess.run(
        [binary, "jsonl", "--artifact-dir", directory],
        input=payload,
        stdout=subprocess.PIPE,
        stderr=subprocess.PIPE,
        check=False,
        timeout=30,
    )


def responses(output: bytes) -> list[dict[str, object]]:
    decoded = [decode_frame(line).value for line in output.splitlines(keepends=True)]
    return [dict(value) for value in decoded]


def main() -> int:
    binary = Path(sys.argv[1])
    with tempfile.TemporaryDirectory(prefix="drawforge-issue11-integration-") as raw:
        directory = Path(raw)
        first = invoke(binary, directory, frame(create()) + frame(render()))
        assert first.returncode == 0, (first.returncode, first.stdout, first.stderr)
        assert first.stderr == b""
        first_responses = responses(first.stdout)
        assert len(first_responses) == 2
        assert all(response["status"] == "ok" for response in first_responses)
        artifact = directory / "preview.rgba8"
        assert artifact.read_bytes() == bytes.fromhex("123456ff") * 64
        assert first_responses[1]["result"]["sha256"] == hashlib.sha256(artifact.read_bytes()).hexdigest()

        collision = invoke(binary, directory, frame(create()) + frame(render()))
        assert collision.returncode == 5
        collision_responses = responses(collision.stdout)
        assert collision_responses[-1]["error"]["source"] == "adapter"
        assert collision_responses[-1]["error"]["code"] == "artifact_exists"

        malformed = invoke(binary, directory, b"\n" + frame(create()))
        assert malformed.returncode == 3
        malformed_responses = responses(malformed.stdout)
        assert len(malformed_responses) == 2
        assert malformed_responses[0]["error"]["code"] == "invalid_json"
        assert malformed_responses[1]["status"] == "ok"

        truncated = invoke(binary, directory, b'{"protocol":')
        assert truncated.returncode == 3
        truncated_responses = responses(truncated.stdout)
        assert len(truncated_responses) == 1
        assert truncated_responses[0]["error"]["code"] == "invalid_json"

        variants = invoke(binary, directory, frame(create()) + frame(all_operation_variants()))
        assert variants.returncode == 4
        variant_responses = responses(variants.stdout)
        assert len(variant_responses) == 2
        assert variant_responses[0]["status"] == "ok"
        assert variant_responses[1]["status"] == "error"
        assert variant_responses[1]["error"]["source"] == "domain"

        query_directory = directory / "queries"
        query_directory.mkdir()
        queried = invoke(binary, query_directory, query_pipeline())
        assert queried.returncode == 0, (queried.returncode, queried.stdout, queried.stderr)
        query_responses = responses(queried.stdout)
        assert len(query_responses) == 6
        assert all(response["status"] == "ok" for response in query_responses)
        assert [response["result"]["kind"] for response in query_responses[2:]] == [
            "inspect",
            "inspect",
            "inspect",
            "inspect",
        ]

        missing = subprocess.run(
            [binary, "jsonl", "--artifact-dir", directory / "missing"],
            input=b"",
            stdout=subprocess.PIPE,
            stderr=subprocess.PIPE,
            check=False,
            timeout=10,
        )
        assert missing.returncode == 2
        assert missing.stdout == b""
        assert b"existing directory" in missing.stderr
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
