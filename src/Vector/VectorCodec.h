#pragma once

#include "Media/MediaIo.h"

#include <optional>
#include <span>

namespace iiSharedCanvas {

struct VectorImportOptions {
    std::string assetId = "vector";
    MediaLimits limits;
};

struct VectorImportResult {
    VectorAsset asset;
    MediaIoResult result;
    [[nodiscard]] bool ok() const noexcept { return result.ok(); }
};

struct VectorExportOptions {
    bool compressed = false; // gzip SVGZ, not a proprietary wrapper.
    bool overwrite = false;
    MediaLimits limits;
};

struct PdfExportOptions {
    FrameIndex firstFrame = 0;
    std::optional<FrameIndex> lastFrame; // Inclusive; absent means one page.
    bool rasterizeUnsupportedBlending = false;
    bool overwrite = false;
    MediaLimits limits;
};

struct RasterizedVectorImportOptions {
    CanvasExtent outputExtent; // Explicit raster dimensions, never inferred DPI.
    std::string assetId = "vector.raster";
    std::uint32_t page = 0;
    MediaLimits limits;
};

IISHAREDCANVAS_EXPORT VectorImportResult decodeSvg(
    std::span<const std::uint8_t> bytes, const VectorImportOptions &options = {});
IISHAREDCANVAS_EXPORT VectorImportResult importSvg(
    const std::string &path, const VectorImportOptions &options = {});
IISHAREDCANVAS_EXPORT MediaBytesResult encodeSvg(
    const VectorAsset &asset, const VectorExportOptions &options = {});
IISHAREDCANVAS_EXPORT MediaIoResult exportSvg(
    const VectorAsset &asset, const std::string &path, const VectorExportOptions &options = {});
IISHAREDCANVAS_EXPORT MediaIoResult exportPdf(
    const Document &document, const std::string &path, const PdfExportOptions &options = {});
// Explicitly creates bitmap content. Requires the corresponding Qt image plugin.
IISHAREDCANVAS_EXPORT BitmapImportResult rasterizeVectorFile(
    const std::string &path, const RasterizedVectorImportOptions &options);

} // namespace iiSharedCanvas
