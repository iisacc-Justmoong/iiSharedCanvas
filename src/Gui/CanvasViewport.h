#pragma once

#include "Document/Document.h"

#include <iiGUI/EditorViewports.h>

#include <expected>

namespace iiSharedCanvas {

enum class CanvasViewportAction { Reset, Fit, Pan, Zoom };
enum class CanvasViewportError { InvalidValue };

// Detached command data. A fit uses region and surface dimensions; pan uses
// screen-space deltas; zoom uses a positive factor and screen-space anchor.
struct CanvasViewportRequest {
    CanvasViewportAction action = CanvasViewportAction::Reset;
    CanvasRegion region;
    double width = 0, height = 0;
    double deltaX = 0, deltaY = 0;
    double factor = 1, anchorX = 0, anchorY = 0;
    double minimumZoom = .01, maximumZoom = 256;
};

// Calculates a viewport transition without touching a widget, editor or shared
// state. The owner controls publication, ordering and redraw. Calls are safe to
// run concurrently when their borrowed inputs are not modified.
class CanvasViewportPlanner final {
public:
    // Returns identity, fit, pan or anchor-preserving zoom. Fit accounts for
    // document origin, viewport rotation and reflection. Invalid input fails
    // without changing either borrowed argument or publishing partial state.
    [[nodiscard]] std::expected<ii::gui::EditorCanvasViewportConfiguration, CanvasViewportError> execute(
        const ii::gui::EditorCanvasViewportConfiguration &viewport,
        const CanvasViewportRequest &request) const noexcept;
};

} // namespace iiSharedCanvas
