#pragma once

#include "Document/Document.h"
#include "iiSharedCanvas/Export.h"

#include <span>
#include <string>

namespace iiSharedCanvas {

// Sequential difference subtracts each later operand from the first.
enum class ShapeBooleanOperation : std::uint8_t { Union, Subtract, Intersect, ExclusiveOr };
// A read-only borrowed source and an unevaluated source-to-result transform.
// The caller keeps each asset alive and unchanged until planning returns.
struct ShapeOperand { const VectorAsset *asset=nullptr; AffineTransform transform; };
// A newly owned vector asset or a reason no safe expression could be created.
struct ShapeCombinationResult {
    VectorAsset asset;
    std::string error;
    [[nodiscard]] bool ok() const noexcept { return error.empty(); }
};

// Check the persistent expression's index, tree, geometry and work bounds.
// An empty result means the vector has either no expression or a valid one.
IISHAREDCANVAS_EXPORT std::string validateShapeExpression(const VectorAsset &asset);

// Builds an exact, editable Boolean expression without computing intersections.
// Input Bezier controls and transform coefficients are copied unchanged. This
// stateless planner neither renders nor writes a document or filesystem.
class IISHAREDCANVAS_EXPORT ShapeCombinationPlan final {
public:
    // Each operand supplies closed, filled paths. Strokes must be handled by
    // the caller because a stroked outline has different geometry. A failed
    // plan returns no partial asset and never changes an operand.
    [[nodiscard]] ShapeCombinationResult operator()(std::span<const ShapeOperand> operands,
        ShapeBooleanOperation operation, std::string assetId, CanvasExtent viewport,
        std::uint32_t color) const;
};

} // namespace iiSharedCanvas
