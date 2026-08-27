#!/usr/bin/env python3
"""Run the ADR-0004 deterministic transaction failure matrix."""

from __future__ import annotations

import hashlib
import json
import unittest
from dataclasses import asdict

from model import (
    ApplyMode,
    Cancellation,
    CreateObject,
    Disposition,
    Engine,
    ErrorCode,
    Fault,
    HistoryAction,
    MAX_REVISION,
    ObjectRecord,
    OperationBatch,
    Rect,
    RejectOperation,
    RetryAdvice,
    SetObject,
    Transaction,
    WarningCode,
)


def record(value: int, x: int = 0) -> ObjectRecord:
    return ObjectRecord(value=value, bounds=Rect(x=x, y=0, width=10, height=10))


def batch(tx_id: str, revision: int, *operations, document_id: str = "scene") -> Transaction:
    return Transaction(document_id, revision, tx_id, OperationBatch(tuple(operations)))


def history(tx_id: str, revision: int, action: HistoryAction) -> Transaction:
    return Transaction("scene", revision, tx_id, action)


def assert_error(test: unittest.TestCase, result, code: ErrorCode) -> None:
    test.assertFalse(result.succeeded)
    test.assertIsNotNone(result.error)
    test.assertEqual(result.error.code, code)


class TransactionContractTests(unittest.TestCase):
    def test_wrong_document_precedes_registry_and_mutation(self) -> None:
        engine = Engine()
        before = engine.snapshot()
        result = engine.apply(batch("wrong", 0, CreateObject("a", record(1)), document_id="other"))
        assert_error(self, result, ErrorCode.WRONG_DOCUMENT)
        self.assertEqual(engine.snapshot(), before)

    def test_failure_matrix_rolls_back_every_aggregate(self) -> None:
        cases = [
            (batch("empty", 0), ErrorCode.EMPTY_TRANSACTION),
            (
                batch("duplicate", 0, CreateObject("a", record(1)), CreateObject("a", record(2))),
                ErrorCode.DUPLICATE_IDENTITY,
            ),
            (batch("missing", 0, SetObject("missing", record(1))), ErrorCode.MISSING_IDENTITY),
            (
                batch("partial", 0, CreateObject("a", record(1)), RejectOperation()),
                ErrorCode.INVALID_CONTENT,
            ),
        ]
        for transaction, code in cases:
            with self.subTest(code=code):
                engine = Engine()
                before = engine.snapshot()
                result = engine.apply(transaction)
                assert_error(self, result, code)
                self.assertEqual(engine.snapshot(), before)

        engine = Engine(max_operations=1)
        before = engine.snapshot()
        result = engine.apply(
            batch(
                "large",
                0,
                CreateObject("a", record(1)),
                CreateObject("b", record(2)),
            )
        )
        assert_error(self, result, ErrorCode.RESOURCE_LIMIT)
        self.assertEqual(engine.snapshot(), before)

    def test_forward_reference_reports_operation_and_typed_path(self) -> None:
        engine = Engine()
        before = engine.snapshot()
        result = engine.apply(
            batch(
                "forward",
                0,
                SetObject("later", record(2)),
                CreateObject("later", record(1)),
            )
        )
        assert_error(self, result, ErrorCode.MISSING_IDENTITY)
        self.assertEqual(result.error.operation_index, 0)
        self.assertEqual(
            [type(segment).__name__ for segment in result.error.field_path],
            ["FieldKey", "FieldKey", "FieldIndex", "FieldKey"],
        )
        self.assertEqual(engine.snapshot(), before)

    def test_operations_observe_staged_state_and_receipt_order(self) -> None:
        engine = Engine()
        result = engine.apply(
            batch(
                "create-update",
                0,
                CreateObject("a", record(1, 0)),
                SetObject("a", record(2, 20)),
                CreateObject("b", record(3, 5)),
            )
        )
        self.assertTrue(result.succeeded)
        self.assertEqual(result.disposition, Disposition.COMMITTED)
        self.assertEqual(result.receipt.created, ("a", "b"))
        self.assertEqual(result.receipt.changed, ())
        self.assertEqual(result.receipt.dirty_bounds, Rect(5, 0, 25, 10))
        self.assertEqual(engine.revision, 1)
        self.assertEqual(engine.objects["a"], record(2, 20))

    def test_dry_run_does_not_reserve_then_commit_and_replay_are_exact(self) -> None:
        engine = Engine()
        transaction = batch("once", 0, CreateObject("a", record(1)))
        before = engine.snapshot()
        dry_run = engine.apply(transaction, ApplyMode.DRY_RUN)
        self.assertEqual(dry_run.disposition, Disposition.DRY_RUN)
        self.assertEqual(engine.snapshot(), before)

        committed = engine.apply(transaction)
        self.assertEqual(committed.disposition, Disposition.COMMITTED)
        self.assertEqual(committed.receipt, dry_run.receipt)
        committed_state = engine.snapshot()

        engine.apply(batch("later", 1, CreateObject("b", record(2))))
        later_state = engine.snapshot()
        replayed = engine.apply(transaction, cancellation=Cancellation(cancel_on_poll=1))
        self.assertEqual(replayed.disposition, Disposition.REPLAYED)
        self.assertEqual(replayed.receipt, committed.receipt)
        self.assertEqual(engine.snapshot(), later_state)
        self.assertNotEqual(committed_state, later_state)

        conflict = Transaction("scene", 2, "once", transaction.body)
        before_conflict = engine.snapshot()
        result = engine.apply(conflict)
        assert_error(self, result, ErrorCode.TRANSACTION_ID_CONFLICT)
        self.assertEqual(engine.snapshot(), before_conflict)

    def test_stale_revision_precedes_operation_validation(self) -> None:
        engine = Engine(revision=4)
        result = engine.apply(batch("stale", 3, RejectOperation()))
        assert_error(self, result, ErrorCode.STALE_REVISION)
        self.assertEqual(result.error.retry_advice, RetryAdvice.REFRESH_THEN_RETRY)
        self.assertIsNone(result.error.operation_index)

    def test_same_value_is_an_accepted_revision_with_warning(self) -> None:
        engine = Engine(objects={"a": record(1)})
        result = engine.apply(batch("no-effect", 0, SetObject("a", record(1))))
        self.assertTrue(result.succeeded)
        self.assertEqual(result.receipt.changed, ())
        self.assertIsNone(result.receipt.dirty_bounds)
        self.assertEqual(result.receipt.warnings[0].code, WarningCode.NO_EFFECT)
        self.assertEqual(engine.revision, 1)
        self.assertEqual(len(engine.undo), 1)

    def test_cancellation_at_every_boundary_is_atomic(self) -> None:
        transaction = batch(
            "cancel",
            0,
            CreateObject("a", record(1)),
            CreateObject("b", record(2)),
        )
        for poll in range(1, 5):
            with self.subTest(poll=poll):
                engine = Engine()
                before = engine.snapshot()
                result = engine.apply(transaction, cancellation=Cancellation(cancel_on_poll=poll))
                assert_error(self, result, ErrorCode.CANCELLED)
                self.assertEqual(engine.snapshot(), before)

        engine = Engine()
        committed = engine.apply(transaction, cancellation=Cancellation(cancel_on_poll=5))
        self.assertTrue(committed.succeeded)
        self.assertEqual(engine.revision, 1)

    def test_allocation_revision_and_retention_exhaustion_are_atomic(self) -> None:
        transaction = batch("candidate", 0, CreateObject("a", record(1)))
        for fault in (Fault.STAGE_ALLOCATION, Fault.RECEIPT_ALLOCATION):
            with self.subTest(fault=fault):
                engine = Engine()
                before = engine.snapshot()
                result = engine.apply(transaction, fault=fault)
                assert_error(self, result, ErrorCode.ALLOCATION_FAILURE)
                self.assertEqual(engine.snapshot(), before)

        overflow = Engine(revision=MAX_REVISION)
        before = overflow.snapshot()
        result = overflow.apply(batch("overflow", MAX_REVISION, CreateObject("a", record(1))))
        assert_error(self, result, ErrorCode.REVISION_OVERFLOW)
        self.assertEqual(overflow.snapshot(), before)

        replay_full = Engine(max_replay_records=0)
        before = replay_full.snapshot()
        result = replay_full.apply(transaction)
        assert_error(self, result, ErrorCode.RESOURCE_LIMIT)
        self.assertEqual(replay_full.snapshot(), before)

        history_full = Engine(max_history_weight=0)
        before = history_full.snapshot()
        result = history_full.apply(transaction)
        assert_error(self, result, ErrorCode.RESOURCE_LIMIT)
        self.assertEqual(history_full.snapshot(), before)

    def test_undo_redo_are_atomic_replay_safe_transactions(self) -> None:
        engine = Engine()
        create = engine.apply(batch("create", 0, CreateObject("a", record(1))))
        update = engine.apply(batch("update", 1, SetObject("a", record(2, 20))))
        self.assertTrue(create.succeeded and update.succeeded)

        undo_transaction = history("undo-update", 2, HistoryAction.UNDO)
        undo = engine.apply(undo_transaction)
        self.assertEqual(engine.objects["a"], record(1))
        self.assertEqual(engine.revision, 3)
        self.assertEqual(len(engine.undo), 1)
        self.assertEqual(len(engine.redo), 1)

        undo_state = engine.snapshot()
        replay = engine.apply(undo_transaction)
        self.assertEqual(replay.disposition, Disposition.REPLAYED)
        self.assertEqual(replay.receipt, undo.receipt)
        self.assertEqual(engine.snapshot(), undo_state)

        redo = engine.apply(history("redo-update", 3, HistoryAction.REDO))
        self.assertTrue(redo.succeeded)
        self.assertEqual(engine.objects["a"], record(2, 20))
        self.assertEqual(engine.revision, 4)
        self.assertEqual(len(engine.undo), 2)
        self.assertEqual(len(engine.redo), 0)

    def test_history_dry_run_projects_without_moving_stacks(self) -> None:
        engine = Engine()
        engine.apply(batch("create", 0, CreateObject("a", record(1))))
        transaction = history("undo", 1, HistoryAction.UNDO)
        before = engine.snapshot()
        projected = engine.apply(transaction, ApplyMode.DRY_RUN)
        self.assertEqual(projected.disposition, Disposition.DRY_RUN)
        self.assertEqual(projected.receipt.changed, ("a",))
        self.assertEqual(engine.snapshot(), before)

        committed = engine.apply(transaction)
        self.assertEqual(committed.receipt, projected.receipt)
        self.assertEqual(engine.objects, {})

    def test_new_commit_after_undo_clears_redo_only_on_success(self) -> None:
        engine = Engine()
        engine.apply(batch("create", 0, CreateObject("a", record(1))))
        engine.apply(batch("update", 1, SetObject("a", record(2))))
        engine.apply(history("undo", 2, HistoryAction.UNDO))
        self.assertEqual(len(engine.redo), 1)

        failed = engine.apply(batch("bad", 3, RejectOperation()))
        assert_error(self, failed, ErrorCode.INVALID_CONTENT)
        self.assertEqual(len(engine.redo), 1)

        committed = engine.apply(batch("branch", 3, CreateObject("b", record(3))))
        self.assertTrue(committed.succeeded)
        self.assertEqual(len(engine.redo), 0)
        no_redo = engine.apply(history("redo", 4, HistoryAction.REDO))
        assert_error(self, no_redo, ErrorCode.NOTHING_TO_REDO)

    def test_history_evicts_oldest_but_replay_records_remain(self) -> None:
        engine = Engine(max_undo_steps=2, max_history_weight=100)
        first = batch("one", 0, CreateObject("a", record(1)))
        engine.apply(first)
        engine.apply(batch("two", 1, CreateObject("b", record(2))))
        engine.apply(batch("three", 2, CreateObject("c", record(3))))
        self.assertEqual(len(engine.undo), 2)
        self.assertEqual([entry.source_transaction_id for entry in engine.undo], ["two", "three"])
        self.assertEqual(set(engine.replay), {"one", "two", "three"})
        state = engine.snapshot()
        replay = engine.apply(first)
        self.assertEqual(replay.disposition, Disposition.REPLAYED)
        self.assertEqual(engine.snapshot(), state)

    def test_full_replay_registry_rejects_new_commit_but_serves_replay(self) -> None:
        engine = Engine(max_replay_records=1)
        first = batch("one", 0, CreateObject("a", record(1)))
        committed = engine.apply(first)
        full_state = engine.snapshot()

        rejected = engine.apply(batch("two", 1, CreateObject("b", record(2))))
        assert_error(self, rejected, ErrorCode.RESOURCE_LIMIT)
        self.assertEqual(engine.snapshot(), full_state)

        replay = engine.apply(first)
        self.assertEqual(replay.disposition, Disposition.REPLAYED)
        self.assertEqual(replay.receipt, committed.receipt)
        self.assertEqual(engine.snapshot(), full_state)


