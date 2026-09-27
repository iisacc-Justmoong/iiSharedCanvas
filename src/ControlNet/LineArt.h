#pragma once
#include "iiSharedCanvas/Export.h"
#include "ControlNet/ControlNet.h"
#include <Layer/RasterLayer.h>
#include <cstdint>
#include <string>
#include <vector>

namespace iiSharedCanvas {
struct Document; struct LineArtAsset; struct LineArtLayer;
// Linear ink coverage: 0 = white background, 1 = full black ink.
// Intermediate values preserve antialiased contours and soft line intensity.
struct LineArtControlMapResult {
    RasterLayer pixels; // Opaque RGB8, nearest integer((1 - coverage) * 255); opaque white background.
    std::vector<double> coverage; // Exact authored coverage, row-major, no auto-normalization.
    std::vector<std::uint8_t> inkPixels; // value > 0, including coverage rounding to white.
    std::string message;
    [[nodiscard]] bool ok() const noexcept { return message.empty(); }
};
IISHAREDCANVAS_EXPORT LineArtAsset *findLineArtAsset(Document &, const std::string &) noexcept;
IISHAREDCANVAS_EXPORT const LineArtAsset *findLineArtAsset(const Document &, const std::string &) noexcept;
IISHAREDCANVAS_EXPORT LineArtLayer *findLineArtLayer(Document &, const std::string &) noexcept;
IISHAREDCANVAS_EXPORT const LineArtLayer *findLineArtLayer(const Document &, const std::string &) noexcept;
IISHAREDCANVAS_EXPORT std::string validateLineArtAsset(const LineArtAsset &);
// Throws invalid_argument for invalid data, length_error before exceeding maxPixels.
IISHAREDCANVAS_EXPORT RasterLayer lineArtRasterPreview(const LineArtAsset &,
    std::uint64_t maxPixels = 16ULL * 1024ULL * 1024ULL);
// Native asset coordinates. Ignores display visibility/opacity/transform, honors
// control.enabled and source frame range. Bounds all dense outputs before allocation.
IISHAREDCANVAS_EXPORT LineArtControlMapResult renderLineArtControlMap(const Document &,
    const std::string &layerId, std::uint32_t frame,
    std::uint64_t maxPixels = 16ULL * 1024ULL * 1024ULL);
} // namespace iiSharedCanvas
