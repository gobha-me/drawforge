# Evaluation protocol v1

## Purpose

The experiment compares complete SVG replacement with bounded semantic
inspection and typed transactions. Both routes receive the same task request,
starting artwork, interaction budget, and deterministic injected events. The
semantic route will be added after its contract exists; this version makes the
direct-SVG baseline runnable now.

The corpus and run metadata are evaluation-only JSON. They do not define the
DrawForge domain API, command encoding, persistence format, or compatibility
policy.

## Trial controls

- Freeze the corpus commit, prompt bytes, provider, immutable model version,
  sampling settings, and tool/schema version before a comparison.
- Run five paired trials for every task and model configuration. Pair routes by
  seed when the provider exposes deterministic seeds; otherwise record `null`
  and randomize route order.
- Give each route at most three submissions and twelve tool interactions per
  task. Do not reveal reference SVGs, criteria files, or route-specific hidden
  information to the model.
- Record input/output tokens when the provider reports them. Missing token data
  remains `null`; it is never estimated from text after the fact.
- Hash every prompt, input, attempt, and final artifact with SHA-256. Times are
  diagnostic only and are not a pass criterion.

Recovery tasks use deterministic events described in their prompts. A direct
SVG stale-revision submission uses the SHA-256 of the supplied starting SVG as
its expected revision. The harness rejects it after the concurrent fixture is
installed, returns the new hash, and scores only a candidate rebased on that
fixture. A compaction task records a context-reset event before the final
inspection and submission.

## Automated scoring

Every candidate must be bounded, well-formed SVG with unique IDs, finite
numbers, no scripts, and no external resource references. Per-task criteria
then check named elements, exact properties demanded by the prompt,
parent/order relationships, animation declarations, protected attributes, and
unchanged protected subtrees. These checks measure semantic correctness rather
than renderer pixel equality; renderer selection remains a separate spike.

The result records:

- final validity and task completion;
- failed submissions and recovery evidence;
- protected-state violations as unintended changes;
- attempts, tool interactions, and provider-reported token use; and
- hashes required to reproduce and audit the run.

## Human review

Artifacts are renamed and shuffled so reviewers cannot see their route. Two
reviewers independently score visual fidelity, legibility, composition, and
whether the requested change introduced visible collateral damage on a
five-point scale. A third reviewer adjudicates any category differing by more
than one point. Reviewers see the common prompt and starting artwork, but not
reference files, machine criteria, transcripts, or route metadata.

## Gate decision

Compare paired trials by task family. A difference is material at twenty
percent or more in a family-level rate or median:

- **revision integrity:** completion and unintended-change rate;
- **recovery reliability:** successful completion after invalid, stale, or
  context-reset events, plus rejected attempts; and
- **interaction cost:** tool interactions and input/output tokens when present.

The semantic route passes when it has no material regression in valid-result
or human-review scores and materially improves at least two families. It is
narrowed when it improves exactly one family without a material regression. It
must be redesigned or stopped when it improves none, materially regresses
validity or human review, or cannot reproduce every accepted result from its
recorded inputs. The Phase 1 gate records both aggregate and per-task results;
an aggregate must not hide a task-family regression.
