#pragma once
#include <cstdint>
#include <string>
#include "iiSharedCanvas/Export.h"

namespace iiSharedCanvas {
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
    bool operator==(const ColorAdjustment &) const = default;
};
enum class RasterEffectKind { GaussianBlur, MotionBlur, LensBlur, Texture, Clarity, Dehaze,
    Vignette, Grain, Sharpen, Noise, LuminanceDenoise, ColorDenoise, LensDistortion,
    ChromaticAberration, Average, Solarize, Invert, Grayscale };
struct RasterEffect {
    RasterEffectKind kind = RasterEffectKind::GaussianBlur;
    double amount = 1, radius = 1, angle = 0, scale = 1, detail = 0.5;
    double threshold = 0, midpoint = 0.5, roundness = 1, feather = 0.5;
    bool monochromatic = true, edgeAware = false, natural = false;
    std::uint32_t seed = 1;
    bool operator==(const RasterEffect &) const = default;
};
IISHAREDCANVAS_EXPORT std::string validateColorAdjustment(const ColorAdjustment &);
IISHAREDCANVAS_EXPORT std::string validateRasterEffect(const RasterEffect &);
} // namespace iiSharedCanvas
