#include <Gui/CanvasInteraction.h>

#include <cmath>
#include <iostream>
#include <limits>
#include <stdexcept>

namespace {
void require(bool value, const char *message) { if (!value) throw std::runtime_error(message); }
bool near(double a, double b) { return std::abs(a - b) < 1e-10; }
}

int main()
{
    try {
        using namespace iiSharedCanvas;
        CanvasInteractionRouter router;
        CanvasInteractionState state;
        CanvasInteractionOptions options; options.rasterLayerSelected = true;
        ii::gui::EditorCanvasViewportConfiguration view;
        CanvasInputEvent event;
        event.pointer.device = PointerDeviceKind::Mouse;
        event.pointer.phase = PointerEventPhase::Press;
        event.pointer.button = PointerButton::Middle;
        event.screenX = 10; event.screenY = 20;
        auto result = router.execute(state, view, options, event);
        require(result && result->state.mode == CanvasGestureMode::Pan && !result->stroke,
                "middle press must start pan, not a stroke");
        state = result->state; event.pointer.phase = PointerEventPhase::Move;
        event.screenX = 13; event.screenY = 18;
        result = router.execute(state, view, options, event);
        require(result && result->viewportChanged && result->viewport.offset_x == 3
                    && result->viewport.offset_y == -2, "middle drag must pan in screen coordinates");
        state = result->state; view = result->viewport;
        event.pointer.phase = PointerEventPhase::Cancel;
        result = router.execute(state, view, options, event);
        require(result && result->state.mode == CanvasGestureMode::Idle && !result->stroke,
                "cancelling a pan must end it without a stroke");

        view = {.scale = 2, .offset_x = 10, .offset_y = -5, .rotation_radians = .7, .flip_x = true};
        event.kind = CanvasInputKind::Scroll; event.screenX = 60; event.screenY = 35;
        event.scrollY = 120;
        result = router.execute({}, view, options, event);
        require(result && result->viewportChanged && result->viewport.scale > view.scale,
                "wheel must zoom");
        const auto ratio = result->viewport.scale / view.scale;
        require(near((event.screenX - result->viewport.offset_x) / ratio, event.screenX - view.offset_x)
                    && near((event.screenY - result->viewport.offset_y) / ratio, event.screenY - view.offset_y),
                "zoom must retain its pointer anchor under rotation and flipping");
        view.scale = 256;
        result = router.execute({}, view, options, event);
        require(result && !result->viewportChanged && result->viewport.scale == 256,
                "zoom must clamp to the prior canvas bounds");

        view = {}; event = {}; event.pointer.device = PointerDeviceKind::Tablet;
        event.pointer.tool = PointerToolKind::Eraser; event.pointer.eraserActive = true;
        event.pointer.phase = PointerEventPhase::Press; event.pointer.pressure = .35;
        event.pointer.documentPosition = {12, 14}; event.deviceId = 7;
        result = router.execute({}, view, options, event);
        require(result && result->stroke && result->stroke->phase == PointerEventPhase::Press
                    && result->stroke->pressure == .35 && result->eraser
                    && result->state.mode == CanvasGestureMode::Stroke,
                "tablet eraser must start an eraser stroke with its pressure");
        state = result->state; event.pointer.phase = PointerEventPhase::Move; event.deviceId = 8;
        result = router.execute(state, view, options, event);
        require(result && !result->stroke && result->state.deviceId == 7,
                "another device must not change the active gesture");
        event.deviceId = 7; event.pointer.device = PointerDeviceKind::Mouse;
        result = router.execute(state, view, options, event);
        require(result && !result->stroke, "a different device kind must not finish a tablet gesture");
        event.pointer.device = PointerDeviceKind::Tablet;
        event.deviceId = 7; event.pointer.phase = PointerEventPhase::Release;
        result = router.execute(state, view, options, event);
        require(result && result->stroke && result->stroke->phase == PointerEventPhase::Release
                    && result->state.mode == CanvasGestureMode::Idle && result->eraser,
                "matching eraser release must finish its stroke");

        event = {}; event.pointer.device = PointerDeviceKind::Mouse;
        event.pointer.phase = PointerEventPhase::Press; event.pointer.button = PointerButton::Primary;
        event.pointer.primaryButtonDown = true; event.pointer.pressure = .2;
        result = router.execute({}, view, options, event);
        require(result && result->stroke && result->stroke->pressure == 1,
                "mouse strokes must use full pressure");
        state = result->state; options.rasterLayerSelected = false;
        event.pointer.phase = PointerEventPhase::Move;
        result = router.execute(state, view, options, event);
        require(result && result->stroke && result->stroke->phase == PointerEventPhase::Cancel
                    && result->state.mode == CanvasGestureMode::Idle,
                "loss of selection must cancel an active stroke");
        event.screenX = std::numeric_limits<double>::quiet_NaN();
        require(!router.execute({}, view, options, event), "nonfinite input must be rejected");
        event.screenX = 0; event.pointer.tiltX = std::numeric_limits<double>::quiet_NaN();
        require(!router.execute({}, view, options, event), "nonfinite pointer data must be rejected");
        return 0;
    } catch (const std::exception &error) {
        std::cerr << error.what() << '\n';
        return 1;
    }
}
