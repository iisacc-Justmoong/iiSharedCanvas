#include "Document/Document.h"

#include <algorithm>
#include <iterator>

namespace iiSharedCanvas {

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

Keyframe *findKeyframe(KeyframedSource &source, FrameIndex frame) noexcept
{
    const auto match = std::lower_bound(
        source.keyframes.begin(), source.keyframes.end(), frame,
        [](const Keyframe &keyframe, FrameIndex requestedFrame) {
            return keyframe.frame < requestedFrame;
        });
    return match != source.keyframes.end() && match->frame == frame ? &*match : nullptr;
}

const Keyframe *findKeyframe(const KeyframedSource &source, FrameIndex frame) noexcept
{
    const auto match = std::lower_bound(
        source.keyframes.begin(), source.keyframes.end(), frame,
        [](const Keyframe &keyframe, FrameIndex requestedFrame) {
            return keyframe.frame < requestedFrame;
        });
    return match != source.keyframes.end() && match->frame == frame ? &*match : nullptr;
}

std::optional<std::size_t> keyframeIndex(const KeyframedSource &source,
                                         FrameIndex frame) noexcept
{
    const auto match = std::lower_bound(
        source.keyframes.begin(), source.keyframes.end(), frame,
        [](const Keyframe &keyframe, FrameIndex requestedFrame) {
            return keyframe.frame < requestedFrame;
        });
    if (match == source.keyframes.end() || match->frame != frame) {
        return std::nullopt;
    }
    return static_cast<std::size_t>(std::distance(source.keyframes.begin(), match));
}

std::vector<AssetReference> assetReferences(const Document &document,
                                            const std::string &referencedAssetId)
{
    std::vector<AssetReference> references;
    for (std::size_t layerPosition = 0;
         layerPosition < document.layers.size();
         ++layerPosition) {
        const LayerSource &source = layerSource(document.layers[layerPosition]);
        if (const auto *staticSource = std::get_if<StaticSource>(&source)) {
            if (staticSource->assetId == referencedAssetId) {
                references.push_back({layerPosition, std::nullopt});
            }
            continue;
        }

        const auto &keyframed = std::get<KeyframedSource>(source);
        for (std::size_t keyframePosition = 0;
             keyframePosition < keyframed.keyframes.size();
             ++keyframePosition) {
            if (keyframed.keyframes[keyframePosition].assetId == referencedAssetId) {
                references.push_back({layerPosition, keyframePosition});
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

    const ContentKind requiredKind = contentKind(layer);
    const LayerSource &sourceValue = layerSource(layer);
    if (const auto *source = std::get_if<StaticSource>(&sourceValue)) {
        const Asset *asset = findAsset(document, source->assetId);
        return asset && contentKind(*asset) == requiredKind ? asset : nullptr;
    }

    const auto &source = std::get<KeyframedSource>(sourceValue);
    const auto next = std::upper_bound(
        source.keyframes.begin(), source.keyframes.end(), frame,
        [](FrameIndex requestedFrame, const Keyframe &keyframe) {
            return requestedFrame < keyframe.frame;
        });
    if (next == source.keyframes.begin()) {
        return nullptr;
    }

    const Asset *asset = findAsset(document, std::prev(next)->assetId);
    if (!asset || contentKind(*asset) != requiredKind) {
        return nullptr;
    }
    return asset;
}

} // namespace iiSharedCanvas
