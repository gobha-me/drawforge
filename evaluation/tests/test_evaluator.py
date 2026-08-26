#!/usr/bin/env python3

from __future__ import annotations

import importlib.util
import json
import shutil
import tempfile
import unittest
from pathlib import Path


SCRIPT = Path(__file__).resolve().parents[1] / "tools" / "evaluate.py"
SPEC = importlib.util.spec_from_file_location("drawforge_evaluate", SCRIPT)
assert SPEC and SPEC.loader
evaluate = importlib.util.module_from_spec(SPEC)
SPEC.loader.exec_module(evaluate)


class EvaluatorTests(unittest.TestCase):
    def setUp(self) -> None:
        self.corpus = evaluate.load_corpus()
        self.tasks = evaluate.task_index(self.corpus)
        self.temporary = tempfile.TemporaryDirectory()
        self.root = Path(self.temporary.name)

    def tearDown(self) -> None:
        self.temporary.cleanup()

    def write_svg(self, name: str, content: str) -> Path:
        path = self.root / name
        path.write_text(content, encoding="utf-8")
        return path

    def test_corpus_references_pass(self) -> None:
        self.assertEqual([], evaluate.validate_corpus())

    def test_malformed_svg_is_rejected(self) -> None:
        path = self.write_svg("bad.svg", '<svg xmlns="http://www.w3.org/2000/svg">')
        with self.assertRaisesRegex(evaluate.EvaluationError, "malformed SVG"):
            evaluate.parse_svg(path, self.corpus["limits"])

    def test_duplicate_ids_are_rejected(self) -> None:
        path = self.write_svg(
            "duplicate.svg",
            '<svg xmlns="http://www.w3.org/2000/svg"><circle id="same"/><rect id="same"/></svg>',
        )
        with self.assertRaisesRegex(evaluate.EvaluationError, "duplicate SVG id"):
            evaluate.parse_svg(path, self.corpus["limits"])

    def test_external_reference_is_rejected(self) -> None:
        path = self.write_svg(
            "external.svg",
            '<svg xmlns="http://www.w3.org/2000/svg"><image href="https://example.test/a.png"/></svg>',
        )
        with self.assertRaisesRegex(evaluate.EvaluationError, "external"):
            evaluate.parse_svg(path, self.corpus["limits"])

    def test_entity_declarations_are_rejected(self) -> None:
        path = self.write_svg(
            "entity.svg",
            '<!DOCTYPE svg [<!ENTITY x "x">]><svg xmlns="http://www.w3.org/2000/svg"/>',
        )
        with self.assertRaisesRegex(evaluate.EvaluationError, "entities"):
            evaluate.parse_svg(path, self.corpus["limits"])

    def test_protected_change_fails_semantic_score(self) -> None:
        source = evaluate._safe_corpus_path("references/revise-named-sun.svg")
        content = source.read_text(encoding="utf-8").replace("#8ed6ff", "#000000")
        path = self.write_svg("changed.svg", content)
        failures = evaluate.score_candidate(self.corpus, self.tasks["revise-named-sun"], path)
        self.assertIn("protected element #sky changed", failures)

    def test_element_ceiling_is_enforced(self) -> None:
        limits = dict(self.corpus["limits"])
        limits["max_elements"] = 2
        path = self.write_svg(
            "large.svg",
            '<svg xmlns="http://www.w3.org/2000/svg"><g/><g/></svg>',
        )
        with self.assertRaisesRegex(evaluate.EvaluationError, "exceeds 2 elements"):
            evaluate.parse_svg(path, limits)

    def test_svg_byte_ceiling_is_enforced(self) -> None:
        limits = dict(self.corpus["limits"])
        limits["max_svg_bytes"] = 32
        path = self.write_svg("too-large.svg", '<svg xmlns="http://www.w3.org/2000/svg"/>')
        with self.assertRaisesRegex(evaluate.EvaluationError, "exceeds 32 bytes"):
            evaluate.parse_svg(path, limits)

    def test_nonfinite_json_number_is_rejected(self) -> None:
        path = self.root / "nonfinite.json"
        path.write_text('{"value": NaN}', encoding="utf-8")
        with self.assertRaisesRegex(evaluate.EvaluationError, "non-finite JSON number"):
            evaluate.load_json(path)

    def test_duplicate_json_key_is_rejected(self) -> None:
        path = self.root / "duplicate.json"
        path.write_text('{"value": 1, "value": 2}', encoding="utf-8")
        with self.assertRaisesRegex(evaluate.EvaluationError, "duplicate JSON key"):
            evaluate.load_json(path)

    def test_complete_run_is_scored_and_hashed(self) -> None:
        run = self.root / "run"
        (run / "attempts").mkdir(parents=True)
        reference = evaluate._safe_corpus_path("references/create-status-badge.svg")
        shutil.copyfile(evaluate._safe_corpus_path("prompts/create-status-badge.md"), run / "prompt.md")
        shutil.copyfile(reference, run / "attempts" / "001.svg")
        metadata = {
            "schema_version": 1,
            "corpus_id": self.corpus["corpus_id"],
            "task_id": "create-status-badge",
            "route": "direct-svg",
            "trial": 1,
            "model": {"provider": "test", "id": "fake", "version": "1"},
            "sampling": {"seed": 1, "temperature": 0},
            "usage": {"tool_interactions": 1, "input_tokens": 10, "output_tokens": 20},
            "events": ["submission_accepted"],
        }
        (run / "run.json").write_text(json.dumps(metadata), encoding="utf-8")
        result = evaluate.evaluate_run(run)
        self.assertTrue(result["task_complete"])
        self.assertEqual(1, result["attempt_count"])
        self.assertEqual(0, result["unintended_change_count"])
        self.assertRegex(result["hashes"]["attempts"][0]["sha256"], r"^sha256:[0-9a-f]{64}$")

    def test_recovery_requires_ordered_events_and_two_attempts(self) -> None:
        run = self.root / "recovery"
        (run / "attempts").mkdir(parents=True)
        reference = evaluate._safe_corpus_path("references/recover-invalid-edit.svg")
        shutil.copyfile(evaluate._safe_corpus_path("prompts/recover-invalid-edit.md"), run / "prompt.md")
        shutil.copyfile(evaluate._safe_corpus_path("fixtures/recover-invalid-edit.svg"), run / "source.svg")
        shutil.copyfile(reference, run / "attempts" / "001.svg")
        metadata = {
            "schema_version": 1,
            "corpus_id": self.corpus["corpus_id"],
            "task_id": "recover-invalid-edit",
            "route": "direct-svg",
            "trial": 1,
            "model": {"provider": "test", "id": "fake", "version": "1"},
            "sampling": {"seed": None, "temperature": None},
            "usage": {"tool_interactions": 1, "input_tokens": None, "output_tokens": None},
            "events": ["submission_accepted"],
        }
        (run / "run.json").write_text(json.dumps(metadata), encoding="utf-8")
        result = evaluate.evaluate_run(run)
        self.assertFalse(result["task_complete"])
        self.assertTrue(any("event order" in item for item in result["diagnostics"]))
        self.assertTrue(any("two attempts" in item for item in result["diagnostics"]))

    def test_malformed_final_result_is_not_valid(self) -> None:
        run = self.root / "malformed-run"
        (run / "attempts").mkdir(parents=True)
        shutil.copyfile(evaluate._safe_corpus_path("prompts/create-status-badge.md"), run / "prompt.md")
        (run / "attempts" / "001.svg").write_text(
            '<svg xmlns="http://www.w3.org/2000/svg">', encoding="utf-8"
        )
        metadata = {
            "schema_version": 1,
            "corpus_id": self.corpus["corpus_id"],
            "task_id": "create-status-badge",
            "route": "direct-svg",
            "trial": 1,
            "model": {"provider": "test", "id": "fake", "version": "1"},
            "sampling": {"seed": None, "temperature": None},
            "usage": {"tool_interactions": 1, "input_tokens": None, "output_tokens": None},
            "events": ["submission_accepted"],
        }
        (run / "run.json").write_text(json.dumps(metadata), encoding="utf-8")
        result = evaluate.evaluate_run(run)
        self.assertFalse(result["valid_result"])
        self.assertFalse(result["task_complete"])

    def test_modified_prompt_is_rejected(self) -> None:
        run = self.root / "modified-prompt"
        (run / "attempts").mkdir(parents=True)
        (run / "prompt.md").write_text("different", encoding="utf-8")
        shutil.copyfile(
            evaluate._safe_corpus_path("references/create-status-badge.svg"),
            run / "attempts" / "001.svg",
        )
        metadata = {
            "schema_version": 1,
            "corpus_id": self.corpus["corpus_id"],
            "task_id": "create-status-badge",
            "route": "direct-svg",
            "trial": 1,
            "model": {"provider": "test", "id": "fake", "version": "1"},
            "sampling": {"seed": None, "temperature": None},
            "usage": {"tool_interactions": 1, "input_tokens": None, "output_tokens": None},
            "events": ["submission_accepted"],
        }
        (run / "run.json").write_text(json.dumps(metadata), encoding="utf-8")
        with self.assertRaisesRegex(evaluate.EvaluationError, "frozen corpus prompt"):
            evaluate.evaluate_run(run)

    def test_reference_runs_complete_every_task(self) -> None:
        for task_id, task in self.tasks.items():
            with self.subTest(task=task_id):
                run = self.root / f"all-{task_id}"
                (run / "attempts").mkdir(parents=True)
                shutil.copyfile(evaluate._safe_corpus_path(task["prompt"]), run / "prompt.md")
                events = list(task.get("required_events", []))
                if "submission_accepted" not in events:
                    events.append("submission_accepted")
                if task.get("input"):
                    source_key = "concurrent_input" if "source_refreshed" in events else "input"
                    shutil.copyfile(evaluate._safe_corpus_path(task[source_key]), run / "source.svg")
                if task_id == "recover-invalid-edit":
                    shutil.copyfile(evaluate._safe_corpus_path(task["input"]), run / "attempts" / "001.svg")
                    attempt_name = "002.svg"
                else:
                    attempt_name = "001.svg"
                shutil.copyfile(evaluate._safe_corpus_path(task["reference"]), run / "attempts" / attempt_name)
                metadata = {
                    "schema_version": 1,
                    "corpus_id": self.corpus["corpus_id"],
                    "task_id": task_id,
                    "route": "direct-svg",
                    "trial": 1,
                    "model": {"provider": "test", "id": "fake", "version": "1"},
                    "sampling": {"seed": 1, "temperature": 0},
                    "usage": {
                        "tool_interactions": len(events),
                        "input_tokens": None,
                        "output_tokens": None,
                    },
                    "events": events,
                }
                (run / "run.json").write_text(json.dumps(metadata), encoding="utf-8")
                result = evaluate.evaluate_run(run)
                self.assertTrue(result["task_complete"], result["diagnostics"])


if __name__ == "__main__":
    unittest.main()
