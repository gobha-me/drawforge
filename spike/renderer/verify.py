#!/usr/bin/env python3
"""Build and repeat the isolated renderer spike with bounded concurrency."""

from __future__ import annotations

import argparse
import hashlib
import json
import os
from pathlib import Path
import shutil
import struct
import subprocess
import sys
import tarfile
import time
import urllib.request
import zlib


ROOT = Path(__file__).resolve().parents[2]
SPIKE = Path(__file__).resolve().parent
CAIRO_URL = "https://cairographics.org/releases/cairo-1.18.4.tar.xz"
CAIRO_SHA256 = "445ed8208a6e4823de1226a74ca319d3600e83f6369f99b14265006599c32ccb"
BACKENDS = ("blend2d", "plutovg", "cairo")


def run(
    command: list[str], *, env: dict[str, str] | None = None, announce: bool = True
) -> subprocess.CompletedProcess[str]:
    if announce:
        print("+", " ".join(command), flush=True)
    completed = subprocess.run(command, text=True, capture_output=True, env=env)
    if completed.returncode != 0:
        if completed.stdout:
            print(completed.stdout, file=sys.stderr)
        if completed.stderr:
            print(completed.stderr, file=sys.stderr)
        completed.check_returncode()
    return completed


def digest(path: Path) -> str:
    return hashlib.sha256(path.read_bytes()).hexdigest()


def compiler_slug(compiler: str) -> str:
    return "clang" if "clang" in Path(compiler).name else "gcc"


def c_compiler(cxx: str) -> str:
    name = Path(cxx).name
    candidate = "clang" if "clang" in name else "gcc"
    resolved = shutil.which(candidate)
    if resolved is None:
        raise RuntimeError(f"required C compiler {candidate} was not found")
    return resolved


def safe_extract(archive: Path, destination: Path) -> None:
    with tarfile.open(archive) as bundle:
        bundle.extractall(destination, filter="data")


def prepare_cairo(cxx: str) -> Path:
    slug = compiler_slug(cxx)
    base = ROOT / f"build-spike-cairo-deps-{slug}"
    archive = base / "cairo-1.18.4.tar.xz"
    source = base / "cairo-1.18.4"
    build = base / "build"
    prefix = base / "prefix"
    venv = base / "venv"
    meson = venv / "bin" / "meson"
    ninja = venv / "bin" / "ninja"
    base.mkdir(parents=True, exist_ok=True)

    if not archive.exists() or digest(archive) != CAIRO_SHA256:
        urllib.request.urlretrieve(CAIRO_URL, archive)
    if digest(archive) != CAIRO_SHA256:
        raise RuntimeError("Cairo archive SHA-256 mismatch")
    if not source.exists():
        safe_extract(archive, base)
    if not meson.exists():
        run([sys.executable, "-m", "venv", str(venv)])
    if not meson.exists() or not ninja.exists():
        run([str(venv / "bin" / "pip"), "install", "meson==1.7.2", "ninja==1.11.1.4"])

    environment = os.environ.copy()
    environment["CC"] = c_compiler(cxx)
    environment["PATH"] = str(venv / "bin") + os.pathsep + environment["PATH"]
    if not (build / "build.ninja").exists():
        run(
            [
                str(meson),
                "setup",
                str(build),
                str(source),
                f"--prefix={prefix}",
                "--default-library=static",
                "--wrap-mode=nofallback",
                "-Dtests=disabled",
                "-Ddwrite=disabled",
                "-Dfontconfig=disabled",
                "-Dfreetype=disabled",
                "-Dglib=disabled",
                "-Dlzo=disabled",
                "-Dpng=enabled",
                "-Dquartz=disabled",
                "-Dspectre=disabled",
                "-Dsymbol-lookup=disabled",
                "-Dtee=disabled",
                "-Dxcb=disabled",
                "-Dxlib=disabled",
                "-Dxlib-xcb=disabled",
                "-Dzlib=enabled",
            ],
            env=environment,
        )
    run([str(meson), "compile", "-C", str(build), "-j", "2"], env=environment)
    run([str(meson), "install", "-C", str(build)], env=environment)

    pkgconfig = list(prefix.glob("lib*/**/pkgconfig"))
    if not pkgconfig:
        raise RuntimeError("Cairo install did not produce a pkg-config directory")
    return pkgconfig[0]


def paeth(left: int, above: int, upper_left: int) -> int:
    estimate = left + above - upper_left
    distances = (abs(estimate - left), abs(estimate - above), abs(estimate - upper_left))
    return (left, above, upper_left)[distances.index(min(distances))]


