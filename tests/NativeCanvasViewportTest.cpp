#include <Gui/CanvasViewport.h>

#include <cmath>
#include <iostream>
#include <limits>
#include <stdexcept>

namespace {
void require(bool value, const char *message) { if (!value) throw std::runtime_error(message); }
bool near(double a, double b) { return std::abs(a - b) < 1e-9; }
}

int main()
{
    try {
        using namespace iiSharedCanvas;
        CanvasViewportPlanner planner;
        CanvasViewportRequest request; request.action = CanvasViewportAction::Fit;
        request.region = {{10, 20}, {100, 50}}; request.width = 200; request.height = 200;
        auto result = planner.execute({}, request);
        require(result && near(result->scale, 2) && near(result->offset_x, -20) && near(result->offset_y, 10),
                "fit must account for document origin and center the content");
        ii::gui::EditorCanvasViewportConfiguration rotated;
        rotated.rotation_radians = std::acos(-1.) / 2;
        result = planner.execute(rotated, request);
        require(result && near(result->scale, 2) && near(result->offset_x, 190) && near(result->offset_y, -20),
                "fit must enclose rotated content");
        request.action = CanvasViewportAction::Pan; request.deltaX = 3; request.deltaY = -4;
        result = planner.execute(*result, request);
        require(result && near(result->offset_x, 193) && near(result->offset_y, -24), "pan must use screen pixels");
        const auto previous = *result;
        request.action = CanvasViewportAction::Zoom; request.factor = 3; request.anchorX = 20; request.anchorY = 70;
        result = planner.execute(previous, request);
        require(result && near(result->scale, 6)
                    && near((request.anchorX - result->offset_x) / 3, request.anchorX - previous.offset_x)
                    && near((request.anchorY - result->offset_y) / 3, request.anchorY - previous.offset_y),
                "zoom must keep its anchor fixed despite viewport rotation");
        request.action = CanvasViewportAction::Reset;
        result = planner.execute(*result, request);
        require(result && *result == ii::gui::EditorCanvasViewportConfiguration{}, "reset must restore identity");
        request.action = CanvasViewportAction::Zoom; request.factor = std::numeric_limits<double>::infinity();
        require(!planner.execute({}, request), "non-finite zoom must be rejected");
        return 0;
    } catch (const std::exception &error) {
        std::cerr << error.what() << '\n'; return 1;
    }
}
