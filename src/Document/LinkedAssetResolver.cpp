#include "Document/LinkedAssetResolver.h"

namespace iiSharedCanvas {
LinkedCanvasCopyResult LinkedAssetResolver::copy(std::string id, const Document &source,
                                                  FrameIndex frame, SerializationLimits limits)
{
    if (id.empty() || frame >= source.timeline.frameCount)
        return {{}, {IiscErrorCode::InvalidDocument, 0, "linked asset id or frame is invalid"}};
    if (!limits.maximumEmbeddedCanvasDepth)
        return {{}, {IiscErrorCode::LimitExceeded, 0, "embedded canvas depth exceeded"}};
    --limits.maximumEmbeddedCanvasDepth;
    auto encoded = encodeIisc(source, limits);
    if (!encoded.ok()) return {{}, std::move(encoded.error)};
    if (encoded.bytes.size() > limits.maximumEmbeddedCanvasBytes)
        return {{}, {IiscErrorCode::LimitExceeded, 0, "embedded canvas bytes exceeded"}};
    return {{std::move(id), std::move(encoded.bytes), frame}, {}};
}

FrameRenderResult LinkedAssetResolver::resolve(const LinkedCanvasAsset &asset, SerializationLimits limits)
{
    FrameRenderResult failure;
    failure.status = FrameRenderStatus::AssetResolutionFailed;
    if (!limits.maximumEmbeddedCanvasDepth || asset.document.size() > limits.maximumEmbeddedCanvasBytes) {
        failure.message = "embedded canvas depth or byte budget exceeded"; return failure;
    }
    --limits.maximumEmbeddedCanvasDepth;
    auto decoded = decodeIisc(asset.document, limits);
    if (!decoded.ok()) { failure.message = decoded.error.message; return failure; }
    if (asset.frame >= decoded.document.timeline.frameCount) {
        failure.message = "embedded canvas frame is out of range"; return failure;
    }
    const auto view = documentViewRegion(decoded.document);
    if (view.extent.width <= 0 || view.extent.height <= 0 ||
        std::uint64_t(view.extent.width) * std::uint64_t(view.extent.height) > limits.maximumCanvasPixels) {
        failure.message = "embedded canvas rendered view exceeds the pixel budget"; return failure;
    }
    return renderFrame(decoded.document, asset.frame);
}
} // namespace iiSharedCanvas
