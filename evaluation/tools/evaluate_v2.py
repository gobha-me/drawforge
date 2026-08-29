#!/usr/bin/env python3
"""Route-neutral DrawForge v2 corpus preparation, replay, and scoring."""

from __future__ import annotations

import argparse
import hashlib
import importlib.util
import json
import math
import re
import shutil
import statistics
import subprocess
import sys
import tempfile
import xml.etree.ElementTree as ET
from pathlib import Path
from typing import Any, Iterable


EVALUATION_ROOT = Path(__file__).resolve().parents[1]
V2_ROOT = EVALUATION_ROOT / "corpus" / "v2"
V2_PATH = V2_ROOT / "corpus.json"
V1_SCRIPT = Path(__file__).with_name("evaluate.py")
PROTOCOL_SCHEMA = EVALUATION_ROOT.parent / "schema" / "experimental" / "v1" / "protocol.schema.json"
PROTOCOL = "drawforge.experimental/v1"
IDENTITY = {"a": 1.0, "b": 0.0, "c": 0.0, "d": 1.0, "e": 0.0, "f": 0.0}
SEMANTIC_FIELDS = [
    "kind", "parent_order", "visibility", "transform", "geometry", "style",
    "opacity", "opacity_track",
]
PATH_TOKEN = re.compile(r"[MmLlHhVvZz]|[-+]?(?:\d+(?:\.\d*)?|\.\d+)(?:[eE][-+]?\d+)?")
PNG_SIGNATURE = b"\x89PNG\r\n\x1a\n"
FAILURE_PNG = bytes.fromhex(
    "89504e470d0a1a0a0000000d49484452000000010000000108060000001f15c489"
    "0000000d4944415408d763f8cfc0f01f000500023f49c2fe590000000049454e44"
    "ae426082"
)
MAX_REVIEW_PNG_BYTES = 16 * 1024 * 1024
MAX_REVIEW_DIMENSION = 4096


def _load_v1() -> Any:
    spec = importlib.util.spec_from_file_location("drawforge_evaluate_v1", V1_SCRIPT)
    if spec is None or spec.loader is None:
        raise RuntimeError("cannot load the v1 evaluator")
    module = importlib.util.module_from_spec(spec)
    spec.loader.exec_module(module)
    return module


v1 = _load_v1()
EvaluationError = v1.EvaluationError


def _safe_v2_path(relative: str) -> Path:
    if not isinstance(relative, str) or not relative:
        raise EvaluationError("v2 corpus path must be non-empty text")
    candidate = (V2_ROOT / relative).resolve()
    if not candidate.is_relative_to(V2_ROOT.resolve()):
        raise EvaluationError(f"corpus path escapes v2 root: {relative}")
    if not candidate.is_file():
        raise EvaluationError(f"v2 corpus file does not exist: {relative}")
    return candidate


def load_corpus() -> dict[str, Any]:
    return v1.load_json(V2_PATH)


def task_index(corpus: dict[str, Any]) -> dict[str, dict[str, Any]]:
    return v1.task_index(corpus)


def _v1_tasks() -> tuple[dict[str, Any], dict[str, dict[str, Any]]]:
    corpus = v1.load_corpus()
    return corpus, v1.task_index(corpus)


def _v1_task(task: dict[str, Any]) -> dict[str, Any]:
    _, tasks = _v1_tasks()
    key = task.get("v1_task")
    if key not in tasks:
        raise EvaluationError(f"unknown inherited v1 task: {key!r}")
    return tasks[key]


def _frame(request: dict[str, Any]) -> str:
    return json.dumps({"protocol": PROTOCOL, "request": request}, separators=(",", ":"))


def _number(value: str | None, field: str, default: float = 0.0) -> float:
    if value is None:
        return default
    try:
        result = float(value)
    except ValueError as error:
        raise EvaluationError(f"unsupported numeric {field}: {value!r}") from error
    if not math.isfinite(result):
        raise EvaluationError(f"non-finite numeric {field}")
    return result


def _color(value: str | None, *, default: str | None) -> str | None:
    if value is None:
        return default
    if value == "none":
        return None
    if re.fullmatch(r"#[0-9a-fA-F]{6}", value):
        return value.lower() + "ff"
    if re.fullmatch(r"#[0-9a-fA-F]{8}", value):
        return value.lower()
    raise EvaluationError(f"unsupported corpus color: {value!r}")


def _transform(value: str | None) -> dict[str, float]:
    if value is None:
        return dict(IDENTITY)
    match = re.fullmatch(
        r"translate\(\s*([-+]?(?:\d+(?:\.\d*)?|\.\d+))"
        r"(?:[ ,]+([-+]?(?:\d+(?:\.\d*)?|\.\d+)))?\s*\)",
        value,
    )
    if match is None:
        raise EvaluationError(f"unsupported corpus transform: {value!r}")
    result = dict(IDENTITY)
    result["e"] = float(match.group(1))
    result["f"] = float(match.group(2) or 0)
    return result


def _path_commands(value: str) -> list[dict[str, Any]]:
    tokens = PATH_TOKEN.findall(value)
    if "".join(tokens).lower() != re.sub(r"[\s,]", "", value).lower():
        raise EvaluationError(f"unsupported corpus path data: {value!r}")
    commands: list[dict[str, Any]] = []
    index = 0
    command: str | None = None
    x = 0.0
    y = 0.0
    first_move = False
    while index < len(tokens):
        token = tokens[index]
        if token.isalpha():
            command = token
            index += 1
            if command in "Zz":
                commands.append({"kind": "close"})
                command = None
                continue
            first_move = command in "Mm"
        if command is None:
            raise EvaluationError(f"incomplete corpus path data: {value!r}")
        if command in "HhVv":
            if index >= len(tokens) or tokens[index].isalpha():
                raise EvaluationError(f"incomplete corpus path data: {value!r}")
            coordinate = float(tokens[index])
            index += 1
            if command == "H":
                x = coordinate
            elif command == "h":
                x += coordinate
            elif command == "V":
                y = coordinate
            else:
                y += coordinate
            commands.append({"kind": "line_to", "point": {"x": x, "y": y}})
            continue
        if index + 1 >= len(tokens) or tokens[index].isalpha() or tokens[index + 1].isalpha():
            raise EvaluationError(f"incomplete corpus path data: {value!r}")
        next_x = float(tokens[index])
        next_y = float(tokens[index + 1])
        index += 2
        relative = command.islower()
        if relative:
            x += next_x
            y += next_y
        else:
            x = next_x
            y = next_y
        kind = "move_to" if first_move else "line_to"
        commands.append({"kind": kind, "point": {"x": x, "y": y}})
        if first_move:
            first_move = False
            command = "l" if relative else "L"
    if not commands:
        raise EvaluationError("corpus path must contain commands")
    return commands


