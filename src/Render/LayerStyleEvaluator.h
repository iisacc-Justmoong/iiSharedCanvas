#pragma once

#include "Bitmap/BitmapProcessing.h"
#include "Document/LayerStyleStack.h"
#include "iiSharedCanvas/Export.h"

namespace iiSharedCanvas {

// Evaluates a style stack into a detached raster of the same dimensions. The
// caller supplies transparent padding wherever an effect may extend outside
// the original shape; this makes allocation and tiling explicit.
class IISHAREDCANVAS_EXPORT LayerStyleEvaluator final {
public:
    // Scale converts document-pixel distances into output pixels. Style alpha
    // derives from the unscaled source even when fillOpacity is zero.
    [[nodiscard]] RasterProcessResult operator()(const RasterLayer &source,
        const LayerStyleStack &styles, double fillOpacity = 1,
        double scaleX = 1, double scaleY = 1) const;
};

} // namespace iiSharedCanvas
