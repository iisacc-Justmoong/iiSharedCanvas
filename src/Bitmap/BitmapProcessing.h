#pragma once
#include "Document/Document.h"
#include "Bitmap/RasterProcessingTypes.h"
#include "iiSharedCanvas/Export.h"
#include <span>
#include <vector>
#include <string>

namespace iiSharedCanvas {
struct RasterProcessResult {
    RasterLayer pixels;
    std::string error;
    [[nodiscard]] bool ok() const noexcept { return error.empty(); }
};
struct RasterMask { CanvasExtent extent; std::vector<std::uint8_t> alpha; };
struct RasterMaskResult {
    RasterMask mask;
    std::string error;
    [[nodiscard]] bool ok() const noexcept { return error.empty(); }
};
enum class MaskOperation { Replace, Add, Subtract, Intersect };
enum class SelectionGeometry { Rectangle, Ellipse };
// Document-local half-open bounds; reversed corners and out-of-canvas areas are supported.
IISHAREDCANVAS_EXPORT RasterMaskResult selectionGeometry(CanvasExtent, SelectionGeometry,
    Point first, Point second, bool antialias = true);
// A closed, even-odd path in document-local coordinates. The first point is
// joined to the last. Returns an error for invalid or excessive input.
IISHAREDCANVAS_EXPORT RasterMaskResult selectionPolygon(CanvasExtent,
    std::span<const Point> points, bool antialias = true);
struct MagneticPathOptions { double radius = 12; double minimumStrength = .05; std::size_t maxSearchPixels = 1048576; };
struct MagneticPathResult { std::vector<Point> points; bool attracted = false; std::string error; [[nodiscard]] bool ok() const noexcept { return error.empty(); } };
// Deterministic edge-following path in raster-local coordinates; detached from document state.
IISHAREDCANVAS_EXPORT MagneticPathResult magneticSelectionPath(const RasterLayer&, Point, Point, const MagneticPathOptions& = {});
enum class GradientKind { Linear, Radial };
enum class GradientSpread { Pad, Repeat };
struct GradientStop { double position = 0; std::uint32_t argb = 0xff000000; };
struct GradientDefinition {
    Point start{}, end{};
    GradientKind kind = GradientKind::Linear;
    GradientSpread spread = GradientSpread::Pad;
    std::vector<GradientStop> stops;
    double opacity = 1;
};
// Endpoint-driven detached raster gradient; uses premultiplied alpha interpolation.
IISHAREDCANVAS_EXPORT RasterProcessResult gradientRaster(CanvasExtent, const GradientDefinition&);
enum class RasterFillKind { Solid, Linear, Radial, Angular, Pattern };
struct RasterFill {
    RasterFillKind kind = RasterFillKind::Solid;
    std::uint32_t firstArgb = 0xff000000, secondArgb = 0xffffffff;
    double angle = 0, scale = 16, opacity = 1;
    bool dither = false, seamless = true;
};
// Detached standard-C++ pixel operations. No document writes, UI, model inference or Qt dependency.
IISHAREDCANVAS_EXPORT RasterProcessResult adjustRaster(const RasterLayer &, const ColorAdjustment &);
IISHAREDCANVAS_EXPORT RasterProcessResult effectRaster(const RasterLayer &, const RasterEffect &);
IISHAREDCANVAS_EXPORT RasterProcessResult fillRaster(CanvasExtent, const RasterFill &);
IISHAREDCANVAS_EXPORT RasterProcessResult maskRaster(const RasterLayer &, const RasterMask &);
IISHAREDCANVAS_EXPORT RasterProcessResult blendRaster(const RasterLayer &, const RasterLayer &, const RasterMask &);
// Reconstruct a binary pixel mask with deterministic 4x4 subpixel coverage.
// Pixel-aligned straight boundaries and document edges remain unchanged.
IISHAREDCANVAS_EXPORT RasterMaskResult antialiasBinaryMask(const RasterMask &);
IISHAREDCANVAS_EXPORT RasterMaskResult featherMask(const RasterMask &, double radius);
IISHAREDCANVAS_EXPORT RasterMaskResult combineMasks(const RasterMask &, const RasterMask &, MaskOperation);
IISHAREDCANVAS_EXPORT RasterMaskResult floodMask(const RasterLayer &, CanvasOrigin seed, double tolerance, bool contiguous);
// Select by premultiplied RGBA distance; transparent RGB is ignored.
IISHAREDCANVAS_EXPORT RasterMaskResult selectionWand(const RasterLayer &, CanvasOrigin seed, double tolerance, bool contiguous);
} // namespace iiSharedCanvas
