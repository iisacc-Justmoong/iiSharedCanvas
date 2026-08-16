#include "Render/FrameRenderer.h"

#include "Validation/Validation.h"

#include <Layer/DrawingSurface.h>
#include <Layer/Layer.h>
#include <Layer/LayerStack.h>
#include <Render/Compositor.h>

#include <algorithm>
#include <array>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <limits>
#include <type_traits>
#include <utility>
#include <vector>

namespace iiSharedCanvas {

namespace {

constexpr int CoverageAxisSamples = 4;
constexpr int CoverageSampleCount = CoverageAxisSamples * CoverageAxisSamples;
constexpr int MaximumCurveSegments = 1024;

struct Contour {
    std::vector<Point> points;
    bool closed = false;
};

struct Bounds {
    double left = std::numeric_limits<double>::infinity();
    double top = std::numeric_limits<double>::infinity();
    double right = -std::numeric_limits<double>::infinity();
    double bottom = -std::numeric_limits<double>::infinity();

    void include(Point point) noexcept
    {
        left = std::min(left, point.x);
        top = std::min(top, point.y);
        right = std::max(right, point.x);
        bottom = std::max(bottom, point.y);
    }

    [[nodiscard]] bool valid() const noexcept
    {
        return left <= right && top <= bottom;
    }
};

std::size_t pixelIndex(int width, int x, int y) noexcept
{
    return static_cast<std::size_t>(y) * static_cast<std::size_t>(width)
        + static_cast<std::size_t>(x);
}

double squaredDistance(Point first, Point second) noexcept
{
    const double dx = second.x - first.x;
    const double dy = second.y - first.y;
    return dx * dx + dy * dy;
}

double distance(Point first, Point second) noexcept
{
    return std::sqrt(squaredDistance(first, second));
}

int curveSegmentCount(double controlPolygonLength) noexcept
{
    if (!std::isfinite(controlPolygonLength)
        || controlPolygonLength >= static_cast<double>(MaximumCurveSegments) * 0.5) {
        return MaximumCurveSegments;
    }
    return std::clamp(static_cast<int>(std::ceil(controlPolygonLength * 2.0)),
                      1,
                      MaximumCurveSegments);
}

int floorToExtent(double value, int extent) noexcept
{
    if (value <= 0.0) {
        return 0;
    }
    if (value >= static_cast<double>(extent)) {
        return extent;
    }
    return static_cast<int>(std::floor(value));
}

int ceilToExtent(double value, int extent) noexcept
{
    if (value <= 0.0) {
        return 0;
    }
    if (value >= static_cast<double>(extent)) {
        return extent;
    }
    return static_cast<int>(std::ceil(value));
}

Point quadraticPoint(Point start, Point control, Point end, double t) noexcept
{
    const double oneMinusT = 1.0 - t;
    return {
        oneMinusT * oneMinusT * start.x
            + 2.0 * oneMinusT * t * control.x
            + t * t * end.x,
        oneMinusT * oneMinusT * start.y
            + 2.0 * oneMinusT * t * control.y
            + t * t * end.y,
    };
}

Point cubicPoint(Point start, Point control1, Point control2, Point end, double t) noexcept
{
    const double oneMinusT = 1.0 - t;
    const double oneMinusTSquared = oneMinusT * oneMinusT;
    const double tSquared = t * t;
    return {
        oneMinusTSquared * oneMinusT * start.x
            + 3.0 * oneMinusTSquared * t * control1.x
            + 3.0 * oneMinusT * tSquared * control2.x
            + tSquared * t * end.x,
        oneMinusTSquared * oneMinusT * start.y
            + 3.0 * oneMinusTSquared * t * control1.y
            + 3.0 * oneMinusT * tSquared * control2.y
            + tSquared * t * end.y,
    };
}

std::vector<Contour> flattenPath(const VectorPath &path)
{
    std::vector<Contour> contours;
    Point current{};
    bool hasCurrent = false;

    for (const PathCommand &command : path.commands) {
        std::visit([&](const auto &value) {
            using Command = std::decay_t<decltype(value)>;
            if constexpr (std::is_same_v<Command, MoveTo>) {
                contours.push_back({{value.point}, false});
                current = value.point;
                hasCurrent = true;
            } else if constexpr (std::is_same_v<Command, LineTo>) {
                if (hasCurrent && !contours.empty()) {
                    contours.back().points.push_back(value.point);
                    current = value.point;
                }
            } else if constexpr (std::is_same_v<Command, QuadraticTo>) {
                if (!hasCurrent || contours.empty()) {
                    return;
                }
                const int segments = curveSegmentCount(distance(current, value.control)
                                                       + distance(value.control, value.end));
                const Point start = current;
                for (int segment = 1; segment <= segments; ++segment) {
                    const double t = static_cast<double>(segment) / static_cast<double>(segments);
                    contours.back().points.push_back(quadraticPoint(start, value.control, value.end, t));
                }
                current = value.end;
            } else if constexpr (std::is_same_v<Command, CubicTo>) {
                if (!hasCurrent || contours.empty()) {
                    return;
                }
                const int segments = curveSegmentCount(distance(current, value.control1)
                                                       + distance(value.control1, value.control2)
                                                       + distance(value.control2, value.end));
                const Point start = current;
                for (int segment = 1; segment <= segments; ++segment) {
                    const double t = static_cast<double>(segment) / static_cast<double>(segments);
                    contours.back().points.push_back(cubicPoint(start,
                                                                value.control1,
                                                                value.control2,
                                                                value.end,
                                                                t));
                }
                current = value.end;
            } else if constexpr (std::is_same_v<Command, ClosePath>) {
                if (hasCurrent && !contours.empty()) {
                    contours.back().closed = true;
                    current = contours.back().points.front();
                }
            }
        }, command);
    }
    return contours;
}

Bounds contourBounds(const std::vector<Contour> &contours) noexcept
{
    Bounds bounds;
    for (const Contour &contour : contours) {
        for (Point point : contour.points) {
            bounds.include(point);
        }
    }
    return bounds;
}

bool insideEvenOddFill(const std::vector<Contour> &contours, Point point) noexcept
{
    bool inside = false;
    for (const Contour &contour : contours) {
        if (contour.points.size() < 3) {
            continue;
        }
        for (std::size_t index = 0; index < contour.points.size(); ++index) {
            const Point first = contour.points[index];
            const Point second = contour.points[(index + 1) % contour.points.size()];
            const bool crossesY = (first.y > point.y) != (second.y > point.y);
            if (!crossesY) {
                continue;
            }
            const double crossingX = first.x
                + (point.y - first.y) * (second.x - first.x) / (second.y - first.y);
            if (point.x < crossingX) {
                inside = !inside;
            }
        }
    }
    return inside;
}

double squaredDistanceToSegment(Point point, Point start, Point end) noexcept
{
    const double segmentX = end.x - start.x;
    const double segmentY = end.y - start.y;
    const double lengthSquared = segmentX * segmentX + segmentY * segmentY;
    if (lengthSquared <= std::numeric_limits<double>::epsilon()) {
        return squaredDistance(point, start);
    }

    const double projection = std::clamp(((point.x - start.x) * segmentX
                                          + (point.y - start.y) * segmentY)
                                             / lengthSquared,
                                         0.0,
                                         1.0);
    return squaredDistance(point,
                           {start.x + projection * segmentX,
                            start.y + projection * segmentY});
}

bool insideStroke(const std::vector<Contour> &contours, Point point, double radius) noexcept
{
    const double radiusSquared = radius * radius;
    for (const Contour &contour : contours) {
        if (contour.points.size() < 2) {
            continue;
        }
        for (std::size_t index = 1; index < contour.points.size(); ++index) {
            if (squaredDistanceToSegment(point,
                                         contour.points[index - 1],
                                         contour.points[index]) <= radiusSquared) {
                return true;
            }
        }
        if (contour.closed
            && squaredDistanceToSegment(point,
                                        contour.points.back(),
                                        contour.points.front()) <= radiusSquared) {
            return true;
        }
    }
    return false;
}

double channel(std::uint32_t argb, unsigned shift) noexcept
{
    return static_cast<double>((argb >> shift) & 0xffU) / 255.0;
}

std::uint8_t byteFromUnit(double value) noexcept
{
    return static_cast<std::uint8_t>(std::clamp(
        static_cast<int>(std::lround(std::clamp(value, 0.0, 1.0) * 255.0)),
        0,
        255));
}

std::uint32_t sourceOver(std::uint32_t sourceArgb,
                         std::uint32_t destinationArgb,
                         double coverage) noexcept
{
    const double sourceAlpha = channel(sourceArgb, 24U) * std::clamp(coverage, 0.0, 1.0);
    const double destinationAlpha = channel(destinationArgb, 24U);
    const double outputAlpha = sourceAlpha + destinationAlpha * (1.0 - sourceAlpha);
    if (outputAlpha <= 0.0) {
        return 0x00000000U;
    }

    const auto outputChannel = [&](unsigned shift) {
        return (channel(sourceArgb, shift) * sourceAlpha
                + channel(destinationArgb, shift) * destinationAlpha * (1.0 - sourceAlpha))
            / outputAlpha;
    };
    return (static_cast<std::uint32_t>(byteFromUnit(outputAlpha)) << 24U)
        | (static_cast<std::uint32_t>(byteFromUnit(outputChannel(16U))) << 16U)
        | (static_cast<std::uint32_t>(byteFromUnit(outputChannel(8U))) << 8U)
        | static_cast<std::uint32_t>(byteFromUnit(outputChannel(0U)));
}

template <typename Predicate>
void paintCoverage(RasterLayer &target,
                   Bounds bounds,
                   std::uint32_t argb,
                   Predicate &&covered)
{
    if (!bounds.valid()) {
        return;
    }

    const int left = floorToExtent(bounds.left, target.width);
    const int top = floorToExtent(bounds.top, target.height);
    const int right = ceilToExtent(bounds.right, target.width);
    const int bottom = ceilToExtent(bounds.bottom, target.height);

    for (int y = top; y < bottom; ++y) {
        for (int x = left; x < right; ++x) {
            int sampleCoverage = 0;
            for (int sampleY = 0; sampleY < CoverageAxisSamples; ++sampleY) {
                for (int sampleX = 0; sampleX < CoverageAxisSamples; ++sampleX) {
                    const Point sample{
                        static_cast<double>(x)
                            + (static_cast<double>(sampleX) + 0.5) / CoverageAxisSamples,
                        static_cast<double>(y)
                            + (static_cast<double>(sampleY) + 0.5) / CoverageAxisSamples,
                    };
                    sampleCoverage += covered(sample) ? 1 : 0;
                }
            }
            if (sampleCoverage == 0) {
                continue;
            }
            const std::size_t index = pixelIndex(target.width, x, y);
            target.pixels[index] = sourceOver(argb,
                                               target.pixels[index],
                                               static_cast<double>(sampleCoverage)
                                                   / CoverageSampleCount);
        }
    }
}

RasterLayer rasterizeVector(const VectorAsset &asset)
{
    RasterLayer result = makeRasterLayer(asset.viewport.width,
                                         asset.viewport.height,
                                         0x00000000U);
    for (const VectorPath &path : asset.paths) {
        const std::vector<Contour> contours = flattenPath(path);
        const Bounds bounds = contourBounds(contours);
        if (path.fill) {
            paintCoverage(result, bounds, path.fill->argb,
                          [&](Point point) { return insideEvenOddFill(contours, point); });
        }
        if (path.stroke) {
            const double radius = path.stroke->width * 0.5;
            Bounds strokeBounds = bounds;
            strokeBounds.left -= radius;
            strokeBounds.top -= radius;
            strokeBounds.right += radius;
            strokeBounds.bottom += radius;
            paintCoverage(result, strokeBounds, path.stroke->paint.argb,
                          [&](Point point) { return insideStroke(contours, point, radius); });
        }
    }
    return result;
}

RasterLayer transformedRaster(const RasterLayer &source,
                              const AffineTransform &transform,
                              int outputWidth,
                              int outputHeight)
{
    RasterLayer result = makeRasterLayer(outputWidth, outputHeight, 0x00000000U);
    const double determinant = transform.m11 * transform.m22 - transform.m21 * transform.m12;
    if (std::abs(determinant) <= std::numeric_limits<double>::epsilon()) {
        return result;
    }

    const std::array<DocumentPoint, 4> corners{
        transformPoint(transform, {0.0, 0.0}),
        transformPoint(transform, {static_cast<double>(source.width), 0.0}),
        transformPoint(transform, {0.0, static_cast<double>(source.height)}),
        transformPoint(transform,
                       {static_cast<double>(source.width), static_cast<double>(source.height)}),
    };
    Bounds bounds;
    for (DocumentPoint corner : corners) {
        bounds.include({corner.x, corner.y});
    }

    const int left = floorToExtent(bounds.left, outputWidth);
    const int top = floorToExtent(bounds.top, outputHeight);
    const int right = ceilToExtent(bounds.right, outputWidth);
    const int bottom = ceilToExtent(bounds.bottom, outputHeight);

    for (int y = top; y < bottom; ++y) {
        for (int x = left; x < right; ++x) {
            const double translatedX = static_cast<double>(x) + 0.5 - transform.translationX;
            const double translatedY = static_cast<double>(y) + 0.5 - transform.translationY;
            const double sourceX = (translatedX * transform.m22 - translatedY * transform.m21)
                / determinant;
            const double sourceY = (-translatedX * transform.m12 + translatedY * transform.m11)
                / determinant;
            if (sourceX < 0.0 || sourceY < 0.0
                || sourceX >= source.width || sourceY >= source.height) {
                continue;
            }
            const int nearestX = static_cast<int>(std::floor(sourceX));
            const int nearestY = static_cast<int>(std::floor(sourceY));
            result.pixels[pixelIndex(outputWidth, x, y)] =
                source.pixels[pixelIndex(source.width, nearestX, nearestY)];
        }
    }
    return result;
}

FrameRenderResult errorResult(FrameRenderStatus status, std::string message)
{
    FrameRenderResult result;
    result.status = status;
    result.message = std::move(message);
    return result;
}

} // namespace

bool FrameRenderResult::ok() const noexcept
{
    return status == FrameRenderStatus::Success;
}

FrameRenderResult renderFrame(const Document &document, FrameIndex frame)
{
    const ValidationResult validation = validate(document);
    if (!validation.ok()) {
        return errorResult(FrameRenderStatus::InvalidDocument,
                           validation.issues.front().path + ": "
                               + validation.issues.front().message);
    }
    if (frame >= document.timeline.frameCount) {
        return errorResult(FrameRenderStatus::FrameOutOfRange,
                           "requested frame is outside the document timeline");
    }

    ::LayerStack engineLayers;
    for (const iiSharedCanvas::Layer &documentLayer : document.layers) {
        if (!documentLayer.visible) {
            continue;
        }
        const Asset *asset = resolveAssetAt(document, documentLayer, frame);
        if (!asset) {
            return errorResult(FrameRenderStatus::AssetResolutionFailed,
                               "validated layer asset could not be resolved at the requested frame");
        }

        RasterLayer source = std::visit([](const auto &resolved) {
            using ResolvedAsset = std::decay_t<decltype(resolved)>;
            if constexpr (std::is_same_v<ResolvedAsset, RasterAsset>) {
                return resolved.pixels;
            } else {
                return rasterizeVector(resolved);
            }
        }, *asset);

        ::Layer engineLayer;
        engineLayer.surface = drawingSurfaceFromRasterLayer(
            transformedRaster(source,
                              documentLayer.transform,
                              document.extent.width,
                              document.extent.height));
        engineLayer.metadata.visible = true;
        engineLayer.metadata.opacity = documentLayer.opacity;
        engineLayer.metadata.blendMode = documentLayer.blendMode;
        engineLayers.layers.push_back(std::move(engineLayer));
    }

    FrameRenderResult result;
    result.pixels = compositeLayerStack(engineLayers,
                                        document.extent.width,
                                        document.extent.height,
                                        0x00000000U);
    return result;
}

} // namespace iiSharedCanvas
