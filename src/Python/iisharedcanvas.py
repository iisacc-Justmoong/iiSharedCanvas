"""Python access to an in-memory, tile-backed projected raster stroke.

The native bridge is optional and separate from application binaries.  Pixels
are straight ARGB integers; projected samples are supplied by the caller.
"""

from __future__ import annotations

import ctypes
import ctypes.util
from dataclasses import dataclass
from enum import IntFlag
import json
import math
import os
from pathlib import Path
import struct
import sys
from typing import Iterable


class _GuiInput(ctypes.Structure):
    _fields_ = [("phase", ctypes.c_int), ("device", ctypes.c_int),
                *[(name, ctypes.c_double) for name in
                  ("x", "y", "pressure", "tilt_x", "tilt_y", "rotation", "timestamp")],
                ("device_id", ctypes.c_uint64), ("buttons", ctypes.c_uint32),
                ("scroll_x", ctypes.c_double), ("scroll_y", ctypes.c_double),
                *[(name, ctypes.c_int) for name in ("control", "shift", "alt", "meta", "precise_scroll")]]


class _GuiInputOutput(ctypes.Structure):
    _fields_ = [*[(name, ctypes.c_int) for name in
                  ("kind", "phase", "device", "button", "hovering", "eraser", "primary_down")],
                *[(name, ctypes.c_double) for name in
                  ("x", "y", "pressure", "tilt_x", "tilt_y", "rotation_radians", "timestamp")],
                ("device_id", ctypes.c_uint64), ("buttons", ctypes.c_uint32), ("device_state", ctypes.c_uint32),
                ("scroll_x", ctypes.c_double), ("scroll_y", ctypes.c_double),
                *[(name, ctypes.c_int) for name in ("control", "shift", "alt", "meta", "precise_scroll")]]


class _GuiGestureState(ctypes.Structure):
    _fields_ = [("mode", ctypes.c_int), ("device_id", ctypes.c_uint64),
                ("last_x", ctypes.c_double), ("last_y", ctypes.c_double), ("eraser", ctypes.c_int),
                ("device_kind", ctypes.c_int)]


class _GuiViewport(ctypes.Structure):
    _fields_ = [(name, ctypes.c_double) for name in ("scale", "offset_x", "offset_y", "rotation_radians")] + [
        ("flip_x", ctypes.c_int), ("flip_y", ctypes.c_int)]


class _GuiInteractionOptions(ctypes.Structure):
    _fields_ = [("tool", ctypes.c_int), ("raster_selected", ctypes.c_int),
                ("minimum_zoom", ctypes.c_double), ("maximum_zoom", ctypes.c_double),
                ("scroll_sensitivity", ctypes.c_double)]


class _GuiInteractionOutput(ctypes.Structure):
    _fields_ = [("state", _GuiGestureState), ("viewport", _GuiViewport),
                ("has_stroke", ctypes.c_int), ("viewport_changed", ctypes.c_int), ("eraser", ctypes.c_int),
                ("stroke", _GuiInputOutput)]


def _gui_function(name, library):
    """Resolve a GUI bridge symbol from the caller-selected native library."""
    if library is None:
        raise ValueError("An explicit iiSharedCanvas GUI bridge path is required")
    native = ctypes.CDLL(os.fspath(library))
    try:
        function = getattr(native, name)
        function._library_owner = native
        return function
    except AttributeError as error:
        raise RuntimeError("iiSharedCanvas was built without the iiGUI adapter") from error


def translate_canvas_input(phase, device, document, *, pressure=1, tilt=(0, 0),
                           rotation_degrees=0, timestamp=0, device_id=0, buttons=0,
                           scroll=(0, 0), control=False, shift=False, alt=False,
                           meta=False, precise_scroll=False, library=None):
    """Translate one iiGUI input to detached document-space pointer or scroll data.

    The caller owns event ordering, the document and any resulting edits. Tilt is
    supplied in degrees; returned tilt is normalized and rotation is in radians.
    ``library`` is the explicit path to the separately built GUI bridge.
    """
    phases = ("begin", "update", "end", "cancel", "hover", "scroll")
    devices = ("mouse", "pen", "eraser", "touch")
    pointer_buttons = ("none", "primary", "secondary", "middle", "barrel", "eraser")
    try:
        phase_id, device_index = phases.index(phase), devices.index(device)
        if len(document) != 2 or len(tilt) != 2 or len(scroll) != 2:
            raise ValueError("Coordinates must be pairs")
        if not isinstance(device_id, int) or not 0 <= device_id < 2**64:
            raise ValueError("Invalid device identity")
        if not isinstance(buttons, int) or not 0 <= buttons < 2**32:
            raise ValueError("Invalid button mask")
        value = _GuiInput(phase_id, device_index, *document, pressure, *tilt,
                          rotation_degrees, timestamp, device_id, buttons, *scroll,
                          bool(control), bool(shift), bool(alt), bool(meta), bool(precise_scroll))
    except (TypeError, OverflowError) as error:
        raise ValueError("Invalid canvas input") from error
    fn = _gui_function("iisc_gui_translate", library)
    fn.argtypes = [ctypes.POINTER(_GuiInput), ctypes.POINTER(_GuiInputOutput)]
    fn.restype = ctypes.c_int
    result = _GuiInputOutput()
    if not fn(ctypes.byref(value), ctypes.byref(result)):
        raise ValueError("Canvas input is invalid or non-finite")
    return {"kind": "scroll" if result.kind else "pointer", "phase": phases[result.phase],
            "device": devices[result.device], "button": pointer_buttons[result.button], "position": (result.x, result.y),
            "pressure": result.pressure, "tilt": (result.tilt_x, result.tilt_y),
            "rotation_radians": result.rotation_radians, "timestamp": result.timestamp,
            "device_id": result.device_id, "buttons": result.buttons, "device_state": result.device_state,
            "hovering": bool(result.hovering), "eraser": bool(result.eraser),
            "primary_down": bool(result.primary_down), "scroll": (result.scroll_x, result.scroll_y),
            "control": bool(result.control), "shift": bool(result.shift), "alt": bool(result.alt),
            "meta": bool(result.meta), "precise_scroll": bool(result.precise_scroll)}


def route_canvas_input(phase, device, document, screen, *, state=None, viewport=None,
                       tool="brush", selected=False, pressure=1, tilt=(0, 0),
                       rotation_degrees=0, timestamp=0, device_id=0, buttons=0, scroll=(0, 0),
                       control=False, shift=False, alt=False, meta=False, precise_scroll=False,
                       minimum_zoom=.01, maximum_zoom=256, scroll_sensitivity=.0015, library=None):
    """Return one detached gesture transition using caller-owned state and viewport.

    ``state`` and ``viewport`` can be the preceding result's values. The caller
    applies any returned stroke or viewport change and owns all event ordering,
    document access and widget lifetime. ``library`` is an explicit bridge path.
    """
    phases = ("begin", "update", "end", "cancel", "hover", "scroll")
    devices = ("mouse", "pen", "eraser", "touch")
    modes = ("idle", "stroke", "pan")
    device_kinds = ("mouse", "touch", "tablet")
    tools = ("brush", "eraser", "hand", "none")
    try:
        state = {} if state is None else state
        viewport = {} if viewport is None else viewport
        last = state.get("last_screen", (0, 0))
        offset = viewport.get("offset", (0, 0))
        flip = viewport.get("flip", (False, False))
        if any(len(pair) != 2 for pair in (document, screen, tilt, scroll, last, offset, flip)):
            raise ValueError("Coordinates must be pairs")
        prior_device = state.get("device_id", 0)
        if any(not isinstance(v, int) or not 0 <= v < 2**64 for v in (device_id, prior_device)):
            raise ValueError("Invalid device identity")
        if not isinstance(buttons, int) or not 0 <= buttons < 2**32:
            raise ValueError("Invalid button mask")
        raw = _GuiInput(phases.index(phase), devices.index(device), *document, pressure, *tilt,
                        rotation_degrees, timestamp, device_id, buttons, *scroll,
                        bool(control), bool(shift), bool(alt), bool(meta), bool(precise_scroll))
        prior = _GuiGestureState(modes.index(state.get("mode", "idle")), prior_device,
                                 *last, bool(state.get("eraser", False)),
                                 device_kinds.index(state.get("device_kind", "mouse")))
        view = _GuiViewport(viewport.get("scale", 1), *offset, viewport.get("rotation_radians", 0),
                            bool(flip[0]), bool(flip[1]))
        policy = _GuiInteractionOptions(tools.index(tool), bool(selected), minimum_zoom,
                                        maximum_zoom, scroll_sensitivity)
    except (AttributeError, TypeError, OverflowError) as error:
        raise ValueError("Invalid canvas interaction") from error
    fn = _gui_function("iisc_gui_route", library)
    fn.argtypes = [ctypes.POINTER(_GuiInput), ctypes.c_double, ctypes.c_double,
                   ctypes.POINTER(_GuiGestureState), ctypes.POINTER(_GuiViewport),
                   ctypes.POINTER(_GuiInteractionOptions), ctypes.POINTER(_GuiInteractionOutput)]
    fn.restype = ctypes.c_int
    result = _GuiInteractionOutput()
    if not fn(ctypes.byref(raw), *screen, ctypes.byref(prior), ctypes.byref(view),
              ctypes.byref(policy), ctypes.byref(result)):
        raise ValueError("Invalid canvas interaction")
    stroke = None
    if result.has_stroke:
        p = result.stroke
        stroke = {"phase": phases[p.phase], "device": devices[p.device], "position": (p.x, p.y),
                  "button": ("none", "primary", "secondary", "middle", "barrel", "eraser")[p.button],
                  "pressure": p.pressure, "tilt": (p.tilt_x, p.tilt_y),
                  "rotation_radians": p.rotation_radians, "timestamp": p.timestamp,
                  "device_id": p.device_id, "buttons": p.buttons, "device_state": p.device_state,
                  "hovering": bool(p.hovering), "eraser": bool(p.eraser), "primary_down": bool(p.primary_down),
                  "control": bool(p.control), "shift": bool(p.shift), "alt": bool(p.alt), "meta": bool(p.meta)}
    return {"state": {"mode": modes[result.state.mode], "device_id": result.state.device_id,
                      "last_screen": (result.state.last_x, result.state.last_y),
                      "eraser": bool(result.state.eraser), "device_kind": device_kinds[result.state.device_kind]},
            "viewport": {"scale": result.viewport.scale,
                         "offset": (result.viewport.offset_x, result.viewport.offset_y),
                         "rotation_radians": result.viewport.rotation_radians,
                         "flip": (bool(result.viewport.flip_x), bool(result.viewport.flip_y))},
            "stroke": stroke, "viewport_changed": bool(result.viewport_changed),
            "eraser": bool(result.eraser)}


def present_canvas_frame(pixels, source_extent, region, surface, *, scale=1, offset=(0, 0),
                         rotation_radians=0, flip=(False, False), sampling="nearest",
                         maximum_pixels=16777216, library=None):
    """Return ``((width, height), RGBA bytes)`` from a detached ARGB frame.

    ``region`` is its document-space rectangle; the caller owns rendering,
    scheduling, output publication and any view-model lifecycle. ``library`` is
    the explicit path to the separately built GUI bridge.
    """
    try:
        if (not isinstance(maximum_pixels, int) or not 0 < maximum_pixels <= 8192 * 8192
                or len(source_extent) != 2 or len(region) != 4 or len(surface) != 2
                or len(offset) != 2 or len(flip) != 2 or sampling not in ("nearest", "smooth")):
            raise ValueError("Invalid canvas presentation options")
        if any(not isinstance(v, int) or not -(2**31) <= v < 2**31 for v in region):
            raise ValueError("Invalid document region")
        if any(not math.isfinite(float(v)) for v in (*surface, scale, *offset, rotation_radians)):
            raise ValueError("Non-finite canvas viewport")
        if any(not 0 <= float(v) <= 8192 for v in surface):
            raise ValueError("Invalid canvas surface")
        width, height = (math.ceil(v) for v in surface)
        if width * height > maximum_pixels:
            raise ValueError("Canvas presentation exceeds its pixel budget")
        source = _exchange_pixels(pixels, source_extent, maximum_pixels)
    except (TypeError, OverflowError) as error:
        raise ValueError("Invalid canvas presentation options") from error
    output = (ctypes.c_uint8 * (width * height * 4))()
    out_width, out_height = ctypes.c_uint32(), ctypes.c_uint32()
    fn = _gui_function("iisc_gui_present", library)
    fn.argtypes = [ctypes.POINTER(ctypes.c_uint32), ctypes.c_size_t,
                   *([ctypes.c_int32] * 6), *([ctypes.c_double] * 6),
                   *([ctypes.c_int] * 3), ctypes.c_uint64,
                   ctypes.POINTER(ctypes.c_uint8), ctypes.c_size_t,
                   ctypes.POINTER(ctypes.c_uint32), ctypes.POINTER(ctypes.c_uint32)]
    fn.restype = ctypes.c_int
    if not fn(source, len(source), *source_extent, *region, *surface, scale, *offset,
              rotation_radians, bool(flip[0]), bool(flip[1]), sampling == "smooth", maximum_pixels,
              output, len(output), ctypes.byref(out_width), ctypes.byref(out_height)):
        raise ValueError("Canvas frame or viewport is invalid")
    return (out_width.value, out_height.value), bytes(output[:out_width.value * out_height.value * 4])


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


def _channel_index(name):
    channels = ("composite", "red", "green", "blue", "alpha", "luminance")
    try:
        return channels.index(name)
    except ValueError as error:
        raise ValueError("Unknown channel") from error


