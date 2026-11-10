#pragma once
#include "Bitmap/BitmapProcessing.h"
#include <span>
namespace iiSharedCanvas::detail {
// Rasterize a region of one logical canvas using document-local samples.
RasterMaskResult selectionPolygonRegion(CanvasExtent domain, CanvasRegion region,
                                        std::span<const Point> points, bool antialias);
}
