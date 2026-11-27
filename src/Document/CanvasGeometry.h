#pragma once

#include "Document/Document.h"
#include "iiSharedCanvas/Export.h"

#include <cstdint>
#include <string>

namespace iiSharedCanvas {

// Anchor selects the retained position of existing content in a finite resize.
// Centering an odd difference assigns the extra pixel to right/bottom.
enum class CanvasAnchor : std::uint8_t {
    TopLeft, Top, TopRight, Left, Center, Right, BottomLeft, Bottom, BottomRight
};

// Detached geometry only. Source assets and frames are never held or modified.
// A plan is valid only for the recorded source region, mode and revision.
struct CanvasGeometryPlan {
    std::uint64_t sourceRevision=0;
    CanvasMode mode=CanvasMode::Finite;
    CanvasRegion source{},target{};
    CanvasOrigin contentOffset{};
};
struct CanvasGeometryResult {
    CanvasGeometryPlan plan;
    std::string error;
    [[nodiscard]] bool ok() const noexcept { return error.empty(); }
};

// PreserveSource retains every owned raster and samples it during rendering.
// ReplacePixels requests new owned pixels; it may fail if a source cannot be
// transformed without changing sharing or animation semantics.
enum class RasterResamplingMode : std::uint8_t { PreserveSource, ReplacePixels };
struct CanvasAffinePlan {
    std::uint64_t sourceRevision=0;
    CanvasMode mode=CanvasMode::Finite;
    CanvasRegion source{},target{};
    AffineTransform transform;
    RasterResamplingMode rasterMode=RasterResamplingMode::PreserveSource;
};
struct CanvasAffineResult {
    CanvasAffinePlan plan;
    std::string error;
    [[nodiscard]] bool ok() const noexcept { return error.empty(); }
};

// Plan scaling of the full document without mutating the document or its assets.
// The caller owns publication, memory limits and history execution.
class IISHAREDCANVAS_EXPORT DocumentResamplingPlan final {
public:
    [[nodiscard]] CanvasAffineResult operator()(const Document &document,CanvasExtent extent,
        RasterResamplingMode mode=RasterResamplingMode::PreserveSource) const;
};

// Plan a clockwise image rotation in degrees about the current canvas center.
// The target canvas expands to contain the transformed original canvas.
class IISHAREDCANVAS_EXPORT ImageRotationPlan final {
public:
    [[nodiscard]] CanvasAffineResult operator()(const Document &document,double degrees) const;
};

// Computes a resize without modifying source pixels. The caller controls
// publication, document lifetime and ordering with other edits.
class IISHAREDCANVAS_EXPORT CanvasResizePlan final {
public:
    // Finite documents shift content according to anchor. Infinite documents
    // change only the viewport and keep content in its world coordinates.
    [[nodiscard]] CanvasGeometryResult operator()(const Document &document,
        CanvasExtent extent,CanvasAnchor anchor=CanvasAnchor::Center) const;
};

// Computes a non-destructive crop; outside source assets are retained. The
// caller commits the result as a single document-history operation.
class IISHAREDCANVAS_EXPORT CanvasCropTransaction final {
public:
    // Region is in current document coordinates and must fit the current
    // finite canvas or infinite viewport. Empty and overflowing areas fail.
    [[nodiscard]] CanvasGeometryResult operator()(const Document &document,
        CanvasRegion region) const;
};

} // namespace iiSharedCanvas
