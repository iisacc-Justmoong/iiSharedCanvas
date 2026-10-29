#include "Render/FrameRenderDispatch_p.hpp"
#include "Render/FrameRenderer.h"
#include "Document/CanvasSampling.h"
#include "Bitmap/BitmapProcessing.h"

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
#include <iterator>
#include <limits>
#include <map>
#include <type_traits>
#include <unordered_map>
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

constexpr std::int64_t OverlayTileSide = BitmapPreviewTileSide;
struct OverlayCoordinate {
    std::int64_t column = 0, row = 0;
    friend bool operator==(const OverlayCoordinate &, const OverlayCoordinate &) = default;
};
struct OverlayCoordinateHash {
    std::size_t operator()(OverlayCoordinate key) const noexcept {
        const auto x = std::hash<std::int64_t>{}(key.column);
        const auto y = std::hash<std::int64_t>{}(key.row);
        return x ^ (y + std::size_t(0x9e3779b9U) + (x << 6) + (x >> 2));
    }
};
std::int64_t floorDivide(std::int64_t value, std::int64_t divisor) noexcept {
    const auto quotient = value / divisor;
    return quotient - (value % divisor < 0 ? 1 : 0);
}
struct OverlayAsset {
    std::unordered_map<OverlayCoordinate, std::span<const std::uint32_t>, OverlayCoordinateHash> tiles;
    [[nodiscard]] std::optional<std::uint32_t> pixel(std::int64_t x, std::int64_t y) const noexcept {
        const OverlayCoordinate key{floorDivide(x, OverlayTileSide), floorDivide(y, OverlayTileSide)};
        const auto found = tiles.find(key);
        if (found == tiles.end()) return std::nullopt;
        const auto localX = x - key.column * OverlayTileSide;
        const auto localY = y - key.row * OverlayTileSide;
        return found->second[std::size_t(localY * OverlayTileSide + localX)];
    }
    void apply(RasterLayer &raster) const {
        for (const auto &[key, pixels] : tiles) {
            const auto originX = key.column * OverlayTileSide, originY = key.row * OverlayTileSide;
            const auto left = std::max<std::int64_t>(0, originX);
            const auto top = std::max<std::int64_t>(0, originY);
            const auto right = std::min<std::int64_t>(raster.width, originX + OverlayTileSide);
            const auto bottom = std::min<std::int64_t>(raster.height, originY + OverlayTileSide);
            if (right <= left || bottom <= top) continue;
            for (auto y = top; y < bottom; ++y)
                std::copy_n(pixels.begin() + std::size_t((y - originY) * OverlayTileSide + left - originX),
                            std::size_t(right - left),
                            raster.pixels.begin() + std::size_t(y * raster.width + left));
        }
    }
};
struct PreparedOverlays {
    std::unordered_map<const Asset *, OverlayAsset> assets;
    bool prepare(const Document &document, std::span<const FrameBitmapOverlay> overlays) {
        for (const auto &overlay : overlays) {
            const auto asset = std::find_if(document.assets.begin(), document.assets.end(),
                [&](const Asset &entry) { return assetId(entry) == overlay.assetId; });
            if (asset == document.assets.end() ||
                (!std::holds_alternative<RasterAsset>(*asset) && !std::holds_alternative<ChunkedRasterAsset>(*asset)))
                return false;
            auto [entry, inserted] = assets.try_emplace(&*asset);
            if (!inserted) return false;
            for (const auto &tile : overlay.tiles) {
                if (tile.bounds.width != OverlayTileSide || tile.bounds.height != OverlayTileSide ||
                    tile.bounds.origin.x % OverlayTileSide != 0 || tile.bounds.origin.y % OverlayTileSide != 0 ||
                    tile.pixels.size() != std::size_t(OverlayTileSide * OverlayTileSide)) return false;
                const OverlayCoordinate key{tile.bounds.origin.x / OverlayTileSide,
                                            tile.bounds.origin.y / OverlayTileSide};
                if (!entry->second.tiles.emplace(key, tile.pixels).second) return false;
            }
        }
        return true;
    }
    [[nodiscard]] const OverlayAsset *get(const Asset *asset) const noexcept {
        const auto found = assets.find(asset);
        return found == assets.end() ? nullptr : &found->second;
    }
};
struct RasterSource {
    const RasterLayer *base = nullptr;
    int width = 0, height = 0;
    const OverlayAsset *overlay = nullptr;
    std::int64_t originX = 0, originY = 0;
    [[nodiscard]] std::uint32_t pixel(int x, int y) const noexcept {
        if (overlay) {
            const auto replacement = overlay->pixel(originX + x, originY + y);
            if (replacement) return *replacement;
        }
        return base ? base->pixels[pixelIndex(width, x, y)] : 0;
    }
};

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
                            int outputHeight,
                            bool opaqueMask = false)
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
                          opaqueMask ? 0xffffffffU : path.fill->argb,
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
                          opaqueMask ? 0xffffffffU : path.stroke->paint.argb,
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

std::uint32_t bilinearPixel(const RasterSource &source, double x, double y) noexcept
{
    if (x < 0.0 || y < 0.0 || x >= source.width || y >= source.height) return 0;
    x = std::clamp(x - 0.5, 0.0, static_cast<double>(source.width - 1));
    y = std::clamp(y - 0.5, 0.0, static_cast<double>(source.height - 1));
    const int left = static_cast<int>(std::floor(x)), top = static_cast<int>(std::floor(y));
    const int right = std::min(left + 1, source.width - 1), bottom = std::min(top + 1, source.height - 1);
    const double dx = x - left, dy = y - top;
    FilteredPixel pixel;
    pixel.add(source.pixel(left, top), (1 - dx) * (1 - dy));
    pixel.add(source.pixel(right, top), dx * (1 - dy));
    pixel.add(source.pixel(left, bottom), (1 - dx) * dy);
    pixel.add(source.pixel(right, bottom), dx * dy);
    return pixel.argb();
}

