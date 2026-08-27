"""Executable reference model for ADR-0004.

The model is intentionally not a DrawForge API or interchange encoding. It
uses small immutable Python values to make the transaction state machine and
its failure behavior executable before the production C++ scene exists.
"""

from __future__ import annotations

from dataclasses import dataclass, field
from enum import Enum
from typing import TypeAlias


MAX_REVISION = (1 << 64) - 1


class ApplyMode(str, Enum):
    DRY_RUN = "dry_run"
    COMMIT = "commit"


class Disposition(str, Enum):
    DRY_RUN = "dry_run"
    COMMITTED = "committed"
    REPLAYED = "replayed"


class RetryAdvice(str, Enum):
    SAME_REQUEST = "same_request"
    REFRESH_THEN_RETRY = "refresh_then_retry"
    CHANGE_REQUEST = "change_request"
    NOT_RETRYABLE = "not_retryable"


class ErrorCode(str, Enum):
    WRONG_DOCUMENT = "wrong_document"
    TRANSACTION_ID_CONFLICT = "transaction_id_conflict"
    STALE_REVISION = "stale_revision"
    EMPTY_TRANSACTION = "empty_transaction"
    DUPLICATE_IDENTITY = "duplicate_identity"
    MISSING_IDENTITY = "missing_identity"
    INVALID_CONTENT = "invalid_content"
    NOTHING_TO_UNDO = "nothing_to_undo"
    NOTHING_TO_REDO = "nothing_to_redo"
    CANCELLED = "cancelled"
    RESOURCE_LIMIT = "resource_limit"
    REVISION_OVERFLOW = "revision_overflow"
    ALLOCATION_FAILURE = "allocation_failure"


class WarningCode(str, Enum):
    NO_EFFECT = "no_effect"


class HistoryAction(str, Enum):
    UNDO = "undo"
    REDO = "redo"


class Fault(str, Enum):
    NONE = "none"
    STAGE_ALLOCATION = "stage_allocation"
    RECEIPT_ALLOCATION = "receipt_allocation"


@dataclass(frozen=True, order=True)
class Rect:
    x: int
    y: int
    width: int
    height: int

    def union(self, other: "Rect") -> "Rect":
        left = min(self.x, other.x)
        top = min(self.y, other.y)
        right = max(self.x + self.width, other.x + other.width)
        bottom = max(self.y + self.height, other.y + other.height)
        return Rect(left, top, right - left, bottom - top)


@dataclass(frozen=True)
class ObjectRecord:
    value: int
    bounds: Rect


@dataclass(frozen=True)
class FieldKey:
    value: str


@dataclass(frozen=True)
class FieldIndex:
    value: int


FieldPathSegment: TypeAlias = FieldKey | FieldIndex


@dataclass(frozen=True)
class CreateObject:
    object_id: str
    record: ObjectRecord


@dataclass(frozen=True)
class SetObject:
    object_id: str
    record: ObjectRecord


@dataclass(frozen=True)
class RejectOperation:
    """A deterministic invalid operation used only for rollback evidence."""

    field: str = "value"


Operation: TypeAlias = CreateObject | SetObject | RejectOperation


@dataclass(frozen=True)
class OperationBatch:
    operations: tuple[Operation, ...]


TransactionBody: TypeAlias = OperationBatch | HistoryAction


@dataclass(frozen=True)
class Transaction:
    document_id: str
    expected_revision: int
    transaction_id: str
    body: TransactionBody


@dataclass(frozen=True)
class Warning:
    code: WarningCode
    operation_index: int
    field_path: tuple[FieldPathSegment, ...]
    message: str


@dataclass(frozen=True)
class Receipt:
    document_id: str
    transaction_id: str
    base_revision: int
    result_revision: int
    created: tuple[str, ...]
    changed: tuple[str, ...]
    dirty_bounds: Rect | None
    warnings: tuple[Warning, ...]


@dataclass(frozen=True)
class TransactionError:
    code: ErrorCode
    retry_advice: RetryAdvice
    message: str
    operation_index: int | None = None
    field_path: tuple[FieldPathSegment, ...] = ()


@dataclass(frozen=True)
class ApplyResult:
    disposition: Disposition | None = None
    receipt: Receipt | None = None
    error: TransactionError | None = None

    @property
    def succeeded(self) -> bool:
        return self.error is None