def svg_to_semantic_frames(svg_path: Path, document_id: str) -> list[dict[str, Any]]:
    """Convert only the frozen, trusted corpus SVG subset into protocol frames."""
    v1_corpus = v1.load_corpus()
    root, _, _ = v1.parse_svg(svg_path, v1_corpus["limits"])
    width = _number(root.get("width"), "svg width")
    height = _number(root.get("height"), "svg height")
    if not width.is_integer() or not height.is_integer():
        raise EvaluationError("corpus canvas dimensions must be integers")
    view_box = root.get("viewBox")
    if view_box != f"0 0 {int(width)} {int(height)}":
        raise EvaluationError("corpus converter requires a zero-origin viewBox matching the canvas")

    operations: list[dict[str, Any]] = [
        {"op": "create_layer", "layer_id": "artwork", "index": 0, "visible": True}
    ]

    def visit(
        element: ET.Element,
        parent: dict[str, str],
        sibling_index: int,
        inherited_fill: str | None,
        inherited_stroke: str | None,
        inherited_stroke_width: float,
    ) -> None:
        tag = v1.local_name(element.tag)
        object_id = element.get("id")
        if tag not in {"g", "rect", "circle", "path"} or not object_id:
            raise EvaluationError(f"unsupported or unnamed corpus element: {tag}")
        fill = _color(element.get("fill"), default=inherited_fill)
        stroke = _color(element.get("stroke"), default=inherited_stroke)
        stroke_width = _number(element.get("stroke-width"), "stroke-width", inherited_stroke_width)
        transform = _transform(element.get("transform"))
        if tag == "g":
            operations.append({
                "op": "create_group", "object_id": object_id, "parent": parent,
                "index": sibling_index, "visible": True, "transform": transform,
            })
            child_parent = {"kind": "group", "id": object_id}
            child_index = 0
            for child in element:
                if v1.local_name(child.tag) == "animate":
                    raise EvaluationError("animation cannot target a group in the v2 corpus")
                visit(child, child_parent, child_index, fill, stroke, stroke_width)
                child_index += 1
            return

        style = {
            "fill": fill,
            "stroke": None if stroke is None else {"color": stroke, "width": stroke_width},
        }
        common = {
            "object_id": object_id,
            "parent": parent,
            "index": sibling_index,
            "visible": True,
            "transform": transform,
            "style": style,
            "opacity": _number(element.get("opacity"), "opacity", 1.0),
        }
        if tag == "rect":
            radius_x = _number(element.get("rx"), "rx")
            radius_y = _number(element.get("ry"), "ry", radius_x)
            geometry = {
                "kind": "rectangle", "x": _number(element.get("x"), "x"),
                "y": _number(element.get("y"), "y"),
                "width": _number(element.get("width"), "width"),
                "height": _number(element.get("height"), "height"),
                "radius_x": radius_x, "radius_y": radius_y,
            }
            operations.append({"op": "create_rectangle", **common, "geometry": geometry})
        elif tag == "circle":
            radius = _number(element.get("r"), "r")
            geometry = {
                "kind": "ellipse",
                "center": {"x": _number(element.get("cx"), "cx"), "y": _number(element.get("cy"), "cy")},
                "radius_x": radius, "radius_y": radius,
            }
            operations.append({"op": "create_ellipse", **common, "geometry": geometry})
        else:
            data = element.get("d")
            if data is None:
                raise EvaluationError(f"path #{object_id} has no path data")
            operations.append({
                "op": "create_path", **common,
                "geometry": {"kind": "path", "commands": _path_commands(data)},
            })

        for child in element:
            if v1.local_name(child.tag) != "animate":
                raise EvaluationError(f"unsupported child of #{object_id}: {v1.local_name(child.tag)}")
            track_id = child.get("id")
            duration = child.get("dur", "")
            if (
                not track_id or child.get("attributeName") != "opacity"
                or child.get("fill") != "freeze" or not duration.endswith("ms")
            ):
                raise EvaluationError(f"unsupported opacity animation on #{object_id}")
            operations.append({
                "op": "create_opacity_track", "track_id": track_id,
                "target_object_id": object_id, "start_time_us": 0,
                "duration_us": int(_number(duration[:-2], "animation duration") * 1000),
                "from_opacity": _number(child.get("from"), "animation from"),
                "to_opacity": _number(child.get("to"), "animation to"),
            })

    root_index = 0
    for child in root:
        visit(child, {"kind": "layer", "id": "artwork"}, root_index, "#000000ff", None, 1.0)
        root_index += 1
    create = {
        "kind": "create_document", "document_id": document_id,
        "canvas": {"width": int(width), "height": int(height)}, "background": None,
    }
    apply = {
        "kind": "apply", "mode": "commit",
        "transaction": {
            "document_id": document_id, "expected_revision": 0,
            "transaction_id": f"corpus-{document_id}-v1",
            "body": {"kind": "operations", "operations": operations},
        },
    }
    return [{"protocol": PROTOCOL, "request": create}, {"protocol": PROTOCOL, "request": apply}]


def _write_frames(path: Path, frames: Iterable[dict[str, Any]]) -> None:
    path.write_text(
        "".join(json.dumps(frame, separators=(",", ":"), sort_keys=True) + "\n" for frame in frames),
        encoding="utf-8",
    )


