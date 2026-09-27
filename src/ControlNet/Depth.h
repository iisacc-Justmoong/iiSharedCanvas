#pragma once
#include "iiSharedCanvas/Export.h"
#include "ControlNet/ControlNet.h"
#include <Layer/RasterLayer.h>
#include <cstdint>
#include <string>
#include <vector>

namespace iiSharedCanvas {
struct Document; struct DepthAsset; struct DepthLayer;
// Values are linear normalized proximity, not metric distance: 0 = empty space,
// 1 = camera contact. Intermediate positive values denote occupied space.
struct DepthControlMapResult {
    RasterLayer pixels; // Opaque RGB8, nearest integer(value * 255); black remains opaque.
    std::vector<double> values; // Exact authored values, row-major, no auto-normalization.
    std::vector<std::uint8_t> occupiedPixels; // value > 0, including values rounding to black.
    std::string message;
    [[nodiscard]] bool ok() const noexcept { return message.empty(); }
};
IISHAREDCANVAS_EXPORT DepthAsset *findDepthAsset(Document &, const std::string &) noexcept;
IISHAREDCANVAS_EXPORT const DepthAsset *findDepthAsset(const Document &, const std::string &) noexcept;
IISHAREDCANVAS_EXPORT DepthLayer *findDepthLayer(Document &, const std::string &) noexcept;
IISHAREDCANVAS_EXPORT const DepthLayer *findDepthLayer(const Document &, const std::string &) noexcept;
IISHAREDCANVAS_EXPORT std::string validateDepthAsset(const DepthAsset &);
// Throws invalid_argument for invalid data, length_error before exceeding maxPixels.
IISHAREDCANVAS_EXPORT RasterLayer depthRasterPreview(const DepthAsset &,
    std::uint64_t maxPixels = 16ULL * 1024ULL * 1024ULL);
// Native asset coordinates. Ignores display visibility/opacity/transform, honors
// control.enabled and source frame range. Bounds all dense outputs before allocation.
IISHAREDCANVAS_EXPORT DepthControlMapResult renderDepthControlMap(const Document &,
    const std::string &layerId, std::uint32_t frame,
    std::uint64_t maxPixels = 16ULL * 1024ULL * 1024ULL);
} // namespace iiSharedCanvas