def decode_png(path: Path) -> bytes:
    data = path.read_bytes()
    if data[:8] != b"\x89PNG\r\n\x1a\n":
        raise RuntimeError("invalid PNG signature")
    offset = 8
    compressed = bytearray()
    width = height = color_type = bit_depth = interlace = None
    while offset < len(data):
        length = struct.unpack(">I", data[offset : offset + 4])[0]
        kind = data[offset + 4 : offset + 8]
        payload = data[offset + 8 : offset + 8 + length]
        offset += 12 + length
        if kind == b"IHDR":
            width, height, bit_depth, color_type, _, _, interlace = struct.unpack(">IIBBBBB", payload)
        elif kind == b"IDAT":
            compressed.extend(payload)
        elif kind == b"IEND":
            break
    if bit_depth != 8 or color_type not in (2, 6) or interlace != 0:
        raise RuntimeError(f"unsupported spike PNG layout: depth={bit_depth} type={color_type}")
    channels = 4 if color_type == 6 else 3
    stride = width * channels
    raw = zlib.decompress(bytes(compressed))
    previous = bytearray(stride)
    rgba = bytearray()
    cursor = 0
    for _ in range(height):
        filter_type = raw[cursor]
        cursor += 1
        row = bytearray(raw[cursor : cursor + stride])
        cursor += stride
        for index, value in enumerate(row):
            left = row[index - channels] if index >= channels else 0
            above = previous[index]
            upper_left = previous[index - channels] if index >= channels else 0
            if filter_type == 1:
                row[index] = (value + left) & 0xFF
            elif filter_type == 2:
                row[index] = (value + above) & 0xFF
            elif filter_type == 3:
                row[index] = (value + ((left + above) // 2)) & 0xFF
            elif filter_type == 4:
                row[index] = (value + paeth(left, above, upper_left)) & 0xFF
            elif filter_type != 0:
                raise RuntimeError(f"unsupported PNG filter {filter_type}")
        if channels == 4:
            rgba.extend(row)
        else:
            for index in range(0, len(row), 3):
                rgba.extend(row[index : index + 3])
                rgba.append(255)
        previous = row
    return bytes(rgba)


def verify_backend(backend: str, cxx: str, repeats: int) -> dict[str, object]:
    slug = compiler_slug(cxx)
    build = ROOT / f"build-spike-{backend}-{slug}"
    environment = os.environ.copy()
    environment["CXX"] = cxx
    cmake = [
        "cmake",
        "-S",
        str(SPIKE),
        "-B",
        str(build),
        "-DCMAKE_BUILD_TYPE=Release",
        f"-DCMAKE_CXX_COMPILER={cxx}",
        f"-DCMAKE_C_COMPILER={c_compiler(cxx)}",
        f"-DDRAWFORGE_SPIKE_BACKEND={backend}",
    ]
    if backend == "cairo":
        pkgconfig = prepare_cairo(cxx)
        environment["PKG_CONFIG_PATH"] = str(pkgconfig)

    started = time.monotonic()
    run(cmake, env=environment)
    run(["cmake", "--build", str(build), "--parallel", "2"], env=environment)
    build_seconds = time.monotonic() - started

    executable = build / "drawforge-renderer-spike"
    rgba_hashes: set[str] = set()
    png_hashes: set[str] = set()
    metadata = None
    first_output = None
    for iteration in range(repeats):
        output = build / "runs" / str(iteration)
        completed = run([str(executable), str(output)], env=environment, announce=False)
        current = json.loads(completed.stdout)
        if metadata is None:
            metadata = current
            first_output = output
        elif current != metadata:
            raise RuntimeError(f"{backend}/{slug} metadata changed between repeated runs")
        rgba_hashes.add(digest(output / "scene.rgba"))
        png_hashes.add(digest(output / "scene.png"))
        if decode_png(output / "scene.png") != (output / "scene.rgba").read_bytes():
            raise RuntimeError(f"{backend}/{slug} PNG pixels differ from RGBA output")
    if len(rgba_hashes) != 1 or len(png_hashes) != 1:
        raise RuntimeError(f"{backend}/{slug} output was not deterministic")
    assert metadata is not None and first_output is not None
    if not metadata["fill_hit"] or not metadata["fill_miss"]:
        raise RuntimeError(f"{backend}/{slug} fill hit probes failed")
    if metadata["native_stroke_hit_test"] and not metadata["stroke_hit"]:
        raise RuntimeError(f"{backend}/{slug} stroke hit probe failed")
    return {
        "backend": backend,
        "compiler": slug,
        "compiler_path": cxx,
        "repeat_count": repeats,
        "rgba_sha256": next(iter(rgba_hashes)),
        "png_sha256": next(iter(png_hashes)),
        "executable_bytes": executable.stat().st_size,
        "build_seconds": round(build_seconds, 3),
        "metadata": metadata,
        "artifact_directory": str(first_output.relative_to(ROOT)),
    }


def main() -> int:
    parser = argparse.ArgumentParser()
    parser.add_argument("--backend", choices=(*BACKENDS, "all"), default="all")
    parser.add_argument("--compiler", action="append", default=[])
    parser.add_argument("--repeat", type=int, default=25)
    parser.add_argument("--summary", type=Path, default=ROOT / "build-spike-results/summary.json")
    arguments = parser.parse_args()
    if arguments.repeat < 2 or arguments.repeat > 100:
        parser.error("--repeat must be between 2 and 100")

    backends = BACKENDS if arguments.backend == "all" else (arguments.backend,)
    compilers = arguments.compiler or ["g++", "clang++"]
    results = [verify_backend(backend, compiler, arguments.repeat)
               for backend in backends for compiler in compilers]

    for backend in backends:
        compiler_results = [result for result in results if result["backend"] == backend]
        if len(compiler_results) > 1:
            rgba = {result["rgba_sha256"] for result in compiler_results}
            png = {result["png_sha256"] for result in compiler_results}
            if len(rgba) != 1 or len(png) != 1:
                raise RuntimeError(f"{backend} differs across compilers")

    arguments.summary.parent.mkdir(parents=True, exist_ok=True)
    arguments.summary.write_text(json.dumps({"schema": 1, "results": results}, indent=2) + "\n")
    print(json.dumps({"status": "ok", "summary": str(arguments.summary), "runs": len(results)}))
    return 0


if __name__ == "__main__":
    try:
        raise SystemExit(main())
    except (OSError, RuntimeError, subprocess.CalledProcessError) as error:
        print(f"renderer spike verification failed: {error}", file=sys.stderr)
        raise SystemExit(1)