def _load_frames(path: Path, max_bytes: int) -> list[dict[str, Any]]:
    try:
        raw = path.read_bytes()
    except OSError as error:
        raise EvaluationError(f"cannot read transcript {path}: {error.strerror}") from error
    if len(raw) > max_bytes:
        raise EvaluationError(f"transcript exceeds {max_bytes} bytes: {path}")
    frames: list[dict[str, Any]] = []
    for line_number, raw_line in enumerate(raw.splitlines(), start=1):
        if not raw_line.strip():
            raise EvaluationError(f"blank transcript frame at line {line_number}")
        try:
            frame = json.loads(
                raw_line, object_pairs_hook=v1._reject_duplicate_keys,
                parse_constant=v1._reject_nonfinite_json,
            )
        except (UnicodeDecodeError, json.JSONDecodeError) as error:
            raise EvaluationError(f"invalid transcript JSON at line {line_number}: {error}") from error
        if not isinstance(frame, dict) or frame.get("protocol") != PROTOCOL or not isinstance(frame.get("request"), dict):
            raise EvaluationError(f"invalid protocol frame at line {line_number}")
        frames.append(frame)
    if not frames:
        raise EvaluationError(f"transcript has no frames: {path}")
    return frames


def _invoke(binary: Path, frames: list[dict[str, Any]], artifact_dir: Path) -> tuple[int, list[dict[str, Any]], str]:
    payload = "".join(json.dumps(frame, separators=(",", ":")) + "\n" for frame in frames).encode()
    try:
        completed = subprocess.run(
            [str(binary), "jsonl", "--artifact-dir", str(artifact_dir)],
            input=payload, stdout=subprocess.PIPE, stderr=subprocess.PIPE,
            check=False, timeout=30,
        )
    except (OSError, subprocess.TimeoutExpired) as error:
        raise EvaluationError(f"cannot execute DrawForge replay: {error}") from error
    responses: list[dict[str, Any]] = []
    for line_number, line in enumerate(completed.stdout.splitlines(), start=1):
        try:
            value = json.loads(line)
        except json.JSONDecodeError as error:
            raise EvaluationError(f"DrawForge emitted invalid JSON on line {line_number}") from error
        if not isinstance(value, dict):
            raise EvaluationError("DrawForge response must be an object")
        responses.append(value)
    stderr = completed.stderr.decode("utf-8", errors="replace")[:1024]
    return completed.returncode, responses, stderr


def _query_frames(document_id: str, object_ids: list[str]) -> list[dict[str, Any]]:
    requests: list[dict[str, Any]] = [
        {"kind": "inspect", "query": {"kind": "document_summary", "document_id": document_id}},
        {"kind": "inspect", "query": {
            "kind": "structure", "document_id": document_id,
            "root": {"kind": "document"}, "max_depth": 32, "max_nodes": 4096,
        }},
    ]
    if object_ids:
        requests.append({"kind": "inspect", "query": {
            "kind": "selected_objects", "document_id": document_id,
            "object_ids": object_ids, "fields": SEMANTIC_FIELDS,
        }})
    return [{"protocol": PROTOCOL, "request": request} for request in requests]


def _reference_object_ids(task: dict[str, Any]) -> list[str]:
    v1_task = _v1_task(task)
    reference = v1._safe_corpus_path(v1_task["reference"])
    root, _, _ = v1.parse_svg(reference, v1.load_corpus()["limits"])
    return [
        element.get("id") for element in root.iter()
        if element is not root and v1.local_name(element.tag) in {"rect", "circle", "path"}
        and element.get("id") is not None
    ]


def _normalize_query_response(response: dict[str, Any]) -> dict[str, Any]:
    if response.get("status") != "ok":
        raise EvaluationError(f"semantic inspection failed: {response.get('error')!r}")
    result = response.get("result")
    if not isinstance(result, dict) or result.get("kind") != "inspect":
        raise EvaluationError("semantic replay did not return an inspection result")
    query = result.get("query_result")
    if not isinstance(query, dict):
        raise EvaluationError("semantic replay returned malformed query evidence")
    normalized = json.loads(json.dumps(query))
    normalized.pop("revision", None)
    summary = normalized.get("summary")
    if isinstance(summary, dict):
        summary.pop("revision", None)
        summary.pop("limits", None)
    return normalized


def semantic_snapshot(
    binary: Path,
    frames: list[dict[str, Any]],
    document_id: str,
    object_ids: list[str],
    render_times_us: list[int],
    review_destination: Path | None = None,
) -> tuple[list[dict[str, Any]], list[str]]:
    query_frames = _query_frames(document_id, object_ids)
    with tempfile.TemporaryDirectory(prefix="drawforge-eval-v2-") as raw:
        root = Path(raw)
        first_artifacts = root / "query"
        first_artifacts.mkdir()
        returncode, responses, stderr = _invoke(binary, frames + query_frames, first_artifacts)
        if returncode != 0 or len(responses) != len(frames) + len(query_frames):
            errors = [response.get("error") for response in responses if response.get("status") == "error"]
            raise EvaluationError(f"semantic replay failed ({returncode}): {errors!r} {stderr}".strip())
        if any(response.get("status") != "ok" for response in responses):
            raise EvaluationError("semantic replay contains a rejected request")
        normalized = [_normalize_query_response(response) for response in responses[-len(query_frames):]]
        raw_summary = responses[-len(query_frames)]["result"]["query_result"]["summary"]
        revision = raw_summary.get("revision")
        if not isinstance(revision, int) or isinstance(revision, bool):
            raise EvaluationError("semantic summary has no valid revision")
        render_frames = [
            {"protocol": PROTOCOL, "request": {
                "kind": "render", "document_id": document_id,
                "expected_revision": revision, "time_us": time_us,
                "format": "rgba8", "artifact_id": f"review-{index}",
            }}
            for index, time_us in enumerate(render_times_us)
        ]
        if review_destination is not None:
            render_frames.append(
                {"protocol": PROTOCOL, "request": {
                    "kind": "render", "document_id": document_id,
                    "expected_revision": revision, "time_us": 0,
                    "format": "png", "artifact_id": "review-final",
                }}
            )
        render_artifacts = root / "render"
        render_artifacts.mkdir()
        render_code, render_responses, render_stderr = _invoke(binary, frames + render_frames, render_artifacts)
        if render_code != 0 or len(render_responses) != len(frames) + len(render_frames):
            raise EvaluationError(f"semantic render replay failed ({render_code}): {render_stderr}".strip())
        if any(response.get("status") != "ok" for response in render_responses):
            raise EvaluationError("semantic render replay contains a rejected request")
        hashes = [v1.sha256(render_artifacts / f"review-{index}.rgba8") for index in range(len(render_times_us))]
        if review_destination is not None:
            review = render_artifacts / "review-final.png"
            _validate_review_png(review)
            review_destination.parent.mkdir(parents=True, exist_ok=True)
            shutil.copyfile(review, review_destination)
        return normalized, hashes


