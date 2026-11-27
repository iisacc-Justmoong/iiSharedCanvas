#pragma once

#include "Gui/CanvasPresentation.h"

#include <expected>
#include <optional>

namespace iiSharedCanvas {

enum class CanvasTool { Brush, Eraser, Hand, None };
enum class CanvasGestureMode { Idle, Stroke, Pan };
enum class CanvasInteractionError { InvalidValue };

// A detached, caller-owned description of one in-progress pointer gesture.
// The router never stores it, acquires a lock or retains document access.
struct CanvasInteractionState {
    CanvasGestureMode mode = CanvasGestureMode::Idle;
    std::uint64_t deviceId = 0;
    double lastScreenX = 0, lastScreenY = 0;
    bool eraser = false;
    PointerDeviceKind device = PointerDeviceKind::Mouse;
};

// Current application policy. Selection is explicit so a disappearing layer
// cannot leave an editor transaction active. Zoom bounds use legacy defaults.
struct CanvasInteractionOptions {
    CanvasTool tool = CanvasTool::Brush;
    bool rasterLayerSelected = false;
    double minimumZoom = .01, maximumZoom = 256, scrollSensitivity = .0015;
};

// The caller owns the result and decides when to apply the viewport and stroke.
// An absent stroke means no edit; eraser accompanies every emitted stroke.
struct CanvasInteractionResult {
    CanvasInteractionState state;
    ii::gui::EditorCanvasViewportConfiguration viewport;
    std::optional<PointerEvent> stroke;
    bool viewportChanged = false;
    bool eraser = false;
};

// One pure state transition. No editor, widget, document or ambient resource is
// accessed. Independent calls are thread-safe; the caller serializes gestures.
class CanvasInteractionRouter final {
public:
    // Inputs are borrowed for this call. Invalid, nonfinite or out-of-range
    // values fail atomically; no supplied state is changed. A matching device's
    // cancellation or loss of raster selection emits a stroke cancellation.
    [[nodiscard]] std::expected<CanvasInteractionResult, CanvasInteractionError> execute(
        const CanvasInteractionState &state,
        const ii::gui::EditorCanvasViewportConfiguration &viewport,
        const CanvasInteractionOptions &options,
        const CanvasInputEvent &event) const noexcept;
};

} // namespace iiSharedCanvas
