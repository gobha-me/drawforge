# Transaction contract reference model

This directory makes the state machine accepted by
[ADR-0004](../../docs/decisions/0004-atomic-transaction-contract.md)
executable before the production document and transaction APIs exist.

`model.py` uses typed immutable Python values rather than JSON payloads. It is
not a public API, schema, persistence format, or alternative mutation path.
`verify.py` runs the failure-first matrix, injects cancellation and allocation
failures at deterministic boundaries, and checks a stable summary digest.

Run it with:

```bash
python3 -B spike/transaction/verify.py
```

The verifier is dependency-free and writes no files.