def _validate_review_png(path: Path) -> None:
    try:
        size = path.stat().st_size
        with path.open("rb") as stream:
            signature = stream.read(len(PNG_SIGNATURE))
    except OSError as error:
        raise EvaluationError(f"cannot read review PNG: {error.strerror}") from error
    if size < len(PNG_SIGNATURE) or size > MAX_REVIEW_PNG_BYTES or signature != PNG_SIGNATURE:
        raise EvaluationError("review renderer produced an invalid or oversized PNG")


def _materialize_direct_review(renderer: Path, candidate: Path, destination: Path) -> None:
    root, _, _ = v1.parse_svg(candidate, v1.load_corpus()["limits"])
    width = _number(root.get("width"), "svg.width")
    height = _number(root.get("height"), "svg.height")
    if width <= 0 or height <= 0:
        raise EvaluationError("direct-SVG review requires positive canvas dimensions")
    thumbnail_size = math.ceil(max(width, height))
    if thumbnail_size > MAX_REVIEW_DIMENSION:
        raise EvaluationError("direct-SVG review canvas exceeds 4096 pixels")
    with tempfile.TemporaryDirectory(prefix="drawforge-direct-review-") as raw:
        output = Path(raw) / "final.png"
        try:
            completed = subprocess.run(
                [
                    str(renderer), "--size", str(thumbnail_size), str(candidate), str(output),
                ],
                stdout=subprocess.PIPE,
                stderr=subprocess.PIPE,
                timeout=30,
                check=False,
            )
        except (OSError, subprocess.SubprocessError) as error:
            raise EvaluationError("direct-SVG review renderer could not run") from error
        if completed.returncode != 0:
            stderr = completed.stderr.decode("utf-8", errors="replace")[:1024]
            raise EvaluationError(f"direct-SVG review renderer failed: {stderr}".rstrip())
        _validate_review_png(output)
        destination.parent.mkdir(parents=True, exist_ok=True)
        shutil.copyfile(output, destination)


def _materialize_failure_review(destination: Path) -> None:
    destination.parent.mkdir(parents=True, exist_ok=True)
    destination.write_bytes(FAILURE_PNG)


def validate_corpus(binary: Path | None) -> list[str]:
    corpus = load_corpus()
    if corpus.get("schema_version") != 2 or corpus.get("corpus_id") != "drawforge-semantic-svg-v2":
        raise EvaluationError("unexpected v2 corpus identity")
    if corpus.get("routes") != ["direct-svg", "semantic"] or corpus.get("trials_per_task") != 5:
        raise EvaluationError("v2 route or trial controls changed")
    tasks = task_index(corpus)
    if {task.get("family") for task in tasks.values()} != {"creation", "revision", "recovery"}:
        raise EvaluationError("v2 corpus must cover all task families")
    diagnostics: list[str] = []
    for task_id, task in tasks.items():
        prompt = _safe_v2_path(task.get("prompt"))
        text = prompt.read_text(encoding="utf-8")
        if not text.strip() or "SVG" in text:
            raise EvaluationError(f"task {task_id} prompt is empty or route-specific")
        inherited = _v1_task(task)
        if inherited.get("family") != task.get("family"):
            raise EvaluationError(f"task {task_id} family differs from v1")
        reference = v1._safe_corpus_path(inherited["reference"])
        diagnostics.extend(f"{task_id}: {item}" for item in v1.score_candidate(v1.load_corpus(), inherited, reference))
        frames = svg_to_semantic_frames(reference, task_id)
        if binary is not None:
            object_ids = _reference_object_ids(task)
            times = [0, 300000, 600000] if task_id == "animate-dot-entrance" else [0]
            semantic_snapshot(binary, frames, task_id, object_ids, times)
    for route in corpus["routes"]:
        _safe_v2_path(f"adapters/{route}.md")
    return diagnostics


def _prompt_bytes(task: dict[str, Any], route: str) -> bytes:
    common = _safe_v2_path(task["prompt"]).read_bytes().rstrip() + b"\n\n"
    adapter = _safe_v2_path(f"adapters/{route}.md").read_bytes().rstrip() + b"\n"
    return common + adapter


def _frozen_hashes() -> dict[str, str]:
    return {
        "corpus_v1": v1.sha256(v1.CORPUS_PATH),
        "corpus_v2": v1.sha256(V2_PATH),
        "protocol_schema": v1.sha256(PROTOCOL_SCHEMA),
    }