std::uint32_t areaPixel(const RasterSource &source, double x, double y, double width, double height) noexcept
{
    const double left = x - width * 0.5, right = x + width * 0.5;
    const double top = y - height * 0.5, bottom = y + height * 0.5;
    FilteredPixel pixel;
    for (int row = floorToExtent(top, source.height); row < ceilToExtent(bottom, source.height); ++row) {
        const double wy = std::max(0.0, std::min(bottom, row + 1.0) - std::max(top, double(row)));
        for (int column = floorToExtent(left, source.width); column < ceilToExtent(right, source.width); ++column) {
            const double wx = std::max(0.0, std::min(right, column + 1.0) - std::max(left, double(column)));
            pixel.add(source.pixel(column, row), wx * wy);
        }
    }
    return pixel.argb(width * height);
}

RasterLayer transformedRaster(const RasterSource &source,
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
                source.pixel(nearestX, nearestY);
        }
    }
    return result;
}

Bounds transformedBounds(const RasterSource &source,
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

bool intersectsOutput(const RasterSource &source,
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
    if(document.canvasMode==CanvasMode::Infinite)
        return requestedRight<=std::numeric_limits<std::int32_t>::max()
            &&requestedBottom<=std::numeric_limits<std::int32_t>::max();
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

constexpr int MaskTileSide = 64;
constexpr std::size_t MaskTileCacheCapacity = 32;

struct PreparedMask;
struct MaskTile {
    std::size_t maskId = 0;
    std::int64_t column = 0, row = 0;
    int width = 0, height = 0;
    std::uint64_t lastUse = 0;
    std::vector<double> coverage;
};
struct MaskTileCache {
    std::vector<MaskTile> tiles;
    std::uint64_t clock = 0;
    double sample(const PreparedMask &mask, std::int64_t x, std::int64_t y);
};
struct PreparedMask {
    const RasterLayer *raster = nullptr;
    const VectorAsset *vector = nullptr;
    const OverlayAsset *overlay = nullptr;
    MaskTileCache *cache = nullptr;
    std::size_t cacheId = 0;
    int contentWidth = 0, contentHeight = 0, padding = 0;
    std::int64_t width = 0, height = 0;
    MaskChannel channel = MaskChannel::Luminance;
    std::vector<double> kernel;
    double outside = 1, density = 1;
    bool inverted = false;
    AffineTransform transform;
};

std::uint32_t maskRasterPixel(const PreparedMask &mask, int x, int y) noexcept {
    if (mask.overlay) {
        const auto replacement = mask.overlay->pixel(x, y);
        if (replacement) return *replacement;
    }
    return mask.raster->pixels[pixelIndex(mask.raster->width, x, y)];
}

double coverageOfPixel(std::uint32_t pixel, MaskChannel channel) noexcept
{
    double value = double(pixel >> 24) / 255;
    if (channel == MaskChannel::Luminance)
        value *= (0.2126 * ((pixel >> 16) & 255) + 0.7152 * ((pixel >> 8) & 255)
                  + 0.0722 * (pixel & 255)) / 255;
    return value;
}

// Build exactly the native-space samples needed by one mask tile. The
// intermediate rows include the Gaussian halo, and samples beyond the old
// padded domain remain the explicit outside coverage at both passes.
MaskTile buildMaskTile(const PreparedMask &p, std::int64_t column, std::int64_t row)
{
    const std::int64_t left=column*MaskTileSide, top=row*MaskTileSide;
    const int width=static_cast<int>(std::min<std::int64_t>(MaskTileSide,p.width-left));
    const int height=static_cast<int>(std::min<std::int64_t>(MaskTileSide,p.height-top));
    MaskTile tile; tile.maskId=p.cacheId; tile.column=column; tile.row=row;
    tile.width=width; tile.height=height;
    tile.coverage.resize(std::size_t(width)*height);

    // A vector mask is rasterized only over this tile's contributing source
    // rectangle. Integer translation preserves the original 4x4 coverage grid.
    const auto sourceLeft=std::clamp<std::int64_t>(left-2LL*p.padding,0,p.contentWidth);
    const auto sourceTop=std::clamp<std::int64_t>(top-2LL*p.padding,0,p.contentHeight);
    const auto sourceRight=std::clamp<std::int64_t>(left+width,0,p.contentWidth);
    const auto sourceBottom=std::clamp<std::int64_t>(top+height,0,p.contentHeight);
    RasterLayer vectorPixels;
    if (p.vector && sourceRight>sourceLeft && sourceBottom>sourceTop) {
        AffineTransform translate;
        translate.translationX=-static_cast<double>(sourceLeft);
        translate.translationY=-static_cast<double>(sourceTop);
        vectorPixels=rasterizeVector(*p.vector,translate,static_cast<int>(sourceRight-sourceLeft),
                                     static_cast<int>(sourceBottom-sourceTop),true);
    }
    const auto source=[&](std::int64_t x,std::int64_t y) {
        x-=p.padding; y-=p.padding;
        if (x<0 || y<0 || x>=p.contentWidth || y>=p.contentHeight) return p.outside;
        if (p.raster)
            return coverageOfPixel(maskRasterPixel(p,static_cast<int>(x),static_cast<int>(y)),p.channel);
        return coverageOfPixel(vectorPixels.pixels[pixelIndex(vectorPixels.width,static_cast<int>(x-sourceLeft),static_cast<int>(y-sourceTop))],p.channel);
    };
    if (!p.padding) {
        for (int y=0;y<height;++y) for (int x=0;x<width;++x)
            tile.coverage[pixelIndex(width,x,y)]=source(left+x,top+y);
        return tile;
    }
    const auto firstRow=std::max<std::int64_t>(0,top-p.padding);
    const auto afterRow=std::min<std::int64_t>(p.height,top+height+p.padding);
    std::vector<double> horizontal(std::size_t(width)*std::size_t(afterRow-firstRow));
    for (auto y=firstRow;y<afterRow;++y) for (int x=0;x<width;++x) {
        double value=0;
        for (int i=-p.padding;i<=p.padding;++i) {
            const auto sx=left+x+i;
            value+=p.kernel[std::size_t(i+p.padding)]*(sx<0 || sx>=p.width?p.outside:source(sx,y));
        }
        horizontal[std::size_t(y-firstRow)*width+x]=value;
    }
    for (int y=0;y<height;++y) for (int x=0;x<width;++x) {
        double value=0;
        for (int i=-p.padding;i<=p.padding;++i) {
            const auto sy=top+y+i;
            value+=p.kernel[std::size_t(i+p.padding)]*(sy<0 || sy>=p.height?p.outside:
                horizontal[std::size_t(sy-firstRow)*width+x]);
        }
        tile.coverage[pixelIndex(width,x,y)]=value;
    }
    return tile;
}

double MaskTileCache::sample(const PreparedMask &mask, std::int64_t x, std::int64_t y)
{
    const auto column=x/MaskTileSide, row=y/MaskTileSide;
    auto entry=std::find_if(tiles.begin(),tiles.end(),[&](const MaskTile &t) {
        return t.maskId==mask.cacheId && t.column==column && t.row==row;
    });
    if (entry==tiles.end()) {
        auto next=buildMaskTile(mask,column,row);
        if (tiles.size()<MaskTileCacheCapacity) {
            tiles.push_back(std::move(next)); entry=std::prev(tiles.end());
        } else {
            entry=std::min_element(tiles.begin(),tiles.end(),[](const MaskTile &a,const MaskTile &b) {
                return a.lastUse<b.lastUse;
            });
            *entry=std::move(next);
        }
    }
    if (++clock==0) { for (auto &tile:tiles) tile.lastUse=0; ++clock; }
    entry->lastUse=clock;
    return entry->coverage[pixelIndex(entry->width,static_cast<int>(x-column*MaskTileSide),
                                      static_cast<int>(y-row*MaskTileSide))];
}

AffineTransform concatenate(AffineTransform a, AffineTransform b)
{
    return {a.m11*b.m11 + a.m21*b.m12, a.m12*b.m11 + a.m22*b.m12,
        a.m11*b.m21 + a.m21*b.m22, a.m12*b.m21 + a.m22*b.m22,
        a.m11*b.translationX + a.m21*b.translationY + a.translationX,
        a.m12*b.translationX + a.m22*b.translationY + a.translationY};
}

std::optional<PreparedMask> prepareMask(const Document &document, const std::optional<LayerMask> &mask,
                                      const LayerProperties &properties, const LayerSample &sampled,
                                      MaskTileCache &cache, std::size_t cacheId,
                                      const PreparedOverlays *overlays)
{
    if (!mask || !mask->enabled) return std::nullopt;
    PreparedMask p;
    p.outside = mask->outsideCoverage; p.density = mask->density; p.inverted = mask->inverted;
    p.transform = mask->linkedToLayer ? concatenate(sampled.transform, mask->transform) : mask->transform;
    if (!mask->linkedToLayer && properties.artboardId) {
        const auto &origin = findArtboard(document, *properties.artboardId)->region.origin;
        p.transform.translationX += origin.x; p.transform.translationY += origin.y;
    }
    const auto *asset = findAsset(document, mask->assetId);
    if (const auto *bitmap = std::get_if<RasterAsset>(asset)) {
        p.raster=&bitmap->pixels; p.contentWidth=p.raster->width; p.contentHeight=p.raster->height;
        p.overlay=overlays ? overlays->get(asset) : nullptr;
    } else {
        p.vector=&std::get<VectorAsset>(*asset);
        p.contentWidth=p.vector->viewport.width; p.contentHeight=p.vector->viewport.height;
    }
    p.cache=&cache; p.cacheId=cacheId; p.channel=mask->channel;
    p.padding = static_cast<int>(std::ceil(mask->featherRadius * 3));
    p.width = std::int64_t(p.contentWidth)+2LL*p.padding;
    p.height = std::int64_t(p.contentHeight)+2LL*p.padding;
    if (mask->featherRadius > 0) {
        p.kernel.resize(std::size_t(2*p.padding+1)); double total = 0;
        for (int i = -p.padding; i <= p.padding; ++i) {
            const auto weight = std::exp(-0.5 * (double(i)/mask->featherRadius) * (double(i)/mask->featherRadius));
            p.kernel[std::size_t(i+p.padding)] = weight; total += weight;
        }
        for (auto &weight : p.kernel) weight /= total;
    }
    return p;
}

double maskSample(const PreparedMask &p, double wx, double wy, RasterSampling sampling)
{
    const auto &t = p.transform;
    const auto determinant = t.m11*t.m22 - t.m12*t.m21;
    double value = p.outside;
    if (std::isfinite(determinant) && determinant != 0) {
        const double dx = wx-t.translationX, dy = wy-t.translationY;
        const double sx = (t.m22*dx-t.m21*dy)/determinant + p.padding;
        const double sy = (t.m11*dy-t.m12*dx)/determinant + p.padding;
        const auto at = [&](double x, double y) {
            if (!std::isfinite(x) || !std::isfinite(y) || x < 0 || y < 0 || x >= p.width || y >= p.height) return p.outside;
            const auto ix=static_cast<std::int64_t>(x), iy=static_cast<std::int64_t>(y);
            if (p.raster && !p.padding)
                return coverageOfPixel(maskRasterPixel(p,static_cast<int>(ix),static_cast<int>(iy)),p.channel);
            return p.cache->sample(p,ix,iy);
        };
        if (!std::isfinite(sx) || !std::isfinite(sy)) value=p.outside;
        else if (sampling == RasterSampling::Nearest) value = at(std::floor(sx), std::floor(sy));
        else {
            const double x = std::floor(sx-0.5), y = std::floor(sy-0.5);
            const double fx = sx-0.5-x, fy = sy-0.5-y;
            value = (at(x,y)*(1-fx) + at(x+1,y)*fx)*(1-fy)
                + (at(x,y+1)*(1-fx) + at(x+1,y+1)*fx)*fy;
        }
    }
    if (p.inverted) value = 1-value;
    return std::clamp(1-p.density+p.density*value, 0.0, 1.0);
}

double maskCoverage(const std::optional<PreparedMask> &mask, const FrameRenderTileRequest &request, int x, int y)
{
    if (!mask) return 1;
    const double dx = double(request.region.extent.width)/request.outputExtent.width;
    const double dy = double(request.region.extent.height)/request.outputExtent.height;
    const int samples = request.sampling == RasterSampling::Smooth ? 4 : 1;
    double value = 0;
    for (int iy = 0; iy < samples; ++iy) for (int ix = 0; ix < samples; ++ix)
        value += maskSample(*mask, request.region.origin.x+(x+(ix+0.5)/samples)*dx,
            request.region.origin.y+(y+(iy+0.5)/samples)*dy, request.sampling);
    return value/(samples*samples);
}

RasterProcessResult filterLayer(const RasterLayer &original, const std::vector<LayerFilter> &filters)
{
    auto current = original;
    for (const auto &filter : filters) {
        if (!filter.enabled || filter.opacity == 0) continue;
        auto processed = std::visit([&](const auto &operation) {
            if constexpr (std::is_same_v<std::decay_t<decltype(operation)>, RasterEffect>) return effectRaster(current,operation);
            else return adjustRaster(current,operation);
        }, filter.operation);
        if (!processed.ok()) return processed;
        for (std::size_t i = 0; i < current.pixels.size(); ++i) {
            const auto alpha = current.pixels[i] & 0xff000000U;
            const auto color = compositeRasterPixel(processed.pixels.pixels[i] | 0xff000000U,
                current.pixels[i] | 0xff000000U, filter.blendMode, filter.opacity, static_cast<std::uint32_t>(i));
            current.pixels[i] = alpha ? alpha | (color & 0x00ffffffU) : 0;
        }
    }
    return {std::move(current), {}};
}

FrameRenderResult renderLayerRegion(const Document &document,
                                    const Asset &asset,
                                    const LayerSample &properties,
                                    CanvasRegion region,
                                    CanvasExtent outputExtent,
                                    const RasterLayer *videoFrame,
                                    RasterSampling sampling,
                                    const OverlayAsset *overlay)
{
    ::LayerStack layerPieces;
    const bool streamChunks = overlay && !videoFrame && std::holds_alternative<ChunkedRasterAsset>(asset);
    RasterLayer streamed;
    if (streamChunks) streamed = makeRasterLayer(outputExtent.width, outputExtent.height, 0x00000000U);

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
        if (streamChunks) {
            // All pieces are visible SourceOver layers at unit opacity.
            // Compose them in the same canonical chunk order without keeping
            // one output-sized surface alive for every visible chunk.
            for (int y = 0; y < outputExtent.height; ++y) for (int x = 0; x < outputExtent.width; ++x) {
                const auto index = pixelIndex(outputExtent.width, x, y);
                streamed.pixels[index] = compositeRasterPixel(pixels.pixels[index], streamed.pixels[index],
                    RasterBlendMode::SourceOver, 1.0,
                    static_cast<std::uint32_t>(x) * 0x9e3779b9U ^ static_cast<std::uint32_t>(y));
            }
            return;
        }
        ::Layer engineLayer;
        engineLayer.surface = drawingSurfaceFromRasterLayer(std::move(pixels));
        engineLayer.metadata.visible = true;
        engineLayer.metadata.opacity = 1.0;
        engineLayer.metadata.blendMode = RasterBlendMode::SourceOver;
        layerPieces.layers.push_back(std::move(engineLayer));
    };
    const auto appendSource = [&](const RasterSource &source, AffineTransform transform) {
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
    const auto appendRasterLayer = [&](const RasterLayer &source, AffineTransform transform,
                                       const OverlayAsset *replacement = nullptr,
                                       std::int64_t originX = 0, std::int64_t originY = 0) {
        appendSource({&source, source.width, source.height, replacement, originX, originY}, transform);
    };

    if (videoFrame) {
        appendRasterLayer(*videoFrame, properties.transform);
    } else if (const auto *raster = std::get_if<RasterAsset>(&asset)) {
        appendRasterLayer(raster->pixels, properties.transform, overlay);
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
        const auto appendChunk = [&](const RasterChunk *chunk, std::int64_t column, std::int64_t row) {
            const auto originX = column * chunkSize, originY = row * chunkSize;
            const double chunkX = static_cast<double>(originX);
            const double chunkY = static_cast<double>(originY);
            AffineTransform transform = properties.transform;
            transform.translationX += transform.m11 * chunkX + transform.m21 * chunkY;
            transform.translationY += transform.m12 * chunkX + transform.m22 * chunkY;
            appendSource({chunk ? &chunk->pixels : nullptr, chunkSize, chunkSize,
                          overlay, originX, originY}, transform);
        };
        if (!overlay) {
            for (const RasterChunk &chunk : chunked.chunks)
                appendChunk(&chunk, chunk.column, chunk.row);
        } else {
            // The persisted chunk order is row-major. Merge preview-only
            // chunks in that order so smooth transformed composition matches
            // the committed document, without allocating a blank chunk image.
            std::map<std::pair<std::int64_t, std::int64_t>, const RasterChunk *> chunks;
            for (const RasterChunk &chunk : chunked.chunks)
                chunks.emplace(std::pair{std::int64_t(chunk.row), std::int64_t(chunk.column)}, &chunk);
            for (const auto &[key, pixels] : overlay->tiles) {
                (void)pixels;
                const auto x = key.column * OverlayTileSide, y = key.row * OverlayTileSide;
                const auto firstColumn = floorDivide(x, chunkSize), lastColumn = floorDivide(x + OverlayTileSide - 1, chunkSize);
                const auto firstRow = floorDivide(y, chunkSize), lastRow = floorDivide(y + OverlayTileSide - 1, chunkSize);
                for (auto row = firstRow; row <= lastRow; ++row)
                    for (auto column = firstColumn; column <= lastColumn; ++column)
                        chunks.try_emplace(std::pair{row, column}, nullptr);
            }
            for (const auto &[key, chunk] : chunks)
                appendChunk(chunk, key.second, key.first);
        }
    }

    FrameRenderResult result;
    result.origin = region.origin;
    result.pixels = streamChunks ? std::move(streamed) : compositeLayerStack(layerPieces,
                                        outputExtent.width,
                                        outputExtent.height,
                                        0x00000000U);
    return result;
}

FrameLayerTileRenderResult renderValidatedFrameLayerTiles(
    const Document &document,
    FrameIndex frame,
    std::size_t layerIndex,
    const std::vector<FrameRenderTileRequest> &requests,
    const PreparedOverlays *overlays = nullptr)
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
    const auto &appearance = properties.appearance;
    result.fillOpacity = appearance.fillOpacity; result.clippedToBelow = appearance.clippedToBelow;
    result.blendClippedAsGroup = appearance.blendClippedAsGroup; result.blendIf = appearance.blendIf;
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
    const auto *overlay = overlays ? overlays->get(asset) : nullptr;
    MaskTileCache maskCache;
    std::size_t maskId=0;
    const auto rasterMask = prepareMask(document, appearance.rasterMask, properties, sampled, maskCache, maskId++, overlays);
    const auto vectorMask = prepareMask(document, appearance.vectorMask, properties, sampled, maskCache, maskId++, overlays);
    const auto filterMask = prepareMask(document, appearance.filterMask, properties, sampled, maskCache, maskId++, overlays);
    std::vector<std::pair<MaskCombineMode, std::optional<PreparedMask>>> stack;
    if (appearance.maskStackEnabled) for (const auto &entry : appearance.maskStack) {
        if (entry.mask.enabled) stack.emplace_back(entry.combine, prepareMask(document, entry.mask, properties, sampled, maskCache, maskId++, overlays));
    }
    std::vector<const std::optional<PreparedMask> *> masks{&rasterMask, &vectorMask, &filterMask};
    for (const auto &entry : stack) masks.push_back(&entry.second);
    for (const auto *mask : masks) {
        if (!*mask) continue;
        const auto &t = (*mask)->transform;
        if (!std::isfinite(t.m11) || !std::isfinite(t.m12) || !std::isfinite(t.m21) || !std::isfinite(t.m22)
            || !std::isfinite(t.translationX) || !std::isfinite(t.translationY)
            || !std::isfinite(t.m11*t.m22-t.m12*t.m21)) {
            result.status = FrameRenderStatus::InvalidDocument;
            result.message = "sampled mask transform exceeds finite coordinates"; return result;
        }
    }
    std::optional<RasterLayer> filtered;
    if (std::any_of(appearance.filters.begin(), appearance.filters.end(), [](const LayerFilter &f) { return f.enabled && f.opacity > 0; })) {
        RasterLayer native;
        if (videoFrame) native = *videoFrame;
        else if (const auto *raster = std::get_if<RasterAsset>(asset)) {
            native = raster->pixels;
            if (overlay) overlay->apply(native);
        }
        else {
            const auto &vector = std::get<VectorAsset>(*asset);
            native = rasterizeVector(vector, {}, vector.viewport.width, vector.viewport.height);
        }
        auto processed = filterLayer(native, appearance.filters);
        if (!processed.ok()) { result.status = FrameRenderStatus::AssetResolutionFailed; result.message = processed.error; return result; }
        filtered = std::move(processed.pixels);
    }
    result.tiles.reserve(requests.size());
    for (const FrameRenderTileRequest &request : requests) {
        FrameRenderResult tile = renderLayerRegion(document,
                                                   *asset,
                                                   sampled,
                                                   request.region,
                                                   request.outputExtent,
                                                   videoFrame, request.sampling, overlay);
        if (filtered) {
            auto processed = renderLayerRegion(document,*asset,sampled,request.region,request.outputExtent,&*filtered,request.sampling,nullptr);
            if (filterMask) {
                RasterMask coverage{request.outputExtent,{}}; coverage.alpha.resize(tile.pixels.pixels.size());
                for (int y = 0; y < request.outputExtent.height; ++y) for (int x = 0; x < request.outputExtent.width; ++x)
                    coverage.alpha[pixelIndex(request.outputExtent.width,x,y)] = static_cast<std::uint8_t>(std::lround(255*maskCoverage(filterMask,request,x,y)));
                tile.pixels = blendRaster(tile.pixels,processed.pixels,coverage).pixels;
            } else tile.pixels = std::move(processed.pixels);
        }
        if (rasterMask || vectorMask || !stack.empty()) {
            // Visit each stack operand over a small output block. This retains
            // the bounded shared mask cache even for long ordered stacks,
            // without allocating an output-sized coverage plane.
            constexpr int blockSide=32;
            std::array<double,blockSide*blockSide> stackCoverage;
            for (int top=0;top<request.outputExtent.height;) {
              const int height=std::min(blockSide,request.outputExtent.height-top);
              for (int left=0;left<request.outputExtent.width;) {
                const int width=std::min(blockSide,request.outputExtent.width-left);
                std::fill_n(stackCoverage.begin(),width*height,1.0);
                bool first=true;
                for (const auto &[mode, mask] : stack) {
                    for (int y=0;y<height;++y) for (int x=0;x<width;++x) {
                        const double operand=maskCoverage(mask,request,left+x,top+y);
                        auto &coverage=stackCoverage[std::size_t(y)*width+x];
                        if (first) coverage=mode==MaskCombineMode::Subtract?1-operand:operand;
                        else switch (mode) {
                        case MaskCombineMode::Add: coverage=std::max(coverage,operand); break;
                        case MaskCombineMode::Subtract: coverage*=1-operand; break;
                        case MaskCombineMode::Intersect: coverage*=operand; break;
                        case MaskCombineMode::Replace: coverage=operand; break;
                        }
                    }
                    first=false;
                }
                for (int y=0;y<height;++y) for (int x=0;x<width;++x) {
                    auto &p=tile.pixels.pixels[pixelIndex(request.outputExtent.width,left+x,top+y)];
                    const double coverage=stackCoverage[std::size_t(y)*width+x];
                    const auto alpha=std::uint32_t(std::lround((p>>24)*coverage*
                        maskCoverage(rasterMask,request,left+x,top+y)*maskCoverage(vectorMask,request,left+x,top+y)));
                    p=alpha?(p&0x00ffffffU)|(alpha<<24):0;
                }
                left+=width;
              }
              top+=height;
            }
        }
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

bool validBlendIf(const std::optional<BlendIf> &value)
{
    if (!value) return true;
    const auto range = [](const BlendIfRange &r) {
        return std::isfinite(r.black) && std::isfinite(r.blackFade) && std::isfinite(r.whiteFade) && std::isfinite(r.white)
            && r.black >= 0 && r.black <= r.blackFade && r.blackFade <= r.whiteFade && r.whiteFade <= r.white && r.white <= 1;
    };
    return static_cast<unsigned>(value->channel) <= static_cast<unsigned>(BlendIfChannel::Blue)
        && range(value->source) && range(value->backdrop);
}

double blendIfValue(std::uint32_t p, BlendIfChannel channel)
{
    switch (channel) {
    case BlendIfChannel::Red: return double((p >> 16) & 255)/255;
    case BlendIfChannel::Green: return double((p >> 8) & 255)/255;
    case BlendIfChannel::Blue: return double(p & 255)/255;
    default: return (0.3*((p >> 16) & 255) + 0.59*((p >> 8) & 255) + 0.11*(p & 255))/255;
    }
}

double blendIfCoverage(const std::optional<BlendIf> &value, std::uint32_t source, std::uint32_t backdrop)
{
    if (!value) return 1;
    const auto coverage = [](const BlendIfRange &r, double v) {
        if (v < r.black || v > r.white) return 0.0;
        const double low = r.blackFade > r.black ? std::clamp((v-r.black)/(r.blackFade-r.black),0.0,1.0) : 1;
        const double high = r.white > r.whiteFade ? std::clamp((r.white-v)/(r.white-r.whiteFade),0.0,1.0) : 1;
        return low*high;
    };
    return coverage(value->source,blendIfValue(source,value->channel))
        * coverage(value->backdrop,blendIfValue(backdrop,value->channel));
}

void compositeTile(RasterLayer &destination, const RasterLayer &source,
                   const FrameLayerTileRenderResult &layer, const FrameRenderTileRequest &request,
                   double opacity, bool preserveAlpha = false, const RasterLayer *clip = nullptr)
{
    std::uint32_t idHash = 2166136261U;
    for (unsigned char c : layer.layerId) { idHash ^= c; idHash *= 16777619U; }
    const double dx = double(request.region.extent.width)/request.outputExtent.width;
    const double dy = double(request.region.extent.height)/request.outputExtent.height;
    for (int y = 0; y < destination.height; ++y) for (int x = 0; x < destination.width; ++x) {
        const auto index = pixelIndex(destination.width,x,y);
        const auto backdrop = destination.pixels[index];
        if (preserveAlpha && (backdrop >> 24) == 0) continue;
        const auto wx = static_cast<std::uint32_t>(static_cast<std::int64_t>(std::floor(request.region.origin.x+(x+0.5)*dx)));
        const auto wy = static_cast<std::uint32_t>(static_cast<std::int64_t>(std::floor(request.region.origin.y+(y+0.5)*dy)));
        double coverage = opacity * blendIfCoverage(layer.blendIf,source.pixels[index],backdrop);
        if (clip) coverage *= double(clip->pixels[index] >> 24)/255;
        const auto blended = compositeRasterPixel(source.pixels[index],preserveAlpha ? backdrop | 0xff000000U : backdrop,
            layer.blendMode,coverage,idHash ^ (wx*0x9e3779b9U) ^ wy);
        destination.pixels[index] = preserveAlpha ? (backdrop & 0xff000000U) | (blended & 0x00ffffffU) : blended;
    }
}

} // namespace

namespace render_detail {

FrameLayerBatchRenderResult preflightFrameLayerRender(
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

FrameLayerTileRenderResult renderPreflightedFrameLayerTiles(
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
    return renderFrame(document, frame, {});
}

FrameRenderResult renderFrame(const Document &document, FrameIndex frame,
                              std::span<const FrameBitmapOverlay> overlays)
{
    const auto view = documentViewRegion(document);
    const FrameTileRenderResult tiles = renderFrameTiles(
        document, frame, {{view, view.extent}}, overlays);
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
    const auto *a = findArtboard(document, id);
    return renderArtboardRegion(document, frame, id, a ? a->region : CanvasRegion{}, outputExtent, sampling);
}

FrameRenderResult renderArtboardRegion(const Document &document, FrameIndex frame, const std::string &id,
                                      CanvasRegion region, CanvasExtent outputExtent, RasterSampling sampling)
{
    return renderArtboardRegion(document, frame, id, region, outputExtent, sampling, {});
}

FrameRenderResult renderArtboardRegion(const Document &document, FrameIndex frame, const std::string &id,
                                      CanvasRegion region, CanvasExtent outputExtent, RasterSampling sampling,
                                      std::span<const FrameBitmapOverlay> overlays)
{
    const auto validation = validate(document);
    if (!validation.ok()) return errorResult(FrameRenderStatus::InvalidDocument, validation.issues.front().message);
    const auto *a = findArtboard(document, id);
    if (!a) return errorResult(FrameRenderStatus::ArtboardNotFound, "artboard id was not found");
    if (frame >= document.timeline.frameCount)
        return errorResult(FrameRenderStatus::FrameOutOfRange, "requested frame is outside the document timeline");
    const auto right = [](CanvasRegion r) { return std::int64_t(r.origin.x) + r.extent.width; };
    const auto bottom = [](CanvasRegion r) { return std::int64_t(r.origin.y) + r.extent.height; };
    const std::vector<FrameRenderTileRequest> requests{{region, outputExtent, sampling}};
    if (!validRequests(document, requests) || region.origin.x < a->region.origin.x || region.origin.y < a->region.origin.y
        || right(region) > right(a->region) || bottom(region) > bottom(a->region))
        return errorResult(FrameRenderStatus::InvalidRegion, "render region must be positive, contained by the artboard, and have a valid output extent and sampling policy");
    PreparedOverlays prepared;
    if (!prepared.prepare(document, overlays))
        return errorResult(FrameRenderStatus::InvalidOverlay, "bitmap overlays must reference distinct raster assets and distinct aligned 64x64 tiles");
    FrameLayerBatchRenderResult batch;
    batch.requests = requests;
    batch.artboards = {*a};
    batch.layers.reserve(document.layers.size());
    for (std::size_t index = 0; index < document.layers.size(); ++index) {
        if (layerProperties(document.layers[index]).artboardId == id) {
            auto layer = renderValidatedFrameLayerTiles(document, frame, index, batch.requests, &prepared);
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
    result.origin = region.origin; result.pixels = std::move(composed.tiles.front().pixels);
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
    return renderFrameRegion(document, frame, region, outputExtent, sampling, {});
}

FrameRenderResult renderFrameRegion(const Document &document, FrameIndex frame, CanvasRegion region,
                                   CanvasExtent outputExtent, RasterSampling sampling,
                                   std::span<const FrameBitmapOverlay> overlays)
{
    const FrameTileRenderResult tiles = renderFrameTiles(
        document, frame, {{region, outputExtent, sampling}}, overlays);
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
    return renderFrameLayerTiles(document, frame, layerIndex, requests, {});
}

FrameLayerTileRenderResult renderFrameLayerTiles(
    const Document &document, FrameIndex frame, std::size_t layerIndex,
    const std::vector<FrameRenderTileRequest> &requests,
    std::span<const FrameBitmapOverlay> overlays)
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
    PreparedOverlays prepared;
    if (!prepared.prepare(document, overlays)) {
        FrameLayerTileRenderResult result;
        result.layerIndex = layerIndex;
        result.status = FrameRenderStatus::InvalidOverlay;
        result.message = "bitmap overlays must reference distinct raster assets and distinct aligned 64x64 tiles";
        return result;
    }
    return renderValidatedFrameLayerTiles(document, frame, layerIndex, requests, &prepared);
}

FrameLayerBatchRenderResult renderFrameLayers(
    const Document &document,
    FrameIndex frame,
    const std::vector<FrameRenderTileRequest> &requests)
{
    return renderFrameLayers(document, frame, requests, {});
}

FrameLayerBatchRenderResult renderFrameLayers(
    const Document &document, FrameIndex frame,
    const std::vector<FrameRenderTileRequest> &requests,
    std::span<const FrameBitmapOverlay> overlays)
{
    FrameLayerBatchRenderResult result = render_detail::preflightFrameLayerRender(
        document, frame, requests);
    if (!result.ok()) {
        return result;
    }
    PreparedOverlays prepared;
    if (!prepared.prepare(document, overlays))
        return layerBatchError(FrameRenderStatus::InvalidOverlay,
            "bitmap overlays must reference distinct raster assets and distinct aligned 64x64 tiles", requests);
    result.layers.reserve(document.layers.size());
    for (std::size_t layerIndex = 0; layerIndex < document.layers.size(); ++layerIndex) {
        FrameLayerTileRenderResult layer =
            renderValidatedFrameLayerTiles(document, frame, layerIndex, requests, &prepared);
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

    std::unordered_set<std::string> clippingBases;
    for (const auto &layer : layers.layers) {
        if (!std::isfinite(layer.opacity) || layer.opacity < 0 || layer.opacity > 1
            || !std::isfinite(layer.fillOpacity) || layer.fillOpacity < 0 || layer.fillOpacity > 1
            || layer.blendMode == RasterBlendMode::DestinationOut
            || static_cast<unsigned>(layer.blendMode) > static_cast<unsigned>(RasterBlendMode::Dissolve)
            || !validBlendIf(layer.blendIf))
            return {{}, FrameRenderStatus::InvalidDocument, "layer composition parameters are invalid"};
        if (layer.role == LayerRole::Artwork) {
            const auto scope = layer.artboardId ? "artboard:" + *layer.artboardId : "world:";
            if (layer.clippedToBelow && !clippingBases.contains(scope))
                return {{}, FrameRenderStatus::InvalidDocument, "clipped layer has no base in its composition"};
            if (!layer.clippedToBelow) clippingBases.insert(scope);
        }
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

        const auto appendContent = [&](const std::optional<std::string> &id, RasterLayer &destination) {
            std::vector<const FrameLayerTileRenderResult *> content;
            for (const auto &layer : layers.layers) {
                if (layer.role == LayerRole::Artwork && layer.artboardId == id) content.push_back(&layer);
            }
            for (std::size_t i = 0; i < content.size();) {
                const auto &base = *content[i];
                if (base.clippedToBelow) { ++i; continue; }
                std::size_t end = i+1;
                while (end < content.size() && content[end]->clippedToBelow) ++end;
                if (base.visible) {
                    const auto &pixels = base.tiles[requestIndex].pixels;
                    if (end == i+1) compositeTile(destination,pixels,base,request,base.opacity*base.fillOpacity);
                    else if (base.blendClippedAsGroup) {
                        auto group = pixels;
                        for (auto &p : group.pixels) {
                            const auto alpha = std::uint32_t(std::lround((p >> 24)*base.fillOpacity));
                            p = alpha ? (alpha << 24) | (p & 0x00ffffffU) : 0;
                        }
                        for (std::size_t child = i+1; child < end; ++child) {
                            const auto &layer = *content[child];
                            if (layer.visible) compositeTile(group,layer.tiles[requestIndex].pixels,layer,request,
                                layer.opacity*layer.fillOpacity,true);
                        }
                        compositeTile(destination,group,base,request,base.opacity);
                    } else {
                        compositeTile(destination,pixels,base,request,base.opacity*base.fillOpacity);
                        for (std::size_t child = i+1; child < end; ++child) {
                            const auto &layer = *content[child];
                            if (layer.visible) compositeTile(destination,layer.tiles[requestIndex].pixels,layer,request,
                                layer.opacity*layer.fillOpacity*base.opacity*base.fillOpacity,false,&pixels);
                        }
                    }
                }
                i = end;
            }
        };
        auto composed = makeRasterLayer(request.outputExtent.width,request.outputExtent.height,0);
        for (const auto &a : layers.artboards) {
            if (!a.visible) continue;
            auto group = makeRasterLayer(request.outputExtent.width,request.outputExtent.height,a.backgroundArgb);
            clipToArtboard(group,a.region,request); appendContent(a.id,group);
            FrameLayerTileRenderResult metadata;
            compositeTile(composed,group,metadata,request,1);
        }
        appendContent(std::nullopt,composed);

        result.tiles.push_back({
            request.region,
            std::move(composed),
        });
    }
    return result;
}

FrameTileRenderResult renderFrameTiles(
    const Document &document,
    FrameIndex frame,
    const std::vector<FrameRenderTileRequest> &requests)
{
    return renderFrameTiles(document, frame, requests, {});
}

FrameTileRenderResult renderFrameTiles(
    const Document &document, FrameIndex frame,
    const std::vector<FrameRenderTileRequest> &requests,
    std::span<const FrameBitmapOverlay> overlays)
{
    return composeFrameLayers(renderFrameLayers(document, frame, requests, overlays));
}

} // namespace iiSharedCanvas
