#pragma once

#include "iiSharedCanvas/Export.h"
#include <iiFileProvider.h>
#include "Metadata/StableDiffusionMetadata.h"
#include "ControlNet/SemanticSegment.h"
#include "ControlNet/Pose.h"
#include "ControlNet/Depth.h"
#include "ControlNet/IpAdapter.h"
#include "ControlNet/Reference.h"
#include "ControlNet/Tile.h"
#include "ControlNet/Shuffle.h"
#include "ControlNet/NormalMap.h"
#include "ControlNet/LineArt.h"
#include "ControlNet/Canny.h"
#include "ControlNet/Mlsd.h"
#include "ControlNet/Scribble.h"

#include <Core/RasterBlendMode.h>
#include <Layer/RasterLayer.h>
#include <Transform/Transform.h>

#include <concepts>
#include <type_traits>
#include <cstddef>
#include <cstdint>
#include <optional>
#include <string>
#include <variant>
#include <vector>
#include <utility>

namespace iiSharedCanvas {

inline constexpr std::uint16_t CurrentFormatMajor = 1;
inline constexpr std::uint16_t CurrentFormatMinor = 19;

using FrameIndex = std::uint32_t;

struct FormatVersion {
    std::uint16_t major = CurrentFormatMajor;
    std::uint16_t minor = CurrentFormatMinor;
};

struct CanvasExtent {
    std::int32_t width = 0;
    std::int32_t height = 0;
    bool operator==(const CanvasExtent &) const = default;
};

struct CanvasOrigin {
    std::int32_t x = 0;
    std::int32_t y = 0;
    bool operator==(const CanvasOrigin &) const = default;
};

struct CanvasRegion {
    CanvasOrigin origin;
    CanvasExtent extent;
    bool operator==(const CanvasRegion &) const = default;
};

// Independent, non-nesting canvas group. Owned layer transforms are local to origin.
struct Artboard {
    std::string id;
    std::string name;
    CanvasRegion region;
    std::uint32_t backgroundArgb = 0xffffffffU; // Zero alpha is transparent.
    bool visible = true;
    bool operator==(const Artboard &) const = default;
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
    friend bool operator==(const Point &, const Point &) = default;
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

// Self-contained constant-rate display frames; no external decoder is needed to render.
struct VideoAsset {
    std::string id;
    FrameRate frameRate;
    std::vector<RasterLayer> frames;
};

struct PoseAsset {
    std::string id;
    CanvasExtent viewport;
    std::vector<PosePerson> people;
};

struct DepthAsset {
    std::string id;
    CanvasExtent viewport;
    std::vector<double> values; // Row-major normalized proximity, [0,1], zero = empty.
};

struct LineArtAsset {
    std::string id;
    CanvasExtent viewport;
    std::vector<double> coverage; // Row-major [0,1]: white background to full black ink.
};

struct CannyAsset {
    std::string id;
    CanvasExtent viewport;
    std::vector<std::uint8_t> mask; // Binary edge field: 0 background, 1 edge.
};

struct ScribbleAsset {
    std::string id;
    CanvasExtent viewport;
    std::vector<std::uint8_t> mask; // Binary stroke field: 0 background, 1 stroke.
};

struct MlsdAsset {
    std::string id;
    CanvasExtent viewport;
    std::vector<MlsdSegment> segments; // Editable straight-line geometry, not a baked mask.
};

struct NormalMapAsset {
    std::string id;
    CanvasExtent viewport;
    std::vector<NormalMapSample> samples; // Row-major native signed XYZ unit normals.
};

struct ShuffleAsset {
    std::string id;
    CanvasExtent viewport;
    std::vector<ShuffleColor> colors; // Row-major prepared RGB conditioning image.
};

struct TileAsset {
    std::string id;
    CanvasExtent viewport;
    std::vector<TileColor> colors; // Row-major prepared RGB conditioning image.
};

struct ReferenceAsset {
    std::string id;
    CanvasExtent viewport;
    std::vector<ReferenceColor> colors; // Row-major prepared RGB conditioning image.
};

struct IpAdapterAsset {
    std::string id;
    IpAdapterEmbeddingDescriptor descriptor;
    IpAdapterTensor conditional;
    std::optional<IpAdapterTensor> unconditional; // Absence is explicit; never infer zeros.
    bool operator==(const IpAdapterAsset &) const = default;
};

using Asset = std::variant<RasterAsset, VectorAsset, ChunkedRasterAsset, VideoAsset, PoseAsset, DepthAsset, LineArtAsset, CannyAsset, ScribbleAsset, MlsdAsset, NormalMapAsset, ShuffleAsset, TileAsset, ReferenceAsset, IpAdapterAsset>;

enum class ContentKind {
    Raster,
    Vector,
    Video,
    Pose,
    Depth,
    LineArt,
    Canny,
    Scribble,
    Mlsd,
    NormalMap,
    Shuffle,
    Tile,
    Reference,
    IpAdapter,
};

// Four visual content identities. Nonspatial conditioning is a separate role.
enum class LayerTiming : std::uint8_t { Static, Dynamic };
enum class LayerRepresentation : std::uint8_t { Bitmap, Vector, Embedding };
enum class LayerKind : std::uint8_t {
    StaticBitmap,
    StaticVector,
    DynamicBitmap,
    DynamicVector,
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

// Distinct content types; their common source bases only expose reference/index
// storage. A static content cannot be constructed from a dynamic source.
struct StaticBitmapContent : StaticSource {
    StaticBitmapContent(std::string id = {}) : StaticSource{std::move(id)} {}
    template<class Source> requires std::same_as<std::remove_cvref_t<Source>, StaticSource>
    StaticBitmapContent(Source &&value) : StaticSource{std::forward<Source>(value)} {}
};
struct StaticVectorContent : StaticSource {
    StaticVectorContent(std::string id = {}) : StaticSource{std::move(id)} {}
    template<class Source> requires std::same_as<std::remove_cvref_t<Source>, StaticSource>
    StaticVectorContent(Source &&value) : StaticSource{std::forward<Source>(value)} {}
};
struct DynamicBitmapContent : KeyframedSource {
    DynamicBitmapContent() = default;
    template<class Source> requires std::same_as<std::remove_cvref_t<Source>, KeyframedSource>
    DynamicBitmapContent(Source &&value) : KeyframedSource{std::forward<Source>(value)} {}
};
struct DynamicVectorContent : KeyframedSource {
    DynamicVectorContent() = default;
    template<class Source> requires std::same_as<std::remove_cvref_t<Source>, KeyframedSource>
    DynamicVectorContent(Source &&value) : KeyframedSource{std::forward<Source>(value)} {}
};

struct LayerFrameRange {
    FrameIndex firstFrame = 0;
    FrameIndex lastFrame = 0;

