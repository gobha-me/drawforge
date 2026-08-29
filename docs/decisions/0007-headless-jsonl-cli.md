# ADR-0007: Headless JSONL command and artifact adapter

Status: accepted for the Phase 1 prototype

Date: 2026-08-29

Issue: [#11](https://github.com/gobha-me/drawforge/issues/11)

## Context

ADR-0005 defines the strict provisional JSON encoding and ADR-0006 defines
deterministic in-memory preview bytes. The evaluation harness and scripts need
one production path through those contracts without acquiring a privileged
mutation API or making the filesystem part of `libdrawforge`.

The adapter also needs bounded streaming behavior. A malformed request must not
poison later independent frames, while cancellation and output failure must stop
work predictably. Rendered bytes are too large for inline JSON and existing
artifact identities must not be silently replaced.

## Decision

The top-level executable exposes:

```text
drawforge jsonl --artifact-dir DIRECTORY
```

`DIRECTORY` must already exist. The process owns at most one in-memory document
and dispatcher. It reads one UTF-8 JSON object per line, processes frames in
order, and writes exactly one compact JSON response per input frame. Blank,
malformed, unsupported, and semantically invalid frames produce structured
errors and processing continues. Transaction behavior, including dry runs,
atomic rollback, replay, revision checks, and cancellation, is delegated to the
same public library API used by other consumers.

The command uses yyjson 0.12.0 as a private executable dependency. The decoder
rejects duplicate members and enforces fixed limits on frame bytes, nesting,
object members, array items, and numeric token length before constructing typed
domain values. JSON remains an adapter representation and no yyjson type crosses
the CLI-support or public-library boundary.

Render responses contain metadata and a SHA-256 digest but not image bytes. The
adapter writes `<artifact_id>.rgba8` or `<artifact_id>.png` beneath the explicit
artifact directory. The extension follows the requested format. Files are
created exclusively, never overwritten, and a handled cancellation or write
failure removes only the partial file created by that request. Artifact write
errors have `source: "adapter"`; the v1 protocol adds stable
`artifact_exists` and `artifact_io_failure` codes for this boundary.

`SIGINT` is converted into the provider-neutral cancellation token and returns
exit status 130 after the interrupted response is flushed. Invocation failures
return 2, encoding failures 3, domain failures 4, and adapter failures 5.
Successful input returns 0. When continued processing observes multiple error
categories, the highest category determines the process exit status. A stdout
write or flush failure stops processing because the one-response-per-frame
contract can no longer be maintained.

## Consequences

- Scripts and the evaluation harness exercise the same typed transactions,
  queries, evaluation, and rendering as C++ consumers.
- Ordinary request failures are independently diagnosable without restarting a
  long stream, while cancellation and broken output terminate promptly.
- Render bytes remain bounded artifacts rather than unbounded inline protocol
  content, and retries cannot replace prior evidence accidentally.
- Filesystem layout beyond the explicit directory and deterministic extension,
  multi-document sessions, persistence, sockets, embedded scripting, and a
  compatibility promise remain deferred.
