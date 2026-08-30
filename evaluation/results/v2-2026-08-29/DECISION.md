# Phase 1 semantic-tools gate decision

## Decision

**Stop.** The tested semantic route does not support continued DrawForge
product expansion. The direct-SVG baseline materially outperformed it in every
task family, while the semantic route produced no valid result in any of its 45
runs. This satisfies the frozen `redesign or stop` rule through a material
valid-result regression and no improved family; this report chooses the
terminal `stop` disposition.

The supported claim is deliberately narrow: for the frozen v2 corpus,
`minimax-m25@venice-created-1770854400`, DrawForge 0.12.0, and AIForge
evaluation adapter v0.49.10, direct SVG was more reliable and used fewer model
tokens than the semantic JSONL route. The evidence does not support adding a
general terminal paint application, broad graphics features, or further
semantic-surface expansion.

## Frozen matrix

- Corpus: `drawforge-semantic-svg-v2`
- Runs: 90 of 90; 45 paired trials; 45 runs per route
- Model: `minimax-m25@venice-created-1770854400`
- DrawForge: 0.12.0, executable SHA-256
  `b24f17fa54ae5e69b7df9113f7b3fcac72bd428a64755bf69a69c9b11d7e77d7`
- AIForge adapter: v0.49.10, commit
  `7345945897eefa875e9c3c63d266101a42dc1dc4`
- Direct renderer: `gdk-pixbuf-2.42.10-librsvg-2.58.0`, executable SHA-256
  `9f5c742ee2c698c7e1e9978105305a0d51fd82eedae25a8e4c6eaa8a355a3a44`
- Corpus v1 SHA-256:
  `51fcafa419881aeedccc2194337955b1982ccb1b8d2c97263a4401ffc92b4ba9`
- Corpus v2 SHA-256:
  `b82746b192a91d1e0b170f71de9c177058739b1e3a5711f9e06343d4d11f7d22`
- Protocol schema SHA-256:
  `9a1a28fd3d726308dda486cfffee13f9547e9b0e90771444eab882aaa5164945`

Every scored run is listed in [run-index.tsv](run-index.tsv). Its raw prompt,
source, bounded assistant output, event evidence, attempts, review PNG, and
machine result remain under `runs/`. [aggregate.json](aggregate.json) is the
deterministically generated machine aggregate; its `pending-human-review`
value records the pre-adjudication stage rather than overriding this final
decision.

## Route-level result

| Metric | Direct SVG | Semantic |
| --- | ---: | ---: |
| Valid results | 41/45 (91.1%) | 0/45 (0.0%) |
| Completed tasks | 27/45 (60.0%) | 0/45 (0.0%) |
| Successful recoveries | 9/15 (60.0%) | 0/15 (0.0%) |
| Median tool interactions | 1 | 5 |
| Input tokens | 360,255 | 5,119,380 |
| Output tokens | 66,866 | 441,270 |
| Provider-reported matrix cost | USD 0.00 | USD 0.00 |

The semantic route regressed valid-result rate by 91.1 percentage points and
completion rate by 60 percentage points. Both exceed the frozen 20-point
materiality threshold. It improved no family.

## Family-level result

| Family | Route | Valid | Complete | Recovered |
| --- | --- | ---: | ---: | ---: |
| Creation | Direct SVG | 9/10 | 4/10 | 0/10 |
| Creation | Semantic | 0/10 | 0/10 | 0/10 |
| Revision | Direct SVG | 19/20 | 14/20 | 0/20 |
| Revision | Semantic | 0/20 | 0/20 | 0/20 |
| Recovery | Direct SVG | 13/15 | 9/15 | 9/15 |
| Recovery | Semantic | 0/15 | 0/15 | 0/15 |

## Per-task result

