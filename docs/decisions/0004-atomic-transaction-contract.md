# ADR-0004: Atomic transaction, receipt, error, and retry contract

Status: accepted for the Phase 0 contract

Date: 2026-08-27

Issue: [#6](https://github.com/gobha-me/drawforge/issues/6)

## Context

Scripts, model-facing tools, the headless CLI, and a future workbench need one
mutation path. That path must tolerate transport retries, stale clients,
cancellation, invalid operation batches, and resource exhaustion without
making committed state ambiguous. ADR-0002 supplies typed identities,
revisions, numeric limits, and bounded diagnostics. ADR-0003 supplies the
minimal scene and query model that transactions will edit.

This decision defines domain behavior, not a C++ implementation or JSON
encoding. Issue #7 owns the provisional wire schema, issue #8 owns the compiled
scene/query core, and issue #9 owns the production dispatcher and undo storage.
The executable reference model in `spike/transaction/` exercises the state
machine without becoming a second public API.

## Decision

### Request shape and initial operation algebra

A mutation request is a typed `Transaction` containing a `DocumentId`, the
caller's `expected_revision`, a document-scoped `TransactionId`, and exactly
one body:

- an ordered, non-empty operation batch;
- `undo`; or
- `redo`.

The dispatcher receives an `ApplyMode` of `dry_run` or `commit`. The mode is
not part of transaction identity: a successful dry-run can be followed by a
commit of the same transaction. A transaction is typed data, not executable
source, callbacks, JSON, or a collection of privileged setters.

The first operation algebra contains only behavior required by the accepted
corpus and scene contract:

- create a layer, group, rectangle, ellipse, line path, or opacity track;
- set the canvas background;
- set an accepted node's visibility, local transform, geometry, style, or
  authored opacity;
- set accepted opacity-track properties; and
- reparent or reorder an object within a layer or group.

The document extent and resource limits are fixed at document creation.
Caller-selected IDs are the same-transaction reference mechanism: an operation
may refer to an identity created by an earlier operation in the batch. Forward
references fail as missing identities. Removing scene content, changing the
canvas extent, general property paths, and deferred scene features are not in
the initial algebra.

Operations validate and evaluate in request order against a private staged
state. A later operation therefore observes all earlier successful operations.
The operation count must be positive and no greater than
`max_transaction_operations`. An operation that writes the value already
present is valid, emits a bounded `no_effect` warning, and does not add the
identity to the changed set. Even an all-no-effect accepted batch advances one
revision and creates one undo entry; this keeps receipt, replay, and audit
semantics uniform.

### Deterministic dispatch order

The dispatcher applies this precedence:

1. Reject a mismatched document identity.
2. Look up the transaction ID in the successful replay registry. An identical
   typed transaction returns its established receipt without consulting the
   current revision or cancellation state. Different typed content under the
   same ID fails with `transaction_id_conflict`.
3. Observe cancellation before validation.
4. Compare `expected_revision` with the current committed revision.
5. Validate the body and its resource budgets.
6. For an operation batch, observe cancellation immediately before each
   operation, then validate and apply that operation to staged state.
7. Construct the complete candidate state, receipt, replay record, and history
   change; validate revision advancement and retention budgets.
8. Observe cancellation once more immediately before commit.
9. Commit the already-prepared aggregate with one non-failing atomic state
   replacement.

This precedence is program logic. In particular, a new request with stale
state reports `stale_revision` before operation errors, while an established
replay remains successful after later revisions. Adapters may reject malformed
wire input before a typed transaction exists, but cannot reorder typed domain
validation.

Public fallible boundaries return `std::expected`; allocation or internal
exceptions are caught before crossing them. Candidate construction failure is
`allocation_failure` and leaves committed state untouched. Implementations
must prepare all potentially failing receipt, replay, and history allocation
before the atomic replacement.

### Dry-run, commit, and replay

A successful normal commit advances the revision exactly once, installs all
operations, appends one undo entry, clears redo history, records the request
and receipt under its transaction ID, and returns disposition `committed`.

A successful dry-run performs the same validation and candidate construction
and returns disposition `dry_run` with the receipt that an immediate commit
would establish. It changes no document data, revision, undo/redo history, or
replay registry and does not reserve its transaction ID. Resource checks cover
commit feasibility, so dry-run does not claim success for a transaction that
the current retention budgets cannot commit.

Only successful commits and successful history actions enter the replay
registry. Failed and cancelled IDs may be corrected and reused because no
record exists. A replay matches the complete typed transaction, including its
original expected revision and body. An exact replay returns disposition
`replayed` and the value-for-value original receipt; it never reapplies edits,
advances revision, or changes history. Changing an expected revision, body, or
document while reusing the ID is a conflict, not a new request.

Replay records are never evicted during a document's lifetime. The production
document configuration must bound their count and retained bytes. Once either
budget cannot accept a new record, new commits fail closed with
`resource_limit`; already-recorded replays continue to succeed. Exact numeric
defaults and storage accounting belong to issue #9, where the representation
can be measured, rather than being guessed by this contract.

### Receipt and warnings

A `TransactionReceipt` contains:

- `DocumentId`, `TransactionId`, base revision, and resulting revision;
- created and changed identities, tagged by their nominal identity domain;
- optional document-space dirty bounds; and
- bounded typed warnings.

Created and changed lists are disjoint, contain no duplicates, and follow
first-touch operation order. A created identity stays in `created` if later
operations in the same batch modify it. History-action receipts list every
semantically affected identity as changed in deterministic identity order.

Dirty bounds are the union of the pre-transaction and post-transaction
document-space paint-capable bounds of affected content. They include fill and
stroke geometry but conservatively ignore visibility, authored opacity, and
evaluated animation opacity. This covers both erased and newly paintable pixels
without consulting a renderer, an evaluation time, or wall-clock time.
Reordering includes the affected siblings; a background change dirties the
canvas. An edit with no geometry or paint-capability effect has no dirty bounds.

Warnings have a stable code, optional operation index and typed field path,
and bounded diagnostic text. They are part of the established receipt and are
replayed exactly. Warning count cannot exceed the operation count. Messages
are fixed library text, never quote caller input, and obey ADR-0002's diagnostic
bound.

### Structured failures and retry advice

A `TransactionError` contains a stable code, optional zero-based operation
index, a typed field path, retry advice, and bounded diagnostic text. Field
paths are sequences of field-key and array-index components rooted in the typed
transaction; they are not JSON Pointer strings. Transaction-level failures
have no operation index and may have an empty path.

The initial transaction-level codes are `wrong_document`,
`transaction_id_conflict`, `stale_revision`, `empty_transaction`,
`nothing_to_undo`, `nothing_to_redo`, `cancelled`, `resource_limit`,
`revision_overflow`, and `allocation_failure`. Operation failures reuse the
stable semantic categories from ADR-0003. Issue #7 maps these names onto the
wire without exposing its JSON library through the domain API.

`RetryAdvice` has four values:

- `same_request`: a transient cancellation or allocation failure may succeed
  unchanged;
- `refresh_then_retry`: inspect the new revision, then resubmit under an unused
  transaction ID or reuse the failed unregistered ID with corrected expected
  revision;
- `change_request`: the request or requested resources must change; and
- `not_retryable`: the document cannot accept the request, such as revision
  exhaustion.

Retry advice is guidance, not an alternate execution path. Every retry still
enters the same dispatcher.

### Cancellation and atomicity

Cancellation is an explicitly injected predicate or token observed only at the
operation boundaries listed above. It is not derived from elapsed time. A
cancelled request returns `cancelled` with `same_request` advice and leaves the
document, revision, undo/redo history, and replay registry unchanged.

The final cancellation observation occurs before the atomic replacement. Once
replacement begins, the dispatcher completes and reports success even if a
caller concurrently requests cancellation. This prevents the ambiguous case
where a client receives `cancelled` for state that actually committed.

Every failure—including an invalid later operation, bounds failure, revision
overflow, cancellation, retention exhaustion, and allocation failure—preserves
the complete committed aggregate. No error returns a partial receipt or
partially staged identities.

### Undo and redo

Undo and redo are typed transaction bodies routed through the same dispatcher,
not public back doors into document state. Each carries its own transaction ID
and expected revision, is dry-runnable and replay-safe, and advances revision
exactly once when committed.

A normal commit stores one logical before/after history entry for the entire
batch. Undo transfers the newest undo entry to redo and restores its before
state. Redo transfers the newest redo entry back to undo and restores its after
state. A history action does not create a second logical edit entry. A normal
commit after undo clears redo history. Empty stacks fail as `nothing_to_undo`
or `nothing_to_redo` without mutation.

Undo retention has configured depth and retained-size budgets. Before a normal
commit, the implementation may evict the oldest undo entries deterministically
until the new entry fits. It must not evict replay records. If the new entry
cannot fit by itself, the transaction fails with `resource_limit` and the
existing history is preserved. Issue #9 chooses the delta, snapshot, or other
private representation and its exact byte-accounting rule.

## Failure-first conformance matrix

| Candidate | Required result |
| --- | --- |
| wrong document | `wrong_document`; no registry lookup or mutation |
| exact successful transaction ID replay after later commits | original receipt; no revision or history change |
| successful ID reused with different expected revision or body | `transaction_id_conflict` |
| stale expected revision on an unregistered ID | `stale_revision`; operation validation is not reached |
| empty or oversized operation batch | stable failure; no staged identity leaks |
| duplicate or missing ID in operation N | error names N and its typed field path; earlier staged edits roll back |
| dry-run followed by commit | projected receipt first; one real commit and replay record second |
| cancellation before validation, between operations, or before commit | `cancelled`; complete aggregate unchanged |
| allocation, bounds, revision, replay, or history exhaustion | stable failure; complete aggregate unchanged |
| same-value writes | accepted with deterministic `no_effect` warnings |
| undo/redo of a mixed batch | entire batch moves as one entry and each history action advances once |
| new edit after undo | redo stack is cleared only by the successful commit |
| repeated model run with identical inputs | value-for-value state, receipt, error, and summary digest |

The executable reference model covers this matrix with injected cancellation,
allocation, revision, replay, and history limits. Its scene payload is
deliberately tiny: it proves transaction mechanics and does not compete with
the compiled scene implementation assigned to issues #8 and #9.

## Consequences

- Issue #7 can encode one deterministic request/result state machine and can
  distinguish adapter parse failures from typed domain failures.
- Issue #8 can expose read-only scene construction to issue #9 without adding
  a second public mutation route.
- Issue #9 must implement strong atomic replacement, replay retention, and
  bounded undo storage before exposing the transaction API.
- A future workbench, CLI, or AIForge adapter cannot bypass revision checks,
  replay behavior, cancellation boundaries, or receipts.
- Delete operations, persistence, history representation, compatibility
  promises, JSON field names, and renderer dirty-pixel rounding remain outside
  this decision.
