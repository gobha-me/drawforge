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

DrawForge is at the project-foundation stage. The document API, command schema,
renderer, and scripting surface do not exist yet and must not be inferred from
the bootstrap API. See [the design](DESIGN.md) and the
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

## Current bootstrap API

The only current public API reports project metadata. It exists to prove the
compiled-library, executable, test, install, and downstream-consumer paths.
It is not the first version of the graphics API.

```cpp
#include <drawforge/drawforge.hpp>

const auto info = drawforge::project_info();
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

The bootstrap executable is intentionally truthful about the current state:

```bash
./build/src/bin/drawforge --help
./build/src/bin/drawforge --version
```

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
fallback. The bootstrap library has no runtime dependency; Catch2 is used only
for tests. Rendering, geometry, persistence, and script-runtime dependencies
will be selected only through roadmap decisions backed by prototypes.

## Continuous integration

CI builds and tests GCC and Clang across the default, AddressSanitizer,
ThreadSanitizer, and UndefinedBehaviorSanitizer toolchains. It also exercises
the library-disabled install, downstream consumption, package export, and
version parsing paths inherited from `cpp-template`.

## License

DrawForge is available under the BSD 3-Clause License; see [LICENSE.md](LICENSE.md).
