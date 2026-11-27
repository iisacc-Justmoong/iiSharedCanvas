#pragma once

#include "Document/Document.h"
#include "iiSharedCanvas/Export.h"

#include <cstddef>
#include <cstdint>
#include <optional>
#include <string>

namespace iiSharedCanvas {

// Detached half-open pixel bounds. Empty bounds mean that no pixel exceeded
// the threshold; an error instead means the input could not be examined.
struct CanvasBoundsResult {
    std::optional<CanvasRegion> bounds;
    std::string error;
    [[nodiscard]] bool ok() const noexcept { return error.empty(); }
};

// Scans borrowed straight-ARGB pixels without changing them or accessing any
// document, clock or filesystem. Concurrent calls are safe when the caller
// keeps the raster alive and unmodified for their duration.
class IISHAREDCANVAS_EXPORT TransparentBoundsScanner final {
public:
    // Alpha strictly greater than threshold contributes. RGB under zero alpha
    // does not. The origin maps the first input pixel to document coordinates.
    // Malformed storage, coordinate overflow and a pixel count over the
    // caller's work budget fail without returning partial bounds.
    [[nodiscard]] CanvasBoundsResult operator()(const RasterLayer &pixels,
        CanvasOrigin origin = {}, std::uint8_t threshold = 0,
        std::size_t maximumPixels = 256ULL * 1024 * 1024) const;
};

} // namespace iiSharedCanvas
