#pragma once

#include "Media/MediaIo.h"

#include <span>

namespace iiSharedCanvas {

struct BitmapImportOptions {
    std::string assetId = "bitmap";
    std::string format; // Empty: content detection; useful for old TGA/other weak signatures.
    std::uint32_t imageIndex = 0;
    bool applyOrientation = true;
    bool extendedCodecs = true;
    MediaBackendOptions backend;
    MediaLimits limits;
};

struct BitmapExportOptions {
    std::string format = "png";
    int quality = -1;
    std::uint32_t matteArgb = 0xffffffffU;
    std::vector<MediaTextEntry> text;
    bool overwrite = false;
    bool extendedCodecs = true;
    MediaBackendOptions backend;
    MediaLimits limits;
};

// Actual Qt plugins plus installed extended codecs; excludes vector rasterizers.
IISHAREDCANVAS_EXPORT std::vector<MediaFormatCapability> bitmapFormats(
    const MediaBackendOptions &backend = {}, bool includeExtended = true);
IISHAREDCANVAS_EXPORT BitmapImportResult decodeBitmap(
    std::span<const std::uint8_t> bytes, const BitmapImportOptions &options = {});
IISHAREDCANVAS_EXPORT BitmapImportResult importBitmap(
    const std::string &path, const BitmapImportOptions &options = {});
IISHAREDCANVAS_EXPORT MediaBytesResult encodeBitmap(
    const RasterLayer &pixels, const BitmapExportOptions &options = {});
IISHAREDCANVAS_EXPORT MediaIoResult exportBitmap(
    const RasterLayer &pixels, const std::string &path, const BitmapExportOptions &options = {});
IISHAREDCANVAS_EXPORT MediaIoResult exportBitmapFrame(
    const Document &document, FrameIndex frame, const std::string &path,
    const BitmapExportOptions &options = {});

} // namespace iiSharedCanvas
