#include "Render/FrameRenderer.h"
#include "Document/CanvasSampling.h"

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
#include <unordered_set>
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

Point inverseTransformPoint(const AffineTransform &transform,
                            Point output) noexcept
{
    const double determinant = transform.m11 * transform.m22
        - transform.m21 * transform.m12;
    const double translatedX = output.x - transform.translationX;
    const double translatedY = output.y - transform.translationY;
    return {
        (translatedX * transform.m22 - translatedY * transform.m21) / determinant,
        (-translatedX * transform.m12 + translatedY * transform.m11) / determinant,
    };
}

Bounds transformBounds(Bounds source,
                       const AffineTransform &transform) noexcept
{
    Bounds result;
    if (!source.valid()) {
        return result;
    }
    const std::array<Point, 4> corners{
        Point{source.left, source.top},
        Point{source.right, source.top},
        Point{source.left, source.bottom},
        Point{source.right, source.bottom},
    };
    for (Point corner : corners) {
        const DocumentPoint transformed = transformPoint(
            transform, {corner.x, corner.y});
        result.include({transformed.x, transformed.y});
    }
    return result;
}

RasterLayer rasterizeVector(const VectorAsset &asset,
                            const AffineTransform &transform,
                            int outputWidth,
                            int outputHeight)
{
    RasterLayer result = makeRasterLayer(outputWidth, outputHeight, 0x00000000U);
    const double determinant = transform.m11 * transform.m22
        - transform.m21 * transform.m12;
    if (std::abs(determinant) <= std::numeric_limits<double>::epsilon()) {
        return result;
    }

    for (const VectorPath &path : asset.paths) {
        const std::vector<Contour> contours = flattenPath(path);
        const Bounds sourceBounds = contourBounds(contours);
        if (path.fill) {
            paintCoverage(result,
                          transformBounds(sourceBounds, transform),
                          path.fill->argb,
                          [&](Point point) {
                              return insideEvenOddFill(
                                  contours, inverseTransformPoint(transform, point));
                          });
        }
        if (path.stroke) {
            const double radius = path.stroke->width * 0.5;
            Bounds strokeBounds = sourceBounds;
            strokeBounds.left -= radius;
            strokeBounds.top -= radius;
            strokeBounds.right += radius;
            strokeBounds.bottom += radius;
            paintCoverage(result,
                          transformBounds(strokeBounds, transform),
                          path.stroke->paint.argb,
                          [&](Point point) {
                              return insideStroke(
                                  contours,
                                  inverseTransformPoint(transform, point),
                                  radius);
                          });
        }
    }
    return result;
}

struct FilteredPixel {
    double alpha = 0.0;
    std::array<double, 3> color{};
    void add(std::uint32_t pixel, double weight) noexcept {
        const double coverage = static_cast<double>(pixel >> 24) * weight;
        alpha += coverage;
        for (int channel = 0; channel < 3; ++channel)
            color[channel] += ((pixel >> (16 - channel * 8)) & 255U) * coverage;
    }
    std::uint32_t argb(double weight = 1.0) const noexcept {
        if (alpha <= 0.0 || weight <= 0.0) return 0;
        const auto a = static_cast<std::uint32_t>(std::clamp(std::lround(alpha / weight), 0L, 255L));
        if (!a) return 0;
        std::uint32_t pixel = a << 24;
        for (int channel = 0; channel < 3; ++channel)
            pixel |= static_cast<std::uint32_t>(std::clamp(std::lround(color[channel] / alpha), 0L, 255L))
                << (16 - channel * 8);
        return pixel;
    }
};

