#pragma once
#include "Bitmap/RasterProcessingTypes.h"
#include <Core/RasterBlendMode.h>
#include <Transform/Transform.h>
#include <optional>
#include <string>
#include <variant>
#include <vector>

namespace iiSharedCanvas {
enum class MaskChannel : unsigned char { Luminance, Alpha };
struct LayerMask {
    std::string assetId;
    bool enabled = true;
    bool inverted = false;
    bool linkedToLayer = true;
    MaskChannel channel = MaskChannel::Luminance;
    double density = 1;
    double featherRadius = 0; // Gaussian sigma in mask-asset pixels, [0,128].
    double outsideCoverage = 1;
    AffineTransform transform; // Layer-local when linked; artboard/document-local otherwise.
    IISHAREDCANVAS_EXPORT bool operator==(const LayerMask &other) const;
};
// Ordered non-destructive coverage operands. First enabled operand establishes
// coverage (Subtract starts from white); disabled operands are skipped.
enum class MaskCombineMode : unsigned char { Add, Subtract, Intersect, Replace };
struct LayerMaskEntry {
    std::string id;
    std::string name;
    LayerMask mask;
    MaskCombineMode combine = MaskCombineMode::Add;
    bool operator==(const LayerMaskEntry &) const = default;
};
struct LayerFilter {
    std::string name;
    bool enabled = true;
    double opacity = 1;
    RasterBlendMode blendMode = RasterBlendMode::SourceOver;
    std::variant<RasterEffect, ColorAdjustment> operation;
    bool operator==(const LayerFilter &) const = default;
};
enum class BlendIfChannel : unsigned char { Gray, Red, Green, Blue };
struct BlendIfRange {
    double black = 0, blackFade = 0, whiteFade = 1, white = 1;
    bool operator==(const BlendIfRange &) const = default;
};
struct BlendIf {
    BlendIfChannel channel = BlendIfChannel::Gray;
    BlendIfRange source;
    BlendIfRange backdrop;
    bool operator==(const BlendIf &) const = default;
};
struct LayerAppearance {
    double fillOpacity = 1;
    bool clippedToBelow = false;
    bool blendClippedAsGroup = true;
    std::optional<LayerMask> rasterMask;
    std::optional<LayerMask> vectorMask;
    std::optional<LayerMask> filterMask;
    std::vector<LayerFilter> filters; // First-to-last application order, at native asset resolution.
    std::optional<BlendIf> blendIf;
    std::vector<LayerMaskEntry> maskStack; // Multiplied with the legacy raster/vector masks.
    bool maskStackEnabled = true; // Bypass without losing editable operands.
    bool operator==(const LayerAppearance &) const = default;
};
} // namespace iiSharedCanvas
