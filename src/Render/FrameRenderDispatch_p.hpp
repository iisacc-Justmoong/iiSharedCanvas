#pragma once
#include "Render/FrameRenderer.h"

// Private inter-library contract shared by Core and the Quick scheduler.
// The scheduler never owns document validation or raster composition policy.
namespace iiSharedCanvas::render_detail {
IISHAREDCANVAS_EXPORT FrameLayerBatchRenderResult preflightFrameLayerRender(
    const Document &document, FrameIndex frame,
    const std::vector<FrameRenderTileRequest> &requests);
IISHAREDCANVAS_EXPORT FrameLayerTileRenderResult renderPreflightedFrameLayerTiles(
    const Document &document, FrameIndex frame, std::size_t layerIndex,
    const std::vector<FrameRenderTileRequest> &requests);
}
