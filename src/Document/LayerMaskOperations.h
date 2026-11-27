#pragma once

#include "Document/Document.h"
#include "iiSharedCanvas/Export.h"

#include <optional>
#include <string>
#include <string_view>

namespace iiSharedCanvas {

// An independently owned appearance, or an explanation why it could not be
// prepared. Publishing this value is the caller's explicit responsibility.
struct LayerMaskOperationResult {
    LayerAppearance appearance;
    std::string error;
    [[nodiscard]] bool ok() const noexcept { return error.empty(); }
};

// Prepares the raster-mask slot without changing the supplied document.
class IISHAREDCANVAS_EXPORT LayerMaskOperations final {
public:
    // An empty mask removes the slot. The replacement asset and all mask
    // parameters are validated before a new appearance is returned.
    [[nodiscard]] LayerMaskOperationResult operator()(const Document &document,
        std::string_view layerId, std::optional<LayerMask> mask) const;
};

// Prepares the vector-mask slot while preserving the vector asset's geometry.
class IISHAREDCANVAS_EXPORT VectorMaskOperations final {
public:
    // An empty mask removes the slot. A present mask must reference a vector
    // asset already owned by the supplied document.
    [[nodiscard]] LayerMaskOperationResult operator()(const Document &document,
        std::string_view layerId, std::optional<LayerMask> mask) const;
};

} // namespace iiSharedCanvas