    friend constexpr bool operator==(const LayerFrameRange &,
                                     const LayerFrameRange &) = default;
};

enum class MotionInterpolation : std::uint8_t { Hold, Linear, SmoothStep };

struct MotionValue {
    Point position;
    Point scale{1.0, 1.0};
    Point anchor;
    double rotationDegrees = 0.0; // Unwrapped; 0 -> 720 performs two revolutions.
    double opacity = 1.0; // Multiplies the layer's base opacity.
    friend bool operator==(const MotionValue &, const MotionValue &) = default;
};

struct MotionKeyframe {
    FrameIndex frame = 0; // Absolute document frame.
    MotionValue value;
    MotionInterpolation interpolation = MotionInterpolation::Linear; // Outgoing segment.
    friend bool operator==(const MotionKeyframe &, const MotionKeyframe &) = default;
};

struct LayerProperties {
    std::string id;
    std::string name;
    bool visible = true;
    double opacity = 1.0;
    AffineTransform transform;
    RasterBlendMode blendMode = RasterBlendMode::SourceOver;
    std::optional<LayerFrameRange> frameRange;
    std::vector<MotionKeyframe> motion; // Empty preserves the static layer properties.
    std::optional<std::string> artboardId; // Absence means unclipped world-space artwork.
};

struct StaticBitmapLayer {
    LayerProperties properties;
    StaticBitmapContent content;
};

struct StaticVectorLayer {
    LayerProperties properties;
    StaticVectorContent content;
};

struct DynamicBitmapLayer {
    LayerProperties properties;
    DynamicBitmapContent content;
};

struct DynamicVectorLayer {
    LayerProperties properties;
    DynamicVectorContent content;
};

enum class VideoEndBehavior : std::uint8_t { Transparent, Hold };

struct VideoPlayback {
    FrameIndex sourceInFrame = 0;
    std::optional<FrameIndex> sourceOutFrame; // Exclusive; absent means the asset end.
    VideoEndBehavior endBehavior = VideoEndBehavior::Transparent;
    friend bool operator==(const VideoPlayback &, const VideoPlayback &) = default;
};

struct VideoLayer {
    LayerProperties properties;
    LayerSource source; // Must be a StaticSource referring to one VideoAsset.
    VideoPlayback playback; // Starts at frameRange.firstFrame, or zero when absent.
};

// Conditioning role is orthogonal to timing/representation. Includes IP-Adapter
// attention conditioning; it does not imply the ControlNet network architecture.
enum class LayerRole : std::uint8_t { Artwork, ControlNet };
enum class ControlNetKind : std::uint8_t { SemanticSegment, Pose, Depth, LineArt, Canny, Scribble, Mlsd, NormalMap, Shuffle, Tile, Reference, IpAdapter };

struct SemanticSegmentLayer {
    LayerProperties properties;
    LayerSource source; // RasterAsset identity mask, static or frame-selected.
    ControlNetSettings control;
    SemanticSegmentation segmentation;
};

struct PoseLayer {
    LayerProperties properties;
    LayerSource source; // Static or keyframed PoseAsset, with editable vector anchors.
    ControlNetSettings control;
};

struct DepthLayer {
    LayerProperties properties;
    LayerSource source; // Static or keyframed dense DepthAsset.
    ControlNetSettings control;
};

struct LineArtLayer {
    LayerProperties properties;
    LayerSource source; // Static or keyframed dense LineArtAsset.
    ControlNetSettings control;
};

struct CannyLayer {
    LayerProperties properties;
    LayerSource source; // Static or keyframed CannyAsset.
    ControlNetSettings control;
};

struct ScribbleLayer {
    LayerProperties properties;
    LayerSource source; // Static or keyframed ScribbleAsset.
    ControlNetSettings control;
};

struct MlsdLayer {
    LayerProperties properties;
    LayerSource source; // Static or keyframed MlsdAsset geometry.
    ControlNetSettings control;
};

struct NormalMapLayer {
    LayerProperties properties;
    LayerSource source; // Static or keyframed dense NormalMapAsset.
    ControlNetSettings control;
};

struct ShuffleLayer {
    LayerProperties properties;
    LayerSource source; // Static or keyframed dense ShuffleAsset.
    ControlNetSettings control;
};

struct TileLayer {
    LayerProperties properties;
    LayerSource source; // Static or keyframed dense TileAsset.
    ControlNetSettings control;
};

struct ReferenceLayer {
    LayerProperties properties;
    LayerSource source; // Static or keyframed dense ReferenceAsset.
    ControlNetSettings control;
    ReferenceSettings reference;
};

struct IpAdapterLayer {
    LayerProperties properties;
    LayerSource source; // Static or keyframed embedding state, not spatial pixels.
    ControlNetSettings control; // Adapter identity must match every referenced asset.
};

using Layer = std::variant<StaticBitmapLayer, StaticVectorLayer, DynamicBitmapLayer, DynamicVectorLayer, VideoLayer, SemanticSegmentLayer, PoseLayer, DepthLayer, LineArtLayer, CannyLayer, ScribbleLayer, MlsdLayer, NormalMapLayer, ShuffleLayer, TileLayer, ReferenceLayer, IpAdapterLayer>;

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
    iiFileProvider::Authorship authorship;
    std::vector<Artboard> artboards; // Bottom-to-top group order; no nesting.
};

struct AssetReference {
    std::size_t layerIndex = 0;
    std::optional<std::size_t> frameIndex;
    std::optional<std::size_t> keyframeIndex;
};

// Updates the cached metadata dump and upgrades legacy documents on actual edits.
IISHAREDCANVAS_EXPORT void recordDocumentChange(Document &document);

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
// Core artwork identity comes from its concrete layer/content alternative.
IISHAREDCANVAS_EXPORT LayerTiming layerTiming(const Layer &layer) noexcept;
IISHAREDCANVAS_EXPORT LayerRepresentation layerRepresentation(const Layer &layer) noexcept;
IISHAREDCANVAS_EXPORT std::optional<LayerKind> layerKind(const Layer &layer) noexcept;
IISHAREDCANVAS_EXPORT bool isLineControlNetLayer(const Layer &) noexcept;
IISHAREDCANVAS_EXPORT LayerRole layerRole(const Layer &layer) noexcept;
IISHAREDCANVAS_EXPORT std::optional<ControlNetKind> controlNetKind(const Layer &layer) noexcept;
IISHAREDCANVAS_EXPORT const std::string &assetId(const Asset &asset) noexcept;
IISHAREDCANVAS_EXPORT LayerProperties &layerProperties(Layer &layer) noexcept;
IISHAREDCANVAS_EXPORT const LayerProperties &layerProperties(const Layer &layer) noexcept;
// Detached compatibility/serialization view, never a writable source variant.
IISHAREDCANVAS_EXPORT LayerSource layerSource(const Layer &layer);
IISHAREDCANVAS_EXPORT StaticSource *staticLayerSource(Layer &layer) noexcept;
IISHAREDCANVAS_EXPORT const StaticSource *staticLayerSource(const Layer &layer) noexcept;
IISHAREDCANVAS_EXPORT KeyframedSource *keyframedLayerSource(Layer &layer) noexcept;
IISHAREDCANVAS_EXPORT const KeyframedSource *keyframedLayerSource(const Layer &layer) noexcept;
// Aggregate conversion; call validate afterwards, or use DocumentEditor setters.
IISHAREDCANVAS_EXPORT void setLayerSource(Layer &layer, LayerSource source);
IISHAREDCANVAS_EXPORT Layer makeBitmapLayer(LayerProperties properties, LayerSource source);
IISHAREDCANVAS_EXPORT Layer makeVectorLayer(LayerProperties properties, LayerSource source);
IISHAREDCANVAS_EXPORT bool layerExistsAt(const Document &document,
                                         const Layer &layer,
                                         FrameIndex frame) noexcept;
IISHAREDCANVAS_EXPORT CanvasOrigin canvasOrigin(const Document &document) noexcept;
IISHAREDCANVAS_EXPORT CanvasRegion canvasRegion(const Document &document) noexcept;
// Allocated canvas plus all artboards, including hidden ones. Invalid bounds yield {}.
IISHAREDCANVAS_EXPORT CanvasRegion documentViewRegion(const Document &document) noexcept;
IISHAREDCANVAS_EXPORT Artboard *findArtboard(Document &, const std::string &id) noexcept;
IISHAREDCANVAS_EXPORT const Artboard *findArtboard(const Document &, const std::string &id) noexcept;
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
IISHAREDCANVAS_EXPORT VideoAsset *findVideoAsset(Document &document, const std::string &id) noexcept;
IISHAREDCANVAS_EXPORT const VideoAsset *findVideoAsset(const Document &document, const std::string &id) noexcept;
IISHAREDCANVAS_EXPORT std::optional<std::size_t> assetIndex(const Document &document,
                                                           const std::string &id) noexcept;
IISHAREDCANVAS_EXPORT Layer *findLayer(Document &document,
                                      const std::string &id) noexcept;
IISHAREDCANVAS_EXPORT const Layer *findLayer(const Document &document,
                                             const std::string &id) noexcept;
IISHAREDCANVAS_EXPORT StaticBitmapLayer *findStaticBitmapLayer(Document &, const std::string &) noexcept;
IISHAREDCANVAS_EXPORT const StaticBitmapLayer *findStaticBitmapLayer(const Document &, const std::string &) noexcept;
IISHAREDCANVAS_EXPORT StaticVectorLayer *findStaticVectorLayer(Document &, const std::string &) noexcept;
IISHAREDCANVAS_EXPORT const StaticVectorLayer *findStaticVectorLayer(const Document &, const std::string &) noexcept;
IISHAREDCANVAS_EXPORT DynamicBitmapLayer *findDynamicBitmapLayer(Document &, const std::string &) noexcept;
IISHAREDCANVAS_EXPORT const DynamicBitmapLayer *findDynamicBitmapLayer(const Document &, const std::string &) noexcept;
IISHAREDCANVAS_EXPORT DynamicVectorLayer *findDynamicVectorLayer(Document &, const std::string &) noexcept;
IISHAREDCANVAS_EXPORT const DynamicVectorLayer *findDynamicVectorLayer(const Document &, const std::string &) noexcept;
IISHAREDCANVAS_EXPORT VideoLayer *findVideoLayer(Document &document, const std::string &id) noexcept;
IISHAREDCANVAS_EXPORT const VideoLayer *findVideoLayer(const Document &document, const std::string &id) noexcept;
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