def _blend_index(name):
    blends = ("normal", "destination_out", "multiply", "screen", "overlay", "darken", "lighten",
              "color_burn", "linear_burn", "darker_color", "color_dodge", "linear_dodge",
              "lighter_color", "soft_light", "hard_light", "vivid_light", "linear_light",
              "pin_light", "hard_mix", "difference", "exclusion", "subtract", "divide",
              "hue", "saturation", "color", "luminosity", "dissolve")
    try:
        return blends.index(name)
    except ValueError as error:
        raise ValueError("Unknown blend mode") from error


def _exchange_pixels(pixels, extent, maximum_pixels=16777216):
    """Copy validated straight-ARGB values into a bounded native array."""
    if (len(extent) != 2 or any(not isinstance(v, int) or not 0 < v < 2**31 for v in extent)
            or extent[0] * extent[1] > maximum_pixels):
        raise ValueError("Invalid or excessive raster extent")
    values = list(pixels)
    if (len(values) != extent[0] * extent[1] or
            any(not isinstance(v, int) or not 0 <= v <= 0xffffffff for v in values)):
        raise ValueError("Invalid straight-ARGB raster")
    return (ctypes.c_uint32 * len(values))(*values)


def _exchange_mask(selection, size):
    if selection is None:
        return None, 0
    values = bytes(selection)
    if len(values) != size:
        raise ValueError("Selection coverage must match the output extent")
    return (ctypes.c_uint8 * size).from_buffer_copy(values), size


def calculate_channels(first, second, extent, *, first_channel="red", second_channel="red",
                       invert_first=False, invert_second=False, invert_result=False,
                       blend="normal", opacity=1, selection=None, library=None):
    """Return a detached 8-bit channel computed from two aligned ARGB rasters.

    The caller selects and renders source documents and can publish the result
    with CanvasDocument.save_selection; no document or filesystem is modified.
    """
    first_index, second_index, blend_index = _channel_index(first_channel), _channel_index(second_channel), _blend_index(blend)
    a, b = _exchange_pixels(first, extent), _exchange_pixels(second, extent)
    mask, mask_size = _exchange_mask(selection, len(a))
    native = ctypes.CDLL(os.fspath(library) if library is not None else _default_library())
    fn = native.iisc_calculate_channels
    u32p, u8p = ctypes.POINTER(ctypes.c_uint32), ctypes.POINTER(ctypes.c_uint8)
    fn.argtypes = [u32p, u32p, ctypes.c_size_t, ctypes.c_int32, ctypes.c_int32,
                   ctypes.c_int, ctypes.c_int, ctypes.c_int, ctypes.c_int, ctypes.c_int,
                   ctypes.c_int, ctypes.c_double, u8p, ctypes.c_size_t, u8p]
    fn.restype = ctypes.c_int
    output = (ctypes.c_uint8 * len(a))()
    if not fn(a, b, len(a), *extent, first_index, second_index,
              bool(invert_first), bool(invert_second), bool(invert_result), blend_index,
              float(opacity), mask, mask_size, output):
        raise ValueError("Channel calculation rejected its input, blend mode, or budget")
    return bytes(output)


def estimate_optical_flow(first, second, extent, *, search_radius=4, patch_radius=2,
                          maximum_comparisons=100_000_000, maximum_pixels=16_777_216, library):
    """Estimate independent forward/backward fields from caller-owned ARGB frames.

    The deterministic reference matcher has explicit search and work budgets.
    A caller may instead provide motion fields from a different engine to
    :func:`interpolate_optical_flow`; ``library`` is the explicitly provided
    native bridge. No decoding, file probing or hidden backend is performed.
    """
    if not isinstance(maximum_pixels,int) or not 0<maximum_pixels<=ctypes.c_size_t(-1).value:
        raise ValueError("Invalid optical-flow pixel budget")
    a, b = _exchange_pixels(first, extent,maximum_pixels), _exchange_pixels(second, extent,maximum_pixels)
    if (not isinstance(search_radius, int) or not 0 <= search_radius <= 64 or
            not isinstance(patch_radius, int) or not 0 <= patch_radius <= 16 or
            not isinstance(maximum_comparisons, int) or not 0 <= maximum_comparisons < 2**64):
        raise ValueError("Invalid optical-flow search or work budget")
    native = ctypes.CDLL(os.fspath(library))
    fn = native.iisc_estimate_optical_flow
    u32p, dp = ctypes.POINTER(ctypes.c_uint32), ctypes.POINTER(ctypes.c_double)
    fn.argtypes = [u32p, u32p, ctypes.c_size_t, ctypes.c_int32, ctypes.c_int32,
                   ctypes.c_uint32, ctypes.c_uint32, ctypes.c_uint64, ctypes.c_size_t, dp, dp]
    fn.restype = ctypes.c_int
    forward, backward = (ctypes.c_double * (2 * len(a)))(), (ctypes.c_double * (2 * len(a)))()
    if not fn(a, b, len(a), *extent, search_radius, patch_radius, maximum_comparisons, maximum_pixels, forward, backward):
        raise ValueError("Optical-flow estimation rejected its input or work budget")
    return ([(forward[2*i], forward[2*i+1]) for i in range(len(a))],
            [(backward[2*i], backward[2*i+1]) for i in range(len(a))])


def interpolate_optical_flow(first, second, extent, forward, backward, phase, *, maximum_pixels=16_777_216, library):
    """Return motion-compensated ARGB pixels using caller-supplied vector fields.

    Each field contains one (x, y) displacement per input pixel. ``phase``
    controls numeric rasterization; exact timeline positions are returned by
    ``match_source_frame`` separately. ``library`` explicitly identifies the
    native bridge. Inputs and outputs do not share storage.
    """
    if not isinstance(maximum_pixels,int) or not 0<maximum_pixels<=ctypes.c_size_t(-1).value:
        raise ValueError("Invalid optical-flow pixel budget")
    a, b = _exchange_pixels(first, extent,maximum_pixels), _exchange_pixels(second, extent,maximum_pixels)
    def field(values):
        pairs = list(values)
        if len(pairs) != len(a) or any(len(pair) != 2 or not all(math.isfinite(v) for v in pair) for pair in pairs):
            raise ValueError("Invalid optical-flow vector field")
        return (ctypes.c_double * (2 * len(a)))(*(float(v) for pair in pairs for v in pair))
    f, r = field(forward), field(backward)
    if not math.isfinite(phase) or not 0 <= phase <= 1:
        raise ValueError("Optical-flow phase must be finite and between zero and one")
    native = ctypes.CDLL(os.fspath(library))
    fn = native.iisc_interpolate_optical_flow
    u32p, dp = ctypes.POINTER(ctypes.c_uint32), ctypes.POINTER(ctypes.c_double)
    fn.argtypes = [u32p, u32p, ctypes.c_size_t, ctypes.c_int32, ctypes.c_int32, dp, dp, ctypes.c_double, ctypes.c_size_t, u32p]
    fn.restype = ctypes.c_int
    output = (ctypes.c_uint32 * len(a))()
    if not fn(a, b, len(a), *extent, f, r, phase, maximum_pixels, output):
        raise ValueError("Optical-flow interpolation rejected its input")
    return list(output)


class _SequenceFlattenInfo(ctypes.Structure):
    _fields_ = [("first_frame", ctypes.c_uint64), ("frame_count", ctypes.c_uint64),
                ("width", ctypes.c_int32), ("height", ctypes.c_int32),
                ("source_sequence",ctypes.c_char_p), ("source_clip",ctypes.c_char_p)]


def flatten_nested_sequence(canvas, sequence, clip, *, snapshot_path, note_path, renderer,
                            source_id, representation_id, stream_id, owned_media_id, original_media_id,
                            maximum_frames=100_000, maximum_snapshot_bytes=1024**3,
                            maximum_frame_bytes=1024**3):
    """Render one nested video clip and publish its owned frames as one edit.

    ``note_path`` is a new path in the consuming app's repository; it records
    the deferred editable-group design. ``snapshot_path`` is a new path in
    that app's temporary storage. Both are written before ``renderer`` runs
    and neither existing file is overwritten. The renderer is supplied by the
    application and receives (snapshot_path, sequence, clip, absolute_frame,
    (width, height)); it returns straight-ARGB pixels for the isolated parent
    clip before its final blend with outer tracks. Requested frames can fall
    outside the visible clip when an existing transition needs source handles.
    It must raise if it cannot preserve the requested composition. A render or publication failure
    leaves the document untouched and retains any temporary files for the app.
    """
    if not callable(renderer):
        raise TypeError("A caller-supplied sequence renderer is required")
    for number in (maximum_frames, maximum_snapshot_bytes, maximum_frame_bytes):
        if not isinstance(number, int) or not 0 < number < 2**64:
            raise ValueError("Flatten budgets must be positive 64-bit integers")
    paths = [os.fsencode(os.fspath(path)) for path in (snapshot_path, note_path)]
    if any(not path or b"\0" in path for path in paths) or paths[0] == paths[1]:
        raise ValueError("Two distinct application-owned paths are required")
    sequence_bytes, clip_bytes = canvas._identifier(sequence), canvas._identifier(clip)
    identities = [canvas._identifier(item) for item in (source_id, representation_id, stream_id, owned_media_id,original_media_id)]
    native=canvas._native
    p, u32p = ctypes.c_void_p, ctypes.POINTER(ctypes.c_uint32)
    prepare=native.iisc_canvas_sequence_flatten_prepare
    prepare.argtypes=[p,ctypes.c_char_p,ctypes.c_char_p,ctypes.c_uint64,ctypes.c_uint64,
                      ctypes.POINTER(p),ctypes.POINTER(_SequenceFlattenInfo)]
    prepare.restype=ctypes.c_int
    write=native.iisc_canvas_sequence_flatten_write
    write.argtypes=[p,p,ctypes.c_char_p,ctypes.c_char_p];write.restype=ctypes.c_int
    commit=native.iisc_canvas_sequence_flatten_commit
    commit.argtypes=[p,p,u32p,ctypes.c_size_t,ctypes.c_char_p,ctypes.c_char_p,ctypes.c_char_p,ctypes.c_char_p,
                     ctypes.c_char_p,ctypes.c_uint64]
    commit.restype=ctypes.c_int
    destroy=native.iisc_sequence_flatten_destroy;destroy.argtypes=[p];destroy.restype=None
    handle=p();info=_SequenceFlattenInfo()
    canvas._call("sequence_flatten_prepare",sequence_bytes,clip_bytes,maximum_frames,maximum_snapshot_bytes,
                 ctypes.byref(handle),ctypes.byref(info))
    try:
        canvas._call("sequence_flatten_write",handle,*paths)
        size=info.width*info.height
        if size<=0 or info.frame_count>(maximum_frame_bytes-24)//(size*4):
            raise ValueError("Flatten frame byte budget exceeded")
        packed=(ctypes.c_uint32*(size*info.frame_count))()
        extent=(info.width,info.height)
        source_sequence=info.source_sequence.decode("utf-8")
        source_clip=info.source_clip.decode("utf-8")
        for i in range(info.frame_count):
            result=renderer(os.fspath(snapshot_path),source_sequence,source_clip,info.first_frame+i,extent)
            values=list(result)
            if len(values)!=size or any(not isinstance(v,int) or not 0<=v<=0xffffffff for v in values):
                raise ValueError("Renderer returned an invalid straight-ARGB frame")
            packed[i*size:(i+1)*size]=values
        canvas._call("sequence_flatten_commit",handle,packed,len(packed),*identities,maximum_frame_bytes)
    finally:
        destroy(handle)


def read_raw_sequence_frame(data, frame, *, maximum_pixels=16_777_216, library):
    """Decode one owned ARGB frame using the explicitly provided native bridge."""
    payload=bytes(data)
    if len(payload)<24 or payload[:8]!=b"IISARGB1" or not isinstance(frame,int) or frame<0:
        raise ValueError("Invalid raw sequence header or frame index")
    width,height,count=struct.unpack_from("<IIQ",payload,8)
    if (not isinstance(maximum_pixels,int) or maximum_pixels<=0 or not width or not height or
            width*height>maximum_pixels or frame>=count or len(payload)!=24+width*height*count*4):
        raise ValueError("Malformed or excessive raw sequence")
    native=ctypes.CDLL(os.fspath(library))
    fn=native.iisc_raw_sequence_frame
    fn.argtypes=[ctypes.POINTER(ctypes.c_uint8),ctypes.c_size_t,ctypes.c_uint64,ctypes.c_size_t,
                 ctypes.POINTER(ctypes.c_uint32),ctypes.c_size_t,ctypes.POINTER(ctypes.c_int32),ctypes.POINTER(ctypes.c_int32)]
    fn.restype=ctypes.c_int
    source=(ctypes.c_uint8*len(payload)).from_buffer_copy(payload)
    output=(ctypes.c_uint32*(width*height))();native_width=ctypes.c_int32();native_height=ctypes.c_int32()
    if not fn(source,len(payload),frame,maximum_pixels,output,len(output),ctypes.byref(native_width),ctypes.byref(native_height)):
        raise ValueError("Raw sequence frame could not be decoded")
    return native_width.value,native_height.value,list(output)


def _editable_sequence_reference(project, sequence, clip):
    tracks=next((item["tracks"] for item in project["sequences"] if item["id"]==sequence),None)
    if tracks is None:
        raise ValueError("Unknown sequence")
    target=next((item for track in tracks for item in track["value"]["clips"]
                 if item["properties"]["id"]==clip),None)
    original=target.get("editableOriginal") if target else None
    if not original or not original["$optional"]:
        raise ValueError("The selected clip has no editable original")
    return original["value"]