def prepare_run(args: argparse.Namespace) -> None:
    corpus = load_corpus()
    tasks = task_index(corpus)
    if args.task not in tasks:
        raise EvaluationError(f"unknown task: {args.task}")
    if args.route not in corpus["routes"]:
        raise EvaluationError(f"unknown route: {args.route}")
    if not 1 <= args.trial <= corpus["trials_per_task"]:
        raise EvaluationError("trial is outside the corpus trial range")
    if args.temperature is not None and (not math.isfinite(args.temperature) or args.temperature < 0):
        raise EvaluationError("temperature must be finite and non-negative")
    provider = v1._require_text(args.provider, "provider")
    model = v1._require_text(args.model, "model")
    model_version = v1._require_text(args.model_version, "model_version")
    output = args.output.resolve()
    if output.exists():
        raise EvaluationError(f"output already exists: {output}")
    output.mkdir(parents=True)
    (output / "attempts").mkdir()
    task = tasks[args.task]
    inherited = _v1_task(task)
    (output / "prompt.md").write_bytes(_prompt_bytes(task, args.route))
    if inherited.get("input"):
        source = v1._safe_corpus_path(inherited["input"])
        if args.route == "direct-svg":
            (output / "source.svg").write_bytes(source.read_bytes())
        else:
            _write_frames(output / "source.jsonl", svg_to_semantic_frames(source, args.task))
    if inherited.get("concurrent_input"):
        concurrent = v1._safe_corpus_path(inherited["concurrent_input"])
        if args.route == "direct-svg":
            (output / "concurrent-source.svg").write_bytes(concurrent.read_bytes())
        else:
            _write_frames(output / "concurrent-source.jsonl", svg_to_semantic_frames(concurrent, args.task))
    metadata = {
        "schema_version": 2,
        "corpus_id": corpus["corpus_id"],
        "task_id": args.task,
        "route": args.route,
        "trial": args.trial,
        "pair_id": f"{args.task}-{args.trial:02d}-{args.seed if args.seed is not None else 'unseeded'}",
        "model": {"provider": provider, "id": model, "version": model_version},
        "sampling": {"seed": args.seed, "temperature": args.temperature},
        "runtime": {
            "drawforge_version": args.drawforge_version,
            "drawforge_sha256": v1.sha256(args.drawforge.resolve()),
            "adapter_version": args.adapter_version,
            "adapter_commit": args.adapter_commit,
            "provider_runtime": args.provider_runtime,
            "direct_svg_renderer_version": v1._require_text(
                args.direct_svg_renderer_version, "direct_svg_renderer_version"
            ),
            "direct_svg_renderer_sha256": v1.sha256(args.direct_svg_renderer.resolve()),
        },
        "frozen": _frozen_hashes(),
        "usage": {"tool_interactions": 0, "input_tokens": None, "output_tokens": None, "cost_usd": None},
        "events": [],
    }
    (output / "run.json").write_text(json.dumps(metadata, indent=2) + "\n", encoding="utf-8")
    print(output)


def _validate_metadata(metadata: dict[str, Any], corpus: dict[str, Any]) -> tuple[dict[str, Any], dict[str, Any]]:
    if metadata.get("schema_version") != 2 or metadata.get("corpus_id") != corpus["corpus_id"]:
        raise EvaluationError("run metadata does not match corpus v2")
    tasks = task_index(corpus)
    task_id = metadata.get("task_id")
    if task_id not in tasks:
        raise EvaluationError(f"unknown run task: {task_id}")
    route = metadata.get("route")
    if route not in corpus["routes"]:
        raise EvaluationError(f"unknown run route: {route}")
    trial = metadata.get("trial")
    if not isinstance(trial, int) or isinstance(trial, bool) or not 1 <= trial <= corpus["trials_per_task"]:
        raise EvaluationError("run trial is outside the corpus trial range")
    pair_id = metadata.get("pair_id")
    v1._require_text(pair_id, "pair_id")
    for container_name, fields in {
        "model": ("provider", "id", "version"),
        "runtime": (
            "drawforge_version", "drawforge_sha256", "adapter_version",
            "adapter_commit", "provider_runtime", "direct_svg_renderer_version",
            "direct_svg_renderer_sha256",
        ),
    }.items():
        container = metadata.get(container_name)
        if not isinstance(container, dict):
            raise EvaluationError(f"run {container_name} must be an object")
        for field in fields:
            v1._require_text(container.get(field), f"{container_name}.{field}")
    sampling = metadata.get("sampling")
    if not isinstance(sampling, dict):
        raise EvaluationError("run sampling must be an object")
    seed = sampling.get("seed")
    if seed is not None and (not isinstance(seed, int) or isinstance(seed, bool) or abs(seed) > 2**63 - 1):
        raise EvaluationError("sampling.seed must be a signed 64-bit integer or null")
    temperature = sampling.get("temperature")
    if temperature is not None and (
        not isinstance(temperature, (int, float)) or isinstance(temperature, bool)
        or not math.isfinite(temperature) or temperature < 0
    ):
        raise EvaluationError("sampling.temperature must be finite and non-negative or null")
    usage = metadata.get("usage")
    if not isinstance(usage, dict):
        raise EvaluationError("run usage must be an object")
    for field in ("tool_interactions", "input_tokens", "output_tokens"):
        v1._validate_nonnegative_optional(usage.get(field), f"usage.{field}")
    cost = usage.get("cost_usd")
    if cost is not None and (
        not isinstance(cost, (int, float)) or isinstance(cost, bool) or not math.isfinite(cost) or cost < 0
    ):
        raise EvaluationError("usage.cost_usd must be finite and non-negative or null")
    if usage.get("tool_interactions") is None or usage["tool_interactions"] > corpus["limits"]["max_tool_interactions"]:
        raise EvaluationError("tool interactions exceed the corpus budget")
    events = metadata.get("events")
    if not isinstance(events, list) or any(not isinstance(event, str) for event in events):
        raise EvaluationError("run events must be an array of strings")
    if len(events) > corpus["limits"]["max_tool_interactions"] * 2:
        raise EvaluationError("run events exceed the bounded evidence limit")
    if metadata.get("frozen") != _frozen_hashes():
        raise EvaluationError("run frozen corpus or protocol hashes do not match")
    return tasks[task_id], _v1_task(tasks[task_id])


def _expected_source_name(route: str, concurrent: bool) -> str:
    stem = "concurrent-source" if concurrent else "source"
    return f"{stem}.svg" if route == "direct-svg" else f"{stem}.jsonl"


