#pragma once

#include "iiSharedCanvas/Export.h"
#include "Metadata/StableDiffusionMetadata.h"

#include <Core/RasterBlendMode.h>
#include <Layer/RasterLayer.h>
#include <Transform/Transform.h>

#include <cstddef>
#include <cstdint>
#include <optional>
#include <string>
#include <variant>
#include <vector>

namespace iiSharedCanvas {

inline constexpr std::uint16_t CurrentFormatMajor = 1;
inline constexpr std::uint16_t CurrentFormatMinor = 4;

using FrameIndex = std::uint32_t;

struct FormatVersion {
    std::uint16_t major = CurrentFormatMajor;
    std::uint16_t minor = CurrentFormatMinor;
};

struct CanvasExtent {
    std::int32_t width = 0;
    std::int32_t height = 0;
};

struct CanvasOrigin {
    std::int32_t x = 0;
    std::int32_t y = 0;
};

struct CanvasRegion {
    CanvasOrigin origin;
    CanvasExtent extent;
};

enum class CanvasMode : std::uint8_t {
    Finite,
    Infinite,
};

struct InfiniteCanvas {
    CanvasOrigin origin;
    std::int32_t chunkSize = 256;
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

struct RasterChunk {
    std::int32_t column = 0;
    std::int32_t row = 0;
    RasterLayer pixels;
};

struct ChunkedRasterAsset {
    std::string id;
    std::vector<RasterChunk> chunks;
};

struct VectorAsset {
    std::string id;
    CanvasExtent viewport;
    std::vector<VectorPath> paths;
};

using Asset = std::variant<RasterAsset, VectorAsset, ChunkedRasterAsset>;

enum class ContentKind {
    Raster,
    Vector,
};

struct StaticSource {
    std::string assetId;
};

struct Keyframe {
    std::string layerId;
    std::string assetId;
};

struct Frame {
    FrameIndex index = 0;
    std::vector<Keyframe> keyframes;
};

struct KeyframedSource {
    std::vector<FrameIndex> frameIndices;
};

using LayerSource = std::variant<StaticSource, KeyframedSource>;

struct LayerFrameRange {
    FrameIndex firstFrame = 0;
    FrameIndex lastFrame = 0;

    friend constexpr bool operator==(const LayerFrameRange &,
                                     const LayerFrameRange &) = default;
};

struct LayerProperties {
    std::string id;
    std::string name;
    bool visible = true;
    double opacity = 1.0;
    AffineTransform transform;
    RasterBlendMode blendMode = RasterBlendMode::SourceOver;
    std::optional<LayerFrameRange> frameRange;
};

struct BitmapLayer {
    LayerProperties properties;
    LayerSource source;
};

struct VectorLayer {
    LayerProperties properties;
    LayerSource source;
};

using Layer = std::variant<BitmapLayer, VectorLayer>;

// Owned interleaved signed PCM16. A sample frame contains channelCount samples.
struct AudioAsset {
    std::string id;
    std::uint32_t sampleRate = 48000;
    std::uint16_t channelCount = 2;
    std::vector<std::int16_t> samples;

    friend bool operator==(const AudioAsset &, const AudioAsset &) = default;
};

struct AudioClip {
    std::string id;
    std::string name;
    std::string assetId;
    FrameIndex startFrame = 0;
    FrameIndex durationFrames = 1;
    std::uint64_t sourceOffsetSamples = 0; // Per-channel source sample frames.
    double gainDb = 0.0;
    bool enabled = true;

    friend bool operator==(const AudioClip &, const AudioClip &) = default;
};

struct AudioTrackLayer {
    std::string id;
    std::string name;
    bool muted = false;
    double gainDb = 0.0;
    std::vector<AudioClip> clips; // Ordered by startFrame; no intra-track overlap.

