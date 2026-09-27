#pragma once
#include "iiSharedCanvas/Export.h"
#include "ControlNet/ControlNet.h"
#include <Layer/RasterLayer.h>
#include <cstdint>
#include <string>
#include <vector>

namespace iiSharedCanvas {
struct Document; struct TileAsset; struct TileLayer;
// Spatially aligned RGB reference for local-context/detail conditioning.
struct TileColor {
    std::uint8_t red = 0, green = 0, blue = 0;
    bool operator==(const TileColor &) const = default;
};
// Native asset pixel coordinates; no implicit padding, wrapping or resizing.
struct TileRegion {
    std::int32_t x = 0, y = 0, width = 0, height = 0;
    bool operator==(const TileRegion &) const = default;
};
struct TileControlMapResult {
    RasterLayer pixels;
    std::vector<TileColor> colors;
    TileRegion region; // Exact source region represented by the output.
    std::string message;
    [[nodiscard]] bool ok() const noexcept { return message.empty(); }
};
IISHAREDCANVAS_EXPORT TileAsset *findTileAsset(Document &, const std::string &) noexcept;
IISHAREDCANVAS_EXPORT const TileAsset *findTileAsset(const Document &, const std::string &) noexcept;
IISHAREDCANVAS_EXPORT TileLayer *findTileLayer(Document &, const std::string &) noexcept;
IISHAREDCANVAS_EXPORT const TileLayer *findTileLayer(const Document &, const std::string &) noexcept;
IISHAREDCANVAS_EXPORT std::string validateTileAsset(const TileAsset &);
// Copies opaque RGB pixels without changing detail or spatial arrangement.
// Caller must composite alpha explicitly. Throws invalid_argument for malformed
// input and length_error before exceeding the output pixel budget.
IISHAREDCANVAS_EXPORT TileAsset makeTileAsset(std::string id, const RasterLayer &source,
    std::uint64_t maxPixels = 16ULL * 1024ULL * 1024ULL);
IISHAREDCANVAS_EXPORT RasterLayer tileRasterPreview(const TileAsset &,
    std::uint64_t maxPixels = 16ULL * 1024ULL * 1024ULL);
// Native asset grid; display transforms/opacity/visibility do not affect control.
// Honors control.enabled, frame range and source timeline.
IISHAREDCANVAS_EXPORT TileControlMapResult renderTileControlMap(const Document &,
    const std::string &layerId, std::uint32_t frame,
    std::uint64_t maxPixels = 16ULL * 1024ULL * 1024ULL);
// Crops only the requested region. Budget applies to the crop, not the whole
// source; rejects out-of-bounds/empty regions without clipping or repeating pixels.
IISHAREDCANVAS_EXPORT TileControlMapResult renderTileControlRegion(const Document &,
    const std::string &layerId, std::uint32_t frame, TileRegion region,
    std::uint64_t maxPixels = 16ULL * 1024ULL * 1024ULL);
} // namespace iiSharedCanvas
