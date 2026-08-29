# Evaluation protocol v2

## Purpose

This protocol freezes the paired Phase 1 gate. It compares direct SVG
replacement with the released DrawForge semantic JSONL surface without adding
an import API, persistence format, or provider dependency to DrawForge.

The common task prompt is route-neutral. A frozen adapter instruction tells the
model which representation to submit. Starting and reference artwork remain the
same v1 SVG fixtures. A corpus-only converter maps that trusted, deliberately
small SVG subset into DrawForge requests; it is not an SVG importer and must
never process model output.

## Frozen matrix

- Nine tasks cover creation, revision, and recovery.
- Each task has five paired trials on each of two routes: 90 runs total.
- Paired trials use the same model identity, immutable version, sampling
  settings, and seed. Route order is randomized outside this repository.
- Each run permits at most three submissions and twelve tool interactions.
- The tested DrawForge version and AIForge evaluation-adapter version are
  recorded in every run.
- Provider-reported input tokens, output tokens, and cost are recorded without
  estimation. The complete paid matrix has a hard USD 3 ceiling.

The direct route submits complete SVG files. A semantic submission is a bounded
JSONL transcript of `drawforge.experimental/v1` requests. For revision tasks,
the harness replays the frozen source transcript before every submitted
attempt. It installs the concurrent source only after the recorded stale-state
event. An attempt is never trusted as a replacement for the frozen source.

## Failure injection

Recovery events and diagnostics are route-shaped but semantically equivalent:

- invalid edit: reject the first submission without committing state;
- stale revision: install the concurrent fixture, reject the stale attempt,
  expose the current revision through normal inspection, then require a rebase;
- context reset: clear conversation context, retain the authoritative document,
  and require a fresh bounded inspection before submission.

The ordered event evidence is part of `run.json`. Missing recovery evidence is
a failed task even when the last artifact looks correct.

## Automated scoring and replay

Direct SVG scoring retains the v1 structural criteria. Semantic scoring starts
a fresh released `drawforge jsonl` process, replays the frozen source and final
attempt, and then issues bounded summary, structure, and selected-object
queries. Revision numbers and configured resource ceilings are excluded from
comparison; authoritative document content is compared with a separately
replayed semantic reference.

Semantic rendering is repeated in a second fresh process. Static tasks render
at time zero. The animation task renders at 0, 300, and 600 milliseconds. Exact
RGBA hashes must match the frozen semantic reference. Any rejected replay,
query failure, content difference, or render difference fails the task. These
checks deliberately require more than pixel equality: semantic structure and
deterministic pixels must both survive replay.

## Visual review

The runtime places PNG previews or contact sheets under each run's `review/`
directory. `prepare-review` creates a packet containing only common prompts,
starting SVGs, blinded artifact names, and hashes. The route key is written to a
separate path and must not be shared with reviewers.

Two reviewers independently score visual fidelity, legibility, composition,
and visible collateral change from one to five. They do not see routes, model
transcripts, references, machine criteria, or each other's scores. A third
reviewer adjudicates every category whose primary scores differ by more than
one point. Review files identify only `blind_id`, reviewer identity, category,
score, and a bounded note.

## Gate rule

The v1 rule remains frozen. A difference is material at 20 percentage points
for a family-level rate or 20 percent for a median/count metric.

- Continue when the semantic route has no material valid-result or visual-score
  regression and materially improves at least two task families.
- Narrow when it improves exactly one family without a material regression.
- Redesign or stop when it improves none, materially regresses validity or
  visual review, or cannot reproduce every accepted result.

The decision report includes every run, per-task and per-family aggregates,
paired failures, adjudicated visual scores, exact runtime identities, corpus and
artifact hashes, total spend, and the supported product claim. Aggregate wins
never override a family regression.
