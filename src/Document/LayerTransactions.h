#pragma once

#include "Document/Document.h"
#include "Render/FrameRenderer.h"
#include "iiSharedCanvas/Export.h"

#include <span>
#include <string>
#include <vector>

namespace iiSharedCanvas {

// Caller-supplied per-frame render data. The controller renders the selected
// layers into merged, then renders the complete document before and after its
// candidate replacement. Equal complete frames prove visible preservation.
struct LayerMergeFrame {
    FrameRenderResult merged, before, after;
};

// Detached candidate for a single document-history publication. The caller
// owns all values and must not reuse a plan after its source revision changes.
struct LayerTransactionPlan {
    std::uint64_t sourceRevision=0;
    bool flatten=false;
    std::string resultLayerId;
    std::vector<std::string> removedLayers;
    std::vector<std::string> removedAssets;
    std::vector<Asset> addedAssets;
    std::vector<Layer> layers;
    std::vector<Frame> frames;
    std::vector<Artboard> artboards;
    SelectionLayerRelations relations;
};
struct LayerTransactionResult {
    LayerTransactionPlan plan;
    std::string error;
    [[nodiscard]] bool ok() const noexcept { return error.empty(); }
};

// Prepares a selected-layer replacement using externally supplied rendering
// evidence. It never calls a renderer or mutates the source document.
class IISHAREDCANVAS_EXPORT LayerMergeTransaction final {
public:
    // Requires contiguous artwork in one group and artboard, without a
    // backdrop-dependent blend, clipping or adjustment. The controller must
    // provide before/after full-frame evidence for every timeline frame.
    [[nodiscard]] LayerTransactionResult operator()(const Document &document,
        std::span<const std::string> selected,std::span<const LayerMergeFrame> frames,
        std::string layerId,std::string assetPrefix) const;
};

// Prepares a whole-document flatten from caller-rendered complete frames.
// Audio and saved selection channels are retained; groups and layers disappear.
class IISHAREDCANVAS_EXPORT DocumentFlattenTransaction final {
public:
    // The full document view must fit its canvas region; each supplied image
    // covers that region at native resolution in timeline order.
    [[nodiscard]] LayerTransactionResult operator()(const Document &document,
        std::span<const FrameRenderResult> frames,std::string layerId,std::string assetPrefix) const;
};

// External orchestration for callers that need a merge with independently
// rendered evidence. It retains ownership of the source with the caller,
// renders every timeline frame and a detached candidate, and returns a plan
// only when their complete pixels match. Temporary storage can scale with the
// document and timeline. It performs no publication or file access and must
// not race with mutation of the supplied document.
[[nodiscard]] IISHAREDCANVAS_EXPORT LayerTransactionResult prepareVerifiedLayerMerge(
    const Document &document,std::span<const std::string> selected,
    std::string layerId,std::string assetPrefix);

} // namespace iiSharedCanvas
