# DrawForge

[![CI](https://github.com/gobha-me/drawforge/actions/workflows/ci.yml/badge.svg)](https://github.com/gobha-me/drawforge/actions/workflows/ci.yml)

DrawForge is an experimental C++23 semantic graphics library for LLMs,
scripts, and other tool-driven clients. The experiment asks whether a bounded,
inspectable document model with typed operations can help an LLM create and
revise structured artwork more reliably than writing SVG or graphics source
directly.

The intended library owns document semantics, transactions, deterministic
evaluation, and rendering. A small command-line surface will expose the same
operations for scripts. A future TermForge application will act as a human
workbench and visual debugger rather than as the library's reason to exist.

DrawForge now has its first immutable scene, query, transaction, deterministic
preview, and headless JSONL APIs. The command surface is provisional and
versioned for the Phase 1 experiment; it is not yet a compatibility promise.
See [the design](DESIGN.md) and the
[GitHub roadmap](https://github.com/gobha-me/drawforge/issues).

## Experiment

The primary comparison is direct SVG authoring. DrawForge should earn its
complexity through capabilities such as:

- stable object identities across revisions;
- atomic, retry-safe transactions;
- semantic operations such as grouping, alignment, and animation;
- bounded structural inspection instead of whole-file rereads;
- deterministic previews and structured failures; and
- reviewable diffs that distinguish intended from incidental changes.

The project should be reconsidered if an evaluation corpus shows no meaningful
reliability, efficiency, safety, or revision-quality advantage over direct SVG.

The versioned corpus, direct-SVG baseline, repeatability controls, and gate
decision are documented in [evaluation/](evaluation/). The baseline is
provider-neutral and runs before the semantic document API exists.

The renderer and geometry evidence spike and its provisional Phase 1 boundary
decision are documented in [ADR-0001](docs/decisions/0001-renderer-geometry-boundary.md).
The invariant-bearing identities, numeric values, and resource ceilings are
documented in [ADR-0002](docs/decisions/0002-foundation-value-contract.md).
The accepted minimal scene hierarchy, opacity animation, semantic bounds, and
bounded-query behavior are documented in
[ADR-0003](docs/decisions/0003-minimal-scene-query-contract.md).
The atomic transaction, replay, receipt, structured-error, cancellation, and
undo/redo contract is documented in
[ADR-0004](docs/decisions/0004-atomic-transaction-contract.md), with an
executable reference model under [spike/transaction/](spike/transaction/).
The strict provisional JSONL framing, versioned query/transaction schema, and
provider-neutral result encoding are documented in
[ADR-0005](docs/decisions/0005-versioned-json-encoding.md), with generated
machine artifacts under [schema/experimental/v1/](schema/experimental/v1/) and
executable conformance evidence under [spike/encoding/](spike/encoding/).
The fixed Phase 1 RGBA8 and PNG preview contract, renderer-version metadata,
cancellation boundaries, and semantic dirty-bounds relationship are documented
in [ADR-0006](docs/decisions/0006-deterministic-preview-rendering.md).
The production JSONL command, bounded decoder, artifact-write boundary, and
process exit behavior are documented in
[ADR-0007](docs/decisions/0007-headless-jsonl-cli.md).

## Architectural boundary

```text
                         +----------------------+
                         |    libdrawforge      |
                         | document + commands  |
                         | evaluate + render    |
                         +----------+-----------+
                                    |
               +--------------------+--------------------+
               |                    |                    |
        drawforge CLI        TermForge workbench   external adapters
        JSON transactions     inspect and preview  AIForge / MCP / other
```

The core library does not depend on AIForge, TermForge, a live terminal, or a
provider SDK. An AIForge integration belongs at AIForge's adapter boundary and
maps its tool, policy, event, and artifact contracts onto the stable DrawForge
API.

## Current public API

The public API reports project metadata, provides invariant-bearing scene
values, exposes immutable documents through bounded typed queries, and routes
all changes through one atomic, retry-safe transaction dispatcher. Dispatcher
snapshots remain immutable values, so queries never observe partially staged
state and consumers cannot bypass revision, replay, or history semantics.

```cpp
#include <drawforge/drawforge.hpp>

const auto info = drawforge::project_info();
const auto document = drawforge::DocumentId::create("scene");
const auto extent = drawforge::CanvasExtent::create(640, 480);

if (document && extent) {
  const auto scene = drawforge::Document::create(*document, *extent);
  if (scene) {
    auto dispatcher = drawforge::TransactionDispatcher::create(*scene);
    const auto layer = drawforge::LayerId::create("artwork");
    const auto transaction = drawforge::TransactionId::create("create-v1");
    if (dispatcher && layer && transaction) {
      const auto applied = dispatcher->apply(drawforge::Transaction{
          *document, drawforge::Revision{}, *transaction,
          drawforge::OperationBatch{{
              drawforge::CreateLayer{*layer, 0, true}}}});
      const auto summary = drawforge::inspect(
          dispatcher->snapshot(), drawforge::SummaryQuery{});
      const auto config = drawforge::RenderConfig::create(0, 64 * 1024 * 1024);
      if (config) {
        const auto rgba =
            drawforge::render_rgba(dispatcher->snapshot(), *config);
        if (rgba) {
          const auto png = drawforge::encode_png(*rgba);
        }
      }
    }
  }
}
```

## Build and test

Requirements:

- CMake 3.28 or newer
- GCC 13+ or Clang 19+
- a C++23 standard library

```bash
cmake -B build
cmake --build build --parallel
ctest --test-dir build --output-on-failure

cmake -B build-clang -DCMAKE_TOOLCHAIN_FILE=cmake/toolchain/clang.cmake
cmake --build build-clang --parallel
ctest --test-dir build-clang --output-on-failure
```

The executable reports project metadata and exposes the experimental JSONL
adapter:

```bash
./build/src/bin/drawforge --help
./build/src/bin/drawforge --version
mkdir -p artifacts
./build/src/bin/drawforge jsonl --artifact-dir artifacts \
  < schema/experimental/v1/examples.jsonl
```

The command reads one request object per line and emits one response per line.
It keeps one document in memory for the process lifetime. Render requests write
`<artifact_id>.rgba8` or `<artifact_id>.png` into the already-existing artifact
directory and never overwrite an existing file. Exit statuses are 0 for
success, 2 for invocation errors, 3 for encoding errors, 4 for domain errors, 5
for artifact-adapter errors, and 130 for interruption. Ordinary frame errors do
not stop later frames.

## Consume the library

All acquisition modes expose the same target, `drawforge::lib`:

```cmake
# Vendored or submoduled
add_subdirectory(third_party/drawforge)

# Installed package
find_package(drawforge CONFIG REQUIRED)

target_link_libraries(my_application PRIVATE drawforge::lib)
```

For `FetchContent`, pin `SOURCE_DIR` to a directory named `drawforge`; this
project derives its package and target prefix from its checkout directory.
The downstream harness in `example/consumer/verify.sh` checks vendored,
fetched, and installed consumption.

## Dependency policy

Dependencies use `find_package` first and a pinned CMake `FetchContent`
fallback. The compiled library privately links the exact PlutoVG 1.3.3 Phase 1
renderer selected by ADR-0001; no PlutoVG type crosses the public API. The
headless executable privately uses exact yyjson 0.12.0 for its bounded adapter;
library-only consumers do not acquire it. Catch2 is used only for tests.
Persistence and script-runtime dependencies remain deferred to roadmap
decisions backed by evidence.

## Continuous integration

CI builds and tests GCC and Clang across the default, AddressSanitizer,
ThreadSanitizer, and UndefinedBehaviorSanitizer toolchains. It also exercises
the library-disabled install, downstream consumption, package export, and
version parsing paths inherited from `cpp-template`.

## License

DrawForge is available under the BSD 3-Clause License; see [LICENSE.md](LICENSE.md).
