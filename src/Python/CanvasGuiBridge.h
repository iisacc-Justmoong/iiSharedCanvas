#pragma once

#include "ProjectedStrokeBridge.h"

#ifdef __cplusplus
extern "C" {
#endif

/* Detached iiGUI input. Phase is begin/update/end/cancel/hover/scroll (0..5);
   device is mouse/pen/eraser/touch (0..3). Angles are iiGUI degrees. */
typedef struct IiscGuiInput {
    int phase, device;
    double x, y, pressure, tilt_x, tilt_y, rotation, timestamp;
    uint64_t device_id;
    uint32_t buttons;
    double scroll_x, scroll_y;
    int control, shift, alt, meta, precise_scroll;
} IiscGuiInput;

/* Caller-owned normalized input. No pointer in either structure is retained. */
typedef struct IiscGuiInputOutput {
    int kind, phase, device, button, hovering, eraser, primary_down;
    double x, y, pressure, tilt_x, tilt_y, rotation_radians, timestamp;
    uint64_t device_id;
    uint32_t buttons, device_state;
    double scroll_x, scroll_y;
    int control, shift, alt, meta, precise_scroll;
} IiscGuiInputOutput;

/* Return zero for invalid input, otherwise write one detached event. */
IISC_PYTHON_EXPORT int iisc_gui_translate(const IiscGuiInput *, IiscGuiInputOutput *);

/* All values are detached and caller owned. mode is idle/stroke/pan (0..2),
   tool is brush/eraser/hand/none (0..3); coordinates are logical pixels. */
typedef struct IiscGuiGestureState {
    int mode;
    uint64_t device_id;
    double last_x, last_y;
    int eraser, device_kind; /* mouse/touch/tablet (0..2) */
} IiscGuiGestureState;

typedef struct IiscGuiViewport {
    double scale, offset_x, offset_y, rotation_radians;
    int flip_x, flip_y;
} IiscGuiViewport;

typedef struct IiscGuiInteractionOptions {
    int tool, raster_selected;
    double minimum_zoom, maximum_zoom, scroll_sensitivity;
} IiscGuiInteractionOptions;

typedef struct IiscGuiInteractionOutput {
    IiscGuiGestureState state;
    IiscGuiViewport viewport;
    int has_stroke, viewport_changed, eraser;
    IiscGuiInputOutput stroke;
} IiscGuiInteractionOutput;

/* Transition one event without accessing an editor or widget. Return zero on
   invalid input and leave output unchanged. All pointers are call-scoped. */
IISC_PYTHON_EXPORT int iisc_gui_route(const IiscGuiInput *, double screen_x, double screen_y,
    const IiscGuiGestureState *, const IiscGuiViewport *, const IiscGuiInteractionOptions *,
    IiscGuiInteractionOutput *);

/* Borrow source ARGB and copy the resulting RGBA into the caller's buffer.
   No document, GUI widget, filesystem or global state is accessed. */
IISC_PYTHON_EXPORT int iisc_gui_present(const uint32_t *pixels, size_t count,
    int32_t source_width, int32_t source_height, int32_t x, int32_t y,
    int32_t region_width, int32_t region_height, double surface_width, double surface_height,
    double scale, double offset_x, double offset_y, double rotation_radians,
    int flip_x, int flip_y, int smooth, uint64_t maximum_pixels,
    uint8_t *output, size_t output_bytes, uint32_t *output_width, uint32_t *output_height);

#ifdef __cplusplus
}
#endif
