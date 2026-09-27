#pragma once
#include "iiSharedCanvas/Export.h"
#include "ControlNet/ControlNet.h"
#include <Layer/RasterLayer.h>
#include <cstdint>
#include <string>
#include <vector>

namespace iiSharedCanvas {
struct Document; struct ReferenceAsset; struct ReferenceLayer;
// Prepared conditioning colors, not a geometric constraint or grayscale mask.
struct ReferenceColor {
    std::uint8_t red = 0, green = 0, blue = 0;
    bool operator==(const ReferenceColor &) const = default;
};
// Adapter names: reference_only, reference_adain, reference_adain+attn.
enum class ReferenceMode : std::uint8_t { Attention, AdaIN, AttentionAdaIN };
struct ReferenceSettings {
    ReferenceMode mode = ReferenceMode::Attention;
    double styleFidelity = 0.5;
    bool operator==(const ReferenceSettings &) const = default;
};
struct ReferenceControlMapResult {
    RasterLayer pixels; // Exact opaque RGB8 conditioning pixels.
    std::vector<ReferenceColor> colors;
    ReferenceSettings reference;
    ControlNetSettings control;
    std::string message;
    [[nodiscard]] bool ok() const noexcept { return message.empty(); }
};
IISHAREDCANVAS_EXPORT ReferenceAsset *findReferenceAsset(Document &, const std::string &) noexcept;
IISHAREDCANVAS_EXPORT const ReferenceAsset *findReferenceAsset(const Document &, const std::string &) noexcept;
IISHAREDCANVAS_EXPORT ReferenceLayer *findReferenceLayer(Document &, const std::string &) noexcept;
IISHAREDCANVAS_EXPORT const ReferenceLayer *findReferenceLayer(const Document &, const std::string &) noexcept;
IISHAREDCANVAS_EXPORT std::string validateReferenceAsset(const ReferenceAsset &);
IISHAREDCANVAS_EXPORT std::string validateReferenceSettings(const ReferenceSettings &);
// Copies opaque RGB pixels; caller composites alpha explicitly. No embedding,
// style extraction, resizing or inference is performed. Invalid data throws
// invalid_argument; output budget throws length_error before allocation.
IISHAREDCANVAS_EXPORT ReferenceAsset makeReferenceAsset(std::string id, const RasterLayer &source,
    std::uint64_t maxPixels = 16ULL * 1024ULL * 1024ULL);
IISHAREDCANVAS_EXPORT RasterLayer referenceRasterPreview(const ReferenceAsset &,
    std::uint64_t maxPixels = 16ULL * 1024ULL * 1024ULL);
// Native asset coordinates, display transform/opacity/visibility independent.
// Honors control.enabled, source frame range and timeline. Does not modify the reference image.
IISHAREDCANVAS_EXPORT ReferenceControlMapResult renderReferenceControlMap(const Document &,
    const std::string &layerId, std::uint32_t frame,
    std::uint64_t maxPixels = 16ULL * 1024ULL * 1024ULL);
} // namespace iiSharedCanvas
