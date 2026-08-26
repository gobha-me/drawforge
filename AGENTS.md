# AGENTS.md — conventions for AI agents working in this repo

DrawForge is an experimental C++23 semantic graphics library for LLMs and
scripts. Read `DESIGN.md` before changing architecture or public interfaces.
The GitHub roadmap breaks that charter into evidence-producing milestones.

## Current baseline

- CMake 3.28+, C++23, GCC 13+ and Clang 19+.
- Compiled static library plus a small command-line application.
- Catch2 v3 for tests.
- Dependencies use `find_package` first and pinned `FetchContent` fallback,
  entirely through CMake.
- The public package target is `drawforge::lib`.
- The library, executable, tests, and install rules remain independently
  toggleable through the inherited CMake options.

## Product center

The experiment is whether semantic graphics tools help an LLM create and
revise structured artwork more reliably than direct SVG or source authoring.
The terminal workbench is a consumer and debugger, not the product authority.

Do not expand the project into a general terminal paint program unless the
semantic toolchain has passed its first evaluation gate.

## Architecture rules

- Dependency direction is surfaces and adapters -> `libdrawforge` -> domain
  primitives. The document core does not depend on a live terminal.
- AIForge integration belongs in AIForge. Never expose AIForge runtime, event,
  artifact, or policy types through DrawForge's public API.
- TermForge owns terminal protocols and presentation. DrawForge documents and
  commands contain no Kitty, ANSI, cell-grid, image-residency, or escape-sequence
  concepts.
- RasterForge owns untrusted encoded-media decoding and generic validated RGBA
  operations. Adapt its output explicitly at a DrawForge boundary.
- JSON is an adapter encoding, not the in-memory domain model.
- All mutations pass through typed, atomic transactions. Interactive code does
  not receive a privileged mutation path.
- Public fallible operations return `std::expected`; exceptions do not cross
  API boundaries.
- Stable error codes are program logic. Diagnostic messages are bounded text.
- Every object, document, transaction, revision, and artifact identity has an
  explicit invariant. Avoid stringly typed cross-domain identifiers.
- Successful transaction IDs are replay-safe. A retry must not duplicate an
  edit.
- Evaluation and rendering accept explicit time and configuration. Tests must
  not depend on wall-clock timing or terminal state.
- Large or binary results cross integration boundaries as artifacts, not as
  unbounded inline command output.

## Decision discipline

Do not select a renderer, geometry engine, persistence format, embedded script
runtime, plugin ABI, or compatibility policy before the roadmap issue that
defines the evidence needed for the choice.

Prototype against the direct-SVG baseline before adding broad features. Text,
raster brushes, filters, masks, collaboration, and video encoding are deferred
until the semantic core demonstrates value.

## C++ style

Use `PascalCase` types, `snake_case` functions and members, `m_` private data,
trailing return types, and `[[nodiscard]]` for values callers must inspect.
Prefer small value types with validated construction over public aggregates
whose invalid states must be remembered by every caller.

Public headers live under `include/drawforge/`. Implementation belongs under
`src/lib/`; the application belongs under `src/bin/`. Keep domain headers free
of third-party implementation types.

## Testing

Tests are auto-discovered from `test/*/`. A new ordinary test needs only
`test/<name>/test.cpp`; re-run `cmake -B` after adding a directory.

Write the failure matrix first:

- invalid and duplicate IDs;
- stale expected revisions;
- replayed transaction IDs;
- partial transaction failure and rollback;
- bounds, numeric overflow, and resource ceilings;
- malformed command encodings and documents;
- cancellation at operation boundaries;
- deterministic evaluation and rendering; and
- adapter failure without mutation of committed state.

Happy-path tests come last. Pixel goldens prove renderer determinism, while
semantic invariants prove document correctness; neither substitutes for the
other.

## Required verification

```bash
cmake -B build && cmake --build build --parallel \
  && ctest --test-dir build --output-on-failure

cmake -B build-clang -DCMAKE_TOOLCHAIN_FILE=cmake/toolchain/clang.cmake \
  && cmake --build build-clang --parallel \
  && ctest --test-dir build-clang --output-on-failure

example/consumer/verify.sh
CXX=clang++ example/consumer/verify.sh
```

Run `example/public-dep/verify.sh` when library dependency or package-export
wiring changes. Run `cmake -P cmake/check_artifacts.cmake` after bootstrap or
build-structure changes.

## Repository notes

- `include/version.hpp` is generated. Edit `include/version.hpp.in.cmake`.
- Build directories are ignored and never committed.
- The default toolchain respects `CXX`; Clang and sanitizers are opt-in
  toolchain files.
- Tests and applications default to top-level builds only so a consumer gets
  the library without DrawForge's development surfaces.
- Keep executable stdout suitable for structured output once commands arrive;
  diagnostics belong on stderr.

## Attribution

Agent-authored commits carry trailers naming the model, for example:

```text
Co-authored-by: OpenAI Codex <noreply@openai.com>
Agent: Codex / GPT-5
```

Record the verification commands actually run in pull requests.
