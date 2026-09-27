#pragma once
#include "iiSharedCanvas/Export.h"
#include "ControlNet/ControlNet.h"
#include <Layer/RasterLayer.h>
#include <cstdint>
#include <string>
#include <vector>

namespace iiSharedCanvas {
struct Document; struct CannyAsset; struct CannyLayer;
// Binary line mask: 0 = black background, 1 = white edge.
struct CannyControlMapResult {
    RasterLayer pixels; // Opaque RGB8, 0 = opaque black, 1 = opaque white.
    std::vector<std::uint8_t> mask; // Exact row-major binary mask; only 0 or 1.
    std::string message;
    [[nodiscard]] bool ok() const noexcept { return message.empty(); }
};
IISHAREDCANVAS_EXPORT CannyAsset *findCannyAsset(Document &, const std::string &) noexcept;
IISHAREDCANVAS_EXPORT const CannyAsset *findCannyAsset(const Document &, const std::string &) noexcept;
IISHAREDCANVAS_EXPORT CannyLayer *findCannyLayer(Document &, const std::string &) noexcept;
IISHAREDCANVAS_EXPORT const CannyLayer *findCannyLayer(const Document &, const std::string &) noexcept;
IISHAREDCANVAS_EXPORT std::string validateCannyAsset(const CannyAsset &);
// Throws invalid_argument for invalid data, length_error before exceeding maxPixels.
IISHAREDCANVAS_EXPORT RasterLayer cannyRasterPreview(const CannyAsset &,
    std::uint64_t maxPixels = 16ULL * 1024ULL * 1024ULL);
// Native asset coordinates. Ignores display visibility/opacity/transform, honors
// control.enabled and source frame range. Bounds all dense outputs before allocation.
IISHAREDCANVAS_EXPORT CannyControlMapResult renderCannyControlMap(const Document &,
    const std::string &layerId, std::uint32_t frame,
    std::uint64_t maxPixels = 16ULL * 1024ULL * 1024ULL);
} // namespace iiSharedCanvas
