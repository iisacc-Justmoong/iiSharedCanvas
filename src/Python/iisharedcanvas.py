"""Python access to an in-memory, tile-backed projected raster stroke.

The native bridge is optional and separate from application binaries.  Pixels
are straight ARGB integers; projected samples are supplied by the caller.
"""

from __future__ import annotations

import ctypes
import ctypes.util
from dataclasses import dataclass
import os
from pathlib import Path
import sys
from typing import Iterable


@dataclass(frozen=True)
class RasterSample:
    x: int
    y: int
    argb: int
    opacity_cap: int = 255


class _NativeSample(ctypes.Structure):
    _fields_ = [("x", ctypes.c_int32), ("y", ctypes.c_int32),
                ("argb", ctypes.c_uint32), ("opacity_cap", ctypes.c_uint8)]


def _default_library() -> str:
    configured = os.environ.get("IISHAREDCANVAS_PYTHON_LIBRARY")
    if configured:
        return configured
    name = ("iiSharedCanvasPython.dll" if sys.platform == "win32" else
            "libiiSharedCanvasPython.dylib" if sys.platform == "darwin" else
            "libiiSharedCanvasPython.so")
    parents = Path(__file__).resolve().parents
    if len(parents) > 3:
        for directory in (parents[3] / "lib", parents[3] / "lib64", parents[3] / "bin"):
            candidate = directory / name
            if candidate.is_file():
                return os.fspath(candidate)
    found = ctypes.util.find_library("iiSharedCanvasPython")
    if found:
        return found
    raise RuntimeError("Cannot find the optional iiSharedCanvas Python bridge")


class ProjectedRaster:
    """An exclusively owned in-memory raster with one-entry stroke history.

    Stable samples accumulate in the active stroke. The temporary sample batch
    replaces the prior tip. Ending the stroke retains the displayed pixels;
    cancelling restores the raster and preserves existing redo history.
    Instances are not thread-safe and must be closed by their owner.
    """

    def __init__(self, width: int, height: int, background: int = 0, *,
                 erase: bool = False, library: str | os.PathLike[str] | None = None):
        if (not isinstance(width, int) or not isinstance(height, int) or
                width <= 0 or height <= 0 or width > 0x7FFFFFFF or height > 0x7FFFFFFF):
            raise ValueError("Raster dimensions must be positive 32-bit integers")
        if not isinstance(background, int) or not 0 <= background <= 0xFFFFFFFF:
            raise ValueError("Background must be a 32-bit ARGB integer")
        self.width, self.height = width, height
        self._handle = None
        self._dll_directory = None
        filename = os.fspath(library) if library is not None else _default_library()
        if sys.platform == "win32" and Path(filename).is_file():
            self._dll_directory = os.add_dll_directory(os.fspath(Path(filename).resolve().parent))
        self._native = ctypes.CDLL(filename)
        pointer = ctypes.c_void_p
        sample_pointer = ctypes.POINTER(_NativeSample)
        self._native.iisc_projected_create.argtypes = [ctypes.c_int32, ctypes.c_int32,
                                                       ctypes.c_uint32, ctypes.c_int]
        self._native.iisc_projected_create.restype = pointer
        self._native.iisc_projected_destroy.argtypes = [pointer]
        self._native.iisc_projected_destroy.restype = None
        self._native.iisc_projected_error.argtypes = [pointer]
        self._native.iisc_projected_error.restype = ctypes.c_char_p
        for name in ("begin", "end", "cancel", "undo", "redo"):
            function = getattr(self._native, "iisc_projected_" + name)
            function.argtypes, function.restype = [pointer], ctypes.c_int
        self._native.iisc_projected_update.argtypes = [pointer, sample_pointer, ctypes.c_size_t,
                                                        sample_pointer, ctypes.c_size_t]
        self._native.iisc_projected_update.restype = ctypes.c_int
        for name in ("pixel", "source_pixel"):
            function = getattr(self._native, "iisc_projected_" + name)
            function.argtypes = [pointer, ctypes.c_int32, ctypes.c_int32,
                                 ctypes.POINTER(ctypes.c_uint32)]
            function.restype = ctypes.c_int
        self._handle = self._native.iisc_projected_create(width, height, background, int(bool(erase)))
        if not self._handle:
            self.close()
            raise MemoryError("Cannot allocate or initialize the raster")

    def _open(self):
        if not self._handle:
            raise RuntimeError("Raster is closed")
        return self._handle

    def _call(self, name: str, *arguments) -> None:
        handle = self._open()
        if not getattr(self._native, "iisc_projected_" + name)(handle, *arguments):
            error = self._native.iisc_projected_error(handle)
            raise RuntimeError(error.decode("utf-8", errors="replace") if error else "Native raster error")

    def _batch(self, samples: Iterable[RasterSample]):
        values = list(samples)
        result = (_NativeSample * len(values))()
        for index, sample in enumerate(values):
            if not isinstance(sample, RasterSample):
                raise TypeError("Each projected sample must be a RasterSample")
            if (not isinstance(sample.x, int) or not isinstance(sample.y, int) or
                    not 0 <= sample.x < self.width or not 0 <= sample.y < self.height or
                    not isinstance(sample.argb, int) or not 0 <= sample.argb <= 0xFFFFFFFF or
                    not isinstance(sample.opacity_cap, int) or not 0 <= sample.opacity_cap <= 255):
                raise ValueError("Projected sample is outside the raster or value range")
            result[index] = _NativeSample(sample.x, sample.y, sample.argb, sample.opacity_cap)
        return result

    def begin_stroke(self) -> None:
        self._call("begin")

    def update_stroke(self, stable: Iterable[RasterSample], transient: Iterable[RasterSample]) -> None:
        self._open()
        fixed, tip = self._batch(stable), self._batch(transient)
        self._call("update", fixed, len(fixed), tip, len(tip))

    def end_stroke(self) -> None:
        self._call("end")

    def cancel_stroke(self) -> None:
        self._call("cancel")

    def undo(self) -> None:
        self._call("undo")

    def redo(self) -> None:
        self._call("redo")

    def _pixel(self, operation: str, x: int, y: int) -> int:
        self._open()
        if not isinstance(x, int) or not isinstance(y, int) or not 0 <= x < self.width or not 0 <= y < self.height:
            raise ValueError("Pixel coordinate is outside the raster")
        result = ctypes.c_uint32()
        self._call(operation, x, y, ctypes.byref(result))
        return result.value

    def pixel(self, x: int, y: int) -> int:
        return self._pixel("pixel", x, y)

    def stroke_source_pixel(self, x: int, y: int) -> int:
        return self._pixel("source_pixel", x, y)

    def close(self) -> None:
        if self._handle:
            self._native.iisc_projected_destroy(self._handle)
            self._handle = None
        if self._dll_directory is not None:
            self._dll_directory.close()
            self._dll_directory = None

    def __enter__(self) -> ProjectedRaster:
        self._open()
        return self

    def __exit__(self, *_exc) -> None:
        self.close()

    def __copy__(self):
        raise TypeError("A raster owns its native state and cannot be copied")

    def __deepcopy__(self, _memo):
        raise TypeError("A raster owns its native state and cannot be copied")

    def __del__(self):
        try:
            self.close()
        except Exception:
            pass
