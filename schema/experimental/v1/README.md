# DrawForge experimental JSON protocol v1

This directory contains generated, provisional machine artifacts for
[ADR-0005](../../../docs/decisions/0005-versioned-json-encoding.md).

- `protocol.schema.json` describes requests, successful results, and structured
  errors using JSON Schema Draft 2020-12.
- `examples.jsonl` contains independently framed canonical examples, including
  the adapter-owned artifact error boundary.

The production `drawforge jsonl --artifact-dir DIRECTORY` command consumes and
produces this framing. `DIRECTORY` must already exist; render results write
exclusive `<artifact_id>.rgba8` or `<artifact_id>.png` artifacts rather than
placing binary data in JSON.

Do not edit the generated files directly. Update `spike/encoding/contract.py`,
run it with `--write`, then run the verifier. No compatibility is promised until
the Phase 1 semantic-tools gate supports one.
