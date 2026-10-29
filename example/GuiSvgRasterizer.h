#pragma once

#include <Media/MediaIo.h>

#include <span>
#include <string>

namespace iiSharedCanvas::example {

struct GuiSvgRasterOptions {
    CanvasExtent outputExtent;
    std::string assetId = "vector.raster";
    MediaLimits limits;
};

// Application-owned composition of iiXml, iiCompression and iiGUI.
// This explicitly creates a bitmap rather than editable vector paths.
[[nodiscard]] BitmapImportResult rasterizeGuiSvg(
    std::span<const std::uint8_t> bytes, const GuiSvgRasterOptions &options);

} // namespace iiSharedCanvas::example