def open_flattened_sequence_source(canvas, sequence, clip):
    """Open the document-owned editable source of a rendered nested clip.

    The returned CanvasDocument is independent and must be closed by its
    owner. Temporary paths are not read; the source travels inside the IISC
    document. The caller decides how to edit or publish the original.
    """
    reference=_editable_sequence_reference(canvas.sequence_project_model(),sequence,clip)
    media=next((item for item in canvas.sequence_owned_media() if item["id"]==reference["ownedSnapshotId"]),None)
    if not media or media["media_type"]!="application/vnd.iisacc.ii-shared-canvas":
        raise ValueError("Editable original is not a document-owned IISC snapshot")
    document=CanvasDocument.from_bytes(media["data"],library=canvas._native._name)
    try:
        source=document.sequence_project_model()
        found=next((item for seq in source["sequences"] if seq["id"]==reference["sequenceId"]
                    for track in seq["tracks"] for item in track["value"]["clips"]
                    if item["properties"]["id"]==reference["clipId"]),None)
        if not found or found["properties"]["source"]["$variant"]!=1:
            raise ValueError("The snapshot does not contain the original clip")
        return document
    except Exception:
        document.close()
        raise


def replace_flattened_sequence_source(canvas, sequence, clip, edited_source, *, maximum_snapshot_bytes=1024**3):
    """Publish an edited original as one history entry for later rerendering.

    The current rendered pixels remain visible. The edited source must retain
    the original nested clip identity. Its bytes are copied into the document.
    """
    if not isinstance(maximum_snapshot_bytes,int) or not 0<maximum_snapshot_bytes<2**64:
        raise ValueError("Snapshot byte budget must be a positive 64-bit integer")
    fn=canvas._native.iisc_canvas_sequence_flatten_replace_original
    fn.argtypes=[ctypes.c_void_p,ctypes.c_char_p,ctypes.c_char_p,ctypes.c_void_p,ctypes.c_uint64]
    fn.restype=ctypes.c_int
    canvas._call("sequence_flatten_replace_original",canvas._identifier(sequence),canvas._identifier(clip),
                 edited_source._open(),maximum_snapshot_bytes)


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


@dataclass(frozen=True)
class CanvasSnapshot:
    """Detached information captured without copying raster or child documents."""
    width: int
    height: int
    frame_count: int
    assets: int
    layers: int
    authorship_revision: int
    undo_entries: int
    redo_entries: int


class _CanvasInfo(ctypes.Structure):
    _fields_ = [("width", ctypes.c_int32), ("height", ctypes.c_int32),
                ("frame_count", ctypes.c_uint32), ("assets", ctypes.c_uint64),
                ("layers", ctypes.c_uint64), ("authorship_revision", ctypes.c_uint64),
                ("undo_entries", ctypes.c_uint64), ("redo_entries", ctypes.c_uint64)]


class _CanvasSetting(ctypes.Structure):
    _fields_ = [("name", ctypes.c_char_p), ("value", ctypes.c_double)]


class _FillStop(ctypes.Structure):
    _fields_ = [("position", ctypes.c_double), ("color", ctypes.c_uint32)]


class _FillSpec(ctypes.Structure):
    _fields_ = [("kind", ctypes.c_int32), ("width", ctypes.c_int32), ("height", ctypes.c_int32),
                ("repeat", ctypes.c_int32), ("color", ctypes.c_uint32),
                ("start_x", ctypes.c_double), ("start_y", ctypes.c_double),
                ("end_x", ctypes.c_double), ("end_y", ctypes.c_double),
                ("pattern_x", ctypes.c_double), ("pattern_y", ctypes.c_double),
                ("pattern_asset", ctypes.c_char_p), ("stops", ctypes.POINTER(_FillStop)),
                ("stop_count", ctypes.c_size_t)]


class _LayerStyle(ctypes.Structure):
    _fields_ = [("kind", ctypes.c_int32), ("enabled", ctypes.c_int32), ("color", ctypes.c_uint32),
                ("opacity", ctypes.c_double), ("radius", ctypes.c_double),
                ("offset_x", ctypes.c_double), ("offset_y", ctypes.c_double)]


class _MaskSpec(ctypes.Structure):
    _fields_ = [("asset", ctypes.c_char_p), ("enabled", ctypes.c_int32),
                ("inverted", ctypes.c_int32), ("linked", ctypes.c_int32), ("channel", ctypes.c_int32),
                ("density", ctypes.c_double), ("feather", ctypes.c_double), ("outside", ctypes.c_double),
                ("m11", ctypes.c_double), ("m12", ctypes.c_double), ("m21", ctypes.c_double),
                ("m22", ctypes.c_double), ("x", ctypes.c_double), ("y", ctypes.c_double)]


class _LayerGeometry(ctypes.Structure):
    _fields_ = [("id",ctypes.c_char_p),("x",ctypes.c_double),("y",ctypes.c_double),
                ("width",ctypes.c_double),("height",ctypes.c_double)]


class _ShapeOperand(ctypes.Structure):
    _fields_ = [("asset",ctypes.c_char_p),("m11",ctypes.c_double),("m12",ctypes.c_double),
                ("m21",ctypes.c_double),("m22",ctypes.c_double),("x",ctypes.c_double),("y",ctypes.c_double)]


class _VectorCommand(ctypes.Structure):
    _fields_ = [("kind",ctypes.c_int32),("x1",ctypes.c_double),("y1",ctypes.c_double),
                ("x2",ctypes.c_double),("y2",ctypes.c_double),("x3",ctypes.c_double),("y3",ctypes.c_double)]


class _CanvasBounds(ctypes.Structure):
    _fields_ = [(name,ctypes.c_int32) for name in ("present","x","y","width","height")]


class _SequencePair(ctypes.Structure):
    _fields_ = [("left", ctypes.c_char_p), ("right", ctypes.c_char_p)]


class _SequenceClipRange(ctypes.Structure):
    _fields_ = [("id", ctypes.c_char_p), ("start", ctypes.c_int64), ("duration", ctypes.c_int64)]


class _SequenceCreate(ctypes.Structure):
    _fields_ = [("project_id", ctypes.c_char_p), ("sequence_id", ctypes.c_char_p), ("name", ctypes.c_char_p),
                ("duration", ctypes.c_int64), ("time_num", ctypes.c_uint32), ("time_den", ctypes.c_uint32),
                ("rate_num", ctypes.c_uint32), ("rate_den", ctypes.c_uint32),
                ("width", ctypes.c_int32), ("height", ctypes.c_int32)]


class _SequenceSettings(ctypes.Structure):
    _fields_ = [("flags", ctypes.c_uint32), ("name", ctypes.c_char_p),
                ("rate_num", ctypes.c_uint32), ("rate_den", ctypes.c_uint32),
                ("width", ctypes.c_int32), ("height", ctypes.c_int32),
                ("aspect_num", ctypes.c_int64), ("aspect_den", ctypes.c_uint64),
                ("audio_rate", ctypes.c_uint32), ("audio_layout", ctypes.c_char_p),
                ("timecode_present", ctypes.c_int32), ("timecode_start_frame", ctypes.c_int64),
                ("timecode_rate_num", ctypes.c_uint32), ("timecode_rate_den", ctypes.c_uint32),
                ("timecode_nominal_fps", ctypes.c_uint32), ("timecode_counting", ctypes.c_int32),
                ("timecode_wrap_24_hours", ctypes.c_int32)]


class _SequenceProperty(ctypes.Structure):
    _fields_ = [("key", ctypes.c_char_p), ("kind", ctypes.c_int32), ("boolean_value", ctypes.c_int32),
                ("integer_value", ctypes.c_int64), ("denominator", ctypes.c_uint64),
                ("components", ctypes.c_double * 4), ("text", ctypes.c_char_p),
                ("numbers", ctypes.POINTER(ctypes.c_double)), ("number_count", ctypes.c_size_t)]


class _SequenceCaptionStyle(ctypes.Structure):
    _fields_ = [("font_family", ctypes.c_char_p), ("font_size", ctypes.c_double),
                ("fill", ctypes.c_double * 4), ("outline", ctypes.c_double * 4),
                ("background", ctypes.c_double * 4), ("outline_width", ctypes.c_double),
                ("bold", ctypes.c_int32), ("italic", ctypes.c_int32), ("underline", ctypes.c_int32),
                ("horizontal_alignment", ctypes.c_char_p), ("vertical_alignment", ctypes.c_char_p),
                ("writing_direction", ctypes.c_char_p), ("properties", ctypes.POINTER(_SequenceProperty)),
                ("property_count", ctypes.c_size_t)]


class _SequenceOwnedMedia(ctypes.Structure):
    _fields_ = [("id", ctypes.c_char_p), ("media_type", ctypes.c_char_p),
                ("data", ctypes.POINTER(ctypes.c_uint8)), ("size", ctypes.c_size_t)]


class _SequenceTrackChanges(ctypes.Structure):
    _fields_ = [("flags", ctypes.c_uint32), ("enabled", ctypes.c_int32), ("locked", ctypes.c_int32),
                ("muted", ctypes.c_int32), ("solo", ctypes.c_int32), ("sync_locked", ctypes.c_int32),
                ("role", ctypes.c_char_p), ("language", ctypes.c_char_p), ("blend_mode", ctypes.c_char_p),
                ("output_bus", ctypes.c_char_p), ("opacity", ctypes.c_double)]


class _SequenceKeyframe(ctypes.Structure):
    _fields_ = [("time", ctypes.c_int64), ("center_x", ctypes.c_double),
                ("center_y", ctypes.c_double), ("scale", ctypes.c_double)]


class _SequenceObservation(ctypes.Structure):
    _fields_ = [("time", ctypes.c_int64), ("track_id", ctypes.c_char_p),
                ("x", ctypes.c_double), ("y", ctypes.c_double),
                ("width", ctypes.c_double), ("height", ctypes.c_double)]


class _SequenceDetection(ctypes.Structure):
    _fields_ = [("time", ctypes.c_int64), ("class_id", ctypes.c_int32), ("label", ctypes.c_char_p),
                ("confidence", ctypes.c_double), ("x", ctypes.c_double), ("y", ctypes.c_double),
                ("width", ctypes.c_double), ("height", ctypes.c_double)]


def analyze_reframe_frames(canvas, frames, detector, *, id_prefix, maximum_gap,
                           minimum_confidence=0., minimum_overlap=.25, maximum_detections=100000):
    """Invoke an explicitly supplied detector, then associate its detached results.

    ``frames`` yields (clip_tick, frame) pairs. ``detector(frame)`` returns
    (class_id, label, confidence, x, y, width, height) tuples. The caller owns
    decoding, frame lifetimes, model creation, and selection of a returned track.
    No model or system resource is selected by the SDK.
    """
    if not isinstance(canvas, CanvasDocument) or not callable(detector):
        raise ValueError("A canvas and an explicitly supplied detector are required")
    canvas._identifier(id_prefix)
    if (canvas._sequence_tick(maximum_gap) < 0 or not isinstance(maximum_detections, int) or
            not 0 <= maximum_detections <= 100000 or not math.isfinite(minimum_confidence) or
            not 0 <= minimum_confidence <= 1 or not math.isfinite(minimum_overlap) or not 0 <= minimum_overlap <= 1):
        raise ValueError("Invalid detector tracking limits")
    detections = []
    for frame_index, (time, frame) in enumerate(frames):
        if frame_index >= maximum_detections or canvas._sequence_tick(time) < 0:
            raise ValueError("Analysis frame limit or timestamp is invalid")
        for item in detector(frame):
            if len(detections) >= maximum_detections:
                raise ValueError("Detection limit exceeded")
            if len(item) != 7:
                raise ValueError("Detector result requires class, label, confidence and bounds")
            detections.append((time, *item))
    return canvas.track_sequence_subjects(detections, id_prefix=id_prefix, maximum_gap=maximum_gap,
                                          minimum_confidence=minimum_confidence, minimum_overlap=minimum_overlap)


def scan_transparent_bounds(pixels, width, height, *, origin=(0,0), threshold=0, library=None):
    """Return exact nontransparent ARGB bounds, or None when all alpha is below threshold."""
    if (not isinstance(width,int) or not isinstance(height,int) or width<=0 or height<=0 or
            width>=2**31 or height>=2**31 or width*height>256*1024*1024 or len(origin)!=2 or
            any(not isinstance(v,int) or not -(2**31)<=v<2**31 for v in origin) or
            not isinstance(threshold,int) or not 0<=threshold<=255):
        raise ValueError("Invalid raster dimensions, origin or alpha threshold")
    values=list(pixels)
    if len(values)!=width*height or any(not isinstance(v,int) or not 0<=v<=0xffffffff for v in values):
        raise ValueError("Invalid ARGB pixels")
    native=ctypes.CDLL(os.fspath(library) if library is not None else _default_library())
    fn=native.iisc_scan_transparent_bounds
    fn.argtypes=[ctypes.POINTER(ctypes.c_uint32),ctypes.c_size_t,ctypes.c_int32,ctypes.c_int32,
                 ctypes.c_int32,ctypes.c_int32,ctypes.c_uint8,ctypes.POINTER(_CanvasBounds)]
    fn.restype=ctypes.c_int
    data=(ctypes.c_uint32*len(values))(*values); out=_CanvasBounds()
    if not fn(data,len(values),width,height,*origin,threshold,ctypes.byref(out)):
        raise ValueError("Cannot scan the requested raster bounds")
    return (out.x,out.y,out.width,out.height) if out.present else None


