#!/usr/bin/env python3
"""Provider-neutral DrawForge corpus preparation and direct-SVG scoring."""

from __future__ import annotations

import argparse
import hashlib
import json
import math
import re
import shutil
import sys
import xml.etree.ElementTree as ET
from pathlib import Path
from typing import Any


EVALUATION_ROOT = Path(__file__).resolve().parents[1]
CORPUS_ROOT = EVALUATION_ROOT / "corpus" / "v1"
CORPUS_PATH = CORPUS_ROOT / "corpus.json"
SVG_NAMESPACE = "http://www.w3.org/2000/svg"
EXTERNAL_REFERENCE = re.compile(r"(?:https?:|file:|data:|//)", re.IGNORECASE)
NUMERIC_ATTRIBUTES = {
    "x", "y", "x1", "y1", "x2", "y2", "cx", "cy", "r", "rx", "ry",
    "width", "height", "opacity", "fill-opacity", "stroke-opacity",
    "stroke-width", "stroke-miterlimit", "stroke-dashoffset",
}
UNINTENDED_CHANGE_MARKERS = (" changed", "protected element", "unexpected SVG element count")


class EvaluationError(Exception):
    """A bounded, user-facing evaluation failure."""


def _reject_duplicate_keys(pairs: list[tuple[str, Any]]) -> dict[str, Any]:
    result: dict[str, Any] = {}
    for key, value in pairs:
        if key in result:
            raise EvaluationError(f"duplicate JSON key: {key}")
        result[key] = value
    return result


def _reject_nonfinite_json(value: str) -> None:
    raise EvaluationError(f"non-finite JSON number: {value}")


def load_json(path: Path, max_bytes: int = 1_048_576) -> dict[str, Any]:
    try:
        raw = path.read_bytes()
    except OSError as error:
        raise EvaluationError(f"cannot read {path}: {error.strerror}") from error
    if len(raw) > max_bytes:
        raise EvaluationError(f"JSON exceeds {max_bytes} bytes: {path}")
    try:
        value = json.loads(
            raw,
            object_pairs_hook=_reject_duplicate_keys,
            parse_constant=_reject_nonfinite_json,
        )
    except (UnicodeDecodeError, json.JSONDecodeError) as error:
        raise EvaluationError(f"invalid JSON in {path}: {error}") from error
    if not isinstance(value, dict):
        raise EvaluationError(f"JSON root must be an object: {path}")
    return value


def sha256(path: Path) -> str:
    digest = hashlib.sha256()
    try:
        with path.open("rb") as stream:
            for chunk in iter(lambda: stream.read(65536), b""):
                digest.update(chunk)
    except OSError as error:
        raise EvaluationError(f"cannot hash {path}: {error.strerror}") from error
    return f"sha256:{digest.hexdigest()}"


def local_name(name: str) -> str:
    return name.rsplit("}", 1)[-1]


def _canonical(element: ET.Element) -> tuple[Any, ...]:
    attributes = tuple(sorted((local_name(key), value) for key, value in element.attrib.items()))
    text = (element.text or "").strip()
    children = tuple(_canonical(child) for child in element)
    return local_name(element.tag), attributes, text, children


def _safe_corpus_path(relative: str) -> Path:
    if not isinstance(relative, str) or not relative:
        raise EvaluationError("corpus path must be a non-empty string")
    candidate = (CORPUS_ROOT / relative).resolve()
    if not candidate.is_relative_to(CORPUS_ROOT.resolve()):
        raise EvaluationError(f"corpus path escapes v1 root: {relative}")
    if not candidate.is_file():
        raise EvaluationError(f"corpus file does not exist: {relative}")
    return candidate


def load_corpus() -> dict[str, Any]:
    return load_json(CORPUS_PATH)


def task_index(corpus: dict[str, Any]) -> dict[str, dict[str, Any]]:
    tasks = corpus.get("tasks")
    if not isinstance(tasks, list):
        raise EvaluationError("corpus tasks must be an array")
    result: dict[str, dict[str, Any]] = {}
    for task in tasks:
        if not isinstance(task, dict):
            raise EvaluationError("every corpus task must be an object")
        task_id = task.get("id")
        if not isinstance(task_id, str) or not re.fullmatch(r"[a-z0-9]+(?:-[a-z0-9]+)*", task_id):
            raise EvaluationError(f"invalid task id: {task_id!r}")
        if task_id in result:
            raise EvaluationError(f"duplicate task id: {task_id}")
        result[task_id] = task
    return result


