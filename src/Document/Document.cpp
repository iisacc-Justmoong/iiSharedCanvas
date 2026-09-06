#include "Document/Document.h"

#include <algorithm>
#include <iterator>
#include <limits>
#include <unordered_map>

namespace iiSharedCanvas {

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
    return std::holds_alternative<VectorAsset>(asset)
        ? ContentKind::Vector
        : ContentKind::Raster;
}

ContentKind contentKind(const Layer &layer) noexcept
{
    return std::holds_alternative<VectorLayer>(layer)
        ? ContentKind::Vector
        : ContentKind::Raster;
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

LayerSource &layerSource(Layer &layer) noexcept
{
    return std::visit([](auto &value) -> LayerSource & {
        return value.source;
    }, layer);
}

const LayerSource &layerSource(const Layer &layer) noexcept
{
    return std::visit([](const auto &value) -> const LayerSource & {
        return value.source;
    }, layer);
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

BitmapLayer *findBitmapLayer(Document &document, const std::string &id) noexcept
{
    Layer *layer = findLayer(document, id);
    return layer ? std::get_if<BitmapLayer>(layer) : nullptr;
}

const BitmapLayer *findBitmapLayer(const Document &document,
                                   const std::string &id) noexcept
{
    const Layer *layer = findLayer(document, id);
    return layer ? std::get_if<BitmapLayer>(layer) : nullptr;
}

VectorLayer *findVectorLayer(Document &document, const std::string &id) noexcept
{
    Layer *layer = findLayer(document, id);
    return layer ? std::get_if<VectorLayer>(layer) : nullptr;
}

const VectorLayer *findVectorLayer(const Document &document,
                                   const std::string &id) noexcept
{
    const Layer *layer = findLayer(document, id);
    return layer ? std::get_if<VectorLayer>(layer) : nullptr;
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
        const LayerSource &source = layerSource(document.layers[layerPosition]);
        if (const auto *staticSource = std::get_if<StaticSource>(&source)) {
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
    const LayerSource &sourceValue = layerSource(layer);
    if (const auto *source = std::get_if<StaticSource>(&sourceValue)) {
        const Asset *asset = findAsset(document, source->assetId);
        return asset && contentKind(*asset) == requiredKind ? asset : nullptr;
    }

    const auto *source = std::get_if<KeyframedSource>(&sourceValue);
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
