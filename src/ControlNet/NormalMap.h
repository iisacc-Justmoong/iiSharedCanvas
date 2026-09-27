#pragma once
#include "iiSharedCanvas/Export.h"
#include "ControlNet/ControlNet.h"
#include <Layer/RasterLayer.h>
#include <cstdint>
#include <string>
#include <vector>

namespace iiSharedCanvas {
struct Document; struct NormalMapAsset; struct NormalMapLayer;
// Canvas camera basis: +X right, +Y down, +Z toward the viewer.
// Valid normals have finite components in [-1,1] and |length - 1| <= 1e-6.
// Missing samples must be {0,0,0,false}; no implicit normalization or repair.
struct NormalMapSample {
    double x = 0.0;
    double y = 0.0;
    double z = 1.0;
    bool valid = true;
    bool operator==(const NormalMapSample &) const = default;
};
enum class NormalMapChannelOrder : std::uint8_t { Xyz, Zyx };
struct NormalMapRenderOptions {
    NormalMapChannelOrder channelOrder = NormalMapChannelOrder::Xyz;
    bool flipY = false;
    std::uint64_t maximumPixels = 16ULL * 1024ULL * 1024ULL;
};
struct NormalMapControlMapResult {
    RasterLayer pixels; // Opaque RGB8: round((component + 1) * 127.5), missing = black.
    std::vector<NormalMapSample> samples; // Exact authored XYZ, independent of output options.
    std::vector<std::uint8_t> validPixels; // 0 = missing, 1 = authored unit normal.
    std::string message;
    [[nodiscard]] bool ok() const noexcept { return message.empty(); }
};
IISHAREDCANVAS_EXPORT NormalMapAsset *findNormalMapAsset(Document &, const std::string &) noexcept;
IISHAREDCANVAS_EXPORT const NormalMapAsset *findNormalMapAsset(const Document &, const std::string &) noexcept;
IISHAREDCANVAS_EXPORT NormalMapLayer *findNormalMapLayer(Document &, const std::string &) noexcept;
IISHAREDCANVAS_EXPORT const NormalMapLayer *findNormalMapLayer(const Document &, const std::string &) noexcept;
IISHAREDCANVAS_EXPORT std::string validateNormalMapAsset(const NormalMapAsset &);
// Throws invalid_argument for invalid data/options, length_error for output budget.
IISHAREDCANVAS_EXPORT RasterLayer normalMapRasterPreview(const NormalMapAsset &,
    NormalMapRenderOptions options = {});
// Native sample grid; ignores display transform/opacity/visibility, honors source
// frame range and control.enabled. No implicit spatial normal rotation or resampling.
IISHAREDCANVAS_EXPORT NormalMapControlMapResult renderNormalMapControlMap(const Document &,
    const std::string &layerId, std::uint32_t frame, NormalMapRenderOptions options = {});
} // namespace iiSharedCanvas
