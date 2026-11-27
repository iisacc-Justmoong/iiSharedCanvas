#include "Document/LayerStyleStack.h"

#include <cmath>

namespace iiSharedCanvas {
// The model owns its validation independently from any renderer. Every
// serialized or evaluated style uses the same bounded, finite parameters.
std::string validateLayerStyleStack(const LayerStyleStack &stack)
{
    if (stack.effects.size()>64) return "a layer cannot contain more than 64 styles";
    for (const auto &effect:stack.effects) {
        if (static_cast<unsigned>(effect.kind)>static_cast<unsigned>(LayerStyleKind::Bevel) ||
            !std::isfinite(effect.opacity) || effect.opacity<0 || effect.opacity>1 ||
            !std::isfinite(effect.radius) || effect.radius<0 || effect.radius>32 ||
            !std::isfinite(effect.offsetX) || std::abs(effect.offsetX)>256 ||
            !std::isfinite(effect.offsetY) || std::abs(effect.offsetY)>256)
            return "style kind, opacity, radius or offset is out of range";
    }
    return {};
}
} // namespace iiSharedCanvas
