# ADR-0005: Provisional versioned JSON interchange encoding

Status: accepted for the Phase 0 contract

Date: 2026-08-28

Issue: [#7](https://github.com/gobha-me/drawforge/issues/7)

## Context

The Phase 1 CLI, evaluation route, scripts, and future external adapters need a
machine encoding for the accepted document queries and transaction state
machine. JSON is useful at those boundaries, but it must not become the domain
model, introduce provider types, or create a second mutation path.

ADR-0002 supplies validated identities, numbers, revisions, extents, and
resource ceilings. ADR-0003 supplies the minimal scene and bounded-query
families. ADR-0004 supplies the transaction envelope, operation algebra,
receipts, structured failures, dry-run, replay, and history semantics. This
decision maps those typed values to a provisional wire format. It does not
select a production C++ JSON dependency or implement the CLI.

## Decision

### Boundary and version

The initial protocol token is exactly:

```text
drawforge.experimental/v1
```

Every normal frame contains that token in its required top-level `protocol`
field. A decoder accepts only the exact version it implements. A different or
missing token fails before domain construction with `unsupported_version`; the
error reports the versions the adapter accepts. There is no major/minor
compatibility promise, field-level fallback, or best-effort downgrade before
the Phase 1 gate.

This exact-version rule is deliberate. Strict failure exposes schema cost and
model mistakes during the experiment instead of silently changing a request.
If the gate supports continuing the product, a later decision can define
compatibility using evidence from captured v1 traffic.

JSON remains an adapter encoding. The typed C++ API does not expose a JSON
value, parser, schema implementation, wire error, or provider/tool runtime
type. Transaction replay compares complete typed transactions, not JSON bytes,
object member order, whitespace, or numeric spelling.

### JSONL framing

The headless streaming form is newline-delimited JSON:

- one non-empty UTF-8 JSON object is one frame;
- LF and CRLF terminators are accepted;
- one complete final frame may end at EOF without a newline;
- emitters always terminate frames with LF;
- blank lines, embedded raw newlines, trailing JSON, a UTF-8 BOM, embedded NUL,
  malformed UTF-8, and truncated final JSON are failures; and
- requests are processed in input order and produce exactly one response in
  that order.

No transport correlation ID is added to v1. Ordered one-for-one framing serves
the CLI and evaluation runner, while model tool calls already have an
adapter-owned invocation identity. A future multiplexed transport can add its
own correlation without changing `DocumentId` or `TransactionId`.

### Strict object and value rules

All field names use `snake_case`. Tagged unions use a required `kind` or `op`
string. Every object rejects unknown fields, every required field must be
present, duplicate JSON object keys are invalid, and an unknown tag or enum
value is invalid. `null` is accepted only where the machine schema explicitly
permits it.

Omitting `limits` from `create_document` selects ADR-0002's complete default
`ResourceLimits`. If `limits` is present, every limit is explicit; partial
limit overrides are not accepted. Other domain values have no implicit wire
defaults. Caller-selected IDs remain ordinary validated strings in the field
whose type gives the identity domain. Same-transaction references use those
IDs directly and do not add JSON Reference or a second placeholder syntax.

The machine contract uses JSON Schema Draft 2020-12. One dependency-free
declarative Python contract generates the committed schema and examples. The
strict reference codec consumes the same shape and adds semantic validation
that JSON Schema cannot express cleanly, including UTF-8 byte limits, canvas
cross-products, rectangle radii, path state, and receipt invariants.

### Domain value mapping

| Typed value | Provisional JSON representation |
| --- | --- |
| nominal identity | validated string in a domain-specific field |
| tagged identity in a receipt | `{ "kind": "layer|object|track|...", "id": "..." }` |
| `Revision` and microsecond time | non-negative JSON integer through `uint64_t` maximum |
| coordinate and opacity | finite JSON number inside its accepted range |
| color | lowercase, explicit-alpha `#rrggbbaa` string |
| point or extent | object with named components |
| affine transform | object with `a`, `b`, `c`, `d`, `e`, and `f` |
| optional value | explicit JSON value or `null` where allowed |
| typed field path | array of non-empty string keys and non-negative integer indexes |
| binary preview | artifact identity plus format, dimensions, byte length, and SHA-256 |

Integers are parsed without a 53-bit restriction because the C++ and Python
reference paths preserve the full accepted `uint64_t` range. Integrations with
number-limited runtimes must use an exact integer parser or reject values they
cannot represent; silently rounding a revision is forbidden.

Non-finite numbers and numeric overflow are rejected. Decoded negative zero is
the same typed value as positive zero, and the reference encoder emits positive
zero. Object member order is insignificant. The reference encoder sorts keys
and uses shortest round-trippable JSON numbers to create deterministic evidence,
but that byte spelling is not a persistence or signature format.

### Requests

The request envelope contains `protocol` and exactly one `request`. The initial
request tags are:

- `create_document`: document identity, canvas extent, explicit nullable
  background, and optional complete resource limits;
- `inspect`: one typed document-summary, structure, selected-objects, or bounds
  query;
- `apply`: `dry_run` or `commit` plus one typed transaction; and
- `render`: document and expected revision, explicit microsecond time, `rgba8`
  or `png`, and a caller-selected artifact identity.

Structure query results use a flat document-ordered node array with explicit
parent and sibling information. They do not recursively reproduce the scene
tree, so JSON nesting remains constant even at the accepted domain nesting
ceiling. Selected-object and bounds results preserve request order and return
only the requested projections.

The operation tags map one-for-one to ADR-0004's initial algebra:
`create_layer`, `create_group`, `create_rectangle`, `create_ellipse`,
`create_path`, `create_opacity_track`, `set_canvas_background`,
`set_visibility`, `set_transform`, `set_geometry`, `set_style`, `set_opacity`,
`set_opacity_track`, `reparent_object`, and `reorder_object`. Transaction bodies
are tagged as `operations`, `undo`, or `redo`. No general property path, delete,
script, persistence, SVG, terminal, or provider operation is added.

### Responses, receipts, and artifacts

A successful response contains `protocol`, `status: "ok"`, and one tagged
`result`. Results cover document creation, query results, transaction results,
and rendering metadata. Transaction results carry the accepted disposition and
complete ADR-0004 receipt.

Created and changed receipt identities are explicitly tagged by nominal domain,
are each duplicate-free, and remain disjoint. The result revision is exactly
one greater than the base revision. Dirty bounds are a semantic rectangle or
`null`; warnings contain their stable code, nullable operation index, typed
field path, and bounded fixed diagnostic.

RGBA8 and PNG bytes never appear inline in a JSON frame. A render request names
an `ArtifactId`; its result reports that identity, format, dimensions, bounded
byte length, and lowercase SHA-256. The CLI or integration adapter owns the
artifact storage action. The wire shape does not expose an AIForge ArtifactStore
type or grant a filesystem path.

### Errors and dispatch boundary

An error response contains `protocol`, `status: "error"`, and a structured
error with:

- `source`: `encoding`, `domain`, or `adapter`;
- stable `code`;
- ADR-0004 retry advice;
- nullable zero-based operation index;
- typed field path;
- a fixed diagnostic shorter than 256 UTF-8 bytes; and
- `supported_versions` only when version negotiation requires it.

Encoding failures occur before a typed request reaches the domain dispatcher.
They cover invalid UTF-8/JSON, duplicate keys, unsupported versions, unknown or
missing fields, invalid types/values, and wire resource limits. Domain errors
retain the accepted foundation, scene, query, and transaction code names. An
adapter cannot reinterpret a malformed frame as a partial request or reorder
ADR-0004's typed validation precedence.

The Phase 1 headless adapter adds the third source for failures that occur
after domain work succeeds but before an external artifact is committed.
`artifact_exists` is fail-closed and asks the caller to select another
identity; `artifact_io_failure` may be retried after repairing the configured
artifact boundary. Renderer and PNG failures retain their library error codes
and remain domain failures. Cancellation reports the source of the boundary at
which it was observed.

Encoding-error paths are rooted at the complete wire frame. Once decoding has
constructed a typed request, domain-error paths are rooted at that request's
typed query or transaction, so ADR-0004 transaction paths do not acquire
adapter-only `request` or `transaction` prefixes.

### Wire resource ceilings

The reference profile applies these independent pre-domain ceilings:

| Resource | Default | Hard ceiling |
| --- | ---: | ---: |
| frame bytes | 8 MiB | 64 MiB |
| JSON nesting depth | 32 | 32 |
| members per object | 64 | 64 |
| items per generic array | 65,536 | 65,536 |
| bytes per number token | 64 | 64 |

The schema applies smaller semantic maxima where available: 4,096 transaction
operations, 128-byte identifiers, 64-KiB bounded text, three bounds projections,
eight selected field kinds, and the accepted scene/resource hard ceilings.
The frame limit is an additional adapter constraint; satisfying a domain limit
does not bypass it. Production adapters may configure a smaller frame limit but
never exceed 64 MiB under this version.

## Failure-first conformance matrix

| Candidate | Required result |
| --- | --- |
| invalid UTF-8, BOM, NUL, blank, multiple, or truncated frame | encoding failure; no typed request |
| duplicate object key or unknown/missing field | explicit encoding failure with bounded path |
| missing, old, or future protocol token | `unsupported_version` plus accepted version list |
| unknown request, query, operation, result, code, or enum | fail closed; no fallback variant |
| non-finite, fractional integer, overflowed, or overlong number | explicit invalid-value/resource failure |
| uppercase/short color, invalid identity, or unauthorized `null` | explicit invalid value |
| duplicate selection or projection | explicit invalid value; no partial query |
| invalid rectangle radii or path command state | explicit invalid value before domain dispatch |
| frame, depth, member, array, string, or semantic ceiling exceeded | `resource_limit` or `nesting_limit` |
| created/changed overlap or invalid receipt revision | invalid result; never emitted as success |
| every accepted request/result variant decoded and re-encoded | value-for-value identical typed frame |
| repeated generation and verification | byte-identical schema/examples and stable summary digest |

The reference verifier exercises all request/result families, all fifteen
operation tags, both history bodies, maximum `uint64_t` values, injected small
resource limits, LF/CRLF/EOF framing, and malformed cases without allocating at
the 64-MiB ceiling.

## Consequences

- Issue #8 can implement the scene and query types without taking a JSON
  dependency or changing their invariants.
- Issue #9 can implement the dispatcher against the exact typed operation and
  receipt mapping while keeping replay identity independent of JSON spelling.
- Issue #10 can return bounded RGBA8/PNG artifacts without embedding binary
  output in command responses.
- Issue #11 can implement the streaming CLI against a checked provisional
  schema and preserve stdout for structured frames and stderr for diagnostics.
- The Phase 1 evaluation can measure real schema/tool cost before any
  compatibility or scripting-language commitment.