def evaluate_run(
    run_dir: Path,
    binary: Path | None,
    direct_renderer: Path | None,
    output_override: Path | None = None,
) -> dict[str, Any]:
    corpus = load_corpus()
    metadata = v1.load_json(run_dir / "run.json")
    task, inherited = _validate_metadata(metadata, corpus)
    route = metadata["route"]
    task_id = metadata["task_id"]
    if binary is not None and v1.sha256(binary) != metadata["runtime"]["drawforge_sha256"]:
        raise EvaluationError("DrawForge executable does not match the frozen runtime hash")
    if direct_renderer is None:
        raise EvaluationError("--direct-svg-renderer is required for review artifacts")
    if v1.sha256(direct_renderer) != metadata["runtime"]["direct_svg_renderer_sha256"]:
        raise EvaluationError("direct-SVG renderer does not match the frozen runtime hash")
    prompt = run_dir / "prompt.md"
    if prompt.read_bytes() != _prompt_bytes(task, route):
        raise EvaluationError("run prompt does not match the frozen v2 prompt")
    events = metadata["events"]
    concurrent = bool(inherited.get("concurrent_input") and "source_refreshed" in events)
    if inherited.get("input"):
        source_name = _expected_source_name(route, concurrent)
        source = run_dir / source_name
        if not source.is_file():
            raise EvaluationError(f"run is missing frozen source: {source_name}")
        inherited_key = "concurrent_input" if concurrent else "input"
        if route == "direct-svg":
            expected = v1._safe_corpus_path(inherited[inherited_key]).read_bytes()
        else:
            expected_frames = svg_to_semantic_frames(v1._safe_corpus_path(inherited[inherited_key]), task_id)
            expected = "".join(
                json.dumps(frame, separators=(",", ":"), sort_keys=True) + "\n" for frame in expected_frames
            ).encode()
        if source.read_bytes() != expected:
            raise EvaluationError(f"run source does not match frozen {inherited_key}")
    suffix = ".svg" if route == "direct-svg" else ".jsonl"
    attempts = sorted((run_dir / "attempts").glob(f"*{suffix}")) if (run_dir / "attempts").is_dir() else []
    if len(attempts) > corpus["limits"]["max_attempts"]:
        raise EvaluationError("run exceeds the attempt budget")
    required_events = list(inherited.get("required_events", []))
    if "submission_accepted" not in required_events:
        required_events.append("submission_accepted")
    diagnostics: list[str] = []
    if not attempts:
        diagnostics.append(f"run has no {route} attempts")
    if not v1._events_contain_in_order(events, required_events):
        diagnostics.append(f"required event order is {required_events!r}")
    if task_id == "recover-invalid-edit" and len(attempts) < 2:
        diagnostics.append("invalid-edit recovery requires at least two attempts")
    valid_result = bool(attempts)
    render_hashes: list[str] = []
    review = run_dir / "review" / "final.png"
    if route == "direct-svg" and attempts:
        try:
            diagnostics.extend(v1.score_candidate(v1.load_corpus(), inherited, attempts[-1]))
            _materialize_direct_review(direct_renderer, attempts[-1], review)
        except EvaluationError as error:
            valid_result = False
            diagnostics.append(str(error))
        if task_id == "recover-invalid-edit" and len(attempts) >= 2:
            try:
                if not v1.score_candidate(v1.load_corpus(), inherited, attempts[0]):
                    diagnostics.append("first invalid-edit attempt unexpectedly satisfied the task")
            except EvaluationError:
                pass
    elif route == "semantic" and attempts:
        if binary is None:
            raise EvaluationError("--drawforge is required for semantic evaluation")
        limits = corpus["limits"]["max_transcript_bytes"]
        source_frames: list[dict[str, Any]] = []
        if inherited.get("input"):
            source_frames = _load_frames(run_dir / _expected_source_name(route, concurrent), limits)
        attempt_frames = _load_frames(attempts[-1], limits)
        reference_frames = svg_to_semantic_frames(v1._safe_corpus_path(inherited["reference"]), task_id)
        object_ids = _reference_object_ids(task)
        times = [0, 300000, 600000] if task_id == "animate-dot-entrance" else [0]
        try:
            actual_snapshot, render_hashes = semantic_snapshot(
                binary, source_frames + attempt_frames, task_id, object_ids, times, review
            )
            expected_snapshot, expected_render_hashes = semantic_snapshot(
                binary, reference_frames, task_id, object_ids, times
            )
            if actual_snapshot != expected_snapshot:
                diagnostics.append("semantic document differs from the frozen reference snapshot")
            if render_hashes != expected_render_hashes:
                diagnostics.append("semantic render differs from the frozen reference render")
        except EvaluationError as error:
            valid_result = False
            diagnostics.append(str(error))
        if task_id == "recover-invalid-edit" and len(attempts) >= 2:
            try:
                semantic_snapshot(binary, source_frames + _load_frames(attempts[0], limits), task_id, object_ids, times)
                diagnostics.append("first invalid-edit attempt unexpectedly replayed successfully")
            except EvaluationError:
                pass
    if not review.is_file():
        _materialize_failure_review(review)
    _validate_review_png(review)
    hashes: dict[str, Any] = {
        "prompt": v1.sha256(prompt),
        "attempts": [{"file": path.name, "sha256": v1.sha256(path)} for path in attempts],
    }
    if inherited.get("input"):
        source_name = _expected_source_name(route, concurrent)
        hashes["input"] = v1.sha256(run_dir / source_name)
    if render_hashes:
        hashes["renders"] = render_hashes
    hashes["review"] = v1.sha256(review)
    bounded = v1._bounded_diagnostics(diagnostics, corpus["limits"]["max_diagnostic_bytes"])
    result = {
        "schema_version": 2,
        "corpus_id": corpus["corpus_id"],
        "task_id": task_id,
        "family": task["family"],
        "route": route,
        "trial": metadata["trial"],
        "pair_id": metadata["pair_id"],
        "model": metadata["model"],
        "sampling": metadata["sampling"],
        "runtime": metadata["runtime"],
        "frozen": metadata["frozen"],
        "valid_result": valid_result,
        "task_complete": not diagnostics,
        "attempt_count": len(attempts),
        "rejected_attempt_count": sum(1 for event in events if event.startswith("submission_rejected_")),
        "recovered": task["family"] == "recovery" and not diagnostics,
        "unintended_change_count": sum(
            1 for diagnostic in diagnostics
            if any(marker in diagnostic for marker in v1.UNINTENDED_CHANGE_MARKERS)
            or "differs from the frozen reference" in diagnostic
        ),
        "usage": metadata["usage"],
        "diagnostics": bounded,
        "hashes": hashes,
    }
    output = output_override.resolve() if output_override else run_dir / "result.json"
    output.write_text(json.dumps(result, indent=2, sort_keys=True) + "\n", encoding="utf-8")
    return result