| Task | Route | Valid | Complete | Recovered | Tool interactions | Input tokens | Output tokens |
| --- | --- | ---: | ---: | ---: | ---: | ---: | ---: |
| `create-status-badge` | Direct SVG | 4/5 | 4/5 | 0/5 | 5 | 22,937 | 3,828 |
| `create-status-badge` | Semantic | 0/5 | 0/5 | 0/5 | 52 | 3,218,562 | 88,694 |
| `revise-named-sun` | Direct SVG | 5/5 | 5/5 | 0/5 | 5 | 23,988 | 3,783 |
| `revise-named-sun` | Semantic | 0/5 | 0/5 | 0/5 | 0 | 139,719 | 37,375 |
| `recolor-card-theme` | Direct SVG | 5/5 | 5/5 | 0/5 | 5 | 23,834 | 3,583 |
| `recolor-card-theme` | Semantic | 0/5 | 0/5 | 0/5 | 7 | 146,813 | 38,907 |
| `align-toolbar-icons` | Direct SVG | 4/5 | 4/5 | 0/5 | 4 | 54,858 | 12,587 |
| `align-toolbar-icons` | Semantic | 0/5 | 0/5 | 0/5 | 26 | 165,557 | 42,440 |
| `group-and-move-mascot` | Direct SVG | 5/5 | 0/5 | 0/5 | 5 | 25,338 | 5,023 |
| `group-and-move-mascot` | Semantic | 0/5 | 0/5 | 0/5 | 37 | 645,075 | 69,200 |
| `animate-dot-entrance` | Direct SVG | 5/5 | 0/5 | 0/5 | 5 | 24,845 | 4,868 |
| `animate-dot-entrance` | Semantic | 0/5 | 0/5 | 0/5 | 27 | 347,243 | 47,729 |
| `recover-invalid-edit` | Direct SVG | 4/5 | 0/5 | 0/5 | 11 | 90,965 | 16,597 |
| `recover-invalid-edit` | Semantic | 0/5 | 0/5 | 0/5 | 0 | 134,519 | 37,224 |
| `recover-stale-revision` | Direct SVG | 4/5 | 4/5 | 4/5 | 8 | 67,156 | 11,951 |
| `recover-stale-revision` | Semantic | 0/5 | 0/5 | 0/5 | 18 | 120,190 | 36,227 |
| `continue-after-compaction` | Direct SVG | 5/5 | 5/5 | 5/5 | 5 | 26,334 | 4,646 |
| `continue-after-compaction` | Semantic | 0/5 | 0/5 | 0/5 | 29 | 201,702 | 43,474 |

## Failure analysis

The semantic route accepted no artifact in all 45 runs. It often consumed
multiple tool interactions and substantially more tokens before ending without
a submission. The adapter and scorer still completed every run, recorded exact
provider accounting, and produced the common transparent failure preview, so
these are model-route outcomes rather than hidden infrastructure failures.

The direct route was not perfect. Exact task checks caught missing or differently
encoded animation attributes, a transform spelling mismatch, a malformed
recovery attempt, and one no-submission result. Those failures remain visible
instead of being hidden by the aggregate. Direct SVG nevertheless completed
tasks in all three families and produced reproducible valid artifacts in 41
runs.

## Visual adjudication

The route-blinded packet contains 90 PNGs, common prompts, starting artwork,
and hashes. The route key was generated outside the packet. On 2026-08-30 the
repository owner reported: “I adjudicated the results, looks good.” Individual
reviewer score files were not supplied to the repository, so this report makes
no numerical visual-score comparison and does not infer one. The adjudication
is recorded in [adjudication.json](adjudication.json). The stop decision does
not depend on a visual regression because the machine-validity regression is
independently decisive under the frozen rule.

## Spend

All 90 matrix runs contain provider-reported USD cost and sum to USD 0.00. The
hard ceiling was USD 3.00. Eleven earlier diagnostic or timed-out calls lacked
provider cost persistence; [cost-reservations.json](cost-reservations.json)
reserves their exact calculated maximum of USD 0.61869808, rounded up to a USD
0.619 ceiling. Reported matrix spend plus the conservative reservation remains
within the hard ceiling.

## Scope disposition

- Stop Phase 1 product expansion at the released semantic-core boundary.
- Do not build the deferred terminal paint/workbench product or add broad text,
  raster-brush, filter, mask, collaboration, or video scope.
- Retain the released library, frozen corpus, harness, and results as an
  auditable experiment.
- Reopening requires a new, explicitly approved redesign issue with a newly
  pre-registered evaluation; it is not an automatic continuation of issue 12.
