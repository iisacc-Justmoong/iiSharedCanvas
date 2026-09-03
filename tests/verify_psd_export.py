#!/usr/bin/env python3
"""Read-only, optional oracle for the installed consumer's frame-zero PSD fixture.

Usage: python3 -B tests/verify_psd_export.py build/consumer/installed-frame-zero.psd
Requires psd-tools and pypdf; neither is a library/runtime dependency. If needed,
install them explicitly with:
    python3 -m pip install --target build/psd-oracle-packages psd-tools pypdf
Then prefix the invocation with PYTHONPATH=build/psd-oracle-packages.

This is a regression check for tests/consumer/main.cpp, not a general validator
for arbitrary PSD/PDF files or proof of Photoshop UI interoperability. It never
downloads, installs, extracts, renders to disk, or changes the input file.
"""

from __future__ import annotations

import argparse
import io
from pathlib import Path
import sys
import uuid

# Dependency imports must not leave __pycache__ files during this read-only run.
sys.dont_write_bytecode = True

MAX_FIXTURE_BYTES = 16 * 1024 * 1024
EXPECTED_QUAD = (0.0, 0.0, 4.0, 0.0, 4.0, 4.0, 0.0, 4.0)
RED = (255, 0, 0, 255)
GREEN = (0, 255, 0, 255)
TRANSPARENT = (0, 0, 0, 0)


class VerificationError(Exception):
    """A concrete fixture invariant failed."""


def require(condition: bool, message: str) -> None:
    # Keep checks enabled even when Python is invoked with -O.
    if not condition:
        raise VerificationError(message)


def check_pixels(image, outside: tuple[int, int, int, int], label: str) -> None:
    require(image is not None, f"{label}: missing stored pixels")
    require(image.size == (4, 4), f"{label}: expected 4x4 pixels, got {image.size}")
    rgba = image.convert("RGBA")
    for y in range(4):
        for x in range(4):
            expected = GREEN if 1 <= x < 3 and 1 <= y < 3 else outside
            actual = rgba.getpixel((x, y))
            require(actual == expected,
                    f"{label}: pixel ({x}, {y}) is {actual}, expected {expected}")


def check_pdf(payload: bytes, PdfReader) -> int:
    require(payload.startswith(b"%PDF-"), "embedded data is not a PDF")
    require(payload.rstrip().endswith(b"%%EOF"), "embedded PDF has no terminal EOF")
    reader = PdfReader(io.BytesIO(payload), strict=True)
    require(not reader.is_encrypted, "embedded vector PDF must not be encrypted")
    require(len(reader.pages) == 1, "embedded vector PDF must contain one page")
    page = reader.pages[0]
    require(tuple(float(value) for value in page.mediabox) == (0.0, 0.0, 4.0, 4.0),
            f"unexpected PDF page geometry: {page.mediabox}")
    content = page.get_contents()
    require(content is not None, "embedded PDF has no page content")
    operations = content.operations
    operators = {operator for _, operator in operations}
    require(b"INLINE IMAGE" not in operators, "PDF contains an inline raster image")
    require(len(page.images) == 0, "PDF contains raster Image XObjects")
    require(bool(operators & {b"m", b"l", b"c", b"v", b"y", b"re"}),
            "PDF contains no vector path construction operators")
    require(bool(operators & {b"f", b"f*", b"F", b"B", b"B*", b"b", b"b*"}),
            "PDF contains no filled vector paths")
    fill_colors = [list(values) for values, operator in operations
                   if operator in (b"rg", b"sc", b"scn")]
    require([0, 1, 0] in fill_colors, "PDF does not retain the green frame-zero fill")
    require([0, 0, 1] not in fill_colors, "PDF incorrectly contains the later blue fill")
    return len(operations)


