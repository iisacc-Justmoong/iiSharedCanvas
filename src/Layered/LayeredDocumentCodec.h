#pragma once

#include "Media/MediaIo.h"

#include <span>

namespace iiSharedCanvas {

struct LayeredDocumentImportOptions {
    // Deterministic ids: <prefix>-asset-<index> and <prefix>-layer-<index>.
    // Indices begin at zero in the resulting bottom-to-top layer order.
    std::string idPrefix = "import";
    MediaLimits limits;
    std::uint32_t maxLayers = 4096;
    std::uint32_t maxArchiveEntries = 16384;
};

struct LayeredDocumentImportResult {
    Document document;
    std::string format; // "ora" or "psd" when identified from content.
    MediaIoResult result;
    [[nodiscard]] bool ok() const noexcept { return result.ok(); }
};

struct PsdExportOptions {
    bool overwrite = false; // File export only; never replaces a working .iisc file.
    MediaLimits limits;
    std::uint32_t maxLayers = 4096;
};

// Layer-preserving readers, not composite-image fallbacks. See MEDIA_IO.md
// for the supported subsets. No file is changed and no resource is extracted.
IISHAREDCANVAS_EXPORT std::vector<MediaFormatCapability> layeredDocumentFormats();
IISHAREDCANVAS_EXPORT LayeredDocumentImportResult decodeLayeredDocument(
    std::span<const std::uint8_t> bytes,
    const LayeredDocumentImportOptions &options = {});
IISHAREDCANVAS_EXPORT LayeredDocumentImportResult importLayeredDocument(
    const std::string &path,
    const LayeredDocumentImportOptions &options = {});

// Static PSD projection of native frame zero. Vector layers embed vector PDF
// Smart Objects; nontrivial bitmap transforms/chunks use canvas-clipped pixels.
// Source documents are never mutated. Animation/viewport losses are warnings.
IISHAREDCANVAS_EXPORT MediaBytesResult encodePsd(
    const Document &document, const PsdExportOptions &options = {});
IISHAREDCANVAS_EXPORT MediaIoResult exportPsd(
    const Document &document, const std::string &path,
    const PsdExportOptions &options = {});

} // namespace iiSharedCanvas
