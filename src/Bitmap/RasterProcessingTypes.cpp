#include "Bitmap/RasterProcessingTypes.h"
#include <algorithm>
#include <array>
#include <cmath>

namespace iiSharedCanvas {
std::string validateColorAdjustment(const ColorAdjustment &a) {
    const std::array params{a.exposure, a.offset, a.contrast, a.pivot, a.highlights, a.highlightRange,
        a.rolloff, a.shadows, a.shadowRange, a.blackProtection, a.whites, a.whiteClip, a.blacks,
        a.blackClip, a.temperature, a.tint, a.vibrance, a.lowSaturationBias, a.saturation,
        a.channelSaturation, a.skinProtection, a.gradeHue, a.gradeBalance, a.curve};
    if (std::ranges::any_of(params, [](double v) { return !std::isfinite(v) || std::abs(v) > 100; }))
        return "color parameters must be finite and bounded";
    if (a.saturationChannel < -1 || a.saturationChannel > 2 || a.curveChannel < -1
        || a.curveChannel > 2 || a.gradeRange < 0 || a.gradeRange > 2)
        return "color channel and grading range must be supported";
    return {};
}

std::string validateRasterEffect(const RasterEffect &e) {
    if (static_cast<unsigned>(e.kind) > static_cast<unsigned>(RasterEffectKind::Grayscale))
        return "unknown raster effect kind";
    for (double p : {e.amount, e.radius, e.angle, e.scale, e.detail, e.threshold, e.midpoint, e.roundness, e.feather})
        if (!std::isfinite(p) || std::abs(p) > 1e9) return "effect parameters must be finite and bounded";
    if (e.radius < 0 || e.radius > 256 || std::abs(e.amount) > 16 || e.scale < 1.0/65536 || e.scale > 65536)
        return "effect parameters exceed processing limits";
    return {};
}

} // namespace iiSharedCanvas
