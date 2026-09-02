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
    const CanvasRegion available = canvasRegion(document);
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
            && validOutputExtent(request.outputExtent);
    });
}

FrameRenderResult renderLayerRegion(const Document &document,
                                    const Asset &asset,
                                    const LayerProperties &properties,
                                    CanvasRegion region,
                                    CanvasExtent outputExtent)
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
                                            outputExtent.height));
    };

    if (const auto *raster = std::get_if<RasterAsset>(&asset)) {
        appendRasterLayer(raster->pixels, properties.transform);
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

    FrameLayerTileRenderResult result;
    result.layerIndex = layerIndex;
    result.layerId = properties.id;
    result.visible = properties.visible
        && layerExistsAt(document, documentLayer, frame);
    result.opacity = properties.opacity;
    result.blendMode = properties.blendMode;
    if (!result.visible) {
        return result;
    }

    const Asset *asset = resolveAssetAt(document, documentLayer, frame);
    if (!asset) {
        result.status = FrameRenderStatus::AssetResolutionFailed;
        result.message = "validated layer asset could not be resolved at the requested frame";
        return result;
    }

    result.tiles.reserve(requests.size());
    for (const FrameRenderTileRequest &request : requests) {
        FrameRenderResult tile = renderLayerRegion(document,
                                                   *asset,
                                                   properties,
                                                   request.region,
                                                   request.outputExtent);
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
    const FrameTileRenderResult tiles = renderFrameTiles(
        document, frame, {{canvasRegion(document), document.extent}});
    if (!tiles.ok()) {
        return errorResult(tiles.status, tiles.message);
    }
    FrameRenderResult result;
    result.origin = canvasRegion(document).origin;
    result.pixels = tiles.tiles.front().pixels;
    return result;
}

FrameRenderResult renderFrameRegion(const Document &document,
                                    FrameIndex frame,
                                    CanvasRegion region,
                                    CanvasExtent outputExtent)
{
    const FrameTileRenderResult tiles = renderFrameTiles(
        document, frame, {{region, outputExtent}});
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

    for (std::size_t layerIndex = 0; layerIndex < layers.layers.size(); ++layerIndex) {
        const FrameLayerTileRenderResult &layer = layers.layers[layerIndex];
        if (!layer.ok()) {
            return {{}, layer.status, layer.message};
        }
        if (layer.layerIndex != layerIndex) {
            return {{}, FrameRenderStatus::InvalidDocument,
                    "layer render results must remain in bottom-to-top document order"};
        }
        if (layer.visible && layer.tiles.size() != layers.requests.size()) {
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
        if (!validOutputExtent(request.outputExtent)) {
            return {{}, FrameRenderStatus::InvalidRegion,
                    "composed layer output extent must be positive and valid"};
        }

        ::LayerStack engineLayers;
        engineLayers.layers.reserve(layers.layers.size());
        for (const FrameLayerTileRenderResult &layer : layers.layers) {
            if (!layer.visible) {
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

            ::Layer engineLayer;
            engineLayer.surface = drawingSurfaceFromRasterLayer(tile.pixels);
            engineLayer.metadata.visible = true;
            engineLayer.metadata.opacity = layer.opacity;
            engineLayer.metadata.blendMode = layer.blendMode;
            engineLayers.layers.push_back(std::move(engineLayer));
        }

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
