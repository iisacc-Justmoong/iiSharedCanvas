"""Verify native regression tests preserve pre-existing files in their output parent."""
from pathlib import Path
import shutil
import subprocess
import sys

SENTINELS = {
    "RasterSampling": ["sampling.iisc"],
    "LayerCompositing": ["layer-compositing.iisc"],
    "MaskStack": ["mask-stack.iisc"],
    "BitmapHistory": ["bitmap-history.iisc", "moved-binding/original/preserve.txt"],
    "BitmapPixelPreview": ["finite/preserve.txt", "chunked/preserve.txt"],
}


def main():
    binary, directory, case = sys.argv[1:]
    root = Path(directory)
    if case not in SENTINELS or root.exists() or root.is_symlink():
        raise RuntimeError("test output directory must be new and the case must be known")
    root.mkdir(parents=True)
    owner = root.stat()
    try:
        expected = {}
        for name in SENTINELS[case]:
            path = root / name
            path.parent.mkdir(parents=True, exist_ok=True)
            expected[path] = ("preserve existing " + name).encode()
            path.write_bytes(expected[path])
        result = subprocess.run([binary], capture_output=True, text=True, timeout=850)
        if result.returncode != 0:
            raise RuntimeError("test failed: " + result.stdout + result.stderr)
        for path, contents in expected.items():
            if not path.is_file() or path.read_bytes() != contents:
                raise RuntimeError("test modified a pre-existing output: " + str(path))
        print(case + ": pre-existing files preserved")
    finally:
        if root.is_dir() and not root.is_symlink():
            current = root.stat()
            if (current.st_dev, current.st_ino) == (owner.st_dev, owner.st_ino):
                shutil.rmtree(root)


if __name__ == "__main__":
    main()
