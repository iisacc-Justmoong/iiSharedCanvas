#pragma once

#include "Document/Document.h"
#include "iiSharedCanvas/Export.h"

#include <span>
#include <string>
#include <vector>

namespace iiSharedCanvas {

// World-space axis-aligned bounds supplied by the caller. Width and height
// may be zero, but all coordinates and edges must be finite.
struct LayerBounds { double x=0, y=0, width=0, height=0; };
// A stable layer identifier and its externally computed world-space bounds.
struct LayerGeometry { std::string id; LayerBounds bounds; };
// A detached world-space displacement. The caller controls its application.
struct LayerOffset { std::string id; double x=0, y=0; };
// Immutable-by-convention calculation result; error is empty on success.
struct LayerLayoutResult {
    std::vector<LayerOffset> offsets;
    std::string error;
    [[nodiscard]] bool ok() const noexcept { return error.empty(); }
};

enum class LayerArrangement { Forward, Backward, Front, Back };
enum class LayerAlignment { Left, Center, Right, Top, Middle, Bottom };
enum class LayerAxis { Horizontal, Vertical };
enum class LayerDistribution { Centers, Gaps };

// Detached bottom-to-top order. It neither owns nor mutates the document.
struct LayerArrangementResult {
    std::vector<std::string> order;
    std::string error;
    [[nodiscard]] bool ok() const noexcept { return error.empty(); }
};

// Computes a stable layer-order plan from borrowed document state. Each direct
// group is arranged independently and nested groups move as indivisible units.
// The caller keeps the input unchanged for the call and owns publication.
// Results that violate group, clipping or structural-lock contracts are errors.
class IISHAREDCANVAS_EXPORT LayerArrangementPlanner final {
public:
    [[nodiscard]] LayerArrangementResult operator()(const Document &document,
        std::span<const std::string> selected, LayerArrangement action) const;
};

// Calculates displacements relative to a caller-supplied reference rectangle.
// It has no document authority or retained state and may run concurrently on
// independently controlled inputs. Invalid values return an error and no plan.
class IISHAREDCANVAS_EXPORT LayerAlignmentSolver final {
public:
    [[nodiscard]] LayerLayoutResult operator()(std::span<const LayerGeometry> layers,
        LayerBounds reference, LayerAlignment alignment) const;
};

// Calculates equal center distances or equal empty gaps while fixing the two
// outer layers. Equal coordinates preserve input order; output follows input order.
class IISHAREDCANVAS_EXPORT LayerDistributionSolver final {
public:
    [[nodiscard]] LayerLayoutResult operator()(std::span<const LayerGeometry> layers,
        LayerAxis axis, LayerDistribution distribution) const;
};

} // namespace iiSharedCanvas
