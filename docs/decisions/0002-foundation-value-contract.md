# ADR-0002: Foundation value and resource-limit contract

Status: accepted for the Phase 0 contract

Date: 2026-08-27

Issue: [#4](https://github.com/gobha-me/drawforge/issues/4)

## Context

Documents, queries, transactions, rendering, and adapters need to exchange
identities and numbers without allowing invalid or unbounded values into the
domain model. The values must remain independent of JSON, PlutoVG, AIForge,
TermForge, and RasterForge types. They also need deterministic comparison and
serialization identities before the scene and command contracts are built.

## Decision

### Identities

`DocumentId`, `LayerId`, `ObjectId`, `TransactionId`, `AssetId`, and `TrackId`
are separate nominal types. Their factories accept ASCII identifiers matching
`[A-Za-z][A-Za-z0-9._-]*`. Bytes are preserved exactly: there is no case
folding, Unicode normalization, or implicit prefixing. Equality and ordering
are bytewise, so an adapter can serialize `value()` without changing identity.

Each type has its own collision namespace. Document IDs are unique in their
owning store. Layer, object, transaction, asset, and track IDs are each unique
within a document. Equal spellings in different namespaces are valid. An
owning aggregate must validate its complete candidate registry before commit;
a duplicate never partially populates committed state.

Revision zero is the initial committed revision. Every `uint64_t` revision is
representable, but advancing the maximum value fails with
`revision_overflow`.

### Text and diagnostics

`BoundedText` preserves valid UTF-8 bytes, permits empty text, and rejects
embedded NUL and malformed encodings. Size is checked before content so a
hostile oversized value does not trigger an unbounded scan. Diagnostic messages
are fixed library text, do not quote caller input, and remain below 256 bytes.

### Numeric representation

Semantic coordinates and affine-transform components use finite `double`
values. Negative zero is canonicalized to positive zero. The default absolute
magnitude is 65,536 and the hard ceiling is 1,048,576 (2^20), which leaves
useful subpixel precision when a value is explicitly narrowed to the
provisional renderer's `float` boundary. Non-finite and out-of-range values
never reach that adapter.

Identities, bounded text, and revisions have stable ordering. Coordinates,
points, transforms, extents, and limit sets expose equality but deliberately
do not invent an ordering with no domain meaning. Their future interchange
encoding must preserve the validated numeric values; this ADR does not choose
a textual number format.

The affine convention is `x' = a*x + c*y + e` and
`y' = b*x + d*y + f`. `first.then(second)` applies `first` and then `second`.
Construction permits singular matrices because empty or collapsed geometry is
a semantic scene decision. Application and composition use wider intermediate
arithmetic and revalidate every result.

### Resource ceilings

`ResourceLimits` is immutable after validated construction. Callers can select
any value from zero through the corresponding hard ceiling; zero intentionally
denies that resource. Independent limits all apply, so satisfying a dimension
limit does not bypass pixel or output-byte accounting.
The scene-node budget counts layers, groups, and drawable objects together so
no node kind can bypass the document ceiling.

| Resource | Default | Hard ceiling |
| --- | ---: | ---: |
| Identifier bytes | 64 | 128 |
| Text bytes per value | 4 KiB | 64 KiB |
| Numeric magnitude | 65,536 | 1,048,576 |
| Canvas dimension | 4,096 | 16,384 |
| Canvas pixels | 16,777,216 | 67,108,864 |
| Scene nodes | 4,096 | 65,536 |
| Operations per transaction | 256 | 4,096 |
| Output bytes | 64 MiB | 256 MiB |
| Nesting depth | 32 | 128 |

Canvas layout checks non-zero dimensions, per-axis limits, checked pixel-count
multiplication, the pixel ceiling, checked RGBA8 byte multiplication, and the
output-byte ceiling in that order. General checked size arithmetic reports
`arithmetic_overflow`; an otherwise representable value beyond a caller budget
reports `resource_limit`.

`ValueErrorCode` is stable program logic. `value_error_code_name()` provides
its lowercase encoding identity, while the detailed JSON error and field-path
shape remains deferred to issue #7 and transaction retryability to issue #6.

## Consequences

- Issue #5 can build semantic nodes and bounds entirely from validated
  DrawForge values.
- Issue #6 can reuse typed transaction IDs, revisions, checked budgets, and
  duplicate detection without inventing another mutation path.
- Issue #10 must still validate package wiring, allocation behavior, pixel
  semantics, and the explicit conversion into PlutoVG.
- These Phase 0 types are experimental; no compatibility promise is made
  before the first evaluation gate.
