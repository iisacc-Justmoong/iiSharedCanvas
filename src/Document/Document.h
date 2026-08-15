#pragma once

#include "Export.h"

#include <Core/RasterBlendMode.h>
#include <Layer/RasterLayer.h>
#include <Transform/Transform.h>

#include <cstdint>
#include <optional>
#include <string>
#include <variant>
#include <vector>

namespace iiSharedCanvas {

inline constexpr std::uint16_t CurrentFormatMajor = 1;
inline constexpr std::uint16_t CurrentFormatMinor = 0;

using FrameIndex = std::uint32_t;

struct FormatVersion {
    std::uint16_t major = CurrentFormatMajor;
    std::uint16_t minor = CurrentFormatMinor;
};

struct CanvasExtent {
    std::int32_t width = 0;
    std::int32_t height = 0;
};

struct FrameRate {
    std::uint32_t numerator = 24;
    std::uint32_t denominator = 1;
};

struct Timeline {
    FrameRate frameRate;
    FrameIndex frameCount = 1;
};

struct Point {
    double x = 0.0;
    double y = 0.0;
};

struct MoveTo { Point point; };
struct LineTo { Point point; };
struct QuadraticTo { Point control; Point end; };
struct CubicTo { Point control1; Point control2; Point end; };
struct ClosePath {};
using PathCommand = std::variant<MoveTo, LineTo, QuadraticTo, CubicTo, ClosePath>;

struct SolidPaint {
    std::uint32_t argb = 0xff000000U;
};

struct StrokeStyle {
    SolidPaint paint;
    double width = 1.0;
};

struct VectorPath {
    std::vector<PathCommand> commands;
    std::optional<SolidPaint> fill;
    std::optional<StrokeStyle> stroke;
};

struct RasterAsset {
    std::string id;
    RasterLayer pixels;
};

struct VectorAsset {
    std::string id;
    CanvasExtent viewport;
    std::vector<VectorPath> paths;
};

using Asset = std::variant<RasterAsset, VectorAsset>;

enum class ContentKind {
    Raster,
    Vector,
};

struct StaticSource {
    std::string assetId;
};

struct Keyframe {
    FrameIndex frame = 0;
    std::string assetId;
};

struct KeyframedSource {
    ContentKind kind = ContentKind::Raster;
    std::vector<Keyframe> keyframes;
};

using LayerSource = std::variant<StaticSource, KeyframedSource>;

struct Layer {
    std::string id;
    std::string name;
    bool visible = true;
    double opacity = 1.0;
    AffineTransform transform;
    RasterBlendMode blendMode = RasterBlendMode::SourceOver;
    LayerSource source;
};

struct Document {
    FormatVersion formatVersion;
    CanvasExtent extent;
    Timeline timeline;
    std::vector<Asset> assets;
    std::vector<Layer> layers;
};

IISHAREDCANVAS_EXPORT ContentKind contentKind(const Asset &asset) noexcept;
IISHAREDCANVAS_EXPORT const std::string &assetId(const Asset &asset) noexcept;
IISHAREDCANVAS_EXPORT Asset *findAsset(Document &document,
                                       const std::string &id) noexcept;
IISHAREDCANVAS_EXPORT const Asset *findAsset(const Document &document,
                                             const std::string &id) noexcept;
IISHAREDCANVAS_EXPORT const Asset *resolveAssetAt(const Document &document,
                                                  const Layer &layer,
                                                  FrameIndex frame) noexcept;

} // namespace iiSharedCanvas