def parse_svg(path: Path, limits: dict[str, Any]) -> tuple[ET.Element, dict[str, ET.Element], dict[str, ET.Element]]:
    try:
        raw = path.read_bytes()
    except OSError as error:
        raise EvaluationError(f"cannot read SVG {path}: {error.strerror}") from error
    max_bytes = limits.get("max_svg_bytes")
    if not isinstance(max_bytes, int) or max_bytes <= 0:
        raise EvaluationError("max_svg_bytes must be a positive integer")
    if len(raw) > max_bytes:
        raise EvaluationError(f"SVG exceeds {max_bytes} bytes")
    lowered = raw.lower()
    if b"<!doctype" in lowered or b"<!entity" in lowered:
        raise EvaluationError("SVG document types and entities are forbidden")
    try:
        root = ET.fromstring(raw)
    except ET.ParseError as error:
        raise EvaluationError(f"malformed SVG: {error}") from error
    if root.tag != f"{{{SVG_NAMESPACE}}}svg":
        raise EvaluationError("root must be an SVG element in the SVG namespace")

    elements = list(root.iter())
    max_elements = limits.get("max_elements")
    if not isinstance(max_elements, int) or max_elements <= 0:
        raise EvaluationError("max_elements must be a positive integer")
    if len(elements) > max_elements:
        raise EvaluationError(f"SVG exceeds {max_elements} elements")

    by_id: dict[str, ET.Element] = {}
    parents: dict[str, ET.Element] = {}
    for parent in elements:
        if local_name(parent.tag) in {"script", "style"}:
            raise EvaluationError("script and style elements are forbidden")
        for child in parent:
            child_id = child.get("id")
            if child_id:
                parents[child_id] = parent
        element_id = parent.get("id")
        if element_id:
            if element_id in by_id:
                raise EvaluationError(f"duplicate SVG id: {element_id}")
            by_id[element_id] = parent
        for key, value in parent.attrib.items():
            key_name = local_name(key)
            if key_name == "href" and value and not value.startswith("#"):
                raise EvaluationError(f"external reference is forbidden on {element_id or local_name(parent.tag)}")
            if EXTERNAL_REFERENCE.search(value) or "url(" in value.lower() and "url(#" not in value.lower():
                raise EvaluationError(f"external resource is forbidden on {element_id or local_name(parent.tag)}")
            if key_name in NUMERIC_ATTRIBUTES:
                numeric = value.removesuffix("px")
                try:
                    if not math.isfinite(float(numeric)):
                        raise ValueError
                except ValueError as error:
                    raise EvaluationError(f"non-finite numeric attribute {key_name} on {element_id or local_name(parent.tag)}") from error
    return root, by_id, parents


def _source_path(task: dict[str, Any]) -> Path | None:
    selected = task.get("criteria_input", "input")
    relative = task.get(selected)
    return _safe_corpus_path(relative) if relative else None


