#pragma once

#include "Document/Document.h"
#include "Media/MediaIo.h"
#include "iiSharedCanvas/Export.h"

#include <span>
#include <string>
#include <string_view>
#include <vector>

namespace iiSharedCanvas {

// A CSS declaration block, without a selector or braces. Losses describe
// document properties that the declaration block cannot faithfully express.
struct LayerCssResult {
    std::string declarations;
    std::vector<std::string> losses;
    std::string error;
    [[nodiscard]] bool ok() const noexcept { return error.empty(); }
};

// Stateless projection of explicitly supplied layer data. It neither resolves
// assets nor writes to a clipboard or file. A null fill means that content is
// supplied by another component; this omission is reported in losses. The
// optional group tree is borrowed; supply it when projecting a document layer
// so inherited visibility and group composition can be reported as losses.
class IISHAREDCANVAS_EXPORT LayerCssEncoder final {
public:
    [[nodiscard]] LayerCssResult operator()(const LayerProperties &properties,
        CanvasExtent extent, const FillLayerContent *fill = nullptr,
        const LayerGroupTree *groups = nullptr) const;
};

// A caller-rendered, fully composed single layer covering the requested region.
// The raster and ID are borrowed; neither is retained by the projection.
struct SvgLayerRasterFallback {
    std::string_view layerId;
    const RasterLayer *pixels = nullptr;
};

// SVG is standalone UTF-8. rasterized reports any embedded PNG; the layer IDs
// identify which selected layers lost vector editability.
struct SelectedLayerSvgResult {
    std::string svg;
    bool rasterized = false;
    std::vector<std::string> warnings;
    std::string error;
    std::vector<std::string> rasterizedLayers;
    [[nodiscard]] bool ok() const noexcept { return error.empty(); }
};

// Synchronous selected-layer export. The caller owns the document and supplies
// both the selected composition and, optionally, isolated layer rasters for
// the chosen frame and region. Independent layers use their own fallback;
// backdrop-dependent compositions require the whole-selection fallback.
// No paths, global state or external resources are read. Inputs stay borrowed.
class IISHAREDCANVAS_EXPORT SelectedLayerSvgProjection final {
public:
    // Region uses document coordinates; output dimensions account for the
    // document pixel aspect ratio. Missing IDs, frame/geometry errors and
    // unsupported vector semantics without a fallback return no partial SVG.
    [[nodiscard]] SelectedLayerSvgResult operator()(const Document &document, FrameIndex frame,
        std::span<const std::string_view> selectedLayerIds, CanvasRegion region,
        const RasterLayer *fallback = nullptr, const MediaLimits &limits = {},
        std::span<const SvgLayerRasterFallback> layerFallbacks = {}) const;
};

} // namespace iiSharedCanvas
