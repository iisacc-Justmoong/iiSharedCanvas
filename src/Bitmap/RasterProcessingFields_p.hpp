#pragma once
namespace iiSharedCanvas::detail {
// Keep persistence field order explicit and independent of compiler object layout.
template<class T, class Visitor> void visitColorAdjustmentFields(T &v, Visitor f)
{
    f(v.exposure); f(v.offset); f(v.contrast); f(v.pivot); f(v.highlights); f(v.highlightRange);
    f(v.rolloff); f(v.shadows); f(v.shadowRange); f(v.blackProtection); f(v.whites); f(v.whiteClip);
    f(v.blacks); f(v.blackClip); f(v.temperature); f(v.tint); f(v.vibrance); f(v.lowSaturationBias);
    f(v.saturation); f(v.channelSaturation); f(v.skinProtection); f(v.gradeHue); f(v.gradeBalance);
    f(v.saturationChannel); f(v.gradeRange); f(v.curveChannel); f(v.curve);
    f(v.protectHighlights); f(v.preserveColor); f(v.linearLight); f(v.adaptive); f(v.liftBlacks);
    f(v.gamutLimit); f(v.colorize); f(v.smoothCurve);
}
template<class T, class Visitor> void visitRasterEffectFields(T &v, Visitor f)
{
    f(v.kind); f(v.amount); f(v.radius); f(v.angle); f(v.scale); f(v.detail); f(v.threshold);
    f(v.midpoint); f(v.roundness); f(v.feather); f(v.monochromatic); f(v.edgeAware); f(v.natural); f(v.seed);
}
}
