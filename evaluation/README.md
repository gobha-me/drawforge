# DrawForge evaluation

This directory contains the versioned corpus and provider-neutral tooling used
to compare direct SVG authoring with DrawForge's future semantic route. It is
an experiment contract, not a public DrawForge file format or command API.

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
