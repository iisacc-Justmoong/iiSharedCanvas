#pragma once
#include <Brush/BrushResolve.h>
#include <Brush/BrushState.h>
namespace iiSharedCanvas::detail {
inline std::vector<std::string> validateEngineState(const BrushState &s) {
    BrushPreset p; p.dynamics = s.dynamics; p.material = s.material;
    p.shape = s.rasterizer.shape; p.color = s.rasterizer.color; p.tipSequence = s.rasterizer.tipSequence;
    p.tip.width = s.rasterizer.brushWidth; p.tip.height = s.rasterizer.brushHeight;
    for (auto alpha : s.rasterizer.brushAlpha) p.tip.mask.push_back(static_cast<std::byte>(alpha));
    p.stroke.blendMode = s.rasterizer.blendMode;
    p.stroke.spacing = s.rasterizer.spacing; p.stroke.spacingRatio = s.rasterizer.spacingRatio;
    p.stroke.warmupDistance = s.rasterizer.warmupDistance; p.stroke.taperMinimum = s.rasterizer.taperMinimum;
    p.stroke.warmupTaperShape = s.rasterizer.warmupTaperShape; p.stroke.airbrushRate = s.rasterizer.airbrushRate;
    return validateBrushPreset(p);
}
}
