#pragma once
#include "iiSharedCanvas/Export.h"
#include "ControlNet/ControlNet.h"
#include <cstdint>
#include <optional>
#include <string>
#include <vector>

namespace iiSharedCanvas {
struct Document; struct IpAdapterAsset; struct IpAdapterLayer;
// Encoder output and adapter-projected image prompt tokens are different spaces.
enum class IpAdapterEmbeddingStage : std::uint8_t { EncoderPooled, EncoderHiddenStates, ProjectedTokens };
enum class IpAdapterBranch : std::uint8_t { Conditional, Unconditional };
struct IpAdapterEmbeddingDescriptor {
    IpAdapterEmbeddingStage stage = IpAdapterEmbeddingStage::EncoderPooled;
    std::string encoderId;
    std::string encoderRevision; // Immutable revision or content hash supplied by the producer.
    std::string adapterId;
    std::string adapterRevision;
    std::string baseModelId; // Producer's diffusion model compatibility identifier.
    std::string preprocessingId; // Identifies resize/crop/normalization and hidden-state selection.
    bool operator==(const IpAdapterEmbeddingDescriptor &) const = default;
};
// One image, batch=1. Row-major [tokenCount,channelCount], finite IEEE binary32.
// Pooled encoder output has one token. Values are not clamped or normalized.
struct IpAdapterTensor {
    std::uint32_t tokenCount = 0;
    std::uint32_t channelCount = 0;
    std::vector<float> values;
    // Compare float bits so a signed-zero edit is not discarded as unchanged.
    IISHAREDCANVAS_EXPORT bool operator==(const IpAdapterTensor &) const;
};
struct IpAdapterExportOptions {
    bool requireUnconditional = true; // CFG requires a separately produced negative branch.
    std::uint64_t maximumValues = 16ULL * 1024ULL * 1024ULL; // Sum of both branches.
};
struct IpAdapterEmbeddingResult {
    IpAdapterEmbeddingDescriptor descriptor;
    IpAdapterTensor conditional;
    std::optional<IpAdapterTensor> unconditional;
    ControlNetSettings control; // modelId/revision identify the adapter, not the image encoder.
    std::string message;
    [[nodiscard]] bool ok() const noexcept { return message.empty(); }
};
IISHAREDCANVAS_EXPORT IpAdapterAsset *findIpAdapterAsset(Document &, const std::string &) noexcept;
IISHAREDCANVAS_EXPORT const IpAdapterAsset *findIpAdapterAsset(const Document &, const std::string &) noexcept;
IISHAREDCANVAS_EXPORT IpAdapterLayer *findIpAdapterLayer(Document &, const std::string &) noexcept;
IISHAREDCANVAS_EXPORT const IpAdapterLayer *findIpAdapterLayer(const Document &, const std::string &) noexcept;
IISHAREDCANVAS_EXPORT std::string validateIpAdapterAsset(const IpAdapterAsset &);
// Checks adapter binding and identical descriptor/shape across a layer's frame states.
IISHAREDCANVAS_EXPORT std::string validateIpAdapterLayerSources(const Document &, const IpAdapterLayer &);
// Exact owned tensors, without encoding, projection, CFG synthesis, scaling or inference.
// Honors enabled and timeline/frame range; independent of display transforms and visibility.
IISHAREDCANVAS_EXPORT IpAdapterEmbeddingResult exportIpAdapterEmbeddings(const Document &,
    const std::string &layerId, std::uint32_t frame, IpAdapterExportOptions options = {});
} // namespace iiSharedCanvas