class LayerLocks(IntFlag):
    """Layer and inherited group protection flags accepted by the SDK."""
    NONE = 0
    PIXELS = 1
    TRANSPARENCY = 2
    TRANSFORM = 4
    STRUCTURE = 8
    ALL = 15


def _timeline_cbor(model, schema):
    """Encode a tagged model tree in the frozen, bounded canonical CBOR form."""
    out = bytearray()
    count = 0
    limit = 64 * 1024 * 1024

    def append(data):
        if len(data) > limit - len(out):
            raise ValueError("Timeline model exceeds the encoded byte budget")
        out.extend(data)

    def head(major, number):
        if not isinstance(number, int) or not 0 <= number < 2**64:
            raise ValueError("Timeline integer is outside the CBOR range")
        prefix = major << 5
        if number < 24: append(bytes((prefix | number,)))
        elif number <= 0xff: append(bytes((prefix | 24, number)))
        elif number <= 0xffff: append(bytes((prefix | 25,)) + struct.pack(">H", number))
        elif number <= 0xffffffff: append(bytes((prefix | 26,)) + struct.pack(">I", number))
        else: append(bytes((prefix | 27,)) + struct.pack(">Q", number))

    def floating(value):
        if not math.isfinite(value):
            raise ValueError("Timeline model floats must be finite")
        for marker, form in ((b"\xf9", ">e"), (b"\xfa", ">f")):
            try:
                packed = struct.pack(form, value)
                restored = struct.unpack(form, packed)[0]
                if restored == value and (value != 0 or math.copysign(1., restored) == math.copysign(1., value)):
                    append(marker + packed); return
            except OverflowError:
                pass
        append(b"\xfb" + struct.pack(">d", value))

    def encode(value, depth=0):
        nonlocal count
        count += 1
        if depth > 96 or count > 8_000_000:
            raise ValueError("Timeline model exceeds its nesting or item budget")
        if value is None: append(b"\xf6")
        elif isinstance(value, bool): append(b"\xf5" if value else b"\xf4")
        elif isinstance(value, int):
            if value < -(2**63): raise ValueError("Timeline integer is outside the supported range")
            head(0, value) if value >= 0 else head(1, -1-value)
        elif isinstance(value, float): floating(value)
        elif isinstance(value, str):
            text = value.encode("utf-8"); head(3, len(text)); append(text)
        elif isinstance(value, (list, tuple)):
            if len(value) > 1_000_000: raise ValueError("Timeline array exceeds its item budget")
            head(4, len(value))
            for child in value: encode(child, depth+1)
        elif isinstance(value, dict):
            if "$type" in value:
                name = value["$type"]
                if not isinstance(name, str) or name not in schema:
                    raise ValueError("Unknown timeline model type")
                fields = schema[name]
                if set(value) != {"$type", *fields}:
                    raise ValueError("Timeline model fields do not match the schema")
                head(4, len(fields))
                for field in fields: encode(value[field], depth+1)
            elif "$variant" in value:
                if set(value) != {"$variant", "value"} or isinstance(value["$variant"], bool):
                    raise ValueError("Invalid timeline variant")
                head(4, 2); encode(value["$variant"], depth+1); encode(value["value"], depth+1)
            elif "$optional" in value:
                present = value["$optional"]
                if not isinstance(present, bool) or set(value) != ({"$optional", "value"} if present else {"$optional"}):
                    raise ValueError("Invalid timeline optional")
                head(4, int(present))
                if present: encode(value["value"], depth+1)
            elif "$bytes" in value:
                if set(value) != {"$bytes"} or not isinstance(value["$bytes"], str):
                    raise ValueError("Invalid timeline bytes")
                try: data = bytes.fromhex(value["$bytes"])
                except ValueError as error: raise ValueError("Invalid timeline byte hex") from error
                head(2, len(data)); append(data)
            else: raise ValueError("A timeline object needs an explicit type tag")
        else: raise ValueError("Unsupported timeline model value")
    encode(model)
    return bytes(out)


