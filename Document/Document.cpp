#include "Document/Document.h"

#include <algorithm>
#include <iterator>

namespace iiSharedCanvas {

ContentKind contentKind(const Asset &asset) noexcept
{
    return std::holds_alternative<RasterAsset>(asset)
        ? ContentKind::Raster
        : ContentKind::Vector;
}

const std::string &assetId(const Asset &asset) noexcept
{
    return std::visit([](const auto &value) -> const std::string & { return value.id; }, asset);
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

const Asset *resolveAssetAt(const Document &document,
                            const Layer &layer,
                            FrameIndex frame) noexcept
{
    if (frame >= document.timeline.frameCount) {
        return nullptr;
    }

    if (const auto *source = std::get_if<StaticSource>(&layer.source)) {
        return findAsset(document, source->assetId);
    }

    const auto &source = std::get<KeyframedSource>(layer.source);
    const auto next = std::upper_bound(
        source.keyframes.begin(), source.keyframes.end(), frame,
        [](FrameIndex requestedFrame, const Keyframe &keyframe) {
            return requestedFrame < keyframe.frame;
        });
    if (next == source.keyframes.begin()) {
        return nullptr;
    }

    const Asset *asset = findAsset(document, std::prev(next)->assetId);
    if (!asset || contentKind(*asset) != source.kind) {
        return nullptr;
    }
    return asset;
}

} // namespace iiSharedCanvas
