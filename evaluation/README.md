# DrawForge evaluation

This directory contains the versioned corpus and provider-neutral tooling used
to compare direct SVG authoring with DrawForge's semantic route. It is an
experiment contract, not a public DrawForge file format or command API.

The first route is runnable before the DrawForge document model exists:

```bash
python3 evaluation/tools/evaluate.py verify-corpus
python3 evaluation/tools/evaluate.py prepare-run \
  --task create-status-badge \
  --output /tmp/drawforge-run \
  --provider example --model example-model --model-version 2026-08-26 \
  --trial 1 --seed 1001

# Put one or more model-produced SVGs in /tmp/drawforge-run/attempts, update
# run.json with usage and interaction evidence, then score the final attempt.
python3 evaluation/tools/evaluate.py evaluate-run --run /tmp/drawforge-run
```

`verify-corpus` validates the corpus contract and proves every checked-in
reference satisfies its machine criteria. `prepare-run` copies only the
information available to the direct-SVG route. `evaluate-run` validates the
run record, scores the last SVG attempt, and writes a bounded `result.json`.
It never invokes a model or accesses the network.

See [PROTOCOL.md](PROTOCOL.md) for the comparison and gate rules.

The v1 baseline above remains frozen. Protocol v2 adds paired route-neutral
runs while preserving all v1 fixtures and criteria:

```bash
python3 evaluation/tools/evaluate_v2.py verify-corpus \
  --drawforge build/src/bin/drawforge

python3 evaluation/tools/evaluate_v2.py prepare-run \
  --task revise-named-sun --route semantic \
  --output /tmp/drawforge-v2-run \
  --provider example --model immutable-model --model-version 2026-08-29 \
  --drawforge build/src/bin/drawforge --drawforge-version 0.12.0 \
  --adapter-version aiforge-eval-v1 --adapter-commit 0123456789abcdef \
  --provider-runtime venice-chat-completions-v1 \
  --direct-svg-renderer /usr/bin/gdk-pixbuf-thumbnailer \
  --direct-svg-renderer-version gdk-pixbuf-2.42.10-librsvg-2.58.0 \
  --trial 1 --seed 1001 --temperature 0

# Put bounded submissions in attempts/, update run.json with actual usage and
# event evidence, and replay the final attempt through the released executable.
python3 evaluation/tools/evaluate_v2.py evaluate-run \
  --run /tmp/drawforge-v2-run --drawforge build/src/bin/drawforge \
  --direct-svg-renderer /usr/bin/gdk-pixbuf-thumbnailer
```

`aggregate` rejects duplicate paired-route results and reports the expected
90-run matrix. `prepare-review` consumes runtime-produced PNG previews, assigns
route-blinded IDs using a caller-held key, and writes the route key outside the
review packet. See [PROTOCOL-v2.md](PROTOCOL-v2.md) for the frozen v2 matrix,
failure injections, replay contract, blinded review, and decision rule.