def score_candidate(corpus: dict[str, Any], task: dict[str, Any], candidate: Path) -> list[str]:
    limits = corpus.get("limits")
    if not isinstance(limits, dict):
        raise EvaluationError("corpus limits must be an object")
    candidate_root, candidate_ids, candidate_parents = parse_svg(candidate, limits)
    source_path = _source_path(task)
    source_root: ET.Element | None = None
    source_ids: dict[str, ET.Element] = {}
    if source_path:
        source_root, source_ids, _ = parse_svg(source_path, limits)

    criteria = task.get("criteria")
    if not isinstance(criteria, dict):
        raise EvaluationError(f"task {task['id']} criteria must be an object")
    diagnostics: list[str] = []

    element_count = criteria.get("element_count")
    if element_count is not None:
        if not isinstance(element_count, int) or isinstance(element_count, bool) or element_count <= 0:
            raise EvaluationError("element_count must be a positive integer")
        actual_count = sum(1 for _ in candidate_root.iter())
        if actual_count != element_count:
            diagnostics.append(f"unexpected SVG element count: wanted {element_count}, got {actual_count}")

    root_attributes = criteria.get("root_attributes", {})
    if not isinstance(root_attributes, dict):
        raise EvaluationError("root_attributes must be an object")
    for attribute, expected in root_attributes.items():
        if candidate_root.get(attribute) != expected:
            diagnostics.append(f"root attribute {attribute} must be {expected!r}")
    if source_root is not None:
        for attribute in ("width", "height", "viewBox"):
            if candidate_root.get(attribute) != source_root.get(attribute):
                diagnostics.append(f"root attribute {attribute} changed")

    for expected in criteria.get("elements", []):
        if not isinstance(expected, dict) or not isinstance(expected.get("id"), str):
            raise EvaluationError("element criteria require an id")
        element_id = expected["id"]
        element = candidate_ids.get(element_id)
        if element is None:
            diagnostics.append(f"missing element #{element_id}")
            continue
        expected_tag = expected.get("tag")
        if expected_tag and local_name(element.tag) != expected_tag:
            diagnostics.append(f"#{element_id} must be <{expected_tag}>")
        attributes = expected.get("attributes", {})
        if not isinstance(attributes, dict):
            raise EvaluationError(f"attributes for #{element_id} must be an object")
        for attribute, value in attributes.items():
            if element.get(attribute) != value:
                diagnostics.append(f"#{element_id} attribute {attribute} must be {value!r}")

    for element_id in criteria.get("unchanged", []):
        before = source_ids.get(element_id)
        after = candidate_ids.get(element_id)
        if before is None:
            raise EvaluationError(f"unchanged source element does not exist: {element_id}")
        if after is None:
            diagnostics.append(f"protected element #{element_id} is missing")
        elif _canonical(before) != _canonical(after):
            diagnostics.append(f"protected element #{element_id} changed")

    for preservation in criteria.get("preserve_attributes", []):
        element_id = preservation.get("id") if isinstance(preservation, dict) else None
        attributes = preservation.get("attributes") if isinstance(preservation, dict) else None
        if not isinstance(element_id, str) or not isinstance(attributes, list):
            raise EvaluationError("preserve_attributes entries require id and attributes")
        before = source_ids.get(element_id)
        after = candidate_ids.get(element_id)
        if before is None:
            raise EvaluationError(f"preserved source element does not exist: {element_id}")
        if after is None:
            diagnostics.append(f"preserved element #{element_id} is missing")
            continue
        for attribute in attributes:
            if not isinstance(attribute, str):
                raise EvaluationError("preserved attribute names must be strings")
            if before.get(attribute) != after.get(attribute):
                diagnostics.append(f"#{element_id} attribute {attribute} changed")

    for allowance in criteria.get("only_change_attributes", []):
        element_id = allowance.get("id") if isinstance(allowance, dict) else None
        allowed = allowance.get("attributes") if isinstance(allowance, dict) else None
        if not isinstance(element_id, str) or not isinstance(allowed, list) or any(not isinstance(item, str) for item in allowed):
            raise EvaluationError("only_change_attributes entries require id and attributes")
        before = source_ids.get(element_id)
        after = candidate_ids.get(element_id)
        if before is None:
            raise EvaluationError(f"change-controlled source element does not exist: {element_id}")
        if after is None:
            diagnostics.append(f"protected element #{element_id} is missing")
            continue
        before_attributes = {key: value for key, value in before.attrib.items() if local_name(key) not in allowed}
        after_attributes = {key: value for key, value in after.attrib.items() if local_name(key) not in allowed}
        if local_name(before.tag) != local_name(after.tag) or before_attributes != after_attributes:
            diagnostics.append(f"#{element_id} attributes outside {allowed!r} changed")

    for relation in criteria.get("parent", []):
        element_id = relation.get("id") if isinstance(relation, dict) else None
        parent_id = relation.get("parent") if isinstance(relation, dict) else None
        if not isinstance(element_id, str) or not isinstance(parent_id, str):
            raise EvaluationError("parent criteria require id and parent")
        parent = candidate_parents.get(element_id)
        if parent is None or parent.get("id") != parent_id:
            diagnostics.append(f"#{element_id} must be a direct child of #{parent_id}")

    child_order = criteria.get("child_order")
    if child_order is not None:
        if not isinstance(child_order, dict) or not isinstance(child_order.get("parent"), str) or not isinstance(child_order.get("ids"), list):
            raise EvaluationError("child_order requires parent and ids")
        parent = candidate_ids.get(child_order["parent"])
        if parent is None:
            diagnostics.append(f"missing parent #{child_order['parent']}")
        else:
            actual = [child.get("id") for child in parent]
            if actual != child_order["ids"]:
                diagnostics.append(f"#{child_order['parent']} child order must be {child_order['ids']!r}")
    return diagnostics


