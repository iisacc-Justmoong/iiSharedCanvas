#pragma once
#include "Document/Document.h"
#include "Bitmap/RasterProcessingTypes.h"
#include "iiSharedCanvas/Export.h"
#include <string>

namespace iiSharedCanvas {
struct RasterProcessResult {
    RasterLayer pixels;
    std::string error;
    [[nodiscard]] bool ok() const noexcept { return error.empty(); }
};
struct RasterMask { CanvasExtent extent; std::vector<std::uint8_t> alpha; };
struct RasterMaskResult {
    RasterMask mask;
    std::string error;
    [[nodiscard]] bool ok() const noexcept { return error.empty(); }
};
enum class MaskOperation { Replace, Add, Subtract, Intersect };
enum class RasterFillKind { Solid, Linear, Radial, Angular, Pattern };
struct RasterFill {
    RasterFillKind kind = RasterFillKind::Solid;
    std::uint32_t firstArgb = 0xff000000, secondArgb = 0xffffffff;
    double angle = 0, scale = 16, opacity = 1;
    bool dither = false, seamless = true;
};
// Detached standard-C++ pixel operations. No document writes, UI, model inference or Qt dependency.
IISHAREDCANVAS_EXPORT RasterProcessResult adjustRaster(const RasterLayer &, const ColorAdjustment &);
IISHAREDCANVAS_EXPORT RasterProcessResult effectRaster(const RasterLayer &, const RasterEffect &);
IISHAREDCANVAS_EXPORT RasterProcessResult fillRaster(CanvasExtent, const RasterFill &);
IISHAREDCANVAS_EXPORT RasterProcessResult maskRaster(const RasterLayer &, const RasterMask &);
IISHAREDCANVAS_EXPORT RasterProcessResult blendRaster(const RasterLayer &, const RasterLayer &, const RasterMask &);
IISHAREDCANVAS_EXPORT RasterMaskResult featherMask(const RasterMask &, double radius);
IISHAREDCANVAS_EXPORT RasterMaskResult combineMasks(const RasterMask &, const RasterMask &, MaskOperation);
IISHAREDCANVAS_EXPORT RasterMaskResult floodMask(const RasterLayer &, CanvasOrigin seed, double tolerance, bool contiguous);
} // namespace iiSharedCanvas