def aggregate_results(root: Path, output: Path | None) -> dict[str, Any]:
    results = [v1.load_json(path) for path in sorted(root.glob("**/result.json"))]
    if not results:
        raise EvaluationError("no result.json files found")
    corpus = load_corpus()
    expected = len(task_index(corpus)) * corpus["trials_per_task"] * len(corpus["routes"])
    keys: set[tuple[str, int, str]] = set()
    pair_routes: dict[str, dict[str, dict[str, Any]]] = {}
    for result in results:
        if result.get("schema_version") != 2 or result.get("corpus_id") != corpus["corpus_id"]:
            raise EvaluationError("aggregate contains a non-v2 result")
        task_id = result.get("task_id")
        trial = result.get("trial")
        route = result.get("route")
        if task_id not in task_index(corpus) or route not in corpus["routes"]:
            raise EvaluationError("aggregate contains an unknown task or route")
        if not isinstance(trial, int) or isinstance(trial, bool) or not 1 <= trial <= corpus["trials_per_task"]:
            raise EvaluationError("aggregate contains an invalid trial")
        key = (task_id, trial, route)
        if key in keys:
            raise EvaluationError(f"duplicate task trial route result: {key!r}")
        keys.add(key)
        pair_id = result.get("pair_id")
        if not isinstance(pair_id, str):
            raise EvaluationError("aggregate result has no pair_id")
        routes = pair_routes.setdefault(pair_id, {})
        if route in routes:
            raise EvaluationError(f"duplicate paired route result: {(pair_id, route)!r}")
        routes[route] = result
    for pair_id, routes in pair_routes.items():
        if set(routes) == set(corpus["routes"]):
            direct = routes["direct-svg"]
            semantic = routes["semantic"]
            for field in ("task_id", "trial", "model", "sampling", "runtime", "frozen"):
                if direct.get(field) != semantic.get(field):
                    raise EvaluationError(f"paired runs disagree on {field}: {pair_id}")

    def metrics(items: list[dict[str, Any]]) -> dict[str, Any]:
        count = len(items)
        interactions = [item["usage"]["tool_interactions"] for item in items]
        reported_costs = [item["usage"].get("cost_usd") for item in items]
        return {
            "runs": count,
            "valid_rate": sum(bool(item["valid_result"]) for item in items) / count,
            "completion_rate": sum(bool(item["task_complete"]) for item in items) / count,
            "unintended_change_rate": sum(item["unintended_change_count"] > 0 for item in items) / count,
            "recovery_rate": sum(bool(item["recovered"]) for item in items) / count,
            "median_tool_interactions": statistics.median(interactions),
            "input_tokens": sum(item["usage"]["input_tokens"] or 0 for item in items),
            "output_tokens": sum(item["usage"]["output_tokens"] or 0 for item in items),
            "cost_reported_runs": sum(cost is not None for cost in reported_costs),
            "cost_usd": round(sum(cost for cost in reported_costs if cost is not None), 6),
        }

    families: dict[str, Any] = {}
    for family in ("creation", "revision", "recovery"):
        families[family] = {}
        for route in corpus["routes"]:
            selected = [item for item in results if item.get("family") == family and item.get("route") == route]
            if selected:
                families[family][route] = metrics(selected)
    reported_costs = [item["usage"].get("cost_usd") for item in results]
    total_cost = round(sum(cost for cost in reported_costs if cost is not None), 6)
    aggregate = {
        "schema_version": 2,
        "corpus_id": corpus["corpus_id"],
        "complete_matrix": len(results) == expected,
        "expected_runs": expected,
        "observed_runs": len(results),
        "paired_runs": sum(set(routes) == set(corpus["routes"]) for routes in pair_routes.values()),
        "spend": {
            "hard_ceiling_usd": 3.0,
            "reported_runs": sum(cost is not None for cost in reported_costs),
            "total_usd": total_cost,
            "verified_within_ceiling": len(reported_costs) == expected and all(
                cost is not None for cost in reported_costs
            ) and total_cost <= 3.0,
        },
        "routes": {route: metrics([item for item in results if item.get("route") == route]) for route in corpus["routes"]},
        "families": families,
        "decision": "pending-human-review",
    }
    destination = output.resolve() if output else root / "aggregate.json"
    destination.write_text(json.dumps(aggregate, indent=2, sort_keys=True) + "\n", encoding="utf-8")
    return aggregate


