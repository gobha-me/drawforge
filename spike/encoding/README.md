# Provisional encoding evidence

This directory makes [ADR-0005](../../docs/decisions/0005-versioned-json-encoding.md)
executable without selecting the production C++ JSON adapter.

- `contract.py` is the dependency-free declarative source for the committed
  schema and examples.
- `codec.py` is a strict reference decoder/encoder with bounded framing and
  semantic checks.
- `verify.py` runs the failure-first matrix and checks a stable digest across
  every accepted request/result family.

Run the evidence from the repository root:

```bash
python3 -B spike/encoding/contract.py --check
python3 -B spike/encoding/verify.py
```

`contract.py --write` updates generated files during contract development.
Generated files are reviewed and committed; CI uses only `--check`.
