#!/usr/bin/env python3

from __future__ import annotations

import argparse
import importlib.util
import json
import os
import tempfile
import unittest
from pathlib import Path


SCRIPT = Path(__file__).resolve().parents[1] / "tools" / "evaluate_v2.py"
SPEC = importlib.util.spec_from_file_location("drawforge_evaluate_v2", SCRIPT)
assert SPEC and SPEC.loader
evaluate = importlib.util.module_from_spec(SPEC)
SPEC.loader.exec_module(evaluate)


class EvaluatorV2Tests(unittest.TestCase):
    def setUp(self) -> None:
        self.corpus = evaluate.load_corpus()
        self.tasks = evaluate.task_index(self.corpus)
        self.temporary = tempfile.TemporaryDirectory()
        self.root = Path(self.temporary.name)
        configured = os.environ.get("DRAWFORGE_BINARY")
        self.binary = Path(configured).resolve() if configured else None
        self.renderer = self.root / "review-renderer.py"
        self.renderer.write_text(
            "#!/usr/bin/env python3\n"
            "import pathlib, sys\n"
            f"pathlib.Path(sys.argv[-1]).write_bytes(bytes.fromhex('{evaluate.FAILURE_PNG.hex()}'))\n",
            encoding="utf-8",
        )
        self.renderer.chmod(0o700)

    def tearDown(self) -> None:
        self.temporary.cleanup()

    def prepare(self, task: str, route: str) -> Path:
        run = self.root / f"{task}-{route}"
        evaluate.prepare_run(argparse.Namespace(
            task=task,
            route=route,
            output=run,
            provider="test",
            model="fake",
            model_version="immutable-test",
            drawforge_version="0.11.0",
            drawforge=self.binary or Path("/bin/true"),
            adapter_version="test-v1",
            adapter_commit="0123456789abcdef",
            provider_runtime="offline-test",
            direct_svg_renderer=self.renderer,
            direct_svg_renderer_version="offline-test-renderer-v1",
            trial=1,
            seed=1001,
            temperature=0.0,
        ))
        return run

    def accept(self, run: Path, interactions: int = 1) -> None:
        metadata = json.loads((run / "run.json").read_text(encoding="utf-8"))
        metadata["usage"]["tool_interactions"] = interactions
        metadata["events"] = ["submission_accepted"]
        (run / "run.json").write_text(json.dumps(metadata), encoding="utf-8")

    def test_corpus_references_pass_route_neutral_checks(self) -> None:
        self.assertEqual([], evaluate.validate_corpus(None))

    def test_route_neutral_prompts_do_not_name_svg(self) -> None:
        for task in self.tasks.values():
            prompt = evaluate._safe_v2_path(task["prompt"]).read_text(encoding="utf-8")
            self.assertNotIn("SVG", prompt)

    def test_unsupported_path_command_fails_closed(self) -> None:
        with self.assertRaisesRegex(evaluate.EvaluationError, "unsupported corpus path"):
            evaluate._path_commands("M0 0C1 1 2 2 3 3")

    def test_transcript_duplicate_key_fails_closed(self) -> None:
        path = self.root / "duplicate.jsonl"
        path.write_text(
            '{"protocol":"drawforge.experimental/v1","request":{},"request":{}}\n',
            encoding="utf-8",
        )
        with self.assertRaisesRegex(evaluate.EvaluationError, "duplicate JSON key"):
            evaluate._load_frames(path, 1024)

    def test_transcript_byte_ceiling_is_enforced(self) -> None:
        path = self.root / "large.jsonl"
        path.write_text("{}" * 20, encoding="utf-8")
        with self.assertRaisesRegex(evaluate.EvaluationError, "exceeds 8 bytes"):
            evaluate._load_frames(path, 8)

    def test_review_key_must_remain_outside_packet(self) -> None:
        result_root = self.root / "results"
        run = result_root / "run"
        (run / "review").mkdir(parents=True)
        (run / "review" / "final.png").write_bytes(b"png")
        (run / "result.json").write_text(json.dumps({
            "schema_version": 2,
            "corpus_id": self.corpus["corpus_id"],
            "task_id": "create-status-badge",
            "route": "direct-svg",
            "trial": 1,
            "pair_id": "create-status-badge-01-1001",
        }), encoding="utf-8")
        key = self.root / "blind-key"
        key.write_text("test-key", encoding="utf-8")
        packet = self.root / "packet"
        with self.assertRaisesRegex(evaluate.EvaluationError, "outside the review packet"):
            evaluate.prepare_review(argparse.Namespace(
                root=result_root,
                output=packet,
                key_output=packet / "route-key.json",
                blind_key_file=key,
                allow_incomplete=True,
            ))

    def test_incomplete_review_packet_is_blinded(self) -> None:
        result_root = self.root / "results"
        run = result_root / "run"
        (run / "review").mkdir(parents=True)
        (run / "review" / "final.png").write_bytes(b"png")
        (run / "result.json").write_text(json.dumps({
            "schema_version": 2,
            "corpus_id": self.corpus["corpus_id"],
            "task_id": "create-status-badge",
            "route": "semantic",
            "trial": 1,
            "pair_id": "create-status-badge-01-1001",
        }), encoding="utf-8")
        key = self.root / "blind-key"
        key.write_text("test-key", encoding="utf-8")
        packet = self.root / "packet"
        sealed = self.root / "sealed.json"
        manifest = evaluate.prepare_review(argparse.Namespace(
            root=result_root,
            output=packet,
            key_output=sealed,
            blind_key_file=key,
            allow_incomplete=True,
        ))
        public = json.dumps(manifest)
        self.assertNotIn('"route"', public)
        self.assertNotIn("create-status-badge-01-1001", public)
        self.assertEqual("semantic", json.loads(sealed.read_text(encoding="utf-8"))["items"][0]["route"])

    def test_aggregate_rejects_mismatched_pair_controls(self) -> None:
        results = self.root / "results"
        common = {
            "schema_version": 2,
            "corpus_id": self.corpus["corpus_id"],
            "task_id": "create-status-badge",
            "family": "creation",
            "trial": 1,
            "pair_id": "create-status-badge-01-1001",
            "model": {"provider": "test", "id": "fake", "version": "one"},
            "sampling": {"seed": 1001, "temperature": 0},
            "runtime": {"drawforge_version": "0.12.0"},
            "frozen": evaluate._frozen_hashes(),
            "valid_result": True,
            "task_complete": True,
            "unintended_change_count": 0,
            "recovered": False,
            "usage": {"tool_interactions": 1, "input_tokens": 1, "output_tokens": 1, "cost_usd": 0.01},
        }
        for route in self.corpus["routes"]:
            directory = results / route
            directory.mkdir(parents=True)
            result = dict(common, route=route)
            result["model"] = dict(common["model"])
            if route == "semantic":
                result["model"]["version"] = "two"
            (directory / "result.json").write_text(json.dumps(result), encoding="utf-8")
        with self.assertRaisesRegex(evaluate.EvaluationError, "paired runs disagree on model"):
            evaluate.aggregate_results(results, None)

    def test_semantic_source_is_frozen_independently_of_svg(self) -> None:
        run = self.prepare("revise-named-sun", "semantic")
        frames = evaluate._load_frames(run / "source.jsonl", self.corpus["limits"]["max_transcript_bytes"])
        self.assertEqual("create_document", frames[0]["request"]["kind"])
        self.assertEqual("apply", frames[1]["request"]["kind"])
        self.assertFalse((run / "source.svg").exists())

    def test_direct_reference_run_completes(self) -> None:
        run = self.prepare("create-status-badge", "direct-svg")
        reference = evaluate.v1._safe_corpus_path("references/create-status-badge.svg")
        (run / "attempts" / "001.svg").write_bytes(reference.read_bytes())
        self.accept(run)
        result = evaluate.evaluate_run(run, None, self.renderer)
        self.assertTrue(result["task_complete"], result["diagnostics"])
        self.assertTrue((run / "review" / "final.png").is_file())
        self.assertIn("review", result["hashes"])

    @unittest.skipUnless(os.environ.get("DRAWFORGE_BINARY"), "DRAWFORGE_BINARY is not set")
    def test_semantic_reference_run_replays_and_completes(self) -> None:
        run = self.prepare("create-status-badge", "semantic")
        inherited = evaluate._v1_task(self.tasks["create-status-badge"])
        frames = evaluate.svg_to_semantic_frames(
            evaluate.v1._safe_corpus_path(inherited["reference"]), "create-status-badge"
        )
        evaluate._write_frames(run / "attempts" / "001.jsonl", frames)
        self.accept(run, interactions=2)
        result = evaluate.evaluate_run(run, self.binary, self.renderer)
        self.assertTrue(result["valid_result"], result["diagnostics"])
        self.assertTrue(result["task_complete"], result["diagnostics"])
        self.assertEqual(1, len(result["hashes"]["renders"]))
        self.assertTrue((run / "review" / "final.png").is_file())

    @unittest.skipUnless(os.environ.get("DRAWFORGE_BINARY"), "DRAWFORGE_BINARY is not set")
    def test_semantic_revision_preserves_source_and_completes(self) -> None:
        run = self.prepare("recolor-card-theme", "semantic")
        operation = lambda object_id, fill: {
            "op": "set_style", "object_id": object_id,
            "style": {"fill": fill, "stroke": None},
        }
        frame = {
            "protocol": evaluate.PROTOCOL,
            "request": {
                "kind": "apply", "mode": "commit",
                "transaction": {
                    "document_id": "recolor-card-theme", "expected_revision": 1,
                    "transaction_id": "recolor-card-theme-test-v1",
                    "body": {"kind": "operations", "operations": [
                        operation("background", "#111827ff"),
                        operation("panel", "#1f2937ff"),
                        operation("accent", "#a78bfaff"),
                    ]},
                },
            },
        }
        evaluate._write_frames(run / "attempts" / "001.jsonl", [frame])
        self.accept(run)
        result = evaluate.evaluate_run(run, self.binary, self.renderer)
        self.assertTrue(result["task_complete"], result["diagnostics"])

    @unittest.skipUnless(os.environ.get("DRAWFORGE_BINARY"), "DRAWFORGE_BINARY is not set")
    def test_semantic_unintended_change_is_detected(self) -> None:
        run = self.prepare("recolor-card-theme", "semantic")
        frame = {
            "protocol": evaluate.PROTOCOL,
            "request": {
                "kind": "apply", "mode": "commit",
                "transaction": {
                    "document_id": "recolor-card-theme", "expected_revision": 1,
                    "transaction_id": "wrong-theme-test-v1",
                    "body": {"kind": "operations", "operations": [{
                        "op": "set_style", "object_id": "background",
                        "style": {"fill": "#000000ff", "stroke": None},
                    }]},
                },
            },
        }
        evaluate._write_frames(run / "attempts" / "001.jsonl", [frame])
        self.accept(run)
        result = evaluate.evaluate_run(run, self.binary, self.renderer)
        self.assertFalse(result["task_complete"])
        self.assertGreater(result["unintended_change_count"], 0)

    def test_invalid_direct_run_gets_route_neutral_failure_preview(self) -> None:
        run = self.prepare("create-status-badge", "direct-svg")
        (run / "attempts" / "001.svg").write_text("not-svg", encoding="utf-8")
        self.accept(run)
        result = evaluate.evaluate_run(run, None, self.renderer)
        self.assertFalse(result["valid_result"])
        self.assertEqual(evaluate.FAILURE_PNG, (run / "review" / "final.png").read_bytes())

    def test_oversized_direct_review_fails_before_renderer(self) -> None:
        run = self.prepare("create-status-badge", "direct-svg")
        (run / "attempts" / "001.svg").write_text(
            '<svg xmlns="http://www.w3.org/2000/svg" width="5000" height="64" '
            'viewBox="0 0 5000 64"><rect id="badge"/><circle id="indicator"/>'
            '<path id="check"/></svg>',
            encoding="utf-8",
        )
        self.accept(run)
        result = evaluate.evaluate_run(run, None, self.renderer)
        self.assertFalse(result["valid_result"])
        self.assertIn("exceeds 4096 pixels", result["diagnostics"][-1])
        self.assertEqual(evaluate.FAILURE_PNG, (run / "review" / "final.png").read_bytes())

    def test_zero_submission_run_remains_aggregatable(self) -> None:
        run = self.prepare("create-status-badge", "direct-svg")
        result = evaluate.evaluate_run(run, None, self.renderer)
        self.assertFalse(result["valid_result"])
        self.assertFalse(result["task_complete"])
        self.assertEqual(0, result["attempt_count"])
        self.assertIn("run has no direct-svg attempts", result["diagnostics"])
        self.assertEqual(evaluate.FAILURE_PNG, (run / "review" / "final.png").read_bytes())


if __name__ == "__main__":
    unittest.main()
