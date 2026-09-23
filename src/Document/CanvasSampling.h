#pragma once

#include "Document/Document.h"

namespace iiSharedCanvas {

struct LayerSample {
    bool visible = true;
    double opacity = 1.0;
    AffineTransform transform;
};

// Requires a validated document. Before/after the motion key range, hold the endpoint.
// Motion is local to the base affine transform: base * T(position+anchor) * R * S * T(-anchor).
IISHAREDCANVAS_EXPORT LayerSample sampleLayerAt(
    const Document &document, const Layer &layer, FrameIndex frame) noexcept;

// Exact floor conversion between rational rates; nullopt outside the clip or at transparent EOF.
IISHAREDCANVAS_EXPORT std::optional<FrameIndex> videoFrameIndexAt(
    const Document &document, const VideoLayer &layer, FrameIndex frame) noexcept;
IISHAREDCANVAS_EXPORT const RasterLayer *resolveVideoFrameAt(
    const Document &document, const VideoLayer &layer, FrameIndex frame) noexcept;

} // namespace iiSharedCanvas
