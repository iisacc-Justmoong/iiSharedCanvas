#include "Validation/Validation.h"

#include <cmath>
#include <cstddef>
#include <cstdint>
#include <limits>
#include <string>
#include <type_traits>
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

void validateAssetReference(const Document &document,
                            const std::string &id,
                            const std::string &path,
                            ValidationResult &result,
                            const ContentKind *requiredKind = nullptr)
{
    const Asset *asset = findAsset(document, id);
    if (!asset) {
        addIssue(result, ValidationCode::MissingAsset, path,
                 "layer source references an unknown asset id");
        return;
    }
    if (requiredKind && contentKind(*asset) != *requiredKind) {
        addIssue(result, ValidationCode::ContentKindMismatch, path,
                 "all keyframes in one track must reference the declared content kind");
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

    std::unordered_set<std::string> assetIds;
    for (std::size_t index = 0; index < document.assets.size(); ++index) {
        const Asset &asset = document.assets[index];
        const std::string &id = assetId(asset);
        const std::string idPath = "assets[" + std::to_string(index) + "].id";
        if (id.empty()) {
            addIssue(result, ValidationCode::InvalidAssetId, idPath,
                     "asset id must not be empty");
        } else if (!assetIds.insert(id).second) {
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

    std::unordered_set<std::string> layerIds;
    for (std::size_t index = 0; index < document.layers.size(); ++index) {
        const Layer &layer = document.layers[index];
        const std::string layerPath = "layers[" + std::to_string(index) + "]";
        if (layer.id.empty()) {
            addIssue(result, ValidationCode::InvalidLayer, layerPath + ".id",
                     "layer id must not be empty");
        } else if (!layerIds.insert(layer.id).second) {
            addIssue(result, ValidationCode::DuplicateLayerId, layerPath + ".id",
                     "layer ids must be unique");
        }
        if (!std::isfinite(layer.opacity)
            || layer.opacity < 0.0
            || layer.opacity > 1.0
            || !hasFiniteTransform(layer.transform)
            || !isSupportedLayerBlendMode(layer.blendMode)) {
            addIssue(result, ValidationCode::InvalidLayer, layerPath,
                     "layer opacity, transform, and blend mode must be supported and in range");
        }

        if (const auto *source = std::get_if<StaticSource>(&layer.source)) {
            validateAssetReference(document, source->assetId,
                                   layerPath + ".source.assetId", result);
            continue;
        }

        const auto &source = std::get<KeyframedSource>(layer.source);
        if (source.keyframes.empty()) {
            addIssue(result, ValidationCode::InvalidKeyframes,
                     layerPath + ".source.keyframes",
                     "an animated source must contain at least one keyframe");
            continue;
        }
        if (source.keyframes.front().frame != 0) {
            addIssue(result, ValidationCode::InvalidKeyframes,
                     layerPath + ".source.keyframes[0].frame",
                     "the first keyframe must begin at frame zero");
        }

        for (std::size_t keyframeIndex = 0;
             keyframeIndex < source.keyframes.size();
             ++keyframeIndex) {
            const Keyframe &keyframe = source.keyframes[keyframeIndex];
            const std::string keyframePath = layerPath + ".source.keyframes["
                + std::to_string(keyframeIndex) + "]";
            if (keyframe.frame >= document.timeline.frameCount) {
                addIssue(result, ValidationCode::InvalidKeyframes,
                         keyframePath + ".frame",
                         "keyframe must be inside the document timeline");
            }
            if (keyframeIndex > 0
                && keyframe.frame <= source.keyframes[keyframeIndex - 1].frame) {
                addIssue(result, ValidationCode::InvalidKeyframes,
                         keyframePath + ".frame",
                         "keyframe positions must be strictly increasing");
            }
            validateAssetReference(document, keyframe.assetId,
                                   keyframePath + ".assetId", result, &source.kind);
        }
    }

    return result;
}

} // namespace iiSharedCanvas