class CanvasDocument:
    """Own a document and its session-only history through the native SDK.

    Linked canvases are independent copies. Reediting always uses the pixels
    captured by begin_reeditable; undo or another edit invalidates that session.
    Handles are single-threaded and must be closed by their owner.
    """

    def __init__(self, library: str | os.PathLike[str] | None = None):
        self._handle = None
        self._dll_directory = None
        filename = os.fspath(library) if library is not None else _default_library()
        if sys.platform == "win32" and Path(filename).is_file():
            self._dll_directory = os.add_dll_directory(os.fspath(Path(filename).resolve().parent))
        self._native = ctypes.CDLL(filename)
        p, u8p = ctypes.c_void_p, ctypes.POINTER(ctypes.c_uint8)
        declarations = {
            "raster": ([ctypes.c_int32, ctypes.c_int32, ctypes.c_uint32], p),
            "from_channel": ([ctypes.c_int32,ctypes.c_int32,u8p,ctypes.c_size_t],p),
            "decode": ([u8p, ctypes.c_size_t], p),
            "clone": ([p], p),
            "destroy": ([p], None), "error": ([p], ctypes.c_char_p),
            "info": ([p, ctypes.POINTER(_CanvasInfo)], ctypes.c_int),
            "encode": ([p, u8p, ctypes.c_size_t, ctypes.POINTER(ctypes.c_size_t)], ctypes.c_int),
            "pixel": ([p, ctypes.c_uint32, ctypes.c_int32, ctypes.c_int32, ctypes.POINTER(ctypes.c_uint32)], ctypes.c_int),
            "embed": ([p, p, ctypes.c_char_p, ctypes.c_char_p, ctypes.c_uint32], ctypes.c_int),
            "replace": ([p, p, ctypes.c_char_p, ctypes.c_uint32], ctypes.c_int),
            "undo": ([p], ctypes.c_int), "redo": ([p], ctypes.c_int),
            "capture_last": ([p], ctypes.c_int), "toggle_last": ([p], ctypes.c_int),
            "begin_reedit": ([p, ctypes.c_char_p, ctypes.c_size_t], ctypes.c_int),
            "adjust": ([p, ctypes.POINTER(_CanvasSetting), ctypes.c_size_t], ctypes.c_int),
            "effect": ([p, ctypes.c_int32, ctypes.POINTER(_CanvasSetting), ctypes.c_size_t], ctypes.c_int),
            "put_fill": ([p,ctypes.c_char_p,ctypes.c_char_p,ctypes.POINTER(_FillSpec)],ctypes.c_int),
            "put_adjustment": ([p,ctypes.c_char_p,ctypes.c_char_p,ctypes.c_int32,ctypes.c_int32,
                                ctypes.POINTER(_CanvasSetting),ctypes.c_size_t],ctypes.c_int),
            "styles": ([p,ctypes.c_char_p,ctypes.POINTER(_LayerStyle),ctypes.c_size_t],ctypes.c_int),
            "mask": ([p,ctypes.c_char_p,ctypes.c_int,ctypes.POINTER(_MaskSpec)],ctypes.c_int),
            "slice": ([p,ctypes.c_char_p,ctypes.c_uint32,ctypes.c_int32,ctypes.c_int32,
                       ctypes.c_int32,ctypes.c_int32,ctypes.c_int32,ctypes.c_int32,ctypes.c_int,
                       ctypes.POINTER(ctypes.c_uint32),ctypes.c_size_t],ctypes.c_int),
            "apply_image": ([p,ctypes.c_char_p,ctypes.c_uint32,ctypes.POINTER(ctypes.c_uint32),ctypes.c_size_t,
                             ctypes.c_int32,ctypes.c_int32,ctypes.c_int,ctypes.c_int,ctypes.c_int,
                             ctypes.c_int,ctypes.c_double,u8p,ctypes.c_size_t],ctypes.c_int),
            "set_frame_count": ([p,ctypes.c_uint32],ctypes.c_int),
            "composite": ([p,ctypes.POINTER(ctypes.c_char_p),ctypes.c_size_t,ctypes.c_uint32,
                           ctypes.c_int32,ctypes.c_int32,ctypes.c_int32,ctypes.c_int32,
                           ctypes.c_int32,ctypes.c_int32,ctypes.c_int,u8p,ctypes.c_size_t,
                           ctypes.POINTER(ctypes.c_uint32),ctypes.c_size_t],ctypes.c_int),
            "layer_css": ([p,ctypes.c_char_p,ctypes.c_uint32,u8p,ctypes.c_size_t,ctypes.POINTER(ctypes.c_size_t)],ctypes.c_int),
            "selected_svg": ([p,ctypes.POINTER(ctypes.c_char_p),ctypes.c_size_t,ctypes.c_uint32,
                              ctypes.c_int32,ctypes.c_int32,ctypes.c_int32,ctypes.c_int32,
                              u8p,ctypes.c_size_t,ctypes.POINTER(ctypes.c_size_t)],ctypes.c_int),
            "arrange": ([p,ctypes.POINTER(ctypes.c_char_p),ctypes.c_size_t,ctypes.c_int],ctypes.c_int),
            "align": ([p,ctypes.POINTER(_LayerGeometry),ctypes.c_size_t,ctypes.c_double,ctypes.c_double,
                       ctypes.c_double,ctypes.c_double,ctypes.c_int],ctypes.c_int),
            "distribute": ([p,ctypes.POINTER(_LayerGeometry),ctypes.c_size_t,ctypes.c_int,ctypes.c_int],ctypes.c_int),
            "put_vector": ([p,ctypes.c_char_p,ctypes.c_char_p,ctypes.POINTER(_VectorCommand),ctypes.c_size_t,ctypes.c_uint32],ctypes.c_int),
            "combine_shapes": ([p,ctypes.POINTER(_ShapeOperand),ctypes.c_size_t,ctypes.c_int,ctypes.c_char_p,ctypes.c_char_p,ctypes.c_uint32],ctypes.c_int),
            "vector_command": ([p,ctypes.c_char_p,ctypes.c_size_t,ctypes.c_size_t,ctypes.POINTER(_VectorCommand)],ctypes.c_int),
            "edit_vector_command": ([p,ctypes.c_char_p,ctypes.c_size_t,ctypes.c_size_t,ctypes.POINTER(_VectorCommand)],ctypes.c_int),
            "merge": ([p,ctypes.POINTER(ctypes.c_char_p),ctypes.c_size_t,ctypes.c_char_p,ctypes.c_char_p],ctypes.c_int),
            "flatten": ([p,ctypes.c_char_p,ctypes.c_char_p],ctypes.c_int),
            "resize": ([p,ctypes.c_int32,ctypes.c_int32,ctypes.c_int],ctypes.c_int),
            "crop": ([p,ctypes.c_int32,ctypes.c_int32,ctypes.c_int32,ctypes.c_int32],ctypes.c_int),
            "resample": ([p,ctypes.c_int32,ctypes.c_int32,ctypes.c_int],ctypes.c_int),
            "rotate": ([p,ctypes.c_double],ctypes.c_int),
            "content_bounds": ([p,ctypes.c_int,ctypes.c_int,ctypes.c_int32,ctypes.c_int32,ctypes.c_int32,ctypes.c_int32,
                                ctypes.POINTER(ctypes.c_uint32),ctypes.c_size_t,ctypes.c_uint8,ctypes.c_size_t,
                                ctypes.POINTER(_CanvasBounds)],ctypes.c_int),
            "pixel_aspect": ([p,ctypes.POINTER(ctypes.c_uint32),ctypes.POINTER(ctypes.c_uint32)],ctypes.c_int),
            "set_pixel_aspect": ([p,ctypes.c_uint32,ctypes.c_uint32],ctypes.c_int),
            "display": ([p,ctypes.c_uint32,ctypes.c_int,ctypes.POINTER(ctypes.c_uint32),ctypes.c_size_t,
                         ctypes.POINTER(ctypes.c_int32),ctypes.POINTER(ctypes.c_int32)],ctypes.c_int),
            "selection_set": ([p,ctypes.c_char_p,ctypes.c_char_p,ctypes.c_int32,ctypes.c_int32,
                               ctypes.c_int32,ctypes.c_int32,u8p,ctypes.c_size_t],ctypes.c_int),
            "selection_rename": ([p,ctypes.c_char_p,ctypes.c_char_p],ctypes.c_int),
            "selection_delete": ([p,ctypes.c_char_p],ctypes.c_int),
            "selection_coverage": ([p,ctypes.c_char_p,ctypes.c_int32,ctypes.c_int32,u8p],ctypes.c_int),
            "group_set": ([p,ctypes.c_char_p,ctypes.c_char_p,ctypes.c_char_p,ctypes.c_int,ctypes.c_int],ctypes.c_int),
            "group_delete": ([p,ctypes.c_char_p],ctypes.c_int),
            "group_assign": ([p,ctypes.c_char_p,ctypes.c_char_p],ctypes.c_int),
            "link": ([p,ctypes.c_char_p,ctypes.c_char_p,ctypes.c_int],ctypes.c_int),
            "lock": ([p,ctypes.c_char_p,ctypes.c_int,ctypes.c_uint8],ctypes.c_int),
            "relations": ([p,u8p,ctypes.c_size_t,ctypes.POINTER(ctypes.c_size_t)],ctypes.c_int),
            "query": ([p,ctypes.c_char_p,ctypes.c_int,ctypes.c_int64,ctypes.c_int,ctypes.c_char_p,
                       u8p,ctypes.c_size_t,ctypes.POINTER(ctypes.c_size_t)],ctypes.c_int),
            "sequence_snapshot": ([p,u8p,ctypes.c_size_t,ctypes.POINTER(ctypes.c_size_t)],ctypes.c_int),
            "sequence_settings": ([p,ctypes.c_char_p,ctypes.POINTER(_SequenceSettings)],ctypes.c_int),
            "sequence_tracks": ([p,ctypes.c_char_p,ctypes.POINTER(ctypes.c_char_p),ctypes.c_size_t,
                                 ctypes.POINTER(_SequenceTrackChanges)],ctypes.c_int),
            "sequence_add_track": ([p,ctypes.c_char_p,ctypes.c_char_p,ctypes.c_int],ctypes.c_int),
            "sequence_split": ([p,ctypes.c_char_p,ctypes.POINTER(_SequencePair),ctypes.c_size_t,
                                ctypes.POINTER(_SequencePair),ctypes.c_size_t,ctypes.c_int64],ctypes.c_int),
            "sequence_trim": ([p,ctypes.c_char_p,ctypes.POINTER(_SequenceClipRange),ctypes.c_size_t],ctypes.c_int),
            "sequence_extend": ([p,ctypes.c_char_p,ctypes.POINTER(ctypes.c_char_p),ctypes.c_size_t,
                                 ctypes.c_int,ctypes.c_int64],ctypes.c_int),
            "sequence_range": ([p,ctypes.c_int,ctypes.c_char_p,ctypes.POINTER(ctypes.c_char_p),ctypes.c_size_t,
                                ctypes.c_int64,ctypes.c_int64,ctypes.c_char_p,ctypes.c_char_p],ctypes.c_int),
            "sequence_simplify": ([p,ctypes.c_char_p,ctypes.c_uint32,ctypes.c_char_p],ctypes.c_int),
            "sequence_caption": ([p,ctypes.c_char_p,ctypes.c_char_p,ctypes.c_int,ctypes.c_char_p,
                                  ctypes.c_char_p,ctypes.c_char_p,ctypes.c_int64,ctypes.c_int64,
                                  ctypes.c_char_p,ctypes.c_char_p,ctypes.c_double],ctypes.c_int),
            "sequence_transition": ([p,ctypes.c_char_p,ctypes.c_char_p,ctypes.c_char_p,ctypes.c_char_p,
                                     ctypes.c_char_p,ctypes.c_char_p,ctypes.c_int64,ctypes.c_int64,ctypes.c_int],ctypes.c_int),
            "sequence_reframe": ([p,ctypes.c_char_p,ctypes.c_char_p,ctypes.c_char_p,
                                  ctypes.POINTER(_SequenceKeyframe),ctypes.c_size_t,
                                  ctypes.POINTER(_SequenceObservation),ctypes.c_size_t,ctypes.c_char_p,
                                  ctypes.c_int32,ctypes.c_int32,ctypes.c_int32,ctypes.c_int32,ctypes.c_double],ctypes.c_int),
            "sequence_gaps": ([p,ctypes.c_char_p,ctypes.POINTER(ctypes.c_char_p),ctypes.c_size_t,
                               u8p,ctypes.c_size_t,ctypes.POINTER(ctypes.c_size_t)],ctypes.c_int),
            "sequence_through": ([p,ctypes.c_char_p,ctypes.POINTER(ctypes.c_char_p),ctypes.c_size_t,
                                  u8p,ctypes.c_size_t,ctypes.POINTER(ctypes.c_size_t)],ctypes.c_int),
            "sequence_default": ([p,ctypes.c_int,ctypes.c_int64,ctypes.c_char_p,
                                  u8p,ctypes.c_size_t,ctypes.POINTER(ctypes.c_size_t)],ctypes.c_int),
            "sequence_match": ([p,ctypes.c_char_p,ctypes.c_uint64,u8p,ctypes.c_size_t,
                                ctypes.POINTER(ctypes.c_size_t)],ctypes.c_int),
            "sequence_reverse_match": ([p,ctypes.c_char_p,ctypes.c_char_p,ctypes.c_char_p,
                                        ctypes.c_uint64,ctypes.c_uint64,u8p,ctypes.c_size_t,
                                        ctypes.POINTER(ctypes.c_size_t)],ctypes.c_int),
            "sequence_track_subjects": ([p,ctypes.POINTER(_SequenceDetection),ctypes.c_size_t,ctypes.c_char_p,
                                         ctypes.c_int64,ctypes.c_double,ctypes.c_double,u8p,ctypes.c_size_t,
                                         ctypes.POINTER(ctypes.c_size_t)],ctypes.c_int),
        }
        for name, (arguments, result) in declarations.items():
            function = getattr(self._native, "iisc_canvas_" + name)
            function.argtypes, function.restype = arguments, result

    @classmethod
    def raster(cls, width: int, height: int, argb: int = 0, *, library=None):
        """Create a finite document with a `pixels` asset and `layer` layer."""
        if (not isinstance(width, int) or not isinstance(height, int) or
                width <= 0 or height <= 0 or width > 0x7fffffff or height > 0x7fffffff or
                not isinstance(argb, int) or not 0 <= argb <= 0xffffffff):
            raise ValueError("Invalid raster dimensions or ARGB value")
        canvas = cls(library)
        canvas._handle = canvas._native.iisc_canvas_raster(width, height, argb)
        if not canvas._handle:
            canvas.close()
            raise ValueError("Could not allocate the requested raster")
        return canvas

    @classmethod
    def from_channel(cls, coverage, extent, *, library=None):
        """Create an independent opaque grayscale document with fresh history."""
        width,height=cls._extent(extent)
        values=bytes(coverage)
        if width*height>16777216 or len(values)!=width*height:
            raise ValueError("Invalid or excessive channel coverage")
        data=(ctypes.c_uint8*len(values)).from_buffer_copy(values)
        canvas=cls(library)
        canvas._handle=canvas._native.iisc_canvas_from_channel(width,height,data,len(values))
        if not canvas._handle:
            canvas.close()
            raise ValueError("Could not create the channel document")
        return canvas

    @classmethod
    def from_bytes(cls, document: bytes, *, library=None):
        """Validate and own a detached copy of canonical IISC bytes."""
        data = bytes(document)
        canvas = cls(library)
        buffer = (ctypes.c_uint8 * len(data)).from_buffer_copy(data)
        canvas._handle = canvas._native.iisc_canvas_decode(buffer, len(data))
        if not canvas._handle:
            canvas.close()
            raise ValueError("Invalid or unsupported IISC document")
        return canvas

    def _open(self):
        if not self._handle:
            raise RuntimeError("Canvas is closed")
        return self._handle

    def clone(self):
        """Return an independent document with copied metadata and fresh history."""
        source=self._open()
        copy=type(self)(self._native._name)
        copy._handle=self._native.iisc_canvas_clone(source)
        if not copy._handle:
            message=self._native.iisc_canvas_error(source)
            copy.close()
            raise RuntimeError(message.decode("utf-8",errors="replace") if message else "Document clone failed")
        return copy

    def resize_canvas(self, width, height, *, anchor="center"):
        """Resize the canvas without resampling, as one undoable change."""
        anchors=("top_left","top","top_right","left","center","right",
                 "bottom_left","bottom","bottom_right")
        if (anchor not in anchors or any(not isinstance(v,int) or not 0<v<=0x7fffffff
                                        for v in (width,height))):
            raise ValueError("Invalid canvas extent or anchor")
        self._call("resize",width,height,anchors.index(anchor))

    def crop_canvas(self, x, y, width, height):
        """Crop the canvas while retaining outside source content for undo."""
        if (any(not isinstance(v,int) or not -(2**31)<=v<2**31 for v in (x,y,width,height)) or
                width<=0 or height<=0):
            raise ValueError("Invalid canvas crop")
        self._call("crop",x,y,width,height)

    def resample_document(self, width, height, *, mode="preserve_source"):
        """Scale the document; pixel replacement rejects unsupported source configurations."""
        modes={"preserve_source":0,"replace_pixels":1}
        if any(not isinstance(v,int) or not 0<v<=0x7fffffff for v in (width,height)):
            raise ValueError("Invalid resampling extent")
        if mode not in modes: raise ValueError("Invalid resampling mode")
        self._call("resample",width,height,modes[mode])

    def rotate_image(self, degrees):
        """Rotate the document clockwise about its center as one undoable change."""
        self._call("rotate",float(degrees))

    def content_bounds(self, *, scope="visible", region=None, frames=(), threshold=0, max_pixels=64*1024*1024):
        """Query rendered alpha; all_layers requires an explicit world-space region."""
        scopes={"visible":0,"all_layers":1}
        if scope not in scopes or (scope=="all_layers" and region is None): raise ValueError("Invalid bounds scope or region")
        rect=(0,0,0,0) if region is None else tuple(region)
        if (len(rect)!=4 or any(not isinstance(v,int) or not -(2**31)<=v<2**31 for v in rect) or
            (region is not None and (rect[2]<=0 or rect[3]<=0)) or
            not isinstance(threshold,int) or not 0<=threshold<=255 or
            not isinstance(max_pixels,int) or not 0<max_pixels<=ctypes.c_size_t(-1).value):
            raise ValueError("Invalid content bounds request")
        values=[self._frame(frame) for frame in frames]; native=(ctypes.c_uint32*len(values))(*values)
        output=_CanvasBounds()
        self._call("content_bounds",scopes[scope],int(region is not None),*rect,native,len(values),
                   threshold,max_pixels,ctypes.byref(output))
        return (output.x,output.y,output.width,output.height) if output.present else None

    def pixel_aspect_ratio(self):
        """Return the persisted exact (width, height) pixel aspect ratio."""
        numerator=ctypes.c_uint32(); denominator=ctypes.c_uint32()
        self._call("pixel_aspect",ctypes.byref(numerator),ctypes.byref(denominator))
        return numerator.value,denominator.value

    def set_pixel_aspect_ratio(self,numerator,denominator):
        """Change pixel aspect as one undoable rational metadata edit."""
        if any(not isinstance(v,int) or not 0<v<=0xffffffff for v in (numerator,denominator)):
            raise ValueError("Pixel aspect requires positive 32-bit integers")
        self._call("set_pixel_aspect",numerator,denominator)

    def render_display(self, *, frame=0, sampling="smooth"):
        """Return (width, height, ARGB pixels) at square-pixel display proportions."""
        modes={"nearest":0,"smooth":1}
        if sampling not in modes: raise ValueError("Invalid display sampling")
        width=ctypes.c_int32();height=ctypes.c_int32()
        self._call("display",self._frame(frame),modes[sampling],None,0,ctypes.byref(width),ctypes.byref(height))
        output=(ctypes.c_uint32*(width.value*height.value))()
        self._call("display",self._frame(frame),modes[sampling],output,len(output),ctypes.byref(width),ctypes.byref(height))
        return width.value,height.value,list(output)

    def _call(self, name, *arguments):
        handle = self._open()
        if not getattr(self._native, "iisc_canvas_" + name)(handle, *arguments):
            message = self._native.iisc_canvas_error(handle)
            raise RuntimeError(message.decode("utf-8", errors="replace") if message else "Native canvas error")

    @staticmethod
    def _identifier(value):
        if not isinstance(value, str) or not value or "\0" in value:
            raise ValueError("Identifier must be a nonempty string without NUL")
        return value.encode("utf-8")

    @staticmethod
    def _frame(frame):
        if not isinstance(frame, int) or not 0 <= frame <= 0xffffffff:
            raise ValueError("Frame must be an unsigned 32-bit integer")
        return frame

    @staticmethod
    def _text(value):
        if not isinstance(value,str) or "\0" in value:
            raise ValueError("Text must be a string without NUL")
        return value.encode("utf-8")

    def _json(self, operation, *arguments):
        size=ctypes.c_size_t()
        self._call(operation,*arguments,None,0,ctypes.byref(size))
        output=(ctypes.c_uint8*size.value)()
        self._call(operation,*arguments,output,size.value,ctypes.byref(size))
        return json.loads(bytes(output).decode("utf-8"))

    @staticmethod
    def _sequence_tick(value):
        if not isinstance(value, int) or not -(2**63) <= value < 2**63:
            raise ValueError("Sequence ticks must fit a signed 64-bit integer")
        return value

    @staticmethod
    def _sequence_unsigned(value):
        if not isinstance(value, int) or not 0 <= value < 2**64:
            raise ValueError("Sequence frame or denominator must fit an unsigned 64-bit integer")
        return value

    @classmethod
    def _sequence_ids(cls, values):
        encoded = [cls._identifier(value) for value in values]
        if not encoded or len(encoded) > 65536:
            raise ValueError("At least one and at most 65536 identities are required")
        return (ctypes.c_char_p * len(encoded))(*encoded)

    def sequence_snapshot(self):
        """Return an independent overview of the document-owned sequence project."""
        return self._json("sequence_snapshot")

    def _bind_sequence_model(self):
        """Bind the optional full-model C ABI on the selected native library."""
        p, u8p = ctypes.c_void_p, ctypes.POINTER(ctypes.c_uint8)
        output = [u8p, ctypes.c_size_t, ctypes.POINTER(ctypes.c_size_t)]
        signatures = {
            "model": [p, *output], "model_schema": [p, *output],
            "model_default": [p, ctypes.c_char_p, *output], "media_list": [p, *output],
            "media_data": [p, ctypes.c_char_p, *output],
            "model_replace": [p, u8p, ctypes.c_size_t, ctypes.POINTER(_SequenceOwnedMedia), ctypes.c_size_t],
        }
        for name, arguments in signatures.items():
            function = getattr(self._native, "iisc_canvas_sequence_" + name)
            function.argtypes, function.restype = arguments, ctypes.c_int

    def sequence_project_model(self):
        """Return the complete detached typed tree of the document's project."""
        self._bind_sequence_model()
        return self._json("sequence_model")

    def timeline_default(self, type_name):
        """Return a default value for any documented Timeline aggregate type."""
        self._bind_sequence_model()
        return self._json("sequence_model_default", self._identifier(type_name))

    def sequence_owned_media(self):
        """Return independent source-byte copies and their identities."""
        self._bind_sequence_model()
        items = self._json("sequence_media_list")
        for item in items:
            size = ctypes.c_size_t()
            name = self._identifier(item["id"])
            self._call("sequence_media_data", name, None, 0, ctypes.byref(size))
            output = (ctypes.c_uint8 * size.value)()
            self._call("sequence_media_data", name, output, size.value, ctypes.byref(size))
            item["data"] = bytes(output)
        return items

    def set_sequence_project_model(self, model, *, media):
        """Atomically replace or remove a complete project and its owned bytes.

        Tagged values preserve variants, optionals and binary metadata. The
        native decoder validates all identities, references and size budgets.
        """
        self._bind_sequence_model()
        if model is None:
            encoded = b""
        else:
            if not isinstance(model, dict) or model.get("$type") != "TimelineProject":
                raise ValueError("Expected a TimelineProject model")
            encoded = _timeline_cbor(model, self._json("sequence_model_schema"))
        if len(media) > 65536:
            raise ValueError("Too many owned media objects")
        retained = []
        entries = (_SequenceOwnedMedia * len(media))()
        total = 0
        for index, item in enumerate(media):
            if not isinstance(item, dict) or set(item) != {"id", "media_type", "data"}:
                raise ValueError("Each media object needs id, media_type and data")
            identity = self._identifier(item["id"]); kind = self._text(item["media_type"])
            data = bytes(item["data"]); total += len(data)
            if total > 1024 * 1024 * 1024:
                raise ValueError("Owned media exceed the document byte budget")
            buffer = (ctypes.c_uint8 * len(data)).from_buffer_copy(data)
            retained.extend((identity, kind, buffer))
            entries[index] = _SequenceOwnedMedia(identity, kind, buffer, len(data))
        project_data = (ctypes.c_uint8 * len(encoded)).from_buffer_copy(encoded)
        self._call("sequence_model_replace", project_data, len(encoded), entries, len(entries))

    def create_sequence_project(self, project, sequence, *, duration, time_base, frame_rate, extent, name=""):
        """Create one empty sequence as a single undoable document edit.

        Duration is in the explicitly supplied time base. An existing sequence
        project is never replaced by this operation.
        """
        for pair in (time_base, frame_rate):
            if len(pair) != 2 or any(not isinstance(x, int) or not 0 < x < 2**32 for x in pair):
                raise ValueError("Time base and frame rate require positive 32-bit integer pairs")
        if self._sequence_tick(duration) < 0:
            raise ValueError("Sequence duration cannot be negative")
        width, height = self._extent(extent)
        value = _SequenceCreate(self._identifier(project), self._identifier(sequence), self._text(name),
                                duration, *time_base, *frame_rate, width, height)
        function = self._native.iisc_canvas_sequence_create
        function.argtypes = [ctypes.c_void_p, ctypes.POINTER(_SequenceCreate)]
        function.restype = ctypes.c_int
        self._call("sequence_create", ctypes.byref(value))

    def copy_sequence_from(self, source, *, replace=False):
        """Copy a project's metadata and owned media into this document.

        The source remains independent and need only outlive this call. A
        preexisting target project is replaced only when explicitly requested.
        """
        if not isinstance(source, CanvasDocument) or source._native._handle != self._native._handle:
            raise ValueError("Source must be a document from the same native library")
        if not isinstance(replace, bool):
            raise ValueError("replace must be a boolean")
        function = self._native.iisc_canvas_sequence_copy
        function.argtypes = [ctypes.c_void_p, ctypes.c_void_p, ctypes.c_int]
        function.restype = ctypes.c_int
        self._call("sequence_copy", source._open(), int(replace))

    def edit_sequence_settings(self, sequence, *, name=None, frame_rate=None, extent=None,
                               pixel_aspect=None, audio_rate=None, audio_layout=None, timecode=...):
        """Publish settings as one history entry; None explicitly clears timecode."""
        value = _SequenceSettings()
        if name is not None:
            value.flags |= 1; value.name = self._text(name)
        if frame_rate is not None:
            if len(frame_rate) != 2 or any(not isinstance(x, int) or not 0 < x < 2**32 for x in frame_rate):
                raise ValueError("Frame rate requires two positive 32-bit integers")
            value.flags |= 2; value.rate_num, value.rate_den = frame_rate
        if extent is not None:
            value.flags |= 4; value.width, value.height = self._extent(extent)
        if pixel_aspect is not None:
            if len(pixel_aspect) != 2:
                raise ValueError("Pixel aspect requires a numerator and denominator")
            numerator, denominator = pixel_aspect
            if self._sequence_tick(numerator) <= 0 or self._sequence_unsigned(denominator) == 0:
                raise ValueError("Pixel aspect must be positive")
            value.flags |= 8; value.aspect_num, value.aspect_den = numerator, denominator
        if audio_rate is not None:
            if not isinstance(audio_rate, int) or not 0 <= audio_rate < 2**32:
                raise ValueError("Invalid audio sample rate")
            value.flags |= 16; value.audio_rate = audio_rate
        if audio_layout is not None:
            value.flags |= 32; value.audio_layout = self._text(audio_layout)
        if timecode is not ...:
            value.flags |= 64
            if timecode is not None:
                required = {"start_frame", "frame_rate", "nominal_fps", "counting", "wrap_24_hours"}
                if not isinstance(timecode, dict) or set(timecode) != required:
                    raise ValueError("Timecode must contain all five documented fields")
                rate = timecode["frame_rate"]
                if len(rate) != 2 or any(not isinstance(x, int) or not 0 < x < 2**32 for x in rate):
                    raise ValueError("Timecode frame rate requires positive 32-bit integers")
                nominal = timecode["nominal_fps"]
                counting = {"non_drop_frame": 0, "drop_frame": 1}
                if (not isinstance(nominal, int) or not 0 < nominal < 2**32 or
                        timecode["counting"] not in counting or not isinstance(timecode["wrap_24_hours"], bool)):
                    raise ValueError("Invalid timecode settings")
                value.timecode_present = 1
                value.timecode_start_frame = self._sequence_tick(timecode["start_frame"])
                value.timecode_rate_num, value.timecode_rate_den = rate
                value.timecode_nominal_fps = nominal
                value.timecode_counting = counting[timecode["counting"]]
                value.timecode_wrap_24_hours = timecode["wrap_24_hours"]
        self._call("sequence_settings", self._identifier(sequence), ctypes.byref(value))

    def edit_sequence_tracks(self, sequence, tracks, **changes):
        """Apply the same enabled, lock, mute, solo, sync, role or output changes to tracks."""
        value = _SequenceTrackChanges()
        mapping = {"enabled": (1,"enabled"), "locked": (2,"locked"), "muted": (4,"muted"),
                   "solo": (8,"solo"), "sync_locked": (16,"sync_locked"), "role": (32,"role"),
                   "language": (64,"language"), "blend_mode": (128,"blend_mode"),
                   "output_bus": (256,"output_bus"), "opacity": (512,"opacity")}
        for name, item in changes.items():
            if name not in mapping: raise ValueError("Unknown track setting")
            flag, field = mapping[name]; value.flags |= flag
            if flag <= 16:
                if not isinstance(item, bool): raise ValueError("Track flags must be booleans")
                setattr(value, field, int(item))
            elif flag < 512: setattr(value, field, self._text(item))
            else: setattr(value, field, float(item))
        ids = self._sequence_ids(tracks)
        self._call("sequence_tracks", self._identifier(sequence), ids, len(ids), ctypes.byref(value))

    def add_sequence_track(self, sequence, identifier, *, kind="video"):
        kinds = {"video": 0, "audio": 1, "subtitle": 2, "data": 3}
        if kind not in kinds: raise ValueError("Unknown sequence track kind")
        self._call("sequence_add_track", self._identifier(sequence), self._identifier(identifier), kinds[kind])

    def split_sequence_clips(self, sequence, clips, *, at, links=None):
        """Split clip and link IDs using caller-provided right-hand identities."""
        clip_pairs = [(_SequencePair(self._identifier(a), self._identifier(b))) for a, b in clips.items()]
        link_pairs = [(_SequencePair(self._identifier(a), self._identifier(b))) for a, b in (links or {}).items()]
        clip_array = (_SequencePair * len(clip_pairs))(*clip_pairs)
        link_array = (_SequencePair * len(link_pairs))(*link_pairs)
        self._call("sequence_split", self._identifier(sequence), clip_array, len(clip_array),
                   link_array, len(link_array), self._sequence_tick(at))

    def trim_sequence_clips(self, sequence, ranges):
        values = [_SequenceClipRange(self._identifier(identifier), self._sequence_tick(start), self._sequence_tick(duration))
                  for identifier, (start, duration) in ranges.items()]
        array = (_SequenceClipRange * len(values))(*values)
        self._call("sequence_trim", self._identifier(sequence), array, len(array))

    def extend_sequence_clips(self, sequence, clips, *, edge, playhead):
        edges = {"start": 0, "end": 1}
        if edge not in edges: raise ValueError("Unknown clip edge")
        ids = self._sequence_ids(clips)
        self._call("sequence_extend", self._identifier(sequence), ids, len(ids), edges[edge], self._sequence_tick(playhead))

    def _sequence_range(self, operation, sequence, tracks, start, duration, prefix=None, new_sequence=None):
        ids = self._sequence_ids(tracks) if tracks is not None else None
        self._call("sequence_range", operation, self._identifier(sequence), ids, len(ids) if ids is not None else 0,
                   self._sequence_tick(start), self._sequence_tick(duration),
                   self._identifier(prefix) if prefix is not None else None,
                   self._identifier(new_sequence) if new_sequence is not None else None)

    def lift_sequence_range(self, sequence, tracks, *, start, duration, id_prefix):
        self._sequence_range(0, sequence, tracks, start, duration, id_prefix)

    def extract_sequence_range(self, sequence, tracks, *, start, duration, id_prefix):
        self._sequence_range(1, sequence, tracks, start, duration, id_prefix)

    def close_sequence_gap(self, sequence, tracks, *, start, duration):
        self._sequence_range(2, sequence, tracks, start, duration)

    def extract_subsequence(self, sequence, *, start, duration, new_sequence, mode="copy", id_prefix):
        modes = {"copy": 3, "move": 4}
        if mode not in modes: raise ValueError("Unknown subsequence mode")
        self._sequence_range(modes[mode], sequence, None, start, duration, id_prefix, new_sequence)

    def simplify_sequence(self, sequence, *, remove_empty_tracks=False, remove_disabled_clips=False,
                          remove_gaps=False, flatten_nested=False, new_sequence=None):
        options = (remove_empty_tracks, remove_disabled_clips, remove_gaps, flatten_nested)
        if any(not isinstance(value, bool) for value in options): raise ValueError("Simplification options must be booleans")
        flags = sum((1 << index) for index, selected in enumerate(options) if selected)
        self._call("sequence_simplify", self._identifier(sequence), flags,
                   self._identifier(new_sequence) if new_sequence is not None else None)

    @classmethod
    def _sequence_caption_style(cls, values):
        """Build a complete borrowed C style and retain all backing arrays."""
        defaults = {"font_family": "", "font_size": 0., "fill": (1., 1., 1., 1.),
                    "outline": (0., 0., 0., 1.), "background": (0., 0., 0., 0.),
                    "outline_width": 0., "bold": False, "italic": False, "underline": False,
                    "horizontal_alignment": "", "vertical_alignment": "", "writing_direction": "",
                    "properties": ()}
        if not isinstance(values, dict) or set(values) - set(defaults):
            raise ValueError("Unknown caption style field")
        values = defaults | values
        native = _SequenceCaptionStyle()
        retained = []
        for field in ("font_family", "horizontal_alignment", "vertical_alignment", "writing_direction"):
            encoded = cls._text(values[field]); retained.append(encoded); setattr(native, field, encoded)
        native.font_size = float(values["font_size"])
        native.outline_width = float(values["outline_width"])
        for field in ("fill", "outline", "background"):
            channels = values[field]
            if len(channels) != 4:
                raise ValueError("Caption colors require four components")
            setattr(native, field, (ctypes.c_double * 4)(*(float(x) for x in channels)))
        for field in ("bold", "italic", "underline"):
            if not isinstance(values[field], bool):
                raise ValueError("Caption style flags must be boolean")
            setattr(native, field, values[field])
        properties = values["properties"]
        if len(properties) > 65536:
            raise ValueError("Too many caption style properties")
        kinds = {"null": 0, "bool": 1, "int": 2, "number": 3, "string": 4,
                 "rational": 5, "color": 6, "vector2": 7, "numbers": 8}
        array = (_SequenceProperty * len(properties))()
        retained.append(array)
        for index, item in enumerate(properties):
            if not isinstance(item, dict) or set(item) != {"key", "type", "value"} or item["type"] not in kinds:
                raise ValueError("Invalid caption style property")
            entry = array[index]; encoded = cls._identifier(item["key"]); retained.append(encoded); entry.key = encoded
            entry.kind = kinds[item["type"]]; value = item["value"]
            if entry.kind == 0:
                if value is not None: raise ValueError("Null property requires None")
            elif entry.kind == 1:
                if not isinstance(value, bool): raise ValueError("Boolean property requires bool")
                entry.boolean_value = value
            elif entry.kind == 2:
                if isinstance(value, bool): raise ValueError("Integer property requires int")
                entry.integer_value = cls._sequence_tick(value)
            elif entry.kind == 3:
                entry.components[0] = float(value)
            elif entry.kind == 4:
                encoded = cls._text(value); retained.append(encoded); entry.text = encoded
            elif entry.kind == 5:
                if len(value) != 2: raise ValueError("Rational property requires two integers")
                entry.integer_value = cls._sequence_tick(value[0]); entry.denominator = cls._sequence_unsigned(value[1])
                if entry.denominator == 0: raise ValueError("Rational denominator must be positive")
            elif entry.kind in (6, 7):
                count = 4 if entry.kind == 6 else 2
                if len(value) != count: raise ValueError("Invalid property component count")
                for j in range(count): entry.components[j] = float(value[j])
            else:
                if len(value) > 1000000: raise ValueError("Property array is too large")
                numbers = (ctypes.c_double * len(value))(*(float(x) for x in value))
                retained.append(numbers); entry.numbers = numbers; entry.number_count = len(value)
        native.properties = array; native.property_count = len(array)
        return native, retained

    def edit_caption(self, sequence, track, *, action="upsert", identifier=None, text=None, markup=None,
                     start=0, duration=0, language=None, font=None, font_size=0., style=None):
        """Edit caption content and an optional complete style as one history entry."""
        actions = {"upsert": 0, "remove": 1, "language": 2}
        if action not in actions: raise ValueError("Unknown caption operation")
        arguments = (self._identifier(sequence), self._identifier(track), actions[action],
                     self._identifier(identifier) if identifier is not None else None,
                     self._text(text) if text is not None else None, self._text(markup) if markup is not None else None,
                     self._sequence_tick(start), self._sequence_tick(duration),
                     self._text(language) if language is not None else None,
                     self._text(font) if font is not None else None, float(font_size))
        if style is None:
            self._call("sequence_caption", *arguments)
        else:
            if action != "upsert" or font is not None:
                raise ValueError("A complete style requires upsert and cannot be combined with font")
            value, retained = self._sequence_caption_style(style)
            function = self._native.iisc_canvas_sequence_caption_ex
            function.argtypes = self._native.iisc_canvas_sequence_caption.argtypes + [ctypes.POINTER(_SequenceCaptionStyle)]
            function.restype = ctypes.c_int
            self._call("sequence_caption_ex", *arguments, ctypes.byref(value))

    def place_sequence_transition(self, sequence, track, *, identifier, kind, from_clip=None, to_clip=None,
                                  start, duration, alignment="center"):
        alignments = {"start": 0, "center": 1, "end": 2, "custom": 3}
        if alignment not in alignments: raise ValueError("Unknown transition alignment")
        self._call("sequence_transition", self._identifier(sequence), self._identifier(track),
                   self._identifier(identifier), self._identifier(kind),
                   self._identifier(from_clip) if from_clip is not None else None,
                   self._identifier(to_clip) if to_clip is not None else None,
                   self._sequence_tick(start), self._sequence_tick(duration), alignments[alignment])

    def apply_sequence_reframe(self, sequence, clip, effect, *, keyframes=(), observations=(), selected_subject=None,
                               source_extent=None, target_extent=None, padding=0.):
        """Apply (tick, center_x, center_y, scale) keys or externally tracked boxes."""
        keys = [_SequenceKeyframe(self._sequence_tick(time), float(x), float(y), float(scale))
                for time, x, y, scale in keyframes]
        boxes = [_SequenceObservation(self._sequence_tick(time), self._identifier(subject),
                                      float(x), float(y), float(width), float(height))
                 for time, subject, x, y, width, height in observations]
        key_array = (_SequenceKeyframe * len(keys))(*keys)
        box_array = (_SequenceObservation * len(boxes))(*boxes)
        source = self._extent(source_extent) if source_extent is not None else (0, 0)
        target = self._extent(target_extent) if target_extent is not None else (0, 0)
        self._call("sequence_reframe", self._identifier(sequence), self._identifier(clip), self._identifier(effect),
                   key_array, len(key_array), box_array, len(box_array),
                   self._identifier(selected_subject) if selected_subject is not None else None,
                   *source, *target, float(padding))

    def track_sequence_subjects(self, detections, *, id_prefix, maximum_gap, minimum_confidence=0., minimum_overlap=.25):
        """Associate explicit detector results without choosing or invoking a detector.

        Each input is (tick, class_id, label, confidence, x, y, width, height).
        The caller selects a returned subject and passes its observations to
        apply_sequence_reframe; frame decoding and model execution are external.
        """
        values = []
        for time, class_id, label, confidence, x, y, width, height in detections:
            if not isinstance(class_id, int) or not -(2**31) <= class_id < 2**31:
                raise ValueError("Detection class must fit a signed 32-bit integer")
            values.append(_SequenceDetection(self._sequence_tick(time), class_id, self._text(label),
                                             float(confidence), float(x), float(y), float(width), float(height)))
        array = (_SequenceDetection * len(values))(*values)
        return self._json("sequence_track_subjects", array, len(array), self._identifier(id_prefix),
                          self._sequence_tick(maximum_gap), float(minimum_confidence), float(minimum_overlap))

    def sequence_track_effect_time(self, sequence, track, time):
        """Return exact original effect time, or None for an unmapped interval."""
        if (len(time)!=2 or not isinstance(time[0],int) or not 0<=time[0]<2**63 or
                not isinstance(time[1],int) or not 0<time[1]<2**64):
            raise ValueError("Effect time must be a nonnegative rational")
        fn=self._native.iisc_canvas_sequence_effect_time
        fn.argtypes=[ctypes.c_void_p,ctypes.c_char_p,ctypes.c_char_p,ctypes.c_int64,ctypes.c_uint64,
                     ctypes.POINTER(ctypes.c_int),ctypes.POINTER(ctypes.c_int64),ctypes.POINTER(ctypes.c_uint64)]
        fn.restype=ctypes.c_int
        present=ctypes.c_int();numerator=ctypes.c_int64();denominator=ctypes.c_uint64()
        self._call("sequence_effect_time",self._identifier(sequence),self._identifier(track),time[0],time[1],
                   ctypes.byref(present),ctypes.byref(numerator),ctypes.byref(denominator))
        return (numerator.value,denominator.value) if present.value else None

    def sequence_gaps(self, sequence, tracks):
        ids = self._sequence_ids(tracks)
        return self._json("sequence_gaps", self._identifier(sequence), ids, len(ids))

    def sequence_through_edits(self, sequence, tracks):
        ids = self._sequence_ids(tracks)
        return self._json("sequence_through", self._identifier(sequence), ids, len(ids))

    def default_transition(self, kind, *, duration, type_override=None):
        kinds = {"video": 0, "audio": 1}
        if kind not in kinds: raise ValueError("Unknown transition kind")
        return self._json("sequence_default", kinds[kind], self._sequence_tick(duration),
                          self._identifier(type_override) if type_override is not None else None)

    def match_source_frame(self, sequence, frame):
        return self._json("sequence_match", self._identifier(sequence), self._sequence_unsigned(frame))

    def match_sequence_frames(self, sequence, source, stream, source_frame, *, maximum_frames=1_000_000):
        return self._json("sequence_reverse_match", self._identifier(sequence), self._identifier(source),
                          self._identifier(stream), self._sequence_unsigned(source_frame), self._sequence_unsigned(maximum_frames))

    @staticmethod
    def _extent(value):
        if (len(value) != 2 or any(not isinstance(x, int) or not 0 < x < 2**31 for x in value)):
            raise ValueError("Extent must contain two positive 32-bit integers")
        return value

    def put_fill(self, asset: str, layer: str | None = None, *, kind="solid", color=0xff000000,
                 extent=None, start=(0, 0), end=(1, 0), stops=(), pattern="",
                 pattern_origin=(0, 0), repeat=False):
        """Create or replace an undoable solid, gradient or owned-pattern fill."""
        kinds = {"solid": 0, "linear": 1, "radial": 2, "pattern": 3}
        if kind not in kinds or not isinstance(color, int) or not 0 <= color <= 0xffffffff:
            raise ValueError("Invalid fill kind or ARGB color")
        if extent is None:
            info = self.snapshot(); extent = (info.width, info.height)
        width, height = self._extent(extent)
        if len(start) != 2 or len(end) != 2 or len(pattern_origin) != 2:
            raise ValueError("Fill points require two coordinates")
        if len(stops)>256 or any(not isinstance(c,int) or not 0<=c<=0xffffffff for _,c in stops):
            raise ValueError("Invalid gradient stops")
        native_stops = (_FillStop * len(stops))(*(_FillStop(float(p), c) for p,c in stops))
        spec = _FillSpec(kinds[kind],width,height,bool(repeat),color,*start,*end,*pattern_origin,
                         self._text(pattern),native_stops,len(native_stops))
        self._call("put_fill",self._identifier(asset),None if layer is None else self._identifier(layer),ctypes.byref(spec))

    def put_adjustment(self, asset: str, layer: str | None = None, *, extent=None, **settings):
        """Create or replace an undoable backdrop adjustment in its group."""
        if extent is None:
            info = self.snapshot(); extent = (info.width, info.height)
        width,height = self._extent(extent)
        fields = self._settings(settings)
        self._call("put_adjustment",self._identifier(asset),None if layer is None else self._identifier(layer),
                   width,height,fields,len(fields))

    def set_styles(self, layer: str, effects):
        """Replace the complete ordered style stack as one undoable edit."""
        kinds = {"shadow":0,"inner_shadow":1,"glow":2,"inner_glow":3,"stroke":4,"overlay":5,"bevel":6}
        values=[]
        for effect in effects:
            if len(values)>=64:
                raise ValueError("A layer can have at most 64 styles")
            if set(effect)-{"kind","enabled","color","opacity","radius","offset_x","offset_y"}:
                raise ValueError("Unknown layer style field")
            kind=effect.get("kind"); color=effect.get("color",0xff000000)
            if kind not in kinds or not isinstance(color,int) or not 0<=color<=0xffffffff:
                raise ValueError("Invalid layer style kind or color")
            values.append(_LayerStyle(kinds[kind],bool(effect.get("enabled",True)),color,
                effect.get("opacity",1),effect.get("radius",1),effect.get("offset_x",0),effect.get("offset_y",0)))
        array=(_LayerStyle*len(values))(*values)
        self._call("styles",self._identifier(layer),array,len(values))

    def set_mask(self, layer: str, asset: str | None, *, vector=False, enabled=True, inverted=False,
                 linked=True, channel="luminance", density=1, feather=0, outside=1,
                 transform=(1,0,0,1,0,0)):
        """Set or remove a raster/vector mask without modifying its source asset."""
        channels={"luminance":0,"alpha":1}
        if channel not in channels or len(transform)!=6:
            raise ValueError("Invalid mask channel or transform")
        spec = None if asset is None else _MaskSpec(self._identifier(asset),bool(enabled),bool(inverted),
            bool(linked),channels[channel],density,feather,outside,*transform)
        self._call("mask",self._identifier(layer),bool(vector),None if spec is None else ctypes.byref(spec))

    def slice(self, region, *, layer=None, frame=0, output_extent=None, sampling="nearest"):
        """Return straight-ARGB pixels for a layer or composite world-space crop."""
        if len(region)!=4 or any(not isinstance(v,int) or not -(2**31)<=v<2**31 for v in region):
            raise ValueError("Region must contain four 32-bit integers")
        x,y,width,height=region
        self._extent((width,height))
        ow,oh=self._extent(output_extent if output_extent is not None else (width,height))
        if ow*oh>16777216 or sampling not in ("nearest","smooth"):
            raise ValueError("Invalid slice size or sampling")
        pixels=(ctypes.c_uint32*(ow*oh))()
        self._call("slice",None if layer is None else self._identifier(layer),self._frame(frame),
                   x,y,width,height,ow,oh,int(sampling=="smooth"),pixels,len(pixels))
        return list(pixels)

    def composite(self, region, *, layers=None, frame=0, output_extent=None, sampling="nearest", selection=None):
        """Composite chosen layers, or all visible layers, for an explicit frame.

        Selection, when provided, is one byte per output pixel. Results are
        detached straight-ARGB values; no clipboard or document is changed.
        """
        if len(region)!=4 or any(not isinstance(v,int) or not -(2**31)<=v<2**31 for v in region):
            raise ValueError("Region must contain four 32-bit integers")
        x,y,width,height=region
        self._extent((width,height))
        ow,oh=self._extent(output_extent if output_extent is not None else (width,height))
        if ow*oh>16777216 or sampling not in ("nearest","smooth"):
            raise ValueError("Invalid composite size or sampling")
        ids=None if layers is None else self._layer_ids(layers)
        mask,mask_size=_exchange_mask(selection,ow*oh)
        output=(ctypes.c_uint32*(ow*oh))()
        self._call("composite",ids,0 if ids is None else len(ids),self._frame(frame),x,y,width,height,
                   ow,oh,int(sampling=="smooth"),mask,mask_size,output,len(output))
        return list(output)

    def apply_image(self, layer, source, extent, *, frame=0, source_channel="composite", target_channel="composite",
                    invert=False, blend="normal", opacity=1, selection=None):
        """Apply aligned source pixels only to one layer and frame as one edit.

        The source may be obtained from another canvas and its explicit frame
        using composite(). Shared source assets and other frames remain intact;
        document history enforces layer locks and owns the fresh result.
        """
        source_index,target_index,blend_index=_channel_index(source_channel),_channel_index(target_channel),_blend_index(blend)
        data=_exchange_pixels(source,extent)
        mask,mask_size=_exchange_mask(selection,len(data))
        self._call("apply_image",self._identifier(layer),self._frame(frame),data,len(data),*extent,
                   source_index,target_index,bool(invert),blend_index,
                   float(opacity),mask,mask_size)

    def set_frame_count(self, count):
        """Set the document frame count without changing its rational frame rate."""
        if not isinstance(count,int) or not 0<count<=0xffffffff:
            raise ValueError("Frame count must be a positive 32-bit integer")
        self._call("set_frame_count",count)

    def layer_css(self, layer, *, frame=0):
        """Return CSS declarations and a list of unrepresented layer features."""
        return self._json("layer_css",self._identifier(layer),self._frame(frame))

    def selected_layers_svg(self, layers, region, *, frame=0):
        """Return standalone SVG, rasterization status and projection warnings."""
        if len(region)!=4 or any(not isinstance(v,int) or not -(2**31)<=v<2**31 for v in region):
            raise ValueError("Region must contain four 32-bit integers")
        self._extent(region[2:])
        values=self._layer_ids(layers)
        return self._json("selected_svg",values,len(values),self._frame(frame),*region)

    @classmethod
    def _layer_ids(cls, identifiers):
        values=[cls._identifier(identifier) for identifier in identifiers]
        if not values or len(values)>65536:
            raise ValueError("A nonempty bounded layer selection is required")
        return (ctypes.c_char_p*len(values))(*values)

    @classmethod
    def _layer_geometry(cls, items):
        result=[]
        for identifier,bounds in items:
            if len(bounds)!=4:
                raise ValueError("Bounds must contain x, y, width and height")
            result.append(_LayerGeometry(cls._identifier(identifier),*(float(x) for x in bounds)))
        if not result or len(result)>65536:
            raise ValueError("A nonempty bounded geometry list is required")
        return (_LayerGeometry*len(result))(*result)

    @staticmethod
    def _vector_value(command):
        sizes={"M":2,"L":2,"Q":4,"C":6,"Z":0}; kinds={"M":0,"L":1,"Q":2,"C":3,"Z":4}
        if not command or command[0] not in sizes or len(command)!=sizes[command[0]]+1:
            raise ValueError("Invalid vector command")
        coordinates=[float(value) for value in command[1:]]
        coordinates.extend([0.0]*(6-len(coordinates)))
        return _VectorCommand(kinds[command[0]],*coordinates)

    def arrange_layers(self, layers, action):
        """Change layer stacking order as one lock-aware history operation."""
        actions={"forward":0,"backward":1,"front":2,"back":3}
        if action not in actions: raise ValueError("Unknown arrangement")
        values=self._layer_ids(layers); self._call("arrange",values,len(values),actions[action])

    def align_layers(self, geometry, reference, alignment):
        """Align caller-provided world-space layer bounds to a reference box."""
        modes={"left":0,"center":1,"right":2,"top":3,"middle":4,"bottom":5}
        if alignment not in modes or len(reference)!=4: raise ValueError("Invalid alignment")
        values=self._layer_geometry(geometry)
        self._call("align",values,len(values),*(float(x) for x in reference),modes[alignment])

    def distribute_layers(self, geometry, *, axis="horizontal", mode="gaps"):
        """Distribute centers or gaps, retaining the two outside bounds."""
        axes={"horizontal":0,"vertical":1}; modes={"centers":0,"gaps":1}
        if axis not in axes or mode not in modes: raise ValueError("Invalid distribution")
        values=self._layer_geometry(geometry); self._call("distribute",values,len(values),axes[axis],modes[mode])

    def put_vector(self, asset, layer, commands, *, color=0xff000000):
        """Insert one editable filled vector path as a document-owned layer."""
        if not isinstance(color,int) or not 0<=color<=0xffffffff: raise ValueError("Invalid ARGB color")
        values=[self._vector_value(command) for command in commands]
        if not values or len(values)>1048576: raise ValueError("Invalid vector command count")
        array=(_VectorCommand*len(values))(*values)
        self._call("put_vector",self._identifier(asset),self._identifier(layer),array,len(array),color)

    def combine_shapes(self, assets, result_asset, result_layer, *, operation="union", color=0xff000000, transforms=None):
        """Own exact source curves and their Boolean expression in a new layer."""
        kinds={"union":0,"subtract":1,"intersect":2,"xor":3}
        identifiers=list(assets)
        if operation not in kinds or len(identifiers)<2 or len(identifiers)>4096 or not isinstance(color,int) or not 0<=color<=0xffffffff:
            raise ValueError("Invalid shape combination")
        transforms=list(transforms) if transforms is not None else [(1,0,0,1,0,0)]*len(identifiers)
        if len(transforms)!=len(identifiers) or any(len(t)!=6 for t in transforms): raise ValueError("Invalid shape transforms")
        values=(_ShapeOperand*len(identifiers))(*(_ShapeOperand(self._identifier(identifier),*(float(x) for x in t))
                                                      for identifier,t in zip(identifiers,transforms)))
        self._call("combine_shapes",values,len(values),kinds[operation],self._identifier(result_asset),self._identifier(result_layer),color)

    def vector_command(self, asset, path, index):
        """Read one original vector command without rounding its coefficients."""
        if not isinstance(path,int) or not isinstance(index,int) or path<0 or index<0: raise ValueError("Invalid command index")
        value=_VectorCommand(); self._call("vector_command",self._identifier(asset),path,index,ctypes.byref(value))
        kinds=[("M",2),("L",2),("Q",4),("C",6),("Z",0)]; name,size=kinds[value.kind]
        return (name,*(value.x1,value.y1,value.x2,value.y2,value.x3,value.y3)[:size])

    def edit_vector_command(self, asset, path, index, command):
        """Replace one original path command as a lock-aware undoable edit."""
        if not isinstance(path,int) or not isinstance(index,int) or path<0 or index<0: raise ValueError("Invalid command index")
        value=self._vector_value(command)
        self._call("edit_vector_command",self._identifier(asset),path,index,ctypes.byref(value))

    def merge_layers(self, layers, result_layer, asset_prefix):
        """Verify and merge selected layers across every timeline frame."""
        values=self._layer_ids(layers)
        self._call("merge",values,len(values),self._identifier(result_layer),self._identifier(asset_prefix))

    def flatten(self, result_layer, asset_prefix):
        """Flatten all visible frames and remove hidden layers as one edit."""
        self._call("flatten",self._identifier(result_layer),self._identifier(asset_prefix))

    def save_selection(self, identifier: str, name: str, width: int, height: int,
                       coverage: bytes, *, origin=(0,0)):
        """Save or replace a document-coordinate 8-bit selection as one edit."""
        alpha=bytes(coverage)
        if (not isinstance(width,int) or not isinstance(height,int) or width<=0 or height<=0 or
                width>0x7fffffff or height>0x7fffffff or len(alpha)!=width*height or
                len(origin)!=2 or any(not isinstance(v,int) or not -(2**31)<=v<2**31 for v in origin)):
            raise ValueError("Invalid selection extent, origin or coverage")
        data=(ctypes.c_uint8*len(alpha)).from_buffer_copy(alpha)
        self._call("selection_set",self._identifier(identifier),self._text(name),*origin,width,height,data,len(alpha))

    def rename_selection(self, identifier: str, name: str):
        """Rename an existing channel as an undoable edit."""
        self._call("selection_rename",self._identifier(identifier),self._text(name))

    def delete_selection(self, identifier: str):
        """Delete one named selection channel as an undoable edit."""
        self._call("selection_delete",self._identifier(identifier))

    def selection_coverage(self, identifier: str, x: int, y: int) -> int:
        """Read exact 0–255 channel coverage in document coordinates."""
        if any(not isinstance(v,int) or not -(2**31)<=v<2**31 for v in (x,y)):
            raise ValueError("Coordinates must be signed 32-bit integers")
        value=ctypes.c_uint8()
        self._call("selection_coverage",self._identifier(identifier),x,y,ctypes.byref(value))
        return value.value

    def set_group(self, identifier: str, name: str, *, parent: str = "",
                  mode: str = "pass_through", visible: bool = True):
        """Create or update one nested pass-through or isolated group."""
        if mode not in ("pass_through","isolated") or not isinstance(visible,bool):
            raise ValueError("Invalid group mode or visibility")
        self._call("group_set",self._identifier(identifier),self._text(name),self._text(parent),
                   int(mode=="isolated"),int(visible))

    def delete_group(self, identifier: str):
        """Remove an empty group; children and memberships must be moved first."""
        self._call("group_delete",self._identifier(identifier))

    def assign_group(self, layer: str, group: str = ""):
        """Assign a layer to a group, or move it to the root with an empty group."""
        self._call("group_assign",self._identifier(layer),self._text(group))

    def link_layers(self, first: str, second: str, *, linked: bool = True):
        """Add or remove an undirected link without propagating edits."""
        if not isinstance(linked,bool): raise ValueError("linked must be boolean")
        self._call("link",self._identifier(first),self._identifier(second),int(linked))

    def set_layer_lock(self, identifier: str, flags: LayerLocks, *, group: bool = False):
        """Set direct flags on a layer or inherited flags on a group."""
        if not isinstance(flags,int) or not 0<=flags<=int(LayerLocks.ALL) or not isinstance(group,bool):
            raise ValueError("Invalid lock flags or group selector")
        self._call("lock",self._identifier(identifier),int(group),int(flags))

    def relationships(self):
        """Return a detached dictionary of selections, groups, links and locks."""
        return self._json("relations")

    def query_layers(self, *, name_contains: str = "", visible: bool | None = None,
                     frame: int | None = None, role: str | None = None, group: str | None = None):
        """Query layers in document order using the native revision-aware index."""
        if visible is not None and not isinstance(visible,bool): raise ValueError("visible must be boolean or None")
        if role not in (None,"artwork","control_net"): raise ValueError("Unknown layer role")
        return self._json("query",self._text(name_contains),-1 if visible is None else int(visible),
                          -1 if frame is None else self._frame(frame),
                          -1 if role is None else int(role=="control_net"),None if group is None else self._text(group))

    def _source(self, source):
        if not isinstance(source, CanvasDocument) or source._native._handle != self._native._handle:
            raise ValueError("Source must use the same native bridge")
        return source._open()

    def snapshot(self) -> CanvasSnapshot:
        """Return metadata and cursor counts detached from subsequent edits."""
        info = _CanvasInfo()
        self._call("info", ctypes.byref(info))
        return CanvasSnapshot(*(getattr(info, name) for name, _ in info._fields_))

    def to_bytes(self) -> bytes:
        """Encode the complete document, including its owned child documents."""
        size = ctypes.c_size_t()
        self._call("encode", None, 0, ctypes.byref(size))
        output = (ctypes.c_uint8 * size.value)()
        self._call("encode", output, size.value, ctypes.byref(size))
        return bytes(output)

    def pixel(self, x: int, y: int, frame: int = 0) -> int:
        """Render a frame and read one straight-ARGB document-coordinate pixel."""
        if not isinstance(x, int) or not isinstance(y, int) or not -(2**31) <= x < 2**31 or not -(2**31) <= y < 2**31:
            raise ValueError("Pixel coordinates must be signed 32-bit integers")
        value = ctypes.c_uint32()
        self._call("pixel", self._frame(frame), x, y, ctypes.byref(value))
        return value.value

    def embed(self, source, asset_id: str, layer_id: str, frame: int = 0):
        """Copy a source document into a new owned asset and history-tracked layer."""
        self._call("embed", self._source(source), self._identifier(asset_id), self._identifier(layer_id), self._frame(frame))

    def replace_linked(self, source, asset_id: str, frame: int = 0):
        """Replace the shared owned copy as one undoable history entry."""
        self._call("replace", self._source(source), self._identifier(asset_id), self._frame(frame))

    def undo(self): self._call("undo")
    def redo(self): self._call("redo")
    def capture_last_edit(self): self._call("capture_last")
    def toggle_last_edit(self): self._call("toggle_last")

    def begin_reeditable(self, asset_id: str, maximum_bytes: int = 256 * 1024 * 1024):
        """Capture original raster pixels and reserve the logical buffer budget."""
        if not isinstance(maximum_bytes, int) or not 0 <= maximum_bytes <= ctypes.c_size_t(-1).value:
            raise ValueError("Memory budget must fit size_t")
        self._call("begin_reedit", self._identifier(asset_id), maximum_bytes)

    @staticmethod
    def _settings(values):
        names = [CanvasDocument._identifier(name) for name in values]
        return (_CanvasSetting * len(names))(*(_CanvasSetting(name, float(value))
                                                for name, value in zip(names, values.values())))

    def reedit_adjustment(self, **settings):
        """Apply complete ColorAdjustment fields from the captured original."""
        values = self._settings(settings)
        self._call("adjust", values, len(values))

    def reedit_effect(self, kind: int, **settings):
        """Apply RasterEffect fields and kind from the captured original."""
        if not isinstance(kind, int) or not 0 <= kind <= 0x7fffffff:
            raise ValueError("Effect kind must be a nonnegative 32-bit integer")
        values = self._settings(settings)
        self._call("effect", kind, values, len(values))

    def close(self):
        """Release the owned native document and its session-only history."""
        if self._handle:
            self._native.iisc_canvas_destroy(self._handle)
            self._handle = None
        if self._dll_directory is not None:
            self._dll_directory.close()
            self._dll_directory = None

    def __enter__(self):
        self._open()
        return self

    def __exit__(self, *_exc): self.close()
    def __copy__(self): raise TypeError("A canvas owns its native state and cannot be copied")
    def __deepcopy__(self, _memo): raise TypeError("A canvas owns its native state and cannot be copied")
    def __del__(self):
        try:
            self.close()
        except Exception:
            pass
