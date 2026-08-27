# ADR-0001: Provisional renderer and geometry boundary

Status: accepted for the Phase 1 prototype

Date: 2026-08-27

Issue: [#3](https://github.com/gobha-me/drawforge/issues/3)

## Context

DrawForge needs deterministic headless rendering for the v1 evaluation corpus,
but its document, identities, transactions, and public value types must not
become an implementation library's scene graph. The first slice requires
rounded rectangles, ellipses, filled and stroked line paths, affine transforms,
opacity, bounds, hit testing, caller-bounded RGBA, and PNG previews.

The checked-in standalone spike renders that slice through three pinned
software implementations. Every result below was repeated with GCC 14 and
Clang 20; each candidate produced identical RGBA and PNG bytes across 25 runs
and both compilers, and decoding each PNG reproduced the candidate's RGBA bytes.

## Evidence

| Candidate | Geometry evidence | Build and ownership evidence | GCC static spike |
| --- | --- | --- | ---: |
| [Blend2D 0.21.2](https://blend2d.com/download.html) | exact path bounds and fill hit testing; no public stroke hit test | native CMake, Zlib license, bundled AsmJit, built-in PNG | 3,036,512 bytes |
| [PlutoVG 1.3.3](https://github.com/sammycage/plutovg/releases/tag/v1.3.3) | fill/stroke containment and device-pixel extents, including transformed dirty bounds | native CMake package, MIT license, no mandatory external runtime dependency, built-in PNG | 429,832 bytes |
| [Cairo 1.18.4](https://www.cairographics.org/news/cairo-1.18.4/) | fill/stroke containment and extents; rotated extents are conservative in user space | mature C API and PNG support, but Meson plus system pixman/libpng/zlib does not satisfy DrawForge's pinned CMake fallback policy | 2,183,304 bytes |

The corpus-shaped scene intentionally produces different pixels between
renderers; cross-renderer pixel equality is not a selection criterion. The
candidate-specific SHA-256 values and machine metadata are reproduced by
`python3 spike/renderer/verify.py` and retained as CI artifacts rather than
committed binaries.

ThorVG was screened out because its retained scene graph overlaps DrawForge's
authority and its supported native build is Meson. Skia was screened out before
prototype work because its GN build, dependency breadth, and optional GPU stack
are disproportionate to the first corpus. A new rasterizer was rejected because
it would test DrawForge's ability to write a rasterizer rather than the semantic
tool hypothesis.

## Decision

Use a private, direct-mode software-renderer adapter and carry PlutoVG 1.3.3 as
the provisional implementation candidate for Phase 1. PlutoVG supplied every
required render and hit-test operation, the smallest static prototype, a tagged
source release, and the closest fit to the existing CMake package policy.

The ownership boundary is:

- DrawForge owns validated document and path values, transforms, resource
  ceilings, cancellation boundaries, explicit evaluation time, stable errors,
  and the semantic meaning of bounds and hit queries.
- The private adapter converts those values into short-lived PlutoVG paths and
  canvases, performs software rasterization and geometry probes, and copies the
  result into a caller-bounded DrawForge pixel buffer. No PlutoVG type appears
  in a public header or stored document.
- Renderer raster-span extents may supply conservative dirty bounds. Exact
  semantic object bounds remain a DrawForge contract and may require checked
  analytic bounds where pixel-rounded extents are insufficient.
- PNG encoding remains a separate fallible output adapter over committed RGBA.
  PlutoVG's encoder is useful evidence, but its callback cannot report a sink
  write failure, so it is not the public artifact boundary.

This ADR selects a prototype boundary and candidate, not a compatibility
promise or production dependency. Issue #10 must re-prove package export,
consumer installation, allocation ceilings, cancellation, and exact pixel/alpha
semantics before adding PlutoVG to `libdrawforge`.

## Consequences

- Issue #4 must select domain numeric limits that can be checked before adapting
  to PlutoVG's `float` coordinates; non-finite or unrepresentable values never
  reach the renderer.
- Issue #5 defines exact semantic bounds independently of raster dirty bounds.
- If the production package/consumer probe or numeric contract fails, Blend2D
  is the recorded fallback candidate; Cairo does not become the fallback unless
  the repository's build policy is explicitly reconsidered.
- The normal library, CLI, install, and consumer builds remain dependency-free
  after this decision.
