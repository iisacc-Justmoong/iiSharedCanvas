#pragma once
#include "iiSharedCanvas/Export.h"
#include "ControlNet/ControlNet.h"
#include <Layer/RasterLayer.h>
#include <cstdint>
#include <string>
#include <vector>

namespace iiSharedCanvas {
struct Document; struct MlsdAsset; struct MlsdLayer; struct VectorAsset;
struct MlsdSegment {
    std::string id;
    double x1=0, y1=0, x2=1, y2=1; // Normalized [0,1] endpoints, x right/y down.
    double confidence=1; // [0,1]; metadata/filter, not pixel brightness.
    bool enabled=true;
    friend bool operator==(const MlsdSegment &, const MlsdSegment &) = default;
};
struct MlsdRenderOptions {
    double minimumConfidence=0;
    std::uint64_t maximumPixels=16ULL*1024ULL*1024ULL;
    std::uint64_t maximumRasterSteps=64ULL*1024ULL*1024ULL;
};
struct MlsdControlMapResult {
    RasterLayer pixels; // Opaque black with one-pixel white line segments.
    std::vector<MlsdSegment> segments; // Enabled segments passing confidence filter, in source order.
    std::string message;
    [[nodiscard]] bool ok() const noexcept { return message.empty(); }
};
IISHAREDCANVAS_EXPORT MlsdAsset *findMlsdAsset(Document &, const std::string &) noexcept;
IISHAREDCANVAS_EXPORT const MlsdAsset *findMlsdAsset(const Document &, const std::string &) noexcept;
IISHAREDCANVAS_EXPORT MlsdLayer *findMlsdLayer(Document &, const std::string &) noexcept;
IISHAREDCANVAS_EXPORT const MlsdLayer *findMlsdLayer(const Document &, const std::string &) noexcept;
IISHAREDCANVAS_EXPORT std::string validateMlsdAsset(const MlsdAsset &);
// Direct preview functions throw invalid_argument for invalid data/options,
// or length_error for exceeded budgets. Endpoints map to round(coordinate*(extent-1)).
IISHAREDCANVAS_EXPORT RasterLayer mlsdRasterPreview(const MlsdAsset &, const MlsdRenderOptions & = {});
// Display preview uses vector strokes; antialiasing can differ from the binary control map.
IISHAREDCANVAS_EXPORT VectorAsset mlsdVectorPreview(const MlsdAsset &);
// Native coordinates; ignores display transforms/opacity/visibility, honors enabled and frame range.
IISHAREDCANVAS_EXPORT MlsdControlMapResult renderMlsdControlMap(const Document &, const std::string &layerId,
    std::uint32_t frame, const MlsdRenderOptions & = {});
} // namespace iiSharedCanvas