def validate_corpus() -> list[str]:
    corpus = load_corpus()
    if corpus.get("schema_version") != 1:
        raise EvaluationError("unsupported corpus schema_version")
    if corpus.get("corpus_id") != "drawforge-semantic-svg-v1":
        raise EvaluationError("unexpected corpus_id")
    if corpus.get("trials_per_task") != 5:
        raise EvaluationError("v1 requires five trials per task")
    tasks = task_index(corpus)
    expected_families = {"creation", "revision", "recovery"}
    if {task.get("family") for task in tasks.values()} != expected_families:
        raise EvaluationError("corpus must cover creation, revision, and recovery")
    diagnostics: list[str] = []
    for task_id, task in tasks.items():
        prompt = _safe_corpus_path(task.get("prompt"))
        if not prompt.read_text(encoding="utf-8").strip():
            raise EvaluationError(f"task {task_id} prompt is empty")
        reference = _safe_corpus_path(task.get("reference"))
        if task.get("input"):
            _safe_corpus_path(task["input"])
        if task.get("concurrent_input"):
            _safe_corpus_path(task["concurrent_input"])
        if not isinstance(task.get("human_review"), list) or not task["human_review"]:
            raise EvaluationError(f"task {task_id} needs human-review criteria")
        required_events = task.get("required_events", [])
        if not isinstance(required_events, list) or any(not isinstance(item, str) for item in required_events):
            raise EvaluationError(f"task {task_id} required_events must be strings")
        failures = score_candidate(corpus, task, reference)
        diagnostics.extend(f"{task_id}: {failure}" for failure in failures)
    return diagnostics


def _require_text(value: Any, field: str) -> str:
    if not isinstance(value, str) or not value.strip() or len(value.encode("utf-8")) > 256:
        raise EvaluationError(f"{field} must be non-empty bounded text")
    return value


def prepare_run(args: argparse.Namespace) -> None:
    corpus = load_corpus()
    tasks = task_index(corpus)
    if args.task not in tasks:
        raise EvaluationError(f"unknown task: {args.task}")
    if args.trial < 1 or args.trial > corpus["trials_per_task"]:
        raise EvaluationError(f"trial must be between 1 and {corpus['trials_per_task']}")
    provider = _require_text(args.provider, "provider")
    model = _require_text(args.model, "model")
    model_version = _require_text(args.model_version, "model_version")
    if args.temperature is not None and not math.isfinite(args.temperature):
        raise EvaluationError("temperature must be finite or omitted")
    output = args.output.resolve()
    if output.exists():
        raise EvaluationError(f"output already exists: {output}")
    task = tasks[args.task]
    output.mkdir(parents=True)
    (output / "attempts").mkdir()
    shutil.copyfile(_safe_corpus_path(task["prompt"]), output / "prompt.md")
    if task.get("input"):
        shutil.copyfile(_safe_corpus_path(task["input"]), output / "source.svg")
    metadata = {
        "schema_version": 1,
        "corpus_id": corpus["corpus_id"],
        "task_id": args.task,
        "route": "direct-svg",
        "trial": args.trial,
        "model": {
            "provider": provider,
            "id": model,
            "version": model_version,
        },
        "sampling": {"seed": args.seed, "temperature": args.temperature},
        "usage": {"tool_interactions": 0, "input_tokens": None, "output_tokens": None},
        "events": [],
    }
    (output / "run.json").write_text(json.dumps(metadata, indent=2) + "\n", encoding="utf-8")
    print(output)


def _validate_nonnegative_optional(value: Any, field: str) -> None:
    if value is not None and (not isinstance(value, int) or isinstance(value, bool) or value < 0):
        raise EvaluationError(f"{field} must be a non-negative integer or null")


