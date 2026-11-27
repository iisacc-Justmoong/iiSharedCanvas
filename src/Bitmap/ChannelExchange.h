#pragma once

#include "Bitmap/BitmapProcessing.h"
#include "iiSharedCanvas/Export.h"

#include <cstddef>

namespace iiSharedCanvas {

// RGB and alpha are straight, unpremultiplied 8-bit channels. Luminance uses
// Rec. 709 coefficients on encoded RGB; transparent RGB is not discarded.
enum class RasterChannel { Composite, Red, Green, Blue, Alpha, Luminance };

// A synchronous borrowed view. The caller aligns all sources to the target
// extent and keeps them alive and immutable for the duration of execution.
struct ChannelSource {
    const RasterLayer *pixels = nullptr;
    RasterChannel channel = RasterChannel::Composite;
    bool invert = false;
};

// Optional coverage is borrowed, in target-local coordinates. A null pointer
// denotes complete coverage. A finite opacity must lie in the inclusive unit
// interval; maximumPixels limits the number of pixels in the returned image.
struct ChannelCompositeOptions {
    RasterBlendMode blendMode = RasterBlendMode::SourceOver;
    double opacity = 1;
    const RasterMask *selection = nullptr;
    std::size_t maximumPixels = 64ULL * 1024 * 1024;
};

// Pure, synchronous operation. Source and target remain unchanged. To publish
// the result, the caller chooses a document layer and frame and submits the
// detached raster through DocumentHistory::replaceLayerFrameRaster, which
// isolates shared pixels, enforces locks, and records one undo entry.
class IISHAREDCANVAS_EXPORT ApplyImageComposite final {
public:
    // Composite changes the complete ARGB value. Red, Green, Blue and Alpha
    // replace only the designated component; Luminance is not a writable
    // target. Invalid data and allocation failures return an empty result.
    [[nodiscard]] RasterProcessResult operator()(const ChannelSource &source,
        const RasterLayer &target, RasterChannel targetChannel = RasterChannel::Composite,
        const ChannelCompositeOptions &options = {}) const;
};

// Computes a detached 8-bit channel. It owns no document or editor and cannot
// publish changes; external orchestration may create a named selection channel,
// update the current selection, or create a document from the returned mask.
class IISHAREDCANVAS_EXPORT ChannelCalculations final {
public:
    // Both sources must specify a scalar channel. The first is blended over
    // the second, then optionally inverted. Selection interpolates from the
    // second channel to that final value. Source values remain untouched.
    [[nodiscard]] RasterMaskResult operator()(const ChannelSource &first,
        const ChannelSource &second, bool invertResult = false,
        const ChannelCompositeOptions &options = {}) const;
};

} // namespace iiSharedCanvas
