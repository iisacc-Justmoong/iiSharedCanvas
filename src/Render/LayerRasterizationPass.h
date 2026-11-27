#pragma once

#include "Render/FrameRenderer.h"
#include "iiSharedCanvas/Export.h"

namespace iiSharedCanvas {

// Produces a detached raster from a previously rendered layer tile. Source
// selection, rendering and group visibility are controlled by the caller.
class IISHAREDCANVAS_EXPORT LayerRasterizationPass final {
public:
    // Applies the supplied layer and fill opacity to the sole requested tile.
    // Backdrop-dependent features cannot be represented and produce an error.
    // Neither the input tile nor any document is modified.
    [[nodiscard]] FrameRenderResult operator()(const FrameLayerTileRenderResult &layer,
        const FrameRenderTileRequest &request, bool groupVisible) const;
};

} // namespace iiSharedCanvas
