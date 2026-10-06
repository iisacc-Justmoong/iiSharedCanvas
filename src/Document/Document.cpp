#include "Document/Document.h"

#include <algorithm>
#include <iterator>
#include <limits>
#include <unordered_map>
#include <type_traits>

namespace iiSharedCanvas {

void recordDocumentChange(Document &document)
{
    document.authorship.recordChange();
    document.formatVersion.minor = CurrentFormatMinor;
}


std::optional<std::uint64_t> audioSampleFrameCount(
    FrameIndex frameCount, FrameRate frameRate, std::uint32_t sampleRate) noexcept
{
    if (frameRate.numerator == 0 || frameRate.denominator == 0 || sampleRate == 0) {
        return std::nullopt;
    }
    const std::uint64_t ticks = static_cast<std::uint64_t>(frameCount) * frameRate.denominator;
    const std::uint64_t whole = ticks / frameRate.numerator;
    const std::uint64_t remainder = ticks % frameRate.numerator;
    const std::uint64_t fractionalSamples = remainder * sampleRate;
    const std::uint64_t fraction = fractionalSamples / frameRate.numerator
        + (fractionalSamples % frameRate.numerator != 0 ? 1 : 0);
    if (whole > (std::numeric_limits<std::uint64_t>::max() - fraction) / sampleRate) {
        return std::nullopt;
    }
    return whole * sampleRate + fraction;
}

AudioAsset *findAudioAsset(Document &document, const std::string &id) noexcept
{
    const auto found = std::find_if(document.audioAssets.begin(), document.audioAssets.end(),
        [&id](const AudioAsset &asset) { return asset.id == id; });
    return found == document.audioAssets.end() ? nullptr : &*found;
}

const AudioAsset *findAudioAsset(const Document &document, const std::string &id) noexcept
{
    const auto found = std::find_if(document.audioAssets.begin(), document.audioAssets.end(),
        [&id](const AudioAsset &asset) { return asset.id == id; });
    return found == document.audioAssets.end() ? nullptr : &*found;
}

AudioTrackLayer *findAudioTrack(Document &document, const std::string &id) noexcept
{
    const auto found = std::find_if(document.audioTracks.begin(), document.audioTracks.end(),
        [&id](const AudioTrackLayer &track) { return track.id == id; });
    return found == document.audioTracks.end() ? nullptr : &*found;
}

const AudioTrackLayer *findAudioTrack(const Document &document, const std::string &id) noexcept
{
    const auto found = std::find_if(document.audioTracks.begin(), document.audioTracks.end(),
        [&id](const AudioTrackLayer &track) { return track.id == id; });
    return found == document.audioTracks.end() ? nullptr : &*found;
}

AudioClip *findAudioClip(AudioTrackLayer &track, const std::string &id) noexcept
{
    const auto found = std::find_if(track.clips.begin(), track.clips.end(),
        [&id](const AudioClip &clip) { return clip.id == id; });
    return found == track.clips.end() ? nullptr : &*found;
}

const AudioClip *findAudioClip(const AudioTrackLayer &track, const std::string &id) noexcept
{
    const auto found = std::find_if(track.clips.begin(), track.clips.end(),
        [&id](const AudioClip &clip) { return clip.id == id; });
    return found == track.clips.end() ? nullptr : &*found;
}

ContentKind contentKind(const Asset &asset) noexcept
{
    if (std::holds_alternative<MlsdAsset>(asset)) { return ContentKind::Mlsd; }
    if (std::holds_alternative<CannyAsset>(asset)) { return ContentKind::Canny; }
    if (std::holds_alternative<ScribbleAsset>(asset)) { return ContentKind::Scribble; }
    if (std::holds_alternative<LineArtAsset>(asset)) { return ContentKind::LineArt; }
    if (std::holds_alternative<NormalMapAsset>(asset)) { return ContentKind::NormalMap; }
    if (std::holds_alternative<ShuffleAsset>(asset)) { return ContentKind::Shuffle; }
    if (std::holds_alternative<TileAsset>(asset)) { return ContentKind::Tile; }
    if (std::holds_alternative<ReferenceAsset>(asset)) { return ContentKind::Reference; }
    if (std::holds_alternative<IpAdapterAsset>(asset)) { return ContentKind::IpAdapter; }
    if (std::holds_alternative<DepthAsset>(asset)) { return ContentKind::Depth; }
    if (std::holds_alternative<PoseAsset>(asset)) { return ContentKind::Pose; }
    if (std::holds_alternative<VideoAsset>(asset)) { return ContentKind::Video; }
    return std::holds_alternative<VectorAsset>(asset)
        ? ContentKind::Vector
        : ContentKind::Raster;
}

ContentKind contentKind(const Layer &layer) noexcept
{
    if (std::holds_alternative<MlsdLayer>(layer)) { return ContentKind::Mlsd; }
    if (std::holds_alternative<CannyLayer>(layer)) { return ContentKind::Canny; }
    if (std::holds_alternative<ScribbleLayer>(layer)) { return ContentKind::Scribble; }
    if (std::holds_alternative<LineArtLayer>(layer)) { return ContentKind::LineArt; }
    if (std::holds_alternative<NormalMapLayer>(layer)) { return ContentKind::NormalMap; }
    if (std::holds_alternative<ShuffleLayer>(layer)) { return ContentKind::Shuffle; }
    if (std::holds_alternative<TileLayer>(layer)) { return ContentKind::Tile; }
    if (std::holds_alternative<ReferenceLayer>(layer)) { return ContentKind::Reference; }
    if (std::holds_alternative<IpAdapterLayer>(layer)) { return ContentKind::IpAdapter; }
    if (std::holds_alternative<DepthLayer>(layer)) { return ContentKind::Depth; }
    if (std::holds_alternative<PoseLayer>(layer)) { return ContentKind::Pose; }
    if (std::holds_alternative<VideoLayer>(layer)) { return ContentKind::Video; }
    return (std::holds_alternative<StaticVectorLayer>(layer) || std::holds_alternative<DynamicVectorLayer>(layer))
        ? ContentKind::Vector
        : ContentKind::Raster;
}

bool isLineControlNetLayer(const Layer &layer) noexcept
{
    return std::holds_alternative<LineArtLayer>(layer) || std::holds_alternative<CannyLayer>(layer)
        || std::holds_alternative<ScribbleLayer>(layer) || std::holds_alternative<MlsdLayer>(layer);
}

LayerRole layerRole(const Layer &layer) noexcept
{
    return (std::holds_alternative<SemanticSegmentLayer>(layer) || std::holds_alternative<PoseLayer>(layer) || std::holds_alternative<NormalMapLayer>(layer) || std::holds_alternative<ShuffleLayer>(layer) || std::holds_alternative<TileLayer>(layer) || std::holds_alternative<ReferenceLayer>(layer) || std::holds_alternative<IpAdapterLayer>(layer) || std::holds_alternative<DepthLayer>(layer) || std::holds_alternative<LineArtLayer>(layer) || std::holds_alternative<CannyLayer>(layer) || std::holds_alternative<ScribbleLayer>(layer) || std::holds_alternative<MlsdLayer>(layer)) ? LayerRole::ControlNet : LayerRole::Artwork;
}

std::optional<ControlNetKind> controlNetKind(const Layer &layer) noexcept
{
    if (std::holds_alternative<MlsdLayer>(layer)) { return ControlNetKind::Mlsd; }
    if (std::holds_alternative<CannyLayer>(layer)) { return ControlNetKind::Canny; }
    if (std::holds_alternative<ScribbleLayer>(layer)) { return ControlNetKind::Scribble; }
    if (std::holds_alternative<LineArtLayer>(layer)) { return ControlNetKind::LineArt; }
    if (std::holds_alternative<NormalMapLayer>(layer)) { return ControlNetKind::NormalMap; }
    if (std::holds_alternative<ShuffleLayer>(layer)) { return ControlNetKind::Shuffle; }
    if (std::holds_alternative<TileLayer>(layer)) { return ControlNetKind::Tile; }
    if (std::holds_alternative<ReferenceLayer>(layer)) { return ControlNetKind::Reference; }
    if (std::holds_alternative<IpAdapterLayer>(layer)) { return ControlNetKind::IpAdapter; }
    if (std::holds_alternative<DepthLayer>(layer)) { return ControlNetKind::Depth; }
    if (std::holds_alternative<PoseLayer>(layer)) { return ControlNetKind::Pose; }
    return std::holds_alternative<SemanticSegmentLayer>(layer)
        ? std::optional{ControlNetKind::SemanticSegment} : std::nullopt;
}

LayerTiming layerTiming(const Layer &layer) noexcept
{
    return std::holds_alternative<VideoLayer>(layer)
        || keyframedLayerSource(layer) != nullptr
        ? LayerTiming::Dynamic : LayerTiming::Static;
}

LayerRepresentation layerRepresentation(const Layer &layer) noexcept
{
    if (std::holds_alternative<IpAdapterLayer>(layer)) return LayerRepresentation::Embedding;
    return ((std::holds_alternative<StaticVectorLayer>(layer) || std::holds_alternative<DynamicVectorLayer>(layer)) || std::holds_alternative<PoseLayer>(layer) || std::holds_alternative<MlsdLayer>(layer))
        ? LayerRepresentation::Vector : LayerRepresentation::Bitmap;
}

std::optional<LayerKind> layerKind(const Layer &layer) noexcept
{
    if (layerRepresentation(layer)==LayerRepresentation::Embedding)
        return std::nullopt;
    const bool vector = layerRepresentation(layer) == LayerRepresentation::Vector;
    return layerTiming(layer) == LayerTiming::Static
        ? (vector ? LayerKind::StaticVector : LayerKind::StaticBitmap)
        : (vector ? LayerKind::DynamicVector : LayerKind::DynamicBitmap);
}

VideoAsset *findVideoAsset(Document &document, const std::string &id) noexcept
{
    Asset *asset = findAsset(document, id);
    return asset ? std::get_if<VideoAsset>(asset) : nullptr;
}

const VideoAsset *findVideoAsset(const Document &document, const std::string &id) noexcept
{
    const Asset *asset = findAsset(document, id);
    return asset ? std::get_if<VideoAsset>(asset) : nullptr;
}

VideoLayer *findVideoLayer(Document &document, const std::string &id) noexcept
{
    Layer *layer = findLayer(document, id);
    return layer ? std::get_if<VideoLayer>(layer) : nullptr;
}

const VideoLayer *findVideoLayer(const Document &document, const std::string &id) noexcept
{
    const Layer *layer = findLayer(document, id);
    return layer ? std::get_if<VideoLayer>(layer) : nullptr;
}

const std::string &assetId(const Asset &asset) noexcept
{
    return std::visit([](const auto &value) -> const std::string & { return value.id; }, asset);
}

LayerProperties &layerProperties(Layer &layer) noexcept
{
    return std::visit([](auto &value) -> LayerProperties & {
        return value.properties;
    }, layer);
}

const LayerProperties &layerProperties(const Layer &layer) noexcept
{
    return std::visit([](const auto &value) -> const LayerProperties & {
        return value.properties;
    }, layer);
}

StaticSource *staticLayerSource(Layer &layer) noexcept
{
    return std::visit([](auto &value) -> StaticSource * {
        if constexpr (requires { value.content; }) {
            if constexpr (std::is_base_of_v<StaticSource, std::decay_t<decltype(value.content)>>)
                return &value.content;
            else return nullptr;
        } else return std::get_if<StaticSource>(&value.source);
    }, layer);
}

const StaticSource *staticLayerSource(const Layer &layer) noexcept
{
    return std::visit([](const auto &value) -> const StaticSource * {
        if constexpr (requires { value.content; }) {
            if constexpr (std::is_base_of_v<StaticSource, std::decay_t<decltype(value.content)>>)
                return &value.content;
            else return nullptr;
        } else return std::get_if<StaticSource>(&value.source);
    }, layer);
}

KeyframedSource *keyframedLayerSource(Layer &layer) noexcept
{
    return std::visit([](auto &value) -> KeyframedSource * {
        if constexpr (requires { value.content; }) {
            if constexpr (std::is_base_of_v<KeyframedSource, std::decay_t<decltype(value.content)>>)
                return &value.content;
            else return nullptr;
        } else return std::get_if<KeyframedSource>(&value.source);
    }, layer);
}

const KeyframedSource *keyframedLayerSource(const Layer &layer) noexcept
{
    return std::visit([](const auto &value) -> const KeyframedSource * {
        if constexpr (requires { value.content; }) {
            if constexpr (std::is_base_of_v<KeyframedSource, std::decay_t<decltype(value.content)>>)
                return &value.content;
            else return nullptr;
        } else return std::get_if<KeyframedSource>(&value.source);
    }, layer);
}

LayerSource layerSource(const Layer &layer)
{
    if (const auto *source = staticLayerSource(layer)) return *source;
    return *keyframedLayerSource(layer);
}

Layer makeBitmapLayer(LayerProperties properties, LayerSource source)
{
    if (auto *value = std::get_if<StaticSource>(&source))
        return StaticBitmapLayer{std::move(properties), StaticBitmapContent{std::move(*value)}};
    return DynamicBitmapLayer{std::move(properties), DynamicBitmapContent{std::move(std::get<KeyframedSource>(source))}};
}

Layer makeVectorLayer(LayerProperties properties, LayerSource source)
{
    if (auto *value = std::get_if<StaticSource>(&source))
        return StaticVectorLayer{std::move(properties), StaticVectorContent{std::move(*value)}};
    return DynamicVectorLayer{std::move(properties), DynamicVectorContent{std::move(std::get<KeyframedSource>(source))}};
}

void setLayerSource(Layer &layer, LayerSource source)
{
    if (std::holds_alternative<StaticBitmapLayer>(layer) || std::holds_alternative<DynamicBitmapLayer>(layer)) {
        layer = makeBitmapLayer(layerProperties(layer), std::move(source));
    } else if (std::holds_alternative<StaticVectorLayer>(layer) || std::holds_alternative<DynamicVectorLayer>(layer)) {
        layer = makeVectorLayer(layerProperties(layer), std::move(source));
    } else {
        std::visit([&](auto &value) {
            if constexpr (requires { value.source; }) value.source = std::move(source);
        }, layer);
    }
}

bool layerExistsAt(const Document &document,
                   const Layer &layer,
                   FrameIndex frame) noexcept
{
    const std::optional<LayerFrameRange> &range = layerProperties(layer).frameRange;
    return frame < document.timeline.frameCount
        && (!range || (frame >= range->firstFrame && frame <= range->lastFrame));
}

CanvasOrigin canvasOrigin(const Document &document) noexcept
{
    return document.canvasMode == CanvasMode::Infinite
        ? document.infiniteCanvas.origin
        : CanvasOrigin{};
}

CanvasRegion canvasRegion(const Document &document) noexcept
{
    return {canvasOrigin(document), document.extent};
}

CanvasRegion documentViewRegion(const Document &document) noexcept
{
    const auto base = canvasRegion(document);
    std::int64_t left = base.origin.x, top = base.origin.y;
    std::int64_t right = left + base.extent.width, bottom = top + base.extent.height;
    for (const auto &artboard : document.artboards) {
        left = std::min(left, std::int64_t(artboard.region.origin.x));
        top = std::min(top, std::int64_t(artboard.region.origin.y));
        right = std::max(right, std::int64_t(artboard.region.origin.x) + artboard.region.extent.width);
        bottom = std::max(bottom, std::int64_t(artboard.region.origin.y) + artboard.region.extent.height);
    }
    if (right > std::numeric_limits<std::int32_t>::max()
        || bottom > std::numeric_limits<std::int32_t>::max()
        || right - left > std::numeric_limits<std::int32_t>::max()
        || bottom - top > std::numeric_limits<std::int32_t>::max()
        || right <= left || bottom <= top) return {};
    return {{std::int32_t(left), std::int32_t(top)},
            {std::int32_t(right - left), std::int32_t(bottom - top)}};
}

Artboard *findArtboard(Document &document, const std::string &id) noexcept
{
    const auto found = std::find_if(document.artboards.begin(), document.artboards.end(),
                                  [&](const Artboard &value) { return value.id == id; });
    return found == document.artboards.end() ? nullptr : &*found;
}

const Artboard *findArtboard(const Document &document, const std::string &id) noexcept
{
    const auto found = std::find_if(document.artboards.begin(), document.artboards.end(),
                                  [&](const Artboard &value) { return value.id == id; });
    return found == document.artboards.end() ? nullptr : &*found;
}

Asset *findAsset(Document &document, const std::string &id) noexcept
{
    const auto match = std::find_if(document.assets.begin(), document.assets.end(),
                                    [&id](const Asset &asset) {
                                        return assetId(asset) == id;
                                    });
    return match == document.assets.end() ? nullptr : &*match;
}

const Asset *findAsset(const Document &document, const std::string &id) noexcept
{
    const auto match = std::find_if(document.assets.begin(), document.assets.end(),
                                    [&id](const Asset &asset) {
                                        return assetId(asset) == id;
                                    });
    return match == document.assets.end() ? nullptr : &*match;
}

RasterAsset *findRasterAsset(Document &document, const std::string &id) noexcept
{
    Asset *asset = findAsset(document, id);
    return asset ? std::get_if<RasterAsset>(asset) : nullptr;
}

const RasterAsset *findRasterAsset(const Document &document,
                                   const std::string &id) noexcept
{
    const Asset *asset = findAsset(document, id);
    return asset ? std::get_if<RasterAsset>(asset) : nullptr;
}

ChunkedRasterAsset *findChunkedRasterAsset(Document &document,
                                            const std::string &id) noexcept
{
    Asset *asset = findAsset(document, id);
    return asset ? std::get_if<ChunkedRasterAsset>(asset) : nullptr;
}

const ChunkedRasterAsset *findChunkedRasterAsset(const Document &document,
                                                  const std::string &id) noexcept
{
    const Asset *asset = findAsset(document, id);
    return asset ? std::get_if<ChunkedRasterAsset>(asset) : nullptr;
}

RasterChunk *findRasterChunk(ChunkedRasterAsset &asset,
                             std::int32_t column,
                             std::int32_t row) noexcept
{
    const auto match = std::find_if(asset.chunks.begin(), asset.chunks.end(),
                                    [column, row](const RasterChunk &chunk) {
                                        return chunk.column == column && chunk.row == row;
                                    });
    return match == asset.chunks.end() ? nullptr : &*match;
}

const RasterChunk *findRasterChunk(const ChunkedRasterAsset &asset,
                                   std::int32_t column,
                                   std::int32_t row) noexcept
{
    const auto match = std::find_if(asset.chunks.begin(), asset.chunks.end(),
                                    [column, row](const RasterChunk &chunk) {
                                        return chunk.column == column && chunk.row == row;
                                    });
    return match == asset.chunks.end() ? nullptr : &*match;
}

VectorAsset *findVectorAsset(Document &document, const std::string &id) noexcept
{
    Asset *asset = findAsset(document, id);
    return asset ? std::get_if<VectorAsset>(asset) : nullptr;
}

const VectorAsset *findVectorAsset(const Document &document,
                                   const std::string &id) noexcept
{
    const Asset *asset = findAsset(document, id);
    return asset ? std::get_if<VectorAsset>(asset) : nullptr;
}

std::optional<std::size_t> assetIndex(const Document &document,
                                      const std::string &id) noexcept
{
    const auto match = std::find_if(document.assets.begin(), document.assets.end(),
                                    [&id](const Asset &asset) {
                                        return assetId(asset) == id;
                                    });
    if (match == document.assets.end()) {
        return std::nullopt;
    }
    return static_cast<std::size_t>(std::distance(document.assets.begin(), match));
}

Layer *findLayer(Document &document, const std::string &id) noexcept
{
    const auto match = std::find_if(document.layers.begin(), document.layers.end(),
                                    [&id](const Layer &layer) {
                                        return layerProperties(layer).id == id;
                                    });
    return match == document.layers.end() ? nullptr : &*match;
}

const Layer *findLayer(const Document &document, const std::string &id) noexcept
{
    const auto match = std::find_if(document.layers.begin(), document.layers.end(),
                                    [&id](const Layer &layer) {
                                        return layerProperties(layer).id == id;
                                    });
    return match == document.layers.end() ? nullptr : &*match;
}

StaticBitmapLayer *findStaticBitmapLayer(Document &document, const std::string &id) noexcept
{
    Layer *layer = findLayer(document, id);
    return layer ? std::get_if<StaticBitmapLayer>(layer) : nullptr;
}

const StaticBitmapLayer *findStaticBitmapLayer(const Document &document, const std::string &id) noexcept
{
    const Layer *layer = findLayer(document, id);
    return layer ? std::get_if<StaticBitmapLayer>(layer) : nullptr;
}

StaticVectorLayer *findStaticVectorLayer(Document &document, const std::string &id) noexcept
{
    Layer *layer = findLayer(document, id);
    return layer ? std::get_if<StaticVectorLayer>(layer) : nullptr;
}

const StaticVectorLayer *findStaticVectorLayer(const Document &document, const std::string &id) noexcept
{
    const Layer *layer = findLayer(document, id);
    return layer ? std::get_if<StaticVectorLayer>(layer) : nullptr;
}

DynamicBitmapLayer *findDynamicBitmapLayer(Document &document, const std::string &id) noexcept
{
    Layer *layer = findLayer(document, id);
    return layer ? std::get_if<DynamicBitmapLayer>(layer) : nullptr;
}

const DynamicBitmapLayer *findDynamicBitmapLayer(const Document &document, const std::string &id) noexcept
{
    const Layer *layer = findLayer(document, id);
    return layer ? std::get_if<DynamicBitmapLayer>(layer) : nullptr;
}

DynamicVectorLayer *findDynamicVectorLayer(Document &document, const std::string &id) noexcept
{
    Layer *layer = findLayer(document, id);
    return layer ? std::get_if<DynamicVectorLayer>(layer) : nullptr;
}

const DynamicVectorLayer *findDynamicVectorLayer(const Document &document, const std::string &id) noexcept
{
    const Layer *layer = findLayer(document, id);
    return layer ? std::get_if<DynamicVectorLayer>(layer) : nullptr;
}

std::optional<std::size_t> layerIndex(const Document &document,
                                      const std::string &id) noexcept
{
    const auto match = std::find_if(document.layers.begin(), document.layers.end(),
                                    [&id](const Layer &layer) {
                                        return layerProperties(layer).id == id;
                                    });
    if (match == document.layers.end()) {
        return std::nullopt;
    }
    return static_cast<std::size_t>(std::distance(document.layers.begin(), match));
}

Frame *findFrame(Document &document, FrameIndex frame) noexcept
{
    const auto match = std::lower_bound(
        document.frames.begin(), document.frames.end(), frame,
        [](const Frame &value, FrameIndex requestedFrame) {
            return value.index < requestedFrame;
        });
    return match != document.frames.end() && match->index == frame ? &*match : nullptr;
}

const Frame *findFrame(const Document &document, FrameIndex frame) noexcept
{
    const auto match = std::lower_bound(
        document.frames.begin(), document.frames.end(), frame,
        [](const Frame &value, FrameIndex requestedFrame) {
            return value.index < requestedFrame;
        });
    return match != document.frames.end() && match->index == frame ? &*match : nullptr;
}

std::optional<std::size_t> frameIndex(const Document &document,
                                      FrameIndex frame) noexcept
{
    const auto match = std::lower_bound(
        document.frames.begin(), document.frames.end(), frame,
        [](const Frame &value, FrameIndex requestedFrame) {
            return value.index < requestedFrame;
        });
    if (match == document.frames.end() || match->index != frame) {
        return std::nullopt;
    }
    return static_cast<std::size_t>(std::distance(document.frames.begin(), match));
}

Keyframe *findKeyframe(Frame &frame, const std::string &layerId) noexcept
{
    const auto match = std::lower_bound(
        frame.keyframes.begin(), frame.keyframes.end(), layerId,
        [](const Keyframe &keyframe, const std::string &requestedLayerId) {
            return keyframe.layerId < requestedLayerId;
        });
    return match != frame.keyframes.end() && match->layerId == layerId
        ? &*match
        : nullptr;
}

const Keyframe *findKeyframe(const Frame &frame,
                             const std::string &layerId) noexcept
{
    const auto match = std::lower_bound(
        frame.keyframes.begin(), frame.keyframes.end(), layerId,
        [](const Keyframe &keyframe, const std::string &requestedLayerId) {
            return keyframe.layerId < requestedLayerId;
        });
    return match != frame.keyframes.end() && match->layerId == layerId
        ? &*match
        : nullptr;
}

Keyframe *findKeyframe(Document &document,
                       const std::string &layerId,
                       FrameIndex frame) noexcept
{
    Frame *owner = findFrame(document, frame);
    return owner ? findKeyframe(*owner, layerId) : nullptr;
}

const Keyframe *findKeyframe(const Document &document,
                             const std::string &layerId,
                             FrameIndex frame) noexcept
{
    const Frame *owner = findFrame(document, frame);
    return owner ? findKeyframe(*owner, layerId) : nullptr;
}

std::optional<std::size_t> keyframeIndex(const Frame &frame,
                                         const std::string &layerId) noexcept
{
    const auto match = std::lower_bound(
        frame.keyframes.begin(), frame.keyframes.end(), layerId,
        [](const Keyframe &keyframe, const std::string &requestedLayerId) {
            return keyframe.layerId < requestedLayerId;
        });
    if (match == frame.keyframes.end() || match->layerId != layerId) {
        return std::nullopt;
    }
    return static_cast<std::size_t>(std::distance(frame.keyframes.begin(), match));
}

std::vector<AssetReference> assetReferences(const Document &document,
                                            const std::string &referencedAssetId)
{
    std::vector<AssetReference> references;
    std::unordered_map<std::string, std::size_t> layerIndices;
    layerIndices.reserve(document.layers.size());
    for (std::size_t layerPosition = 0;
         layerPosition < document.layers.size();
         ++layerPosition) {
        layerIndices.emplace(
            layerProperties(document.layers[layerPosition]).id,
            layerPosition);
        if (const auto *staticSource = staticLayerSource(document.layers[layerPosition])) {
            if (staticSource->assetId == referencedAssetId) {
                references.push_back({layerPosition, std::nullopt, std::nullopt});
            }
        }
    }
    for (std::size_t framePosition = 0;
         framePosition < document.frames.size();
         ++framePosition) {
        const Frame &frame = document.frames[framePosition];
        for (std::size_t keyframePosition = 0;
             keyframePosition < frame.keyframes.size();
             ++keyframePosition) {
            const Keyframe &keyframe = frame.keyframes[keyframePosition];
            if (keyframe.assetId == referencedAssetId) {
                const auto owner = layerIndices.find(keyframe.layerId);
                if (owner != layerIndices.end()) {
                    references.push_back(
                        {owner->second, framePosition, keyframePosition});
                }
            }
        }
    }
    return references;
}

const Asset *resolveAssetAt(const Document &document,
                            const Layer &layer,
                            FrameIndex frame) noexcept
{
    if (frame >= document.timeline.frameCount) {
        return nullptr;
    }
    if (!layerExistsAt(document, layer, frame)) {
        return nullptr;
    }

    const ContentKind requiredKind = contentKind(layer);
    if (const auto *source = staticLayerSource(layer)) {
        const Asset *asset = findAsset(document, source->assetId);
        return asset && contentKind(*asset) == requiredKind ? asset : nullptr;
    }

    const auto *source = keyframedLayerSource(layer);
    if (!source) {
        return nullptr;
    }
    const auto next = std::upper_bound(source->frameIndices.begin(),
                                       source->frameIndices.end(),
                                       frame);
    if (next == source->frameIndices.begin()) {
        return nullptr;
    }
    const FrameIndex ownerFrame = *std::prev(next);
    const Frame *owner = findFrame(document, ownerFrame);
    if (!owner) {
        return nullptr;
    }
    const Keyframe *keyframe = findKeyframe(
        *owner, layerProperties(layer).id);
    if (!keyframe) {
        return nullptr;
    }
    const Asset *asset = findAsset(document, keyframe->assetId);
    return asset && contentKind(*asset) == requiredKind ? asset : nullptr;
}

} // namespace iiSharedCanvas
