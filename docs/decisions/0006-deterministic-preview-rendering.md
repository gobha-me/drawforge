# ADR-0006: Deterministic Phase 1 preview rendering

Status: accepted for the Phase 1 prototype

Date: 2026-08-28

Issue: [#10](https://github.com/gobha-me/drawforge/issues/10)

## Context

The immutable scene and transaction dispatcher now provide the accepted
rectangle, ellipse, line-path, transform, style, visibility, ordering, and
opacity-animation semantics. The headless CLI and evaluation gate need a
bounded preview projection without making a renderer scene graph authoritative
or adding filesystem effects to the library.

ADR-0001 selected a private direct-mode PlutoVG 1.3.3 adapter provisionally.
Its spike proved repeatable software output, geometry coverage, and in-memory
PNG encoding. Production use still had to define canonical pixels, allocation
and cancellation boundaries, package export behavior, and the relationship
between raster changes and semantic transaction bounds.

## Decision

`render_rgba` evaluates an immutable `Document` at the explicit time and byte
ceiling in a validated `RenderConfig`. Phase 1 maps one canvas unit to one
output pixel. The only accepted pixel format is tightly packed, row-major,
top-to-bottom sRGB RGBA8 with straight alpha; fully transparent pixels have
zero RGB. `RendererInfo` records render-contract version 1 and the exact
PlutoVG 1.3.3 implementation version used to produce the bytes.

Rendering clears to transparent, applies the optional document background,
and traverses layers and children in document order. It composes group and
drawable transforms, evaluates the frozen linear opacity track at the request
time, and applies visibility before drawing. Paint uses source-over, nonzero
fill, fill before stroke, butt caps, miter joins with limit four, and the
renderer version's fixed software antialiasing. The canvas is the only clip.

`RgbaImage` owns canonical pixels and private renderer evidence needed by the
separate `encode_png` stage. `PngImage` owns bounded encoded bytes. Neither
stage writes files or accepts an artifact sink. This avoids a filesystem API,
keeps PNG encoding optional, and lets the encoder contain PlutoVG's void-return
stream callback before returning a structured failure.

Both stages return `std::expected` with stable render errors. They check the
document and per-call byte ceilings before allocation, contain C++ exceptions,
and poll the shared provider-neutral `CancellationToken` at deterministic
scene, path, row-conversion, and encoder-stream boundaries. Cancellation or
failure returns no partial image.

Transaction receipt bounds remain semantic document-space values from
ADR-0004. Renderer pixel extents do not create a second public bounds contract.
Conformance instead renders before and after a transaction and proves that all
changed pixels lie inside the outward-rounded, canvas-clipped receipt bounds.

PlutoVG remains private to `libdrawforge`, but a static library retains it as a
link-only package dependency. CMake therefore prefers an exact installed
PlutoVG 1.3.3 package or fetches the accepted commit. A top-level DrawForge
install includes the dependency's export; embedded/non-installing builds add
it as an excluded subdirectory so its unconditional install rules cannot leak
into a consumer's prefix.

## Consequences

- Identical scene, time, configuration, DrawForge contract, and renderer
  version produce byte-identical RGBA and PNG outputs covered by goldens.
- PlutoVG types, surfaces, paths, and callbacks never appear in public headers.
- Issue #11 can expose rendering and artifact writes through the CLI without
  inventing alternate evaluation, cancellation, or mutation behavior.
- Output scaling, alternate formats/renderers, partial rendering, raster
  bounds, color management, filesystem output, and renderer compatibility are
  deferred until evaluation evidence requires them.