def _events_contain_in_order(actual: list[str], required: list[str]) -> bool:
    position = 0
    for event in actual:
        if position < len(required) and event == required[position]:
            position += 1
    return position == len(required)


def _bounded_diagnostics(diagnostics: list[str], max_bytes: int) -> list[str]:
    result: list[str] = []
    used = 2
    for diagnostic in diagnostics:
        encoded = diagnostic.encode("utf-8")
        remaining = max_bytes - used
        if remaining <= 0:
            break
        if len(encoded) + 3 > remaining:
            clipped = encoded[: max(0, remaining - 6)].decode("utf-8", errors="ignore")
            result.append(f"{clipped}...")
            break
        result.append(diagnostic)
        used += len(encoded) + 3
    return result


def evaluate_run(run_dir: Path, output_override: Path | None = None) -> dict[str, Any]:
    corpus = load_corpus()
    tasks = task_index(corpus)
    metadata = load_json(run_dir / "run.json")
    if metadata.get("schema_version") != 1 or metadata.get("corpus_id") != corpus["corpus_id"]:
        raise EvaluationError("run metadata does not match corpus v1")
    task_id = metadata.get("task_id")
    if task_id not in tasks:
        raise EvaluationError(f"unknown run task: {task_id}")
    if metadata.get("route") != "direct-svg":
        raise EvaluationError("v1 runner supports only the direct-svg route")
    trial = metadata.get("trial")
    if not isinstance(trial, int) or isinstance(trial, bool) or not 1 <= trial <= corpus["trials_per_task"]:
        raise EvaluationError("run trial is outside the corpus trial range")
    model = metadata.get("model")
    if not isinstance(model, dict):
        raise EvaluationError("run model must be an object")
    for field in ("provider", "id", "version"):
        _require_text(model.get(field), f"model.{field}")
    sampling = metadata.get("sampling")
    if not isinstance(sampling, dict):
        raise EvaluationError("run sampling must be an object")
    seed = sampling.get("seed")
    if seed is not None and (not isinstance(seed, int) or isinstance(seed, bool) or abs(seed) > 2**63 - 1):
        raise EvaluationError("sampling.seed must be a signed 64-bit integer or null")
    temperature = sampling.get("temperature")
    if temperature is not None and (
        not isinstance(temperature, (int, float))
        or isinstance(temperature, bool)
        or not math.isfinite(temperature)
        or temperature < 0
    ):
        raise EvaluationError("sampling.temperature must be a finite non-negative number or null")
    usage = metadata.get("usage")
    if not isinstance(usage, dict):
        raise EvaluationError("run usage must be an object")
    for field in ("tool_interactions", "input_tokens", "output_tokens"):
        _validate_nonnegative_optional(usage.get(field), f"usage.{field}")
    if usage.get("tool_interactions") is None or usage["tool_interactions"] > corpus["limits"]["max_tool_interactions"]:
        raise EvaluationError("tool_interactions exceeds the corpus budget")
    events = metadata.get("events")
    if not isinstance(events, list) or any(not isinstance(event, str) for event in events):
        raise EvaluationError("run events must be an array of strings")
    if len(events) > corpus["limits"]["max_tool_interactions"] * 2:
        raise EvaluationError("run events exceed the bounded evidence limit")
    for event in events:
        _require_text(event, "event")

    task = tasks[task_id]
    expected_prompt = _safe_corpus_path(task["prompt"])
    run_prompt = run_dir / "prompt.md"
    if sha256(run_prompt) != sha256(expected_prompt):
        raise EvaluationError("run prompt does not match the frozen corpus prompt")
    if task.get("input"):
        expected_source_key = "concurrent_input" if task.get("concurrent_input") and "source_refreshed" in events else "input"
        expected_source = _safe_corpus_path(task[expected_source_key])
        if sha256(run_dir / "source.svg") != sha256(expected_source):
            raise EvaluationError(f"run source does not match frozen {expected_source_key}")

    attempts_dir = run_dir / "attempts"
    attempts = sorted(attempts_dir.glob("*.svg")) if attempts_dir.is_dir() else []
    if not attempts:
        raise EvaluationError("run has no SVG attempts")
    if len(attempts) > corpus["limits"]["max_attempts"]:
        raise EvaluationError("run exceeds the attempt budget")
    required_events = list(task.get("required_events", []))
    if "submission_accepted" not in required_events:
        required_events.append("submission_accepted")
    diagnostics: list[str] = []
    if not _events_contain_in_order(events, required_events):
        diagnostics.append(f"required event order is {required_events!r}")
    if task_id == "recover-invalid-edit" and len(attempts) < 2:
        diagnostics.append("invalid-edit recovery requires at least two attempts")

    final = attempts[-1]
    valid_result = True
    try:
        diagnostics.extend(score_candidate(corpus, task, final))
    except EvaluationError as error:
        valid_result = False
        diagnostics.append(str(error))
    if task_id == "recover-invalid-edit" and len(attempts) >= 2:
        try:
            first_failures = score_candidate(corpus, task, attempts[0])
            if not first_failures:
                diagnostics.append("first invalid-edit attempt unexpectedly satisfied the task")
        except EvaluationError:
            pass

    hashes: dict[str, Any] = {
        "prompt": sha256(run_prompt),
        "attempts": [{"file": attempt.name, "sha256": sha256(attempt)} for attempt in attempts],
    }
    if task.get("input"):
        hashes["input"] = sha256(run_dir / "source.svg")
    if task.get("concurrent_input"):
        hashes["concurrent_input"] = sha256(_safe_corpus_path(task["concurrent_input"]))
    bounded = _bounded_diagnostics(diagnostics, corpus["limits"]["max_diagnostic_bytes"])
    rejected_attempts = sum(1 for event in events if event.startswith("submission_rejected_"))
    result = {
        "schema_version": 1,
        "corpus_id": corpus["corpus_id"],
        "task_id": task_id,
        "route": "direct-svg",
        "trial": trial,
        "valid_result": valid_result,
        "task_complete": not diagnostics,
        "attempt_count": len(attempts),
        "rejected_attempt_count": rejected_attempts,
        "recovered": task.get("family") == "recovery" and not diagnostics,
        "unintended_change_count": sum(
            1 for diagnostic in diagnostics if any(marker in diagnostic for marker in UNINTENDED_CHANGE_MARKERS)
        ),
        "usage": usage,
        "diagnostics": bounded,
        "hashes": hashes,
    }
    output = output_override.resolve() if output_override else run_dir / "result.json"
    output.write_text(json.dumps(result, indent=2, sort_keys=True) + "\n", encoding="utf-8")
    return result


