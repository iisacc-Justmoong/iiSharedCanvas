#pragma once

#include "Render/FrameRenderer.h"
#include "iiSharedCanvas/Export.h"

#include <string>

namespace iiSharedCanvas {

// Identifies the source chosen and rendered by the external controller.
enum class LayerSliceSource { Layer, Composite };

// A world-coordinate, half-open crop with explicit output resolution. It is
// independent of any particular image encoder or output filesystem.
struct LayerSliceDescriptor {
    LayerSliceSource source = LayerSliceSource::Composite;
    std::string layerId;
    FrameIndex frame = 0;
    CanvasRegion region;
    CanvasExtent outputExtent;
    RasterSampling sampling = RasterSampling::Nearest;
};

// Crops caller-supplied pixels. This object neither selects nor renders a
// document; the controller chooses the source and owns all data lifetimes.
class IISHAREDCANVAS_EXPORT LayerSliceBuilder final {
public:
    // The source region describes the world area represented by rendered pixels.
    // The requested region must lie inside it. Returns a detached raster or an
    // explicit error without modifying either the input or any document.
    [[nodiscard]] FrameRenderResult operator()(const FrameRenderResult &rendered,
        CanvasRegion sourceRegion, const LayerSliceDescriptor &descriptor) const;
};

} // namespace iiSharedCanvas