std::uint32_t bilinearPixel(const RasterLayer &source, double x, double y) noexcept
{
    if (x < 0.0 || y < 0.0 || x >= source.width || y >= source.height) return 0;
    x = std::clamp(x - 0.5, 0.0, static_cast<double>(source.width - 1));
    y = std::clamp(y - 0.5, 0.0, static_cast<double>(source.height - 1));
    const int left = static_cast<int>(std::floor(x)), top = static_cast<int>(std::floor(y));
    const int right = std::min(left + 1, source.width - 1), bottom = std::min(top + 1, source.height - 1);
    const double dx = x - left, dy = y - top;
    FilteredPixel pixel;
    pixel.add(source.pixels[pixelIndex(source.width, left, top)], (1 - dx) * (1 - dy));
    pixel.add(source.pixels[pixelIndex(source.width, right, top)], dx * (1 - dy));
    pixel.add(source.pixels[pixelIndex(source.width, left, bottom)], (1 - dx) * dy);
    pixel.add(source.pixels[pixelIndex(source.width, right, bottom)], dx * dy);
    return pixel.argb();
}

std::uint32_t areaPixel(const RasterLayer &source, double x, double y, double width, double height) noexcept
{
    const double left = x - width * 0.5, right = x + width * 0.5;
    const double top = y - height * 0.5, bottom = y + height * 0.5;
    FilteredPixel pixel;
    for (int row = floorToExtent(top, source.height); row < ceilToExtent(bottom, source.height); ++row) {
        const double wy = std::max(0.0, std::min(bottom, row + 1.0) - std::max(top, double(row)));
        for (int column = floorToExtent(left, source.width); column < ceilToExtent(right, source.width); ++column) {
            const double wx = std::max(0.0, std::min(right, column + 1.0) - std::max(left, double(column)));
            pixel.add(source.pixels[pixelIndex(source.width, column, row)], wx * wy);
        }
    }
    return pixel.argb(width * height);
}

