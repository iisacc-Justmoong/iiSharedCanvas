#pragma once
#include "iiSharedCanvas/Export.h"
#include "ControlNet/ControlNet.h"
#include <Layer/RasterLayer.h>
#include <cstdint>
#include <string>
#include <vector>

namespace iiSharedCanvas {
struct CanvasExtent;
struct Document; struct ShuffleAsset; struct ShuffleLayer;
// Prepared conditioning colors, not a geometric constraint or grayscale mask.
struct ShuffleColor {
    std::uint8_t red = 0, green = 0, blue = 0;
    bool operator==(const ShuffleColor &) const = default;
};
// Output-to-input coordinates: [0,1] maps to first/last source pixel centers.
struct ShuffleCoordinate {
    double u = 0, v = 0;
    bool operator==(const ShuffleCoordinate &) const = default;
};
struct ShuffleControlMapResult {
    RasterLayer pixels; // Exact opaque RGB8 conditioning pixels.
    std::vector<ShuffleColor> colors;
    std::string message;
    [[nodiscard]] bool ok() const noexcept { return message.empty(); }
};
IISHAREDCANVAS_EXPORT ShuffleAsset *findShuffleAsset(Document &, const std::string &) noexcept;
IISHAREDCANVAS_EXPORT const ShuffleAsset *findShuffleAsset(const Document &, const std::string &) noexcept;
IISHAREDCANVAS_EXPORT ShuffleLayer *findShuffleLayer(Document &, const std::string &) noexcept;
IISHAREDCANVAS_EXPORT const ShuffleLayer *findShuffleLayer(const Document &, const std::string &) noexcept;
IISHAREDCANVAS_EXPORT std::string validateShuffleAsset(const ShuffleAsset &);
// Explicit remapping with bilinear RGB interpolation, nearest integer channels.
// Input must be opaque; caller chooses alpha compositing before this operation.
// Output coordinate count must equal positive width * height; no implicit randomness.
// Invalid data throws invalid_argument; output budget throws length_error.
IISHAREDCANVAS_EXPORT ShuffleAsset makeShuffleAsset(std::string id, const RasterLayer &source,
    CanvasExtent outputExtent, const std::vector<ShuffleCoordinate> &coordinates,
    std::uint64_t maxPixels = 16ULL * 1024ULL * 1024ULL);
IISHAREDCANVAS_EXPORT RasterLayer shuffleRasterPreview(const ShuffleAsset &,
    std::uint64_t maxPixels = 16ULL * 1024ULL * 1024ULL);
// Native asset coordinates, display transform/opacity/visibility independent.
// Honors control.enabled, source frame range and timeline. Does not reshuffle.
IISHAREDCANVAS_EXPORT ShuffleControlMapResult renderShuffleControlMap(const Document &,
    const std::string &layerId, std::uint32_t frame,
    std::uint64_t maxPixels = 16ULL * 1024ULL * 1024ULL);
} // namespace iiSharedCanvas