def prepare_review(args: argparse.Namespace) -> dict[str, Any]:
    corpus = load_corpus()
    results_with_runs: list[tuple[dict[str, Any], Path]] = []
    for result_path in sorted(args.root.resolve().glob("**/result.json")):
        result = v1.load_json(result_path)
        if result.get("schema_version") != 2 or result.get("corpus_id") != corpus["corpus_id"]:
            raise EvaluationError(f"review input is not a v2 result: {result_path}")
        run = result_path.parent
        artifacts = sorted((run / "review").glob("*.png")) if (run / "review").is_dir() else []
        if not artifacts:
            raise EvaluationError(f"review PNG is missing for {run}")
        results_with_runs.append((result, run))
    if not results_with_runs:
        raise EvaluationError("no reviewable v2 results found")
    expected = len(task_index(corpus)) * corpus["trials_per_task"] * len(corpus["routes"])
    if len(results_with_runs) != expected and not args.allow_incomplete:
        raise EvaluationError(f"review matrix is incomplete: wanted {expected}, got {len(results_with_runs)}")
    try:
        blind_key = args.blind_key_file.read_bytes()
    except OSError as error:
        raise EvaluationError(f"cannot read blind key: {error.strerror}") from error
    if not blind_key or len(blind_key) > 256:
        raise EvaluationError("blind key must contain between 1 and 256 bytes")
    output = args.output.resolve()
    key_output = args.key_output.resolve()
    if output.exists() or key_output.exists():
        raise EvaluationError("review output and key output must not already exist")
    if key_output.is_relative_to(output):
        raise EvaluationError("the sealed route key must be outside the review packet")

    prepared: list[tuple[str, dict[str, Any], Path, list[Path]]] = []
    blind_ids: set[str] = set()
    for result, run in results_with_runs:
        identity = f"{result['pair_id']}\0{result['route']}".encode()
        blind_id = hashlib.sha256(blind_key + b"\0" + identity).hexdigest()[:16]
        if blind_id in blind_ids:
            raise EvaluationError("blind identifier collision")
        blind_ids.add(blind_id)
        prepared.append((blind_id, result, run, sorted((run / "review").glob("*.png"))))
    prepared.sort(key=lambda item: item[0])

    (output / "artifacts").mkdir(parents=True)
    (output / "prompts").mkdir()
    (output / "sources").mkdir()
    tasks = task_index(corpus)
    for task_id, task in tasks.items():
        shutil.copyfile(_safe_v2_path(task["prompt"]), output / "prompts" / f"{task_id}.md")
        inherited = _v1_task(task)
        if inherited.get("input"):
            shutil.copyfile(
                v1._safe_corpus_path(inherited["input"]),
                output / "sources" / f"{task_id}.svg",
            )

    public_items: list[dict[str, Any]] = []
    sealed_items: list[dict[str, Any]] = []
    for blind_id, result, _, artifacts in prepared:
        copied: list[dict[str, str]] = []
        for index, source in enumerate(artifacts):
            name = f"{blind_id}-{index:02d}.png"
            destination = output / "artifacts" / name
            shutil.copyfile(source, destination)
            copied.append({"file": f"artifacts/{name}", "sha256": v1.sha256(destination)})
        public_items.append({
            "blind_id": blind_id,
            "task_id": result["task_id"],
            "trial": result["trial"],
            "prompt": f"prompts/{result['task_id']}.md",
            "source": f"sources/{result['task_id']}.svg"
            if (output / "sources" / f"{result['task_id']}.svg").exists() else None,
            "artifacts": copied,
        })
        sealed_items.append({
            "blind_id": blind_id,
            "task_id": result["task_id"],
            "trial": result["trial"],
            "pair_id": result["pair_id"],
            "route": result["route"],
        })
    manifest = {
        "schema_version": 2,
        "corpus_id": corpus["corpus_id"],
        "blinded": True,
        "complete_matrix": len(prepared) == expected,
        "items": public_items,
    }
    sealed = {
        "schema_version": 2,
        "corpus_id": corpus["corpus_id"],
        "packet_manifest_sha256": "",
        "items": sealed_items,
    }
    manifest_path = output / "manifest.json"
    manifest_path.write_text(json.dumps(manifest, indent=2, sort_keys=True) + "\n", encoding="utf-8")
    sealed["packet_manifest_sha256"] = v1.sha256(manifest_path)
    key_output.parent.mkdir(parents=True, exist_ok=True)
    key_output.write_text(json.dumps(sealed, indent=2, sort_keys=True) + "\n", encoding="utf-8")
    return manifest


def build_parser() -> argparse.ArgumentParser:
    parser = argparse.ArgumentParser(description=__doc__)
    subparsers = parser.add_subparsers(dest="command", required=True)
    verify = subparsers.add_parser("verify-corpus", help="validate both v2 routes and references")
    verify.add_argument("--drawforge", type=Path)
    prepare = subparsers.add_parser("prepare-run", help="create a frozen v2 run bundle")
    prepare.add_argument("--task", required=True)
    prepare.add_argument("--route", required=True, choices=("direct-svg", "semantic"))
    prepare.add_argument("--output", required=True, type=Path)
    prepare.add_argument("--provider", required=True)
    prepare.add_argument("--model", required=True)
    prepare.add_argument("--model-version", required=True)
    prepare.add_argument("--drawforge-version", required=True)
    prepare.add_argument("--drawforge", required=True, type=Path)
    prepare.add_argument("--adapter-version", required=True)
    prepare.add_argument("--adapter-commit", required=True)
    prepare.add_argument("--provider-runtime", required=True)
    prepare.add_argument("--direct-svg-renderer", required=True, type=Path)
    prepare.add_argument("--direct-svg-renderer-version", required=True)
    prepare.add_argument("--trial", required=True, type=int)
    prepare.add_argument("--seed", type=int)
    prepare.add_argument("--temperature", type=float)
    evaluate = subparsers.add_parser("evaluate-run", help="score a direct or semantic v2 run")
    evaluate.add_argument("--run", required=True, type=Path)
    evaluate.add_argument("--drawforge", type=Path)
    evaluate.add_argument("--direct-svg-renderer", required=True, type=Path)
    evaluate.add_argument("--output", type=Path)
    aggregate = subparsers.add_parser("aggregate", help="aggregate a complete paired result matrix")
    aggregate.add_argument("--root", required=True, type=Path)
    aggregate.add_argument("--output", type=Path)
    review = subparsers.add_parser("prepare-review", help="create a route-blinded visual review packet")
    review.add_argument("--root", required=True, type=Path)
    review.add_argument("--output", required=True, type=Path)
    review.add_argument("--key-output", required=True, type=Path)
    review.add_argument("--blind-key-file", required=True, type=Path)
    review.add_argument("--allow-incomplete", action="store_true")
    return parser


def main(argv: list[str] | None = None) -> int:
    args = build_parser().parse_args(argv)
    try:
        if args.command == "verify-corpus":
            diagnostics = validate_corpus(args.drawforge.resolve() if args.drawforge else None)
            if diagnostics:
                for diagnostic in diagnostics:
                    print(diagnostic, file=sys.stderr)
                return 1
            suffix = " with semantic replay" if args.drawforge else ""
            print(f"drawforge-semantic-svg-v2: 9 tasks and 2 routes verified{suffix}")
            return 0
        if args.command == "prepare-run":
            prepare_run(args)
            return 0
        if args.command == "evaluate-run":
            binary = args.drawforge.resolve() if args.drawforge else None
            result = evaluate_run(
                args.run.resolve(), binary, args.direct_svg_renderer.resolve(), args.output
            )
            print(json.dumps(result, indent=2, sort_keys=True))
            return 0 if result["task_complete"] else 1
        if args.command == "aggregate":
            result = aggregate_results(args.root.resolve(), args.output)
            print(json.dumps(result, indent=2, sort_keys=True))
            return 0
        if args.command == "prepare-review":
            result = prepare_review(args)
            print(json.dumps(result, indent=2, sort_keys=True))
            return 0
        raise AssertionError(f"unhandled command: {args.command}")
    except EvaluationError as error:
        print(f"error: {error}", file=sys.stderr)
        return 2


if __name__ == "__main__":
    raise SystemExit(main())
