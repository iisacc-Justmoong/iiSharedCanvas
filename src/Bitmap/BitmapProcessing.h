#pragma once
#include "Document/Document.h"
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
struct ColorAdjustment {
    double exposure = 0, offset = 0, contrast = 0, pivot = 0.5;
    double highlights = 0, highlightRange = 0.7, rolloff = 0;
    double shadows = 0, shadowRange = 0.4, blackProtection = 0;
    double whites = 0, whiteClip = 1, blacks = 0, blackClip = 0;
    double temperature = 0, tint = 0, vibrance = 0, lowSaturationBias = 1;
    double saturation = 0, channelSaturation = 0, skinProtection = 0;
    double gradeHue = 0, gradeBalance = 0;
    int saturationChannel = -1, gradeRange = 1, curveChannel = -1;
    double curve = 0;
    bool protectHighlights = false, preserveColor = true, linearLight = false;
    bool adaptive = false, liftBlacks = false, gamutLimit = true, colorize = false;
    bool smoothCurve = true;
};
enum class RasterEffectKind { GaussianBlur, MotionBlur, LensBlur, Texture, Clarity, Dehaze,
    Vignette, Grain, Sharpen, Noise, LuminanceDenoise, ColorDenoise, LensDistortion, ChromaticAberration };
struct RasterEffect {
    RasterEffectKind kind = RasterEffectKind::GaussianBlur;
    double amount = 1, radius = 1, angle = 0, scale = 1, detail = 0.5;
    double threshold = 0, midpoint = 0.5, roundness = 1, feather = 0.5;
    bool monochromatic = true, edgeAware = false, natural = false;
    std::uint32_t seed = 1;
};
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
