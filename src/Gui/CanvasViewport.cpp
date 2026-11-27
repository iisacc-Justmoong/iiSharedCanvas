#include "Gui/CanvasViewport.h"

#include <algorithm>
#include <array>
#include <cmath>
#include <limits>

namespace iiSharedCanvas {

std::expected<ii::gui::EditorCanvasViewportConfiguration, CanvasViewportError> CanvasViewportPlanner::execute(
    const ii::gui::EditorCanvasViewportConfiguration &viewport, const CanvasViewportRequest &request) const noexcept
{
    const auto coordinate = [](double value) { return std::isfinite(value) && std::abs(value) <= 1e15; };
    if (!std::isfinite(viewport.scale) || viewport.scale < .0001 || viewport.scale > 8192
        || !coordinate(viewport.offset_x) || !coordinate(viewport.offset_y)
        || !std::isfinite(viewport.rotation_radians) || std::abs(viewport.rotation_radians) > 1e6
        || !std::isfinite(request.minimumZoom) || request.minimumZoom < .0001
        || !std::isfinite(request.maximumZoom) || request.maximumZoom > 8192
        || request.maximumZoom < request.minimumZoom)
        return std::unexpected(CanvasViewportError::InvalidValue);
    auto result = viewport;
    switch (request.action) {
    case CanvasViewportAction::Reset:
        return ii::gui::EditorCanvasViewportConfiguration{};
    case CanvasViewportAction::Pan:
        if (!coordinate(request.deltaX) || !coordinate(request.deltaY))
            return std::unexpected(CanvasViewportError::InvalidValue);
        result.offset_x += request.deltaX; result.offset_y += request.deltaY;
        break;
    case CanvasViewportAction::Zoom: {
        if (!std::isfinite(request.factor) || request.factor <= 0
            || !coordinate(request.anchorX) || !coordinate(request.anchorY))
            return std::unexpected(CanvasViewportError::InvalidValue);
        const auto target = std::clamp(viewport.scale * request.factor, request.minimumZoom, request.maximumZoom);
        const auto ratio = target / viewport.scale;
        result.scale = target;
        result.offset_x = request.anchorX - (request.anchorX - viewport.offset_x) * ratio;
        result.offset_y = request.anchorY - (request.anchorY - viewport.offset_y) * ratio;
        break;
    }
    case CanvasViewportAction::Fit: {
        if (!std::isfinite(request.width) || !std::isfinite(request.height)
            || request.width <= 0 || request.height <= 0 || request.width > 8192 || request.height > 8192
            || request.region.extent.width <= 0 || request.region.extent.height <= 0)
            return std::unexpected(CanvasViewportError::InvalidValue);
        const auto x0 = double(request.region.origin.x), y0 = double(request.region.origin.y);
        const auto x1 = x0 + request.region.extent.width, y1 = y0 + request.region.extent.height;
        const auto cosine = std::cos(viewport.rotation_radians), sine = std::sin(viewport.rotation_radians);
        const std::array points{ii::gui::EditorPoint{x0, y0}, ii::gui::EditorPoint{x1, y0},
                                ii::gui::EditorPoint{x0, y1}, ii::gui::EditorPoint{x1, y1}};
        auto left = std::numeric_limits<double>::infinity(), top = left;
        auto right = -left, bottom = -left;
        for (const auto point : points) {
            const auto x = point.x * (viewport.flip_x ? -1 : 1);
            const auto y = point.y * (viewport.flip_y ? -1 : 1);
            const auto rx = cosine * x - sine * y, ry = sine * x + cosine * y;
            left = std::min(left, rx); right = std::max(right, rx);
            top = std::min(top, ry); bottom = std::max(bottom, ry);
        }
        const auto width = right - left, height = bottom - top;
        if (!(width > 0 && height > 0) || !std::isfinite(width) || !std::isfinite(height))
            return std::unexpected(CanvasViewportError::InvalidValue);
        result.scale = std::clamp(std::min(request.width / width, request.height / height),
                                  request.minimumZoom, request.maximumZoom);
        result.offset_x = (request.width - width * result.scale) * .5 - left * result.scale;
        result.offset_y = (request.height - height * result.scale) * .5 - top * result.scale;
        break;
    }
    default: return std::unexpected(CanvasViewportError::InvalidValue);
    }
    if (!coordinate(result.offset_x) || !coordinate(result.offset_y))
        return std::unexpected(CanvasViewportError::InvalidValue);
    return result;
}

} // namespace iiSharedCanvas