RasterLayer transformedRaster(const RasterLayer &source,
                              const AffineTransform &transform,
                              int outputWidth,
                              int outputHeight,
                              RasterSampling sampling)
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
    const double footprintX = std::hypot(transform.m22, transform.m12) / std::abs(determinant);
    const double footprintY = std::hypot(transform.m21, transform.m11) / std::abs(determinant);
    const bool axisAligned = std::abs(transform.m12) < 1e-12 && std::abs(transform.m21) < 1e-12;

    for (int y = top; y < bottom; ++y) {
        for (int x = left; x < right; ++x) {
            const double translatedX = static_cast<double>(x) + 0.5 - transform.translationX;
            const double translatedY = static_cast<double>(y) + 0.5 - transform.translationY;
            const double sourceX = (translatedX * transform.m22 - translatedY * transform.m21)
                / determinant;
            const double sourceY = (-translatedX * transform.m12 + translatedY * transform.m11)
                / determinant;
            if (sampling == RasterSampling::Smooth) {
                std::uint32_t pixel = 0;
                if (axisAligned && (footprintX > 1.0 || footprintY > 1.0)) {
                    pixel = areaPixel(source, sourceX, sourceY, footprintX, footprintY);
                } else if (footprintX <= 1.0 && footprintY <= 1.0) {
                    pixel = bilinearPixel(source, sourceX, sourceY);
                } else {
                    // Bounded stratified integration for rotated/sheared minification.
                    const int nx = static_cast<int>(std::clamp(std::ceil(footprintX), 1.0, 16.0));
                    const int ny = static_cast<int>(std::clamp(std::ceil(footprintY), 1.0, 16.0));
                    FilteredPixel filtered;
                    for (int sy = 0; sy < ny; ++sy) for (int sx = 0; sx < nx; ++sx) {
                        const double dx = (sx + 0.5) / nx - 0.5, dy = (sy + 0.5) / ny - 0.5;
                        filtered.add(bilinearPixel(source,
                            sourceX + (dx * transform.m22 - dy * transform.m21) / determinant,
                            sourceY + (-dx * transform.m12 + dy * transform.m11) / determinant), 1.0);
                    }
                    pixel = filtered.argb(nx * ny);
                }
                result.pixels[pixelIndex(outputWidth, x, y)] = pixel;
                continue;
            }
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

Bounds transformedBounds(const RasterLayer &source,
                         const AffineTransform &transform) noexcept
{
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
    return bounds;
}

bool intersectsOutput(const RasterLayer &source,
                      const AffineTransform &transform,
                      int outputWidth,
                      int outputHeight) noexcept
{
    const Bounds bounds = transformedBounds(source, transform);
    return bounds.valid()
        && bounds.right > 0.0 && bounds.bottom > 0.0
        && bounds.left < outputWidth && bounds.top < outputHeight;
}

FrameRenderResult errorResult(FrameRenderStatus status, std::string message)
{
    FrameRenderResult result;
    result.status = status;
    result.message = std::move(message);
    return result;
}

bool regionContainedByDocument(const Document &document,
                               CanvasRegion region) noexcept
{
    if (region.extent.width <= 0 || region.extent.height <= 0) {
        return false;
    }
    const CanvasRegion available = documentViewRegion(document);
    const std::int64_t requestedRight = static_cast<std::int64_t>(region.origin.x)
        + region.extent.width;
    const std::int64_t requestedBottom = static_cast<std::int64_t>(region.origin.y)
        + region.extent.height;
    const std::int64_t availableRight = static_cast<std::int64_t>(available.origin.x)
        + available.extent.width;
    const std::int64_t availableBottom = static_cast<std::int64_t>(available.origin.y)
        + available.extent.height;
    return region.origin.x >= available.origin.x
        && region.origin.y >= available.origin.y
        && requestedRight <= availableRight
        && requestedBottom <= availableBottom;
}

bool validOutputExtent(CanvasExtent extent) noexcept
{
    if (extent.width <= 0 || extent.height <= 0) {
        return false;
    }
    const std::uint64_t pixelCount = static_cast<std::uint64_t>(extent.width)
        * static_cast<std::uint64_t>(extent.height);
    return pixelCount <= std::numeric_limits<std::size_t>::max()
        / sizeof(std::uint32_t);
}

bool sameRegion(CanvasRegion first, CanvasRegion second) noexcept
{
    return first.origin.x == second.origin.x
        && first.origin.y == second.origin.y
        && first.extent.width == second.extent.width
        && first.extent.height == second.extent.height;
}

bool validRequests(const Document &document,
                   const std::vector<FrameRenderTileRequest> &requests) noexcept
{
    return std::all_of(requests.begin(), requests.end(), [&](const auto &request) {
        return regionContainedByDocument(document, request.region)
            && validOutputExtent(request.outputExtent)
            && (request.sampling == RasterSampling::Nearest || request.sampling == RasterSampling::Smooth);
    });
}

double artboardCoverage(CanvasRegion clip, const FrameRenderTileRequest &request, int x, int y)
{
    const double dx = double(request.region.extent.width) / request.outputExtent.width;
    const double dy = double(request.region.extent.height) / request.outputExtent.height;
    const double left = request.region.origin.x + x * dx;
    const double top = request.region.origin.y + y * dy;
    const double right = double(clip.origin.x) + clip.extent.width;
    const double bottom = double(clip.origin.y) + clip.extent.height;
    return std::clamp((std::min(left + dx, right) - std::max(left, double(clip.origin.x))) / dx, 0.0, 1.0)
        * std::clamp((std::min(top + dy, bottom) - std::max(top, double(clip.origin.y))) / dy, 0.0, 1.0);
}

void clipToArtboard(RasterLayer &pixels, CanvasRegion clip, const FrameRenderTileRequest &request)
{
    for (int y = 0; y < pixels.height; ++y) for (int x = 0; x < pixels.width; ++x) {
        auto &pixel = pixels.pixels[pixelIndex(pixels.width, x, y)];
        const auto alpha = std::uint32_t(std::lround((pixel >> 24) * artboardCoverage(clip, request, x, y)));
        pixel = alpha ? ((pixel & 0x00ffffffU) | (alpha << 24)) : 0;
    }
}

FrameRenderResult renderLayerRegion(const Document &document,
                                    const Asset &asset,
                                    const LayerSample &properties,
                                    CanvasRegion region,
                                    CanvasExtent outputExtent,
                                    const RasterLayer *videoFrame,
                                    RasterSampling sampling)
{
    ::LayerStack layerPieces;

    const double scaleX = static_cast<double>(outputExtent.width)
        / static_cast<double>(region.extent.width);
    const double scaleY = static_cast<double>(outputExtent.height)
        / static_cast<double>(region.extent.height);
    const auto outputTransform = [&](AffineTransform transform) {
        transform.translationX -= region.origin.x;
        transform.translationY -= region.origin.y;
        transform.m11 *= scaleX;
        transform.m21 *= scaleX;
        transform.translationX *= scaleX;
        transform.m12 *= scaleY;
        transform.m22 *= scaleY;
        transform.translationY *= scaleY;
        return transform;
    };
    const auto appendOutputPiece = [&](RasterLayer pixels) {
        ::Layer engineLayer;
        engineLayer.surface = drawingSurfaceFromRasterLayer(std::move(pixels));
        engineLayer.metadata.visible = true;
        engineLayer.metadata.opacity = 1.0;
        engineLayer.metadata.blendMode = RasterBlendMode::SourceOver;
        layerPieces.layers.push_back(std::move(engineLayer));
    };
    const auto appendRasterLayer = [&](const RasterLayer &source,
                                       AffineTransform transform) {
        transform = outputTransform(transform);
        if (!intersectsOutput(source, transform,
                              outputExtent.width, outputExtent.height)) {
            return;
        }
        appendOutputPiece(transformedRaster(source,
                                            transform,
                                            outputExtent.width,
                                            outputExtent.height, sampling));
    };

    if (videoFrame) {
        appendRasterLayer(*videoFrame, properties.transform);
    } else if (const auto *raster = std::get_if<RasterAsset>(&asset)) {
        appendRasterLayer(raster->pixels, properties.transform);
    } else if (const auto *mlsd = std::get_if<MlsdAsset>(&asset)) {
        appendOutputPiece(rasterizeVector(mlsdVectorPreview(*mlsd), outputTransform(properties.transform),
            outputExtent.width, outputExtent.height));
    } else if (const auto *canny = std::get_if<CannyAsset>(&asset)) {
        appendRasterLayer(cannyRasterPreview(*canny, canny->mask.size()), properties.transform);
    } else if (const auto *scribble = std::get_if<ScribbleAsset>(&asset)) {
        appendRasterLayer(scribbleRasterPreview(*scribble, scribble->mask.size()), properties.transform);
    } else if (const auto *lineArt = std::get_if<LineArtAsset>(&asset)) {
        appendRasterLayer(lineArtRasterPreview(*lineArt, lineArt->coverage.size()), properties.transform);
    } else if (const auto *normalMap = std::get_if<NormalMapAsset>(&asset)) {
        appendRasterLayer(normalMapRasterPreview(*normalMap, {NormalMapChannelOrder::Xyz, false, normalMap->samples.size()}), properties.transform);
    } else if (const auto *shuffle = std::get_if<ShuffleAsset>(&asset)) {
        appendRasterLayer(shuffleRasterPreview(*shuffle, shuffle->colors.size()), properties.transform);
    } else if (const auto *tile = std::get_if<TileAsset>(&asset)) {
        appendRasterLayer(tileRasterPreview(*tile, tile->colors.size()), properties.transform);
    } else if (const auto *reference = std::get_if<ReferenceAsset>(&asset)) {
        appendRasterLayer(referenceRasterPreview(*reference, reference->colors.size()), properties.transform);
    } else if (const auto *depth = std::get_if<DepthAsset>(&asset)) {
        appendRasterLayer(depthRasterPreview(*depth, depth->values.size()), properties.transform);
    } else if (const auto *pose = std::get_if<PoseAsset>(&asset)) {
        appendOutputPiece(rasterizeVector(poseVectorPreview(*pose), outputTransform(properties.transform),
            outputExtent.width, outputExtent.height));
    } else if (const auto *vector = std::get_if<VectorAsset>(&asset)) {
        appendOutputPiece(rasterizeVector(*vector,
                                          outputTransform(properties.transform),
                                          outputExtent.width,
                                          outputExtent.height));
    } else {
        const auto &chunked = std::get<ChunkedRasterAsset>(asset);
        const std::int32_t chunkSize = document.infiniteCanvas.chunkSize;
        for (const RasterChunk &chunk : chunked.chunks) {
            const double chunkX = static_cast<double>(chunk.column) * chunkSize;
            const double chunkY = static_cast<double>(chunk.row) * chunkSize;
            AffineTransform transform = properties.transform;
            transform.translationX += transform.m11 * chunkX + transform.m21 * chunkY;
            transform.translationY += transform.m12 * chunkX + transform.m22 * chunkY;
            appendRasterLayer(chunk.pixels, transform);
        }
    }

    FrameRenderResult result;
    result.origin = region.origin;
    result.pixels = compositeLayerStack(layerPieces,
                                        outputExtent.width,
                                        outputExtent.height,
                                        0x00000000U);
    return result;
}

FrameLayerTileRenderResult renderValidatedFrameLayerTiles(
    const Document &document,
    FrameIndex frame,
    std::size_t layerIndex,
    const std::vector<FrameRenderTileRequest> &requests)
{
    const iiSharedCanvas::Layer &documentLayer = document.layers[layerIndex];
    const LayerProperties &properties = layerProperties(documentLayer);
    const LayerSample sampled = sampleLayerAt(document, documentLayer, frame);

    FrameLayerTileRenderResult result;
    result.layerIndex = layerIndex;
    result.layerId = properties.id;
    result.role = layerRole(documentLayer);
    result.visible = sampled.visible;
    result.opacity = sampled.opacity;
    result.blendMode = properties.blendMode;
    result.artboardId = properties.artboardId;
    if (std::holds_alternative<IpAdapterLayer>(documentLayer)) {
        result.spatial = false;
        return result;
    }
    if (!result.visible) {
        return result;
    }
    const auto &transform = sampled.transform;
    const double determinant = transform.m11 * transform.m22 - transform.m21 * transform.m12;
    if (!std::isfinite(transform.m11) || !std::isfinite(transform.m12)
        || !std::isfinite(transform.m21) || !std::isfinite(transform.m22)
        || !std::isfinite(transform.translationX) || !std::isfinite(transform.translationY)
        || !std::isfinite(determinant)) {
        result.status = FrameRenderStatus::InvalidDocument;
        result.message = "sampled motion transform exceeds finite coordinates";
        return result;
    }

    const Asset *asset = resolveAssetAt(document, documentLayer, frame);
    if (!asset) {
        result.status = FrameRenderStatus::AssetResolutionFailed;
        result.message = "validated layer asset could not be resolved at the requested frame";
        return result;
    }

    const auto *videoLayer = std::get_if<VideoLayer>(&documentLayer);
    const auto *videoFrame = videoLayer ? resolveVideoFrameAt(document, *videoLayer, frame) : nullptr;
    result.tiles.reserve(requests.size());
    for (const FrameRenderTileRequest &request : requests) {
        FrameRenderResult tile = renderLayerRegion(document,
                                                   *asset,
                                                   sampled,
                                                   request.region,
                                                   request.outputExtent,
                                                   videoFrame, request.sampling);
        if (properties.artboardId)
            clipToArtboard(tile.pixels, findArtboard(document, *properties.artboardId)->region, request);
        result.tiles.push_back({request.region, std::move(tile.pixels)});
    }
    return result;
}

FrameLayerBatchRenderResult layerBatchError(
    FrameRenderStatus status,
    std::string message,
    std::vector<FrameRenderTileRequest> requests = {})
{
    FrameLayerBatchRenderResult result;
    result.requests = std::move(requests);
    result.status = status;
    result.message = std::move(message);
    return result;
}

} // namespace

namespace render_detail {

IISHAREDCANVAS_NO_EXPORT FrameLayerBatchRenderResult preflightFrameLayerRender(
    const Document &document,
    FrameIndex frame,
    const std::vector<FrameRenderTileRequest> &requests)
{
    const ValidationResult validation = validate(document);
    if (!validation.ok()) {
        return layerBatchError(FrameRenderStatus::InvalidDocument,
                               validation.issues.front().path + ": "
                                   + validation.issues.front().message,
                               requests);
    }
    if (frame >= document.timeline.frameCount) {
        return layerBatchError(FrameRenderStatus::FrameOutOfRange,
                               "requested frame is outside the document timeline",
                               requests);
    }
    if (!validRequests(document, requests)) {
        return layerBatchError(FrameRenderStatus::InvalidRegion,
                               "render region must be positive, bounded by the document, and have a valid output extent",
                               requests);
    }

    FrameLayerBatchRenderResult result;
    result.requests = requests;
    result.artboards = document.artboards;
    return result;
}

IISHAREDCANVAS_NO_EXPORT FrameLayerTileRenderResult renderPreflightedFrameLayerTiles(
    const Document &document,
    FrameIndex frame,
    std::size_t layerIndex,
    const std::vector<FrameRenderTileRequest> &requests)
{
    return renderValidatedFrameLayerTiles(document, frame, layerIndex, requests);
}

} // namespace render_detail

FrameRenderResult renderFrame(const Document &document, FrameIndex frame)
{
    const auto view = documentViewRegion(document);
    const FrameTileRenderResult tiles = renderFrameTiles(
        document, frame, {{view, view.extent}});
    if (!tiles.ok()) {
        return errorResult(tiles.status, tiles.message);
    }
    FrameRenderResult result;
    result.origin = view.origin;
    result.pixels = tiles.tiles.front().pixels;
    return result;
}

FrameRenderResult renderArtboard(const Document &document, FrameIndex frame, const std::string &id)
{
    const auto *a = findArtboard(document, id);
    return renderArtboard(document, frame, id, a ? a->region.extent : CanvasExtent{});
}

FrameRenderResult renderArtboard(const Document &document, FrameIndex frame, const std::string &id,
                                  CanvasExtent outputExtent, RasterSampling sampling)
{
    const auto validation = validate(document);
    if (!validation.ok()) return errorResult(FrameRenderStatus::InvalidDocument, validation.issues.front().message);
    const auto *a = findArtboard(document, id);
    if (!a) return errorResult(FrameRenderStatus::ArtboardNotFound, "artboard id was not found");
    auto batch = render_detail::preflightFrameLayerRender(document, frame, {{a->region, outputExtent, sampling}});
    if (!batch.ok()) return errorResult(batch.status, batch.message);
    batch.artboards = {*a};
    batch.layers.reserve(document.layers.size());
    for (std::size_t index = 0; index < document.layers.size(); ++index) {
        if (layerProperties(document.layers[index]).artboardId == id) {
            auto layer = render_detail::renderPreflightedFrameLayerTiles(document, frame, index, batch.requests);
            if (!layer.ok()) return errorResult(layer.status, layer.message);
            batch.layers.push_back(std::move(layer));
        } else {
            FrameLayerTileRenderResult omitted;
            omitted.layerIndex = index; omitted.visible = false;
            batch.layers.push_back(std::move(omitted));
        }
    }
    auto composed = composeFrameLayers(batch);
    if (!composed.ok()) return errorResult(composed.status, composed.message);
    FrameRenderResult result;
    result.origin = a->region.origin; result.pixels = std::move(composed.tiles.front().pixels);
    return result;
}

FrameRenderResult renderFrameRegion(const Document &document,
                                    FrameIndex frame,
                                    CanvasRegion region,
                                    CanvasExtent outputExtent)
{
    return renderFrameRegion(document, frame, region, outputExtent, RasterSampling::Nearest);
}

FrameRenderResult renderFrameRegion(const Document &document,
                                    FrameIndex frame,
                                    CanvasRegion region,
                                    CanvasExtent outputExtent,
                                    RasterSampling sampling)
{
    const FrameTileRenderResult tiles = renderFrameTiles(
        document, frame, {{region, outputExtent, sampling}});
    if (!tiles.ok()) {
        return errorResult(tiles.status, tiles.message);
    }
    FrameRenderResult result;
    result.origin = region.origin;
    result.pixels = tiles.tiles.front().pixels;
    return result;
}

FrameLayerTileRenderResult renderFrameLayerTiles(
    const Document &document,
    FrameIndex frame,
    std::size_t layerIndex,
    const std::vector<FrameRenderTileRequest> &requests)
{
    const ValidationResult validation = validate(document);
    if (!validation.ok()) {
        FrameLayerTileRenderResult result;
        result.layerIndex = layerIndex;
        result.status = FrameRenderStatus::InvalidDocument;
        result.message = validation.issues.front().path + ": "
            + validation.issues.front().message;
        return result;
    }
    if (frame >= document.timeline.frameCount) {
        FrameLayerTileRenderResult result;
        result.layerIndex = layerIndex;
        result.status = FrameRenderStatus::FrameOutOfRange;
        result.message = "requested frame is outside the document timeline";
        return result;
    }
    if (layerIndex >= document.layers.size()) {
        FrameLayerTileRenderResult result;
        result.layerIndex = layerIndex;
        result.status = FrameRenderStatus::LayerOutOfRange;
        result.message = "requested layer is outside the document layer stack";
        return result;
    }
    if (!validRequests(document, requests)) {
        FrameLayerTileRenderResult result;
        result.layerIndex = layerIndex;
        result.status = FrameRenderStatus::InvalidRegion;
        result.message = "render region must be positive, bounded by the document, and have a valid output extent";
        return result;
    }
    return render_detail::renderPreflightedFrameLayerTiles(
        document, frame, layerIndex, requests);
}

FrameLayerBatchRenderResult renderFrameLayers(
    const Document &document,
    FrameIndex frame,
    const std::vector<FrameRenderTileRequest> &requests)
{
    FrameLayerBatchRenderResult result = render_detail::preflightFrameLayerRender(
        document, frame, requests);
    if (!result.ok()) {
        return result;
    }
    result.layers.reserve(document.layers.size());
    for (std::size_t layerIndex = 0; layerIndex < document.layers.size(); ++layerIndex) {
        FrameLayerTileRenderResult layer =
            render_detail::renderPreflightedFrameLayerTiles(
                document, frame, layerIndex, requests);
        if (!layer.ok()) {
            result.status = layer.status;
            result.message = layer.message;
        }
        result.layers.push_back(std::move(layer));
        if (!result.ok()) {
            break;
        }
    }
    return result;
}

FrameTileRenderResult composeFrameLayers(
    const FrameLayerBatchRenderResult &layers)
{
    if (!layers.ok()) {
        return {{}, layers.status, layers.message};
    }

    std::unordered_set<std::string> artboardIds;
    for (const auto &a : layers.artboards) {
        if (a.id.empty() || !artboardIds.insert(a.id).second
            || a.region.extent.width <= 0 || a.region.extent.height <= 0
            || std::int64_t(a.region.origin.x) + a.region.extent.width > std::numeric_limits<std::int32_t>::max()
            || std::int64_t(a.region.origin.y) + a.region.extent.height > std::numeric_limits<std::int32_t>::max())
            return {{}, FrameRenderStatus::InvalidDocument, "artboard composition metadata is invalid"};
    }

    for (const auto &layer : layers.layers) {
        if (layer.artboardId && std::none_of(layers.artboards.begin(), layers.artboards.end(),
            [&](const Artboard &a) { return a.id == *layer.artboardId; }))
            return {{}, FrameRenderStatus::InvalidDocument, "layer composition references a missing artboard"};
    }

    for (std::size_t layerIndex = 0; layerIndex < layers.layers.size(); ++layerIndex) {
        const FrameLayerTileRenderResult &layer = layers.layers[layerIndex];
        if (!layer.ok()) {
            return {{}, layer.status, layer.message};
        }
        if (layer.layerIndex != layerIndex) {
            return {{}, FrameRenderStatus::InvalidDocument,
                    "layer render results must remain in bottom-to-top document order"};
        }
        if (!layer.spatial && layer.role != LayerRole::ControlNet) {
            return {{}, FrameRenderStatus::InvalidDocument, "nonspatial layers must have a conditioning role"};
        }
        if (layer.visible && layer.spatial && layer.tiles.size() != layers.requests.size()) {
            return {{}, FrameRenderStatus::InvalidRegion,
                    "each visible layer must provide one tile per requested region"};
        }
    }

    FrameTileRenderResult result;
    result.tiles.reserve(layers.requests.size());
    for (std::size_t requestIndex = 0;
         requestIndex < layers.requests.size();
         ++requestIndex) {
        const FrameRenderTileRequest &request = layers.requests[requestIndex];
        if (!validOutputExtent(request.outputExtent) || request.region.extent.width <= 0
            || request.region.extent.height <= 0) {
            return {{}, FrameRenderStatus::InvalidRegion,
                    "composed layer output extent must be positive and valid"};
        }

        // Validate tile geometry before composing any artboard groups.
        for (const FrameLayerTileRenderResult &layer : layers.layers) {
            if (!layer.visible || layer.role == LayerRole::ControlNet) {
                continue;
            }
            const FrameRenderTile &tile = layer.tiles[requestIndex];
            const std::uint64_t expectedPixels =
                static_cast<std::uint64_t>(request.outputExtent.width)
                * static_cast<std::uint64_t>(request.outputExtent.height);
            if (!sameRegion(tile.region, request.region)
                || tile.pixels.width != request.outputExtent.width
                || tile.pixels.height != request.outputExtent.height
                || tile.pixels.pixels.size() != expectedPixels) {
                return {{}, FrameRenderStatus::InvalidRegion,
                        "layer tile geometry must match its requested region and output extent"};
            }

        }

        const auto appendPixels = [](const RasterLayer &pixels, double opacity,
                                      RasterBlendMode blendMode, ::LayerStack &stack) {
            ::Layer layer;
            layer.surface = drawingSurfaceFromRasterLayer(pixels);
            layer.metadata.visible = true; layer.metadata.opacity = opacity; layer.metadata.blendMode = blendMode;
            stack.layers.push_back(std::move(layer));
        };
        const auto appendContent = [&](const std::optional<std::string> &id, ::LayerStack &stack) {
            for (const auto &layer : layers.layers) {
                if (layer.visible && layer.role == LayerRole::Artwork && layer.artboardId == id)
                    appendPixels(layer.tiles[requestIndex].pixels, layer.opacity, layer.blendMode, stack);
            }
        };
        ::LayerStack engineLayers;
        for (const auto &a : layers.artboards) {
            if (!a.visible) continue;
            ::LayerStack group;
            auto background = makeRasterLayer(request.outputExtent.width, request.outputExtent.height, a.backgroundArgb);
            clipToArtboard(background, a.region, request);
            appendPixels(background, 1.0, RasterBlendMode::SourceOver, group);
            appendContent(a.id, group);
            auto pixels = compositeLayerStack(group, request.outputExtent.width, request.outputExtent.height, 0);
            appendPixels(pixels, 1.0, RasterBlendMode::SourceOver, engineLayers);
        }
        appendContent(std::nullopt, engineLayers);

        result.tiles.push_back({
            request.region,
            compositeLayerStack(engineLayers,
                                request.outputExtent.width,
                                request.outputExtent.height,
                                0x00000000U),
        });
    }
    return result;
}

FrameTileRenderResult renderFrameTiles(
    const Document &document,
    FrameIndex frame,
    const std::vector<FrameRenderTileRequest> &requests)
{
    return composeFrameLayers(renderFrameLayers(document, frame, requests));
}

} // namespace iiSharedCanvas
