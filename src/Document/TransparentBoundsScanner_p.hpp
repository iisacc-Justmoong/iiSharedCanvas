#pragma once
#include "Document/TransparentBoundsScanner.h"
namespace iiSharedCanvas::detail {
// Shared value-only scan used by independent bounds objects; retains no input.
CanvasBoundsResult scanTransparentBounds(const RasterLayer &,CanvasOrigin,std::uint8_t,std::size_t);
}
