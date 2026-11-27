#include <Gui/CanvasPresentation.h>

#include <cmath>
#include <cstdint>
#include <iostream>
#include <limits>
#include <stdexcept>

namespace {
void require(bool value, const char *message)
{
    if (!value) throw std::runtime_error(message);
}
}

int main()
{
    try {
        using namespace iiSharedCanvas;
        using namespace ii::gui;
        CanvasInputTranslator input;
        EditorCanvasInput event;
        event.phase = EditorCanvasPhase::begin;
        event.document = {20, 30};
        event.native.device = PointerDevice::pen;
        event.native.pressure = .4; event.native.tilt_x = 18; event.native.tilt_y = -27;
        event.native.rotation = 90; event.native.timestamp = 12.5;
        event.native.device_id = 42; event.native.buttons = 1;
        event.native.x = 100; event.native.y = 200;
        event.native.local_position = WidgetPoint{5, 6};
        auto converted = input.execute(event);
        require(converted && converted->kind == CanvasInputKind::Pointer
                    && converted->pointer.phase == PointerEventPhase::Press
                    && converted->pointer.device == PointerDeviceKind::Tablet
                    && converted->pointer.documentPosition.x == 20
                    && converted->pointer.documentPosition.y == 30
                    && converted->pointer.pressure == .4 && converted->pointer.tiltX == .2
                    && converted->pointer.tiltY == -.3 && converted->pointer.time == 12.5
                    && std::abs(converted->pointer.rotationRadians - std::acos(-1.) / 2) < 1e-12
                    && converted->deviceId == 42 && converted->screenX == 5 && converted->screenY == 6,
                "pen event lost document coordinates or device data");
        event.phase = EditorCanvasPhase::hover;
        converted = input.execute(event);
        require(converted && converted->pointer.hovering && converted->pointer.phase == PointerEventPhase::Move,
                "hover must not start a stroke");
        event.phase = EditorCanvasPhase::begin;
        event.native.device = PointerDevice::mouse; event.native.buttons = 4;
        converted = input.execute(event);
        require(converted && converted->pointer.button == PointerButton::Middle && !converted->pointer.primaryButtonDown,
                "middle-button pan must not be reported as a primary brush press");
        event.native.buttons = 2;
        converted = input.execute(event);
        require(converted && converted->pointer.button == PointerButton::Secondary && !converted->pointer.primaryButtonDown,
                "secondary-button input must remain secondary");
        event.phase = EditorCanvasPhase::cancel;
        event.native.device = PointerDevice::eraser; event.native.buttons = 1;
        converted = input.execute(event);
        require(converted && converted->pointer.phase == PointerEventPhase::Cancel
                    && converted->pointer.tool == PointerToolKind::Eraser && converted->pointer.eraserActive
                    && !converted->pointer.primaryButtonDown,
                "eraser cancellation lost its tool or phase");
        event.phase = EditorCanvasPhase::scroll;
        event.native.device = PointerDevice::touch;
        event.native.scroll_x = 2; event.native.scroll_y = -15; event.native.control = true;
        converted = input.execute(event);
        require(converted && converted->kind == CanvasInputKind::Scroll && converted->scrollX == 2
                    && converted->scrollY == -15 && converted->control,
                "scroll must remain an explicit navigation event");
        event.native.pressure = std::numeric_limits<double>::quiet_NaN();
        require(!input.execute(event), "invalid input must be rejected");

        FrameRenderResult frame;
        frame.pixels = makeRasterLayer(2, 2);
        frame.pixels.pixels = {0xffff0000, 0xff00ff00, 0xff0000ff, 0x80112233};
        EditorCanvasQuery query; query.surface = {2, 2};
        CanvasFramePresenter presenter;
        const auto presented = presenter.execute(frame, {{0, 0}, {2, 2}}, query);
        require(presented && (*presented)->images.size() == 1, "frame must produce one iiGUI image");
        const auto image = (*presented)->images.front().image;
        const auto rgba = image->rgba();
        require(image->width() == 2 && image->height() == 2 && rgba[0] == 255 && rgba[1] == 0
                    && rgba[4] == 0 && rgba[5] == 255 && rgba[10] == 255
                    && rgba[12] == 0x11 && rgba[13] == 0x22 && rgba[14] == 0x33 && rgba[15] == 0x80,
                "ARGB to RGBA and alpha conversion changed pixels");
        frame.pixels.pixels[0] = 0;
        require(image->rgba()[0] == 255, "presented frame must own its pixels");
        frame.pixels.pixels[0] = 0xffff0000;
        query.transform.offset_x = 1;
        auto panned = presenter.execute(frame, {{0, 0}, {2, 2}}, query);
        require(panned && (*panned)->images.front().image->rgba()[3] == 0,
                "panning must leave pixels outside the sampled region transparent");
        query.transform.offset_x = 2; query.transform.flip_x = true;
        auto flipped = presenter.execute(frame, {{0, 0}, {2, 2}}, query);
        require(flipped && (*flipped)->images.front().image->rgba()[1] == 255,
                "flipping must use the iiGUI viewport transform");
        query.transform = {.offset_x = 2, .rotation_radians = std::acos(-1.) / 2};
        auto rotated = presenter.execute(frame, {{0, 0}, {2, 2}}, query);
        require(rotated && (*rotated)->images.front().image->rgba()[2] == 255,
                "rotation must use the inverse iiGUI viewport transform");
        query.transform = {};
        query.surface = {4, 4};
        query.transform.scale = 2;
        auto smooth = presenter.execute(frame, {{0, 0}, {2, 2}}, query, RasterSampling::Smooth);
        require(smooth && (*smooth)->images.front().image->width() == 4
                    && (*smooth)->images.front().image->rgba()[3] > 0,
                "smooth scaling must retain alpha coverage");
        require(!presenter.execute(frame, {{0, 0}, {2, 2}}, query, RasterSampling::Nearest, 1),
                "pixel budget must be enforced before allocation");
        return 0;
    } catch (const std::exception &error) {
        std::cerr << error.what() << '\n';
        return 1;
    }
}
