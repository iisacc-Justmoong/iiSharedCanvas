#include "Gui/CanvasInteraction.h"

#include <algorithm>
#include <cmath>

namespace iiSharedCanvas {

std::expected<CanvasInteractionResult, CanvasInteractionError> CanvasInteractionRouter::execute(
    const CanvasInteractionState &state, const ii::gui::EditorCanvasViewportConfiguration &viewport,
    const CanvasInteractionOptions &options, const CanvasInputEvent &event) const noexcept
{
    const auto finiteCoordinate = [](double value) { return std::isfinite(value) && std::abs(value) <= 1e15; };
    const auto validMode = state.mode == CanvasGestureMode::Idle || state.mode == CanvasGestureMode::Stroke
        || state.mode == CanvasGestureMode::Pan;
    const auto validTool = options.tool == CanvasTool::Brush || options.tool == CanvasTool::Eraser
        || options.tool == CanvasTool::Hand || options.tool == CanvasTool::None;
    const auto validDevice = event.pointer.device == PointerDeviceKind::Mouse
        || event.pointer.device == PointerDeviceKind::Tablet || event.pointer.device == PointerDeviceKind::Touch;
    const auto validStateDevice = state.device == PointerDeviceKind::Mouse
        || state.device == PointerDeviceKind::Tablet || state.device == PointerDeviceKind::Touch;
    const auto validPhase = event.pointer.phase == PointerEventPhase::Press
        || event.pointer.phase == PointerEventPhase::Move || event.pointer.phase == PointerEventPhase::Release
        || event.pointer.phase == PointerEventPhase::Cancel;
    if (!validMode || !validTool || !validDevice || !validStateDevice || !validPhase
        || (event.kind != CanvasInputKind::Pointer && event.kind != CanvasInputKind::Scroll)
        || !finiteCoordinate(event.screenX) || !finiteCoordinate(event.screenY)
        || !finiteCoordinate(event.pointer.documentPosition.x) || !finiteCoordinate(event.pointer.documentPosition.y)
        || !finiteCoordinate(state.lastScreenX) || !finiteCoordinate(state.lastScreenY)
        || !std::isfinite(event.pointer.pressure) || event.pointer.pressure < 0 || event.pointer.pressure > 1
        || !std::isfinite(event.pointer.tiltX) || !std::isfinite(event.pointer.tiltY)
        || !std::isfinite(event.pointer.rotationRadians) || !std::isfinite(event.pointer.time)
        || !std::isfinite(event.scrollY) || !std::isfinite(event.scrollX)
        || !std::isfinite(viewport.scale) || viewport.scale < .0001 || viewport.scale > 8192
        || !finiteCoordinate(viewport.offset_x) || !finiteCoordinate(viewport.offset_y)
        || !std::isfinite(viewport.rotation_radians) || std::abs(viewport.rotation_radians) > 1e6
        || !std::isfinite(options.minimumZoom) || options.minimumZoom < .0001
        || !std::isfinite(options.maximumZoom) || options.maximumZoom > 8192
        || options.maximumZoom < options.minimumZoom || !std::isfinite(options.scrollSensitivity)
        || options.scrollSensitivity < 0 || options.scrollSensitivity > 1)
        return std::unexpected(CanvasInteractionError::InvalidValue);

    CanvasInteractionResult result{.state = state, .viewport = viewport};
    if (event.kind == CanvasInputKind::Scroll) {
        if (event.scrollY == 0 || options.scrollSensitivity == 0
            || (event.scrollY > 0 && viewport.scale == options.maximumZoom)
            || (event.scrollY < 0 && viewport.scale == options.minimumZoom)) return result;
        // Compute in log space to clamp large wheel deltas without overflow.
        const auto exponent = std::clamp(std::log(viewport.scale) + event.scrollY * options.scrollSensitivity,
                                         std::log(options.minimumZoom), std::log(options.maximumZoom));
        const auto next = std::clamp(std::exp(exponent), options.minimumZoom, options.maximumZoom);
        if (next != viewport.scale) {
            const auto factor = next / viewport.scale;
            result.viewport.scale = next;
            result.viewport.offset_x = event.screenX - (event.screenX - viewport.offset_x) * factor;
            result.viewport.offset_y = event.screenY - (event.screenY - viewport.offset_y) * factor;
            if (!finiteCoordinate(result.viewport.offset_x) || !finiteCoordinate(result.viewport.offset_y))
                return std::unexpected(CanvasInteractionError::InvalidValue);
            result.viewportChanged = true;
        }
        return result;
    }
    if (state.mode != CanvasGestureMode::Idle && (state.device != event.pointer.device
        || (state.deviceId && event.deviceId && state.deviceId != event.deviceId))) return result;

    if (state.mode == CanvasGestureMode::Stroke) {
        auto pointer = event.pointer;
        result.eraser = state.eraser;
        if (!options.rasterLayerSelected || (options.tool != CanvasTool::Brush && options.tool != CanvasTool::Eraser))
            pointer.phase = PointerEventPhase::Cancel;
        if (pointer.hovering && pointer.phase != PointerEventPhase::Cancel) return result;
        if (pointer.phase == PointerEventPhase::Press) return result;
        if (pointer.device == PointerDeviceKind::Mouse) pointer.pressure = 1;
        result.stroke = pointer;
        if (pointer.phase == PointerEventPhase::Release || pointer.phase == PointerEventPhase::Cancel)
            result.state = {};
        return result;
    }
    if (state.mode == CanvasGestureMode::Pan) {
        if (event.pointer.phase == PointerEventPhase::Release || event.pointer.phase == PointerEventPhase::Cancel) {
            result.state = {};
        } else if (event.pointer.phase == PointerEventPhase::Move && !event.pointer.hovering) {
            result.viewport.offset_x += event.screenX - state.lastScreenX;
            result.viewport.offset_y += event.screenY - state.lastScreenY;
            if (!finiteCoordinate(result.viewport.offset_x) || !finiteCoordinate(result.viewport.offset_y))
                return std::unexpected(CanvasInteractionError::InvalidValue);
            result.viewportChanged = result.viewport.offset_x != viewport.offset_x || result.viewport.offset_y != viewport.offset_y;
            result.state.lastScreenX = event.screenX; result.state.lastScreenY = event.screenY;
        }
        return result;
    }
    if (event.pointer.phase != PointerEventPhase::Press || event.pointer.hovering) return result;
    const bool middle = event.pointer.button == PointerButton::Middle;
    const bool primary = event.pointer.button == PointerButton::Primary || event.pointer.primaryButtonDown;
    if (middle || (options.tool == CanvasTool::Hand && primary)) {
        result.state = {CanvasGestureMode::Pan, event.deviceId, event.screenX, event.screenY, false, event.pointer.device};
    } else if (options.rasterLayerSelected && (options.tool == CanvasTool::Brush || options.tool == CanvasTool::Eraser)
               && (primary || event.pointer.device == PointerDeviceKind::Tablet
                   || event.pointer.device == PointerDeviceKind::Touch)) {
        result.eraser = options.tool == CanvasTool::Eraser || event.pointer.eraserActive;
        result.state = {CanvasGestureMode::Stroke, event.deviceId, event.screenX, event.screenY, result.eraser, event.pointer.device};
        result.stroke = event.pointer;
        if (result.stroke->device == PointerDeviceKind::Mouse) result.stroke->pressure = 1;
    }
    return result;
}

} // namespace iiSharedCanvas
