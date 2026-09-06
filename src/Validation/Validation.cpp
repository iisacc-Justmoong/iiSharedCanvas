#include "Validation/Validation.h"

#include <cmath>
#include <cstddef>
#include <cstdint>
#include <limits>
#include <string>
#include <type_traits>
#include <unordered_map>
#include <unordered_set>
#include <utility>

namespace iiSharedCanvas {

namespace {

void addIssue(ValidationResult &result,
              ValidationCode code,
              std::string path,
              std::string message)
{
    result.issues.push_back({code, std::move(path), std::move(message)});
}

bool isFinite(Point point) noexcept
{
    return std::isfinite(point.x) && std::isfinite(point.y);
}

bool isFinite(const PathCommand &command) noexcept
{
    return std::visit([](const auto &value) {
        using Command = std::decay_t<decltype(value)>;
        if constexpr (std::is_same_v<Command, MoveTo>
                      || std::is_same_v<Command, LineTo>) {
            return isFinite(value.point);
        } else if constexpr (std::is_same_v<Command, QuadraticTo>) {
            return isFinite(value.control) && isFinite(value.end);
        } else if constexpr (std::is_same_v<Command, CubicTo>) {
            return isFinite(value.control1)
                && isFinite(value.control2)
                && isFinite(value.end);
        } else {
            return true;
        }
    }, command);
}

bool hasFiniteTransform(const AffineTransform &transform) noexcept
{
    return std::isfinite(transform.m11)
        && std::isfinite(transform.m12)
        && std::isfinite(transform.m21)
        && std::isfinite(transform.m22)
        && std::isfinite(transform.translationX)
        && std::isfinite(transform.translationY);
}

bool isSupportedLayerBlendMode(RasterBlendMode blendMode) noexcept
{
    switch (blendMode) {
    case RasterBlendMode::SourceOver:
    case RasterBlendMode::Multiply:
    case RasterBlendMode::Screen:
    case RasterBlendMode::Overlay:
        return true;
    case RasterBlendMode::DestinationOut:
        return false;
    }
    return false;
}

void validateRasterAsset(const RasterAsset &asset,
                         std::size_t index,
                         ValidationResult &result)
{
    const std::string path = "assets[" + std::to_string(index) + "]";
    const bool hasPositiveExtent = asset.pixels.width > 0 && asset.pixels.height > 0;
    const std::uint64_t expectedSize = hasPositiveExtent
        ? static_cast<std::uint64_t>(asset.pixels.width)
            * static_cast<std::uint64_t>(asset.pixels.height)
        : 0;
    if (!hasPositiveExtent
        || expectedSize != static_cast<std::uint64_t>(asset.pixels.pixels.size())) {
        addIssue(result, ValidationCode::InvalidRasterAsset, path,
                 "raster extent must be positive and match its ARGB pixel count");
    }
}

bool validChunkSize(std::int32_t chunkSize) noexcept
{
    return chunkSize >= 32 && chunkSize <= 4096
        && (chunkSize & (chunkSize - 1)) == 0;
}

void validateChunkedRasterAsset(const Document &document,
                                const ChunkedRasterAsset &asset,
                                std::size_t index,
                                ValidationResult &result)
{
    const std::string path = "assets[" + std::to_string(index) + "]";
    if (document.canvasMode != CanvasMode::Infinite) {
        addIssue(result, ValidationCode::InvalidRasterChunk, path,
                 "chunked raster assets require an infinite canvas");
        return;
    }

    std::unordered_set<std::uint64_t> coordinates;
    const std::int32_t chunkSize = document.infiniteCanvas.chunkSize;
    for (std::size_t chunkIndex = 0; chunkIndex < asset.chunks.size(); ++chunkIndex) {
        const RasterChunk &chunk = asset.chunks[chunkIndex];
        const std::string chunkPath = path + ".chunks[" + std::to_string(chunkIndex) + "]";
        const std::uint64_t key = (static_cast<std::uint64_t>(
                                       static_cast<std::uint32_t>(chunk.row)) << 32U)
            | static_cast<std::uint32_t>(chunk.column);
        const std::int64_t pixelX = static_cast<std::int64_t>(chunk.column) * chunkSize;
        const std::int64_t pixelY = static_cast<std::int64_t>(chunk.row) * chunkSize;
        const std::uint64_t expectedPixels = static_cast<std::uint64_t>(chunkSize)
            * static_cast<std::uint64_t>(chunkSize);
        if (!coordinates.insert(key).second
            || pixelX < std::numeric_limits<std::int32_t>::min()
            || pixelX > std::numeric_limits<std::int32_t>::max()
            || pixelY < std::numeric_limits<std::int32_t>::min()
            || pixelY > std::numeric_limits<std::int32_t>::max()
            || chunk.pixels.width != chunkSize
            || chunk.pixels.height != chunkSize
            || chunk.pixels.pixels.size() != expectedPixels) {
            addIssue(result, ValidationCode::InvalidRasterChunk, chunkPath,
                     "chunk coordinates must be unique and each chunk must match the canvas chunk size");
        }
        if (chunkIndex > 0) {
            const RasterChunk &prior = asset.chunks[chunkIndex - 1];
            if (chunk.row < prior.row
                || (chunk.row == prior.row && chunk.column <= prior.column)) {
                addIssue(result, ValidationCode::InvalidRasterChunk, chunkPath,
                         "raster chunks must use canonical row-major coordinate order");
            }
        }
    }
}

void validateVectorAsset(const VectorAsset &asset,
                         std::size_t index,
                         ValidationResult &result)
{
    const std::string assetPath = "assets[" + std::to_string(index) + "]";
    if (asset.viewport.width <= 0 || asset.viewport.height <= 0) {
        addIssue(result, ValidationCode::InvalidVectorAsset, assetPath + ".viewport",
                 "vector viewport must have a positive extent");
    }

    for (std::size_t pathIndex = 0; pathIndex < asset.paths.size(); ++pathIndex) {
        const VectorPath &path = asset.paths[pathIndex];
        const std::string pathLocation = assetPath + ".paths["
            + std::to_string(pathIndex) + "]";
        if (path.commands.empty()
            || !std::holds_alternative<MoveTo>(path.commands.front())) {
            addIssue(result, ValidationCode::InvalidVectorAsset, pathLocation,
                     "a vector path must begin with MoveTo");
        }
        if (!path.fill && !path.stroke) {
            addIssue(result, ValidationCode::InvalidVectorAsset, pathLocation,
                     "a vector path must have a fill or stroke");
        }
        if (path.stroke
            && (!std::isfinite(path.stroke->width) || path.stroke->width <= 0.0)) {
            addIssue(result, ValidationCode::InvalidVectorAsset,
                     pathLocation + ".stroke.width",
                     "stroke width must be finite and positive");
        }
        for (std::size_t commandIndex = 0;
             commandIndex < path.commands.size();
             ++commandIndex) {
            if (!isFinite(path.commands[commandIndex])) {
                addIssue(result, ValidationCode::InvalidVectorAsset,
                         pathLocation + ".commands[" + std::to_string(commandIndex) + "]",
                         "vector coordinates must be finite");
            }
        }
    }
}

using AssetLookup = std::unordered_map<std::string, const Asset *>;

void validateAssetReference(const AssetLookup &assets,
                            const std::string &id,
                            const std::string &path,
                            ValidationResult &result,
                            const ContentKind *requiredKind = nullptr)
{
    const auto asset = assets.find(id);
    if (asset == assets.end()) {
        addIssue(result, ValidationCode::MissingAsset, path,
                 "source or keyframe references an unknown asset id");
        return;
    }
    if (requiredKind && contentKind(*asset->second) != *requiredKind) {
        addIssue(result, ValidationCode::ContentKindMismatch, path,
                 "the referenced asset kind must match the owning layer type");
    }
}

} // namespace

ValidationResult validate(const Document &document)
{
    ValidationResult result;

    if (document.formatVersion.major != CurrentFormatMajor
        || document.formatVersion.minor > CurrentFormatMinor) {
        addIssue(result, ValidationCode::UnsupportedFormatVersion, "formatVersion",
                 "the document version is not supported by this reader contract");
    }
    if (document.extent.width <= 0 || document.extent.height <= 0) {
        addIssue(result, ValidationCode::InvalidCanvasExtent, "extent",
                 "canvas extent must be positive");
    }
    const std::int64_t canvasRight = static_cast<std::int64_t>(canvasOrigin(document).x)
        + document.extent.width;
    const std::int64_t canvasBottom = static_cast<std::int64_t>(canvasOrigin(document).y)
        + document.extent.height;
    if (document.canvasMode == CanvasMode::Infinite) {
        if (document.formatVersion.minor < 1
            || !validChunkSize(document.infiniteCanvas.chunkSize)
            || canvasRight > std::numeric_limits<std::int32_t>::max()
            || canvasBottom > std::numeric_limits<std::int32_t>::max()) {
            addIssue(result, ValidationCode::InvalidInfiniteCanvas, "infiniteCanvas",
                     "infinite canvas geometry requires format 1.1, a power-of-two 32-4096 px chunk, and bounded coordinates");
        }
    } else if (document.infiniteCanvas.origin.x != 0
               || document.infiniteCanvas.origin.y != 0) {
        addIssue(result, ValidationCode::InvalidInfiniteCanvas, "infiniteCanvas.origin",
                 "a finite canvas must use the zero document origin");
    }
    if (document.timeline.frameRate.numerator == 0
        || document.timeline.frameRate.denominator == 0
        || document.timeline.frameCount == 0) {
        addIssue(result, ValidationCode::InvalidTimeline, "timeline",
                 "frame rate and frame count must be non-zero");
    }
    if (document.stableDiffusionMetadata) {
        if (document.formatVersion.minor < 2) {
            addIssue(result,
                     ValidationCode::InvalidStableDiffusionMetadata,
                     "stableDiffusionMetadata",
                     "Stable Diffusion metadata requires document format 1.2 or newer");
        }
        const StableDiffusionValidationResult metadataValidation =
            validateStableDiffusionMetadata(*document.stableDiffusionMetadata);
        for (const StableDiffusionValidationIssue &issue : metadataValidation.issues) {
            addIssue(result,
                     ValidationCode::InvalidStableDiffusionMetadata,
                     issue.path.empty()
                         ? "stableDiffusionMetadata"
                         : "stableDiffusionMetadata." + issue.path,
                     issue.message);
        }
    }

    AssetLookup assetsById;
    assetsById.reserve(document.assets.size());
    for (std::size_t index = 0; index < document.assets.size(); ++index) {
        const Asset &asset = document.assets[index];
        const std::string &id = assetId(asset);
        const std::string idPath = "assets[" + std::to_string(index) + "].id";
        if (id.empty()) {
            addIssue(result, ValidationCode::InvalidAssetId, idPath,
                     "asset id must not be empty");
        } else if (!assetsById.emplace(id, &asset).second) {
            addIssue(result, ValidationCode::DuplicateAssetId, idPath,
                     "asset ids must be unique");
        }

        if (const auto *raster = std::get_if<RasterAsset>(&asset)) {
            validateRasterAsset(*raster, index, result);
        } else if (const auto *vector = std::get_if<VectorAsset>(&asset)) {
            validateVectorAsset(*vector, index, result);
        } else {
            validateChunkedRasterAsset(document,
                                       std::get<ChunkedRasterAsset>(asset),
                                       index,
                                       result);
        }
    }

    std::unordered_map<std::string, const AudioAsset *> audioAssetsById;
    if ((!document.audioAssets.empty() || !document.audioTracks.empty())
        && document.formatVersion.minor < 4) {
        addIssue(result, ValidationCode::UnsupportedFormatVersion, "audioTracks",
                 "audio assets and tracks require document format 1.4 or newer");
    }
    for (std::size_t index = 0; index < document.audioAssets.size(); ++index) {
        const AudioAsset &asset = document.audioAssets[index];
        const std::string path = "audioAssets[" + std::to_string(index) + "]";
        if (asset.id.empty()) {
            addIssue(result, ValidationCode::InvalidAssetId, path + ".id",
                     "audio asset id must not be empty");
        } else if (assetsById.contains(asset.id)
                   || !audioAssetsById.emplace(asset.id, &asset).second) {
            addIssue(result, ValidationCode::DuplicateAssetId, path + ".id",
                     "visual and audio asset ids must be unique");
        }
        if (asset.sampleRate < 8000 || asset.sampleRate > 192000
            || asset.channelCount < 1 || asset.channelCount > 2
            || asset.samples.empty()
            || (asset.channelCount != 0 && asset.samples.size() % asset.channelCount != 0)) {
            addIssue(result, ValidationCode::InvalidAudioAsset, path,
                     "audio requires non-empty interleaved PCM16, 1-2 channels, and 8000-192000 Hz");
        }
    }

    std::unordered_set<std::string> layerIds;
    std::unordered_map<std::string, const Layer *> layersById;
    std::unordered_map<std::string, std::size_t> layerPositions;
    std::unordered_map<std::string, const KeyframedSource *> keyframedSources;
    layersById.reserve(document.layers.size());
    layerPositions.reserve(document.layers.size());
    keyframedSources.reserve(document.layers.size());
    for (std::size_t index = 0; index < document.layers.size(); ++index) {
        const Layer &layer = document.layers[index];
        const LayerProperties &properties = layerProperties(layer);
        const LayerSource &sourceValue = layerSource(layer);
        const ContentKind requiredKind = contentKind(layer);
        const std::string layerPath = "layers[" + std::to_string(index) + "]";
        bool hasUniqueLayerId = false;
        if (properties.id.empty()) {
            addIssue(result, ValidationCode::InvalidLayer, layerPath + ".id",
                     "layer id must not be empty");
        } else if (!layerIds.insert(properties.id).second) {
            addIssue(result, ValidationCode::DuplicateLayerId, layerPath + ".id",
                     "layer ids must be unique");
        } else {
            hasUniqueLayerId = true;
            layersById.emplace(properties.id, &layer);
            layerPositions.emplace(properties.id, index);
        }
        if (!std::isfinite(properties.opacity)
            || properties.opacity < 0.0
            || properties.opacity > 1.0
            || !hasFiniteTransform(properties.transform)
            || !isSupportedLayerBlendMode(properties.blendMode)) {
            addIssue(result, ValidationCode::InvalidLayer, layerPath,
                     "layer opacity, transform, and blend mode must be supported and in range");
        }
        if (properties.frameRange
            && (document.formatVersion.minor < 3
                || properties.frameRange->firstFrame
                    > properties.frameRange->lastFrame
                || properties.frameRange->lastFrame
                    >= document.timeline.frameCount)) {
            addIssue(result,
                     ValidationCode::InvalidLayerFrameRange,
                     layerPath + ".properties.frameRange",
                     "an explicit inclusive layer frame range requires format 1.3 and must remain ordered inside the timeline");
        }

        if (const auto *source = std::get_if<StaticSource>(&sourceValue)) {
            validateAssetReference(assetsById, source->assetId,
                                   layerPath + ".source.assetId", result, &requiredKind);
            continue;
        }

        const auto &source = std::get<KeyframedSource>(sourceValue);
        if (hasUniqueLayerId) {
            keyframedSources.emplace(properties.id, &source);
        }
        if (source.frameIndices.empty()) {
            addIssue(result, ValidationCode::InvalidKeyframes,
                     layerPath + ".source.frameIndices",
                     "a keyframed layer must index at least its frame-zero owner");
        }
        for (std::size_t framePosition = 0;
             framePosition < source.frameIndices.size();
             ++framePosition) {
            const FrameIndex frame = source.frameIndices[framePosition];
            const std::string indexPath = layerPath + ".source.frameIndices["
                + std::to_string(framePosition) + "]";
            if (frame >= document.timeline.frameCount) {
                addIssue(result, ValidationCode::InvalidKeyframes, indexPath,
                         "a derived owner-frame index must remain inside the timeline");
            }
            if ((framePosition == 0 && frame != 0)
                || (framePosition > 0
                    && frame <= source.frameIndices[framePosition - 1])) {
                addIssue(result, ValidationCode::InvalidKeyframes, indexPath,
                         "derived owner-frame indices must start at zero and be strictly increasing");
            }
        }
    }

    std::unordered_set<std::string> audioClipIds;
    for (std::size_t trackIndex = 0; trackIndex < document.audioTracks.size(); ++trackIndex) {
        const AudioTrackLayer &track = document.audioTracks[trackIndex];
        const std::string trackPath = "audioTracks[" + std::to_string(trackIndex) + "]";
        if (track.id.empty()) {
            addIssue(result, ValidationCode::InvalidAudioTrack, trackPath + ".id",
                     "audio track id must not be empty");
        } else if (!layerIds.insert(track.id).second) {
            addIssue(result, ValidationCode::DuplicateLayerId, trackPath + ".id",
                     "visual and audio track ids must be unique");
        }
        if (!std::isfinite(track.gainDb) || track.gainDb < -96.0 || track.gainDb > 24.0) {
            addIssue(result, ValidationCode::InvalidAudioTrack, trackPath + ".gainDb",
                     "audio track gain must be finite and within -96 to 24 dB");
        }
        std::uint64_t previousEnd = 0;
        for (std::size_t clipIndex = 0; clipIndex < track.clips.size(); ++clipIndex) {
            const AudioClip &clip = track.clips[clipIndex];
            const std::string clipPath = trackPath + ".clips[" + std::to_string(clipIndex) + "]";
            if (clip.id.empty()) {
                addIssue(result, ValidationCode::InvalidAudioClip, clipPath + ".id",
                         "audio clip id must not be empty");
            } else if (!audioClipIds.insert(clip.id).second) {
                addIssue(result, ValidationCode::DuplicateAudioClipId, clipPath + ".id",
                         "audio clip ids must be unique across all tracks");
            }
            const std::uint64_t end = static_cast<std::uint64_t>(clip.startFrame) + clip.durationFrames;
            if (clip.durationFrames == 0 || end > document.timeline.frameCount
                || (clipIndex > 0 && clip.startFrame < previousEnd)) {
                addIssue(result, ValidationCode::InvalidAudioClip, clipPath,
                         "audio clips require positive duration, timeline bounds, and nonoverlapping start order");
            }
            previousEnd = end;
            if (!std::isfinite(clip.gainDb) || clip.gainDb < -96.0 || clip.gainDb > 24.0) {
                addIssue(result, ValidationCode::InvalidAudioClip, clipPath + ".gainDb",
                         "audio clip gain must be finite and within -96 to 24 dB");
            }
            const auto found = audioAssetsById.find(clip.assetId);
            if (found == audioAssetsById.end()) {
                addIssue(result, ValidationCode::MissingAsset, clipPath + ".assetId",
                         "an audio clip must reference an existing audio asset");
                continue;
            }
            const AudioAsset &asset = *found->second;
            const std::optional<std::uint64_t> required = audioSampleFrameCount(
                clip.durationFrames, document.timeline.frameRate, asset.sampleRate);
            const std::uint64_t available = asset.channelCount == 0
                ? 0 : asset.samples.size() / asset.channelCount;
            if (!required || clip.sourceOffsetSamples > available
                || *required > available - clip.sourceOffsetSamples) {
                addIssue(result, ValidationCode::InvalidAudioClip, clipPath + ".sourceOffsetSamples",
                         "audio clip trim plus its exact rational duration must fit the source sample frames");
            }
        }
    }

    std::unordered_map<std::string, std::vector<FrameIndex>> observedFrameIndices;
    observedFrameIndices.reserve(keyframedSources.size());
    for (std::size_t framePosition = 0;
         framePosition < document.frames.size();
         ++framePosition) {
        const Frame &frame = document.frames[framePosition];
        const std::string framePath = "frames[" + std::to_string(framePosition) + "]";
        if (frame.index >= document.timeline.frameCount) {
            addIssue(result, ValidationCode::InvalidKeyframes,
                     framePath + ".index",
                     "frame-owned keyframes must remain inside the document timeline");
        }
        if (framePosition > 0
            && frame.index <= document.frames[framePosition - 1].index) {
            addIssue(result, ValidationCode::InvalidKeyframes,
                     framePath + ".index",
                     "frames that own keyframes must be strictly increasing");
        }
        if (frame.keyframes.empty()) {
            addIssue(result, ValidationCode::InvalidKeyframes,
                     framePath + ".keyframes",
                     "a persisted frame must directly own at least one keyframe");
            continue;
        }

        for (std::size_t keyframePosition = 0;
             keyframePosition < frame.keyframes.size();
             ++keyframePosition) {
            const Keyframe &keyframe = frame.keyframes[keyframePosition];
            const std::string keyframePath = framePath + ".keyframes["
                + std::to_string(keyframePosition) + "]";
            if (keyframe.layerId.empty()) {
                addIssue(result, ValidationCode::InvalidKeyframes,
                         keyframePath + ".layerId",
                         "a frame-owned keyframe must have a non-empty layer id");
                continue;
            }
            if (keyframePosition > 0
                && keyframe.layerId
                    <= frame.keyframes[keyframePosition - 1].layerId) {
                addIssue(result, ValidationCode::InvalidKeyframes,
                         keyframePath + ".layerId",
                         "frame-owned keyframes must use unique canonical layer-id order");
            }

            const auto owner = layersById.find(keyframe.layerId);
            if (owner == layersById.end()) {
                addIssue(result, ValidationCode::InvalidKeyframes,
                         keyframePath + ".layerId",
                         "a frame-owned keyframe must reference an existing layer");
                continue;
            }
            if (!std::holds_alternative<KeyframedSource>(
                    layerSource(*owner->second))) {
                addIssue(result, ValidationCode::InvalidKeyframes,
                         keyframePath + ".layerId",
                         "a frame-owned keyframe may reference only a keyframed layer");
                continue;
            }

            observedFrameIndices[keyframe.layerId].push_back(frame.index);
            const ContentKind requiredKind = contentKind(*owner->second);
            validateAssetReference(assetsById, keyframe.assetId,
                                   keyframePath + ".assetId", result, &requiredKind);
        }
    }

    for (const auto &[layerId, source] : keyframedSources) {
        const auto observed = observedFrameIndices.find(layerId);
        const std::vector<FrameIndex> empty;
        const std::vector<FrameIndex> &owners = observed == observedFrameIndices.end()
            ? empty
            : observed->second;
        if (source->frameIndices != owners) {
            const auto position = layerPositions.find(layerId);
            addIssue(result, ValidationCode::InvalidKeyframes,
                     position != layerPositions.end()
                         ? "layers[" + std::to_string(position->second)
                             + "].source.frameIndices"
                         : "layers",
                     "the derived frame index must exactly match every frame that owns this layer's keyframe");
        }
    }

    return result;
}

} // namespace iiSharedCanvas