    friend bool operator==(const AudioTrackLayer &, const AudioTrackLayer &) = default;
};

struct Document {
    FormatVersion formatVersion;
    CanvasExtent extent;
    CanvasMode canvasMode = CanvasMode::Finite;
    InfiniteCanvas infiniteCanvas;
    Timeline timeline;
    std::vector<Asset> assets;
    std::vector<Layer> layers;
    std::vector<Frame> frames;
    std::optional<StableDiffusionMetadata> stableDiffusionMetadata;
    std::vector<AudioAsset> audioAssets;
    std::vector<AudioTrackLayer> audioTracks;
};

struct AssetReference {
    std::size_t layerIndex = 0;
    std::optional<std::size_t> frameIndex;
    std::optional<std::size_t> keyframeIndex;
};

IISHAREDCANVAS_EXPORT ContentKind contentKind(const Asset &asset) noexcept;
// ceil(frameCount * frameRate.denominator * sampleRate / frameRate.numerator).
// Invalid rates or uint64 overflow return nullopt.
IISHAREDCANVAS_EXPORT std::optional<std::uint64_t> audioSampleFrameCount(
    FrameIndex frameCount, FrameRate frameRate, std::uint32_t sampleRate) noexcept;
IISHAREDCANVAS_EXPORT AudioAsset *findAudioAsset(Document &document,
                                                const std::string &id) noexcept;
IISHAREDCANVAS_EXPORT const AudioAsset *findAudioAsset(const Document &document,
                                                      const std::string &id) noexcept;
IISHAREDCANVAS_EXPORT AudioTrackLayer *findAudioTrack(Document &document,
                                                     const std::string &id) noexcept;
IISHAREDCANVAS_EXPORT const AudioTrackLayer *findAudioTrack(const Document &document,
                                                           const std::string &id) noexcept;
IISHAREDCANVAS_EXPORT AudioClip *findAudioClip(AudioTrackLayer &track,
                                              const std::string &id) noexcept;
IISHAREDCANVAS_EXPORT const AudioClip *findAudioClip(const AudioTrackLayer &track,
                                                    const std::string &id) noexcept;
IISHAREDCANVAS_EXPORT ContentKind contentKind(const Layer &layer) noexcept;
IISHAREDCANVAS_EXPORT const std::string &assetId(const Asset &asset) noexcept;
IISHAREDCANVAS_EXPORT LayerProperties &layerProperties(Layer &layer) noexcept;
IISHAREDCANVAS_EXPORT const LayerProperties &layerProperties(const Layer &layer) noexcept;
IISHAREDCANVAS_EXPORT LayerSource &layerSource(Layer &layer) noexcept;
IISHAREDCANVAS_EXPORT const LayerSource &layerSource(const Layer &layer) noexcept;
IISHAREDCANVAS_EXPORT bool layerExistsAt(const Document &document,
                                         const Layer &layer,
                                         FrameIndex frame) noexcept;
IISHAREDCANVAS_EXPORT CanvasOrigin canvasOrigin(const Document &document) noexcept;
IISHAREDCANVAS_EXPORT CanvasRegion canvasRegion(const Document &document) noexcept;
IISHAREDCANVAS_EXPORT Asset *findAsset(Document &document,
                                       const std::string &id) noexcept;
IISHAREDCANVAS_EXPORT const Asset *findAsset(const Document &document,
                                             const std::string &id) noexcept;
IISHAREDCANVAS_EXPORT RasterAsset *findRasterAsset(Document &document,
                                                   const std::string &id) noexcept;
IISHAREDCANVAS_EXPORT const RasterAsset *findRasterAsset(const Document &document,
                                                         const std::string &id) noexcept;
IISHAREDCANVAS_EXPORT ChunkedRasterAsset *findChunkedRasterAsset(
    Document &document,
    const std::string &id) noexcept;
IISHAREDCANVAS_EXPORT const ChunkedRasterAsset *findChunkedRasterAsset(
    const Document &document,
    const std::string &id) noexcept;
IISHAREDCANVAS_EXPORT RasterChunk *findRasterChunk(ChunkedRasterAsset &asset,
                                                   std::int32_t column,
                                                   std::int32_t row) noexcept;
IISHAREDCANVAS_EXPORT const RasterChunk *findRasterChunk(const ChunkedRasterAsset &asset,
                                                         std::int32_t column,
                                                         std::int32_t row) noexcept;
IISHAREDCANVAS_EXPORT VectorAsset *findVectorAsset(Document &document,
                                                   const std::string &id) noexcept;
IISHAREDCANVAS_EXPORT const VectorAsset *findVectorAsset(const Document &document,
                                                         const std::string &id) noexcept;
IISHAREDCANVAS_EXPORT std::optional<std::size_t> assetIndex(const Document &document,
                                                           const std::string &id) noexcept;
IISHAREDCANVAS_EXPORT Layer *findLayer(Document &document,
                                      const std::string &id) noexcept;
IISHAREDCANVAS_EXPORT const Layer *findLayer(const Document &document,
                                             const std::string &id) noexcept;
IISHAREDCANVAS_EXPORT BitmapLayer *findBitmapLayer(Document &document,
                                                   const std::string &id) noexcept;
IISHAREDCANVAS_EXPORT const BitmapLayer *findBitmapLayer(const Document &document,
                                                         const std::string &id) noexcept;
IISHAREDCANVAS_EXPORT VectorLayer *findVectorLayer(Document &document,
                                                   const std::string &id) noexcept;
IISHAREDCANVAS_EXPORT const VectorLayer *findVectorLayer(const Document &document,
                                                         const std::string &id) noexcept;
IISHAREDCANVAS_EXPORT std::optional<std::size_t> layerIndex(const Document &document,
                                                           const std::string &id) noexcept;
IISHAREDCANVAS_EXPORT Frame *findFrame(Document &document,
                                      FrameIndex frame) noexcept;
IISHAREDCANVAS_EXPORT const Frame *findFrame(const Document &document,
                                            FrameIndex frame) noexcept;
IISHAREDCANVAS_EXPORT std::optional<std::size_t> frameIndex(
    const Document &document,
    FrameIndex frame) noexcept;
IISHAREDCANVAS_EXPORT Keyframe *findKeyframe(Frame &frame,
                                            const std::string &layerId) noexcept;
IISHAREDCANVAS_EXPORT const Keyframe *findKeyframe(
    const Frame &frame,
    const std::string &layerId) noexcept;
IISHAREDCANVAS_EXPORT Keyframe *findKeyframe(Document &document,
                                            const std::string &layerId,
                                            FrameIndex frame) noexcept;
IISHAREDCANVAS_EXPORT const Keyframe *findKeyframe(
    const Document &document,
    const std::string &layerId,
    FrameIndex frame) noexcept;
IISHAREDCANVAS_EXPORT std::optional<std::size_t> keyframeIndex(
    const Frame &frame,
    const std::string &layerId) noexcept;
IISHAREDCANVAS_EXPORT std::vector<AssetReference> assetReferences(
    const Document &document,
    const std::string &assetId);
IISHAREDCANVAS_EXPORT const Asset *resolveAssetAt(const Document &document,
                                                  const Layer &layer,
                                                  FrameIndex frame) noexcept;

} // namespace iiSharedCanvas
