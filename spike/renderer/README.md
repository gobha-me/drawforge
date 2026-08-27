# Renderer and geometry boundary spike

This standalone project supplies the reproducible evidence for roadmap issue
[#3](https://github.com/gobha-me/drawforge/issues/3). It is deliberately not a
subdirectory of the production DrawForge build, is never installed, and does
not define a public graphics API.

The harness renders one corpus-shaped scene through Blend2D 0.21.2, PlutoVG
1.3.3, and Cairo 1.18.4. It records geometry-query capabilities, runs the
failure matrix, writes straight RGBA plus PNG artifacts, and proves repeated
and cross-compiler determinism. Cairo is built from its signed release source
with a pinned Meson version so the comparison does not silently use the host's
package version.

Run all candidates and both supported compilers serially:

```bash
python3 spike/renderer/verify.py
```

For a focused iteration:

```bash
python3 spike/renderer/verify.py --backend plutovg --compiler g++ --repeat 2
```

All downloads, builds, generated images, and machine summaries live under
root-level `build-spike-*` directories and remain untracked.
