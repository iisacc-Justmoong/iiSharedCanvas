#pragma once

#include <Media/MediaIo.h>

#include <span>
#include <string>

namespace iiSharedCanvas::example {

struct GuiSvgRasterOptions {
    CanvasExtent outputExtent;
    std::string assetId = "vector.raster";
    MediaLimits limits;
    // Opt in to bounded transform lists and elliptical arc commands. The
    // default remains the previously documented conservative SVG subset.
    bool extendedGeometry = false;
};

// Application-owned composition of iiXml, iiCompression and iiGUI.
// This explicitly creates a bitmap rather than editable vector paths.
[[nodiscard]] BitmapImportResult rasterizeGuiSvg(
    std::span<const std::uint8_t> bytes, const GuiSvgRasterOptions &options);

} // namespace iiSharedCanvas::example