def verify(path: Path) -> tuple[int, int]:
    try:
        from psd_tools import PSDImage
        from psd_tools.constants import LinkedLayerType, PlacedLayerType, Tag
        from pypdf import PdfReader
    except ImportError as error:
        raise VerificationError(
            "optional oracle dependencies are unavailable: " + str(error) + ". "
            "Install explicitly with `python3 -m pip install --target "
            "build/psd-oracle-packages psd-tools pypdf`, then run with "
            "`PYTHONPATH=build/psd-oracle-packages python3 -B "
            "tests/verify_psd_export.py PATH.psd`."
        ) from error

    require(path.is_file(), f"fixture is not a regular file: {path}")
    require(path.stat().st_size <= MAX_FIXTURE_BYTES, "fixture exceeds the 16 MiB oracle limit")
    with path.open("rb") as source:
        data = source.read(MAX_FIXTURE_BYTES + 1)
    require(len(data) <= MAX_FIXTURE_BYTES, "fixture grew beyond the oracle limit")
    psd = PSDImage.open(io.BytesIO(data))
    require(psd.size == (4, 4), f"expected 4x4 canvas, got {psd.size}")
    require(psd.depth == 8 and int(psd.color_mode) == 3, "expected 8-bit RGB PSD")
    require(len(psd) == 2, f"expected two layers, got {len(psd)}")
    require([layer.name for layer in psd] == ["Base", "Vector frame zero"],
            "layer names or bottom-to-top order changed")
    base, vector = psd
    require(base.kind == "pixel", "Base must remain a pixel layer")
    require(vector.kind == "smartobject", "vector layer is not a Smart Object")
    for layer in psd:
        require(layer.visible and layer.opacity == 255, f"{layer.name}: unexpected visibility/opacity")
        require(layer.bbox == (0, 0, 4, 4), f"{layer.name}: unexpected layer bounds {layer.bbox}")

    smart = vector.smart_object
    require(smart.kind == "data", "Smart Object must be embedded, not externally linked")
    require(smart.filetype == "pdf", f"unexpected Smart Object file type: {smart.filetype}")
    require(smart.filename.lower().endswith(".pdf"), "embedded source must have a PDF filename")
    identity = smart.unique_id
    require(str(uuid.UUID(identity)) == identity, "Smart Object UUID is not canonical")
    require(Tag(b"SoLd") in vector.tagged_blocks, "missing SoLd metadata")
    require(Tag(b"PlLd") in vector.tagged_blocks, "missing PlLd metadata")
    config = vector.tagged_blocks.get_data(Tag(b"SoLd"))
    placed = vector.tagged_blocks.get_data(Tag(b"PlLd"))
    require(config.kind == b"soLD" and config.version == 4, "unexpected SoLd signature/version")
    require(config.data.version == 16, "unexpected SoLd descriptor version")
    require(config.data[b"Idnt"].value.rstrip("\0") == identity, "SoLd UUID does not match")
    require(placed.uuid.decode("ascii").rstrip("\0") == identity, "PlLd UUID does not match")
    require(placed.layer_type == PlacedLayerType.VECTOR, "PlLd source is not marked vector")
    require(config.data[b"Type"].value == int(PlacedLayerType.VECTOR), "SoLd source is not marked vector")
    require(tuple(placed.transform) == EXPECTED_QUAD, "PlLd transform quad changed")
    require(tuple(value.value for value in config.data[b"Trnf"]) == EXPECTED_QUAD,
            "SoLd transform quad changed")
    require(tuple(value.value for value in config.data[b"nonAffineTransform"]) == EXPECTED_QUAD,
            "SoLd non-affine transform quad changed")
    require(config.data[b"frameCount"].value == 1, "Smart Object is not static frame zero")
    require(config.data[b"Crop"].value == 3, "PDF placement must preserve the entire MediaBox")
    require(config.data[b"PgNm"].value == 1 and config.data[b"totalPages"].value == 1,
            "Smart Object page selection changed")

    require(Tag.LINKED_LAYER2 in psd.tagged_blocks, "missing global lnk2 embedded data")
    linked = psd.tagged_blocks.get_data(Tag.LINKED_LAYER2)
    require(len(linked) == 1, "expected exactly one embedded source")
    require(linked[0].kind == LinkedLayerType.DATA, "lnk2 does not contain embedded data")
    require(linked[0].uuid == identity, "lnk2 UUID does not match SoLd/PlLd")
    require(linked[0].filetype == b"PDF ", "lnk2 file type is not PDF")
    require(Tag.LINKED_LAYER_EXTERNAL not in psd.tagged_blocks,
            "fixture unexpectedly contains external Smart Object links")
    payload = smart.data
    require(payload == linked[0].data and len(payload) == smart.filesize,
            "Smart Object payload does not match its linked data")
    with smart.open() as embedded:
        require(embedded.read() == payload, "independent Smart Object open returned different data")

    base_image = base.topil()
    require(base_image is not None and base_image.size == (4, 4), "Base has no 4x4 stored raster")
    base_rgba = base_image.convert("RGBA")
    require(all(base_rgba.getpixel((x, y)) == RED for y in range(4) for x in range(4)),
            "Base pixels changed")
    check_pixels(vector.topil(), TRANSPARENT, "vector cache")
    # topil reads the file's merged preview; composite() could regenerate it and
    # accidentally hide an incorrect/missing preview written by the exporter.
    check_pixels(psd.topil(), RED, "merged preview")
    return len(payload), check_pdf(payload, PdfReader)


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    parser.add_argument("psd", type=Path, help="installed consumer's installed-frame-zero.psd")
    args = parser.parse_args()
    try:
        payload_size, operation_count = verify(args.psd)
    except Exception as error:
        print(f"PSD export verification failed: {error}", file=sys.stderr)
        return 1
    print(f"Verified {args.psd}: two ordered layers, embedded vector PDF "
          f"({payload_size} bytes, {operation_count} operators), exact frame-zero caches/preview")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
