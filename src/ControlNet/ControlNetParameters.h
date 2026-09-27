#pragma once
#include "Document/Document.h"
#include "iiSharedCanvas/Export.h"
#include <optional>
#include <string>
#include <vector>

namespace iiSharedCanvas {
// Null for artwork. These accessors also work on detached editable layer copies.
IISHAREDCANVAS_EXPORT ControlNetSettings *controlNetSettings(Layer &) noexcept;
IISHAREDCANVAS_EXPORT const ControlNetSettings *controlNetSettings(const Layer &) noexcept;

// Engaged false, zero and empty strings are explicit edits; disengaged fields stay unchanged.
struct ControlNetSettingsPatch {
    std::optional<bool> enabled;
    std::optional<std::string> modelId;
    std::optional<std::string> modelRevision;
    std::optional<double> conditioningScale;
    std::optional<double> guidanceStart;
    std::optional<double> guidanceEnd;
};

// Detached typed parameters for one layer and ALL unique source assets (including
// every dynamic state). Edit concrete types with std::get<T>/std::get_if<T>.
// Identity, type and source topology are fixed; use structural editor APIs for those.
struct ControlNetParameters {
    Layer layer;
    std::vector<Asset> assets; // First occurrence in source/keyframe order, no duplicate ids.
};
struct ControlNetParametersResult {
    std::optional<ControlNetParameters> parameters;
    std::string message;
    [[nodiscard]] bool ok() const noexcept { return parameters.has_value() && message.empty(); }
};
// Includes disabled/hidden states and assets outside the display frame range.
// Validates the document; rejects artwork, unknown ids and invalid source references.
IISHAREDCANVAS_EXPORT ControlNetParametersResult getControlNetParameters(
    const Document &, const std::string &layerId);
} // namespace iiSharedCanvas