def deterministic_summary() -> dict:
    engine = Engine()
    results = [
        engine.apply(batch("create", 0, CreateObject("badge", record(1, 4)))),
        engine.apply(batch("revise", 1, SetObject("badge", record(2, 18)))),
        engine.apply(history("undo", 2, HistoryAction.UNDO)),
        engine.apply(history("redo", 3, HistoryAction.REDO)),
    ]
    return {
        "revision": engine.revision,
        "objects": {key: asdict(value) for key, value in sorted(engine.objects.items())},
        "receipts": [asdict(result.receipt) for result in results],
        "replay_ids": sorted(engine.replay),
        "undo_sources": [entry.source_transaction_id for entry in engine.undo],
        "redo_sources": [entry.source_transaction_id for entry in engine.redo],
    }


def summary_bytes() -> bytes:
    return json.dumps(
        deterministic_summary(), sort_keys=True, separators=(",", ":")
    ).encode("utf-8")


EXPECTED_DIGEST = "bc55680a1259bd43a415717f39480f752f68572967c92c50baa04c5bca731067"


if __name__ == "__main__":
    suite = unittest.defaultTestLoader.loadTestsFromTestCase(TransactionContractTests)
    result = unittest.TextTestRunner(verbosity=2).run(suite)
    first = summary_bytes()
    second = summary_bytes()
    digest = hashlib.sha256(first).hexdigest()
    if first != second:
        raise SystemExit("deterministic summary changed between identical runs")
    if digest != EXPECTED_DIGEST:
        raise SystemExit(f"summary digest mismatch: expected {EXPECTED_DIGEST}, got {digest}")
    print(f"transaction contract summary sha256={digest}")
    raise SystemExit(0 if result.wasSuccessful() else 1)
