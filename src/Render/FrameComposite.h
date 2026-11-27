#pragma once

#include "Bitmap/BitmapProcessing.h"
#include "Render/FrameRenderer.h"
#include "iiSharedCanvas/Export.h"

#include <cstddef>
#include <span>
#include <string_view>

namespace iiSharedCanvas {

// Composites explicitly selected, already-rendered layer values. The caller
// owns the input batch, chooses the frame and controls concurrent access. The
// optional selection masks correspond to output tiles in request order.
class IISHAREDCANVAS_EXPORT SelectedFrameComposite final {
public:
    // Layer IDs must be distinct and present. A selected clipped layer requires
    // its original clipping base to be selected as well. Artboard boundaries
    // are retained but their background colors are excluded. Input is never
    // modified; invalid or over-budget requests return no partial tiles.
    [[nodiscard]] FrameTileRenderResult operator()(const FrameLayerBatchRenderResult &batch,
        std::span<const std::string_view> selectedLayerIds,
        std::span<const RasterMask> selection = {},
        std::size_t maximumPixels = 64ULL * 1024 * 1024) const;
};

// Composites the supplied visible layer batch, including artboard backgrounds.
// It has no access to a document, current-frame state, clipboard or filesystem.
class IISHAREDCANVAS_EXPORT MergedFrameComposite final {
public:
    // A supplied selection contains one output-sized coverage mask per tile;
    // an empty span means no selection restriction. Invalid data and budget
    // failures return an error without modifying the borrowed inputs.
    [[nodiscard]] FrameTileRenderResult operator()(const FrameLayerBatchRenderResult &batch,
        std::span<const RasterMask> selection = {},
        std::size_t maximumPixels = 64ULL * 1024 * 1024) const;
};

} // namespace iiSharedCanvas
