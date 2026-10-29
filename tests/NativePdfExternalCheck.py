"""Check native PDF output with an independent Poppler parser and renderer."""

import pathlib
import re
import subprocess
import sys
import tempfile


def run(*args):
    completed = subprocess.run(args, stdout=subprocess.PIPE, stderr=subprocess.PIPE)
    if completed.returncode:
        raise RuntimeError(f"{args[0]} failed: {completed.stderr.decode('utf-8', errors='replace')}")
    return completed.stdout


def render(pdftoppm, path, size):
    ppm = run(pdftoppm, "-r", "96", "-f", "1", "-l", "1", "-singlefile", str(path))
    header = re.match(rb"P6\s+(?:\#[^\n]*\n\s*)*(\d+)\s+(\d+)\s+(\d+)\s", ppm)
    if not header or tuple(map(int, header.groups())) != (*size, 255):
        raise AssertionError("independent renderer returned an unexpected raster extent")
    pixels = ppm[header.end():]
    if len(pixels) != size[0] * size[1] * 3:
        raise AssertionError("independent renderer returned an unexpected pixel count")
    return pixels


def near(actual, expected):
    return all(abs(channel - value) <= 3 for channel, value in zip(actual, expected))


def main():
    fixture, pdfinfo, pdftoppm, output_root = sys.argv[1:]
    pathlib.Path(output_root).mkdir(parents=True, exist_ok=True)
    with tempfile.TemporaryDirectory(prefix="pdf-independent-", dir=output_root) as directory:
        vector = pathlib.Path(directory) / "vector.pdf"
        raster = pathlib.Path(directory) / "raster.pdf"
        appearance = pathlib.Path(directory) / "appearance.pdf"
        run(fixture, "--vector-fixture", str(vector))
        info = run(pdfinfo, str(vector)).decode("utf-8", errors="replace")
        if not re.search(r"^Pages:\s+2\s*$", info, re.MULTILINE):
            raise AssertionError("independent parser did not find two pages")
        vector_pixels = render(pdftoppm, vector, (8, 6))
        center = (3 * 8 + 3) * 3
        if not near(vector_pixels[center:center + 3], (136, 145, 153)):
            raise AssertionError("PDF vector fill or alpha is incorrect")
        run(fixture, "--appearance-fixture", str(appearance))
        appearance_pixels = render(pdftoppm, appearance, (8, 6))
        if not near(appearance_pixels[center:center + 3], (195, 200, 204)):
            raise AssertionError("rasterized PDF layer appearance is incorrect")
        run(fixture, "--raster-fixture", str(raster))
        pixels = render(pdftoppm, raster, (8, 8))
        for x, y, color in ((2, 1, (255, 127, 127)), (2, 6, (127, 127, 255))):
            offset = (y * 8 + x) * 3
            actual = pixels[offset:offset + 3]
            if not near(actual, color):
                raise AssertionError(f"PDF pixel ({x}, {y}): expected {color}, got {tuple(actual)}")


if __name__ == "__main__":
    main()