@dataclass(frozen=True)
class HistoryEntry:
    before: tuple[tuple[str, ObjectRecord], ...]
    after: tuple[tuple[str, ObjectRecord], ...]
    source_transaction_id: str

    @property
    def weight(self) -> int:
        return max(1, len(self.before) + len(self.after))


@dataclass(frozen=True)
class ReplayEntry:
    transaction: Transaction
    receipt: Receipt


@dataclass
class Cancellation:
    cancel_on_poll: int | None = None
    polls: int = 0

    def poll(self) -> bool:
        self.polls += 1
        return self.cancel_on_poll == self.polls


@dataclass
class Engine:
    document_id: str = "scene"
    revision: int = 0
    objects: dict[str, ObjectRecord] = field(default_factory=dict)
    replay: dict[str, ReplayEntry] = field(default_factory=dict)
    undo: list[HistoryEntry] = field(default_factory=list)
    redo: list[HistoryEntry] = field(default_factory=list)
    max_operations: int = 4
    max_replay_records: int = 8
    max_undo_steps: int = 4
    max_history_weight: int = 32

    def apply(
        self,
        transaction: Transaction,
        mode: ApplyMode = ApplyMode.COMMIT,
        cancellation: Cancellation | None = None,
        fault: Fault = Fault.NONE,
    ) -> ApplyResult:
        cancellation = cancellation or Cancellation()

        if transaction.document_id != self.document_id:
            return self._failure(
                ErrorCode.WRONG_DOCUMENT,
                RetryAdvice.CHANGE_REQUEST,
                "transaction targets a different document",
                path=(FieldKey("document_id"),),
            )

        established = self.replay.get(transaction.transaction_id)
        if established is not None:
            if established.transaction == transaction:
                return ApplyResult(
                    disposition=Disposition.REPLAYED,
                    receipt=established.receipt,
                )
            return self._failure(
                ErrorCode.TRANSACTION_ID_CONFLICT,
                RetryAdvice.CHANGE_REQUEST,
                "transaction ID was committed with different content",
                path=(FieldKey("transaction_id"),),
            )

        cancelled = self._poll_cancel(cancellation)
        if cancelled is not None:
            return cancelled

        if transaction.expected_revision != self.revision:
            return self._failure(
                ErrorCode.STALE_REVISION,
                RetryAdvice.REFRESH_THEN_RETRY,
                "expected revision does not match committed revision",
                path=(FieldKey("expected_revision"),),
            )

        next_revision = self._next_revision()
        if isinstance(next_revision, ApplyResult):
            return next_revision

        if isinstance(transaction.body, OperationBatch):
            return self._apply_batch(
                transaction,
                transaction.body,
                next_revision,
                mode,
                cancellation,
                fault,
            )
        return self._apply_history(
            transaction,
            transaction.body,
            next_revision,
            mode,
            cancellation,
            fault,
        )

    def snapshot(self) -> tuple:
        return (
            self.revision,
            tuple(sorted(self.objects.items())),
            tuple(
                (key, value.transaction, value.receipt)
                for key, value in sorted(self.replay.items())
            ),
            tuple(self.undo),
            tuple(self.redo),
        )

    def _apply_batch(
        self,
        transaction: Transaction,
        batch: OperationBatch,
        next_revision: int,
        mode: ApplyMode,
        cancellation: Cancellation,
        fault: Fault,
    ) -> ApplyResult:
        if not batch.operations:
            return self._failure(
                ErrorCode.EMPTY_TRANSACTION,
                RetryAdvice.CHANGE_REQUEST,
                "operation batch is empty",
                path=(FieldKey("body"), FieldKey("operations")),
            )
        if len(batch.operations) > self.max_operations:
            return self._failure(
                ErrorCode.RESOURCE_LIMIT,
                RetryAdvice.CHANGE_REQUEST,
                "operation count exceeds the configured limit",
                path=(FieldKey("body"), FieldKey("operations")),
            )
        if fault is Fault.STAGE_ALLOCATION:
            return self._allocation_failure()

        staged = dict(self.objects)
        created: list[str] = []
        touched: list[str] = []
        warnings: list[Warning] = []

        for index, operation in enumerate(batch.operations):
            cancelled = self._poll_cancel(cancellation)
            if cancelled is not None:
                return cancelled

            if isinstance(operation, CreateObject):
                if operation.object_id in staged:
                    return self._operation_failure(
                        ErrorCode.DUPLICATE_IDENTITY,
                        index,
                        "object_id",
                        "object identity already exists",
                    )
                staged[operation.object_id] = operation.record
                created.append(operation.object_id)
                touched.append(operation.object_id)
                continue

            if isinstance(operation, SetObject):
                if operation.object_id not in staged:
                    return self._operation_failure(
                        ErrorCode.MISSING_IDENTITY,
                        index,
                        "object_id",
                        "object identity does not exist",
                    )
                if staged[operation.object_id] == operation.record:
                    warnings.append(
                        Warning(
                            code=WarningCode.NO_EFFECT,
                            operation_index=index,
                            field_path=self._operation_path(index, "record"),
                            message="operation leaves the accepted value unchanged",
                        )
                    )
                else:
                    staged[operation.object_id] = operation.record
                touched.append(operation.object_id)
                continue

            return self._operation_failure(
                ErrorCode.INVALID_CONTENT,
                index,
                operation.field,
                "operation is invalid in the staged document",
            )

        if fault is Fault.RECEIPT_ALLOCATION:
            return self._allocation_failure()

        receipt = self._batch_receipt(
            transaction,
            next_revision,
            staged,
            created,
            touched,
            warnings,
        )
        entry = HistoryEntry(
            before=self._freeze_objects(self.objects),
            after=self._freeze_objects(staged),
            source_transaction_id=transaction.transaction_id,
        )
        candidate_undo = self._candidate_undo(entry)
        if candidate_undo is None:
            return self._failure(
                ErrorCode.RESOURCE_LIMIT,
                RetryAdvice.CHANGE_REQUEST,
                "transaction history entry exceeds the configured limit",
            )
        capacity = self._check_replay_capacity()
        if capacity is not None:
            return capacity

        cancelled = self._poll_cancel(cancellation)
        if cancelled is not None:
            return cancelled

        if mode is ApplyMode.DRY_RUN:
            return ApplyResult(disposition=Disposition.DRY_RUN, receipt=receipt)

        self.objects = staged
        self.revision = next_revision
        self.undo = candidate_undo
        self.redo = []
        self.replay[transaction.transaction_id] = ReplayEntry(transaction, receipt)
        return ApplyResult(disposition=Disposition.COMMITTED, receipt=receipt)

    def _apply_history(
        self,
        transaction: Transaction,
        action: HistoryAction,
        next_revision: int,
        mode: ApplyMode,
        cancellation: Cancellation,
        fault: Fault,
    ) -> ApplyResult:
        source = self.undo if action is HistoryAction.UNDO else self.redo
        if not source:
            code = (
                ErrorCode.NOTHING_TO_UNDO
                if action is HistoryAction.UNDO
                else ErrorCode.NOTHING_TO_REDO
            )
            return self._failure(code, RetryAdvice.CHANGE_REQUEST, code.value.replace("_", " "))
        if fault in (Fault.STAGE_ALLOCATION, Fault.RECEIPT_ALLOCATION):
            return self._allocation_failure()

        entry = source[-1]
        target = dict(entry.before if action is HistoryAction.UNDO else entry.after)
        receipt = self._history_receipt(transaction, next_revision, target)
        capacity = self._check_replay_capacity()
        if capacity is not None:
            return capacity

        cancelled = self._poll_cancel(cancellation)
        if cancelled is not None:
            return cancelled

        if mode is ApplyMode.DRY_RUN:
            return ApplyResult(disposition=Disposition.DRY_RUN, receipt=receipt)

        self.objects = target
        self.revision = next_revision
        if action is HistoryAction.UNDO:
            self.undo.pop()
            self.redo.append(entry)
        else:
            self.redo.pop()
            self.undo.append(entry)
        self.replay[transaction.transaction_id] = ReplayEntry(transaction, receipt)
        return ApplyResult(disposition=Disposition.COMMITTED, receipt=receipt)

    def _batch_receipt(
        self,
        transaction: Transaction,
        next_revision: int,
        staged: dict[str, ObjectRecord],
        created: list[str],
        touched: list[str],
        warnings: list[Warning],
    ) -> Receipt:
        created_unique = tuple(dict.fromkeys(created))
        created_set = set(created_unique)
        changed = tuple(
            object_id
            for object_id in dict.fromkeys(touched)
            if object_id not in created_set
            and self.objects.get(object_id) != staged.get(object_id)
        )
        affected = created_unique + changed
        return Receipt(
            document_id=self.document_id,
            transaction_id=transaction.transaction_id,
            base_revision=self.revision,
            result_revision=next_revision,
            created=created_unique,
            changed=changed,
            dirty_bounds=self._dirty_bounds(self.objects, staged, affected),
            warnings=tuple(warnings),
        )

    def _history_receipt(
        self,
        transaction: Transaction,
        next_revision: int,
        target: dict[str, ObjectRecord],
    ) -> Receipt:
        changed = tuple(
            key
            for key in sorted(set(self.objects) | set(target))
            if self.objects.get(key) != target.get(key)
        )
        return Receipt(
            document_id=self.document_id,
            transaction_id=transaction.transaction_id,
            base_revision=self.revision,
            result_revision=next_revision,
            created=(),
            changed=changed,
            dirty_bounds=self._dirty_bounds(self.objects, target, changed),
            warnings=(),
        )

    def _candidate_undo(self, entry: HistoryEntry) -> list[HistoryEntry] | None:
        if self.max_undo_steps < 1 or entry.weight > self.max_history_weight:
            return None
        candidate = list(self.undo)
        candidate.append(entry)
        while len(candidate) > self.max_undo_steps:
            candidate.pop(0)
        while sum(item.weight for item in candidate) > self.max_history_weight:
            if len(candidate) == 1:
                return None
            candidate.pop(0)
        return candidate

    def _check_replay_capacity(self) -> ApplyResult | None:
        if len(self.replay) >= self.max_replay_records:
            return self._failure(
                ErrorCode.RESOURCE_LIMIT,
                RetryAdvice.CHANGE_REQUEST,
                "replay registry cannot retain another transaction",
            )
        return None

    def _next_revision(self) -> int | ApplyResult:
        if self.revision == MAX_REVISION:
            return self._failure(
                ErrorCode.REVISION_OVERFLOW,
                RetryAdvice.NOT_RETRYABLE,
                "document revision cannot advance",
            )
        return self.revision + 1

    @staticmethod
    def _freeze_objects(objects: dict[str, ObjectRecord]) -> tuple[tuple[str, ObjectRecord], ...]:
        return tuple(sorted(objects.items()))

    @staticmethod
    def _dirty_bounds(
        before: dict[str, ObjectRecord],
        after: dict[str, ObjectRecord],
        identities: tuple[str, ...],
    ) -> Rect | None:
        bounds = [
            record.bounds
            for object_id in identities
            for record in (before.get(object_id), after.get(object_id))
            if record is not None
        ]
        if not bounds:
            return None
        dirty = bounds[0]
        for item in bounds[1:]:
            dirty = dirty.union(item)
        return dirty

    @staticmethod
    def _operation_path(index: int, field_name: str) -> tuple[FieldPathSegment, ...]:
        return (
            FieldKey("body"),
            FieldKey("operations"),
            FieldIndex(index),
            FieldKey(field_name),
        )

    def _operation_failure(
        self,
        code: ErrorCode,
        index: int,
        field_name: str,
        message: str,
    ) -> ApplyResult:
        return self._failure(
            code,
            RetryAdvice.CHANGE_REQUEST,
            message,
            operation_index=index,
            path=self._operation_path(index, field_name),
        )

    def _poll_cancel(self, cancellation: Cancellation) -> ApplyResult | None:
        if cancellation.poll():
            return self._failure(
                ErrorCode.CANCELLED,
                RetryAdvice.SAME_REQUEST,
                "transaction was cancelled before commit",
            )
        return None

    def _allocation_failure(self) -> ApplyResult:
        return self._failure(
            ErrorCode.ALLOCATION_FAILURE,
            RetryAdvice.SAME_REQUEST,
            "transaction candidate allocation failed",
        )

    @staticmethod
    def _failure(
        code: ErrorCode,
        advice: RetryAdvice,
        message: str,
        operation_index: int | None = None,
        path: tuple[FieldPathSegment, ...] = (),
    ) -> ApplyResult:
        return ApplyResult(
            error=TransactionError(
                code=code,
                retry_advice=advice,
                message=message,
                operation_index=operation_index,
                field_path=path,
            )
        )