def build_parser() -> argparse.ArgumentParser:
    parser = argparse.ArgumentParser(description=__doc__)
    subparsers = parser.add_subparsers(dest="command", required=True)
    subparsers.add_parser("verify-corpus", help="validate corpus files and reference outputs")

    prepare = subparsers.add_parser("prepare-run", help="create a fresh direct-SVG run bundle")
    prepare.add_argument("--task", required=True)
    prepare.add_argument("--output", required=True, type=Path)
    prepare.add_argument("--provider", required=True)
    prepare.add_argument("--model", required=True)
    prepare.add_argument("--model-version", required=True)
    prepare.add_argument("--trial", required=True, type=int)
    prepare.add_argument("--seed", type=int)
    prepare.add_argument("--temperature", type=float)

    evaluate = subparsers.add_parser("evaluate-run", help="score the final SVG in a run bundle")
    evaluate.add_argument("--run", required=True, type=Path)
    evaluate.add_argument("--output", type=Path)
    return parser


def main(argv: list[str] | None = None) -> int:
    args = build_parser().parse_args(argv)
    try:
        if args.command == "verify-corpus":
            diagnostics = validate_corpus()
            if diagnostics:
                for diagnostic in diagnostics:
                    print(diagnostic, file=sys.stderr)
                return 1
            print("drawforge-semantic-svg-v1: 9 tasks verified")
            return 0
        if args.command == "prepare-run":
            prepare_run(args)
            return 0
        if args.command == "evaluate-run":
            result = evaluate_run(args.run.resolve(), args.output)
            print(json.dumps(result, indent=2, sort_keys=True))
            return 0 if result["task_complete"] else 1
        raise AssertionError(f"unhandled command: {args.command}")
    except EvaluationError as error:
        print(f"error: {error}", file=sys.stderr)
        return 2


if __name__ == "__main__":
    raise SystemExit(main())
