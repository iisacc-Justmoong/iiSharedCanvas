#pragma once

#include "iiSharedCanvas/Export.h"
#include <cstdint>
#include <string>
#include <vector>

namespace iiSharedCanvas {

// Effect families are deliberately independent of a particular editor UI.
// Shadow and Glow use an exterior effect; their Inner variants stay in shape.
enum class LayerStyleKind : std::uint8_t { Shadow, InnerShadow, Glow, InnerGlow, Stroke, Overlay, Bevel };

// Distances and offsets are expressed in document pixels. Color is straight
// ARGB and opacity multiplies its alpha; radius controls blur, stroke or bevel.
struct LayerStyle {
    LayerStyleKind kind = LayerStyleKind::Shadow;
    bool enabled = true;
    std::uint32_t color = 0xff000000U;
    double opacity = 1;
    double radius = 1;
    double offsetX = 0;
    double offsetY = 0;
    bool operator==(const LayerStyle &) const = default;
};

// Value-owned order is retained through serialization and document history.
struct LayerStyleStack {
    std::vector<LayerStyle> effects;
    bool operator==(const LayerStyleStack &) const = default;
};

// Shared model and evaluator checks; an empty result means the stack is valid.
IISHAREDCANVAS_EXPORT std::string validateLayerStyleStack(const LayerStyleStack &);

} // namespace iiSharedCanvas
