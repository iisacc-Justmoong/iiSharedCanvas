#pragma once

#include "Media/MediaIo.h"
#include "Serialization/IiscCodec.h"

namespace iiSharedCanvas {

struct DocumentPreviewOptions {
    // Aspect-fit, never upscale. Each bound must be in [1, 4096].
    CanvasExtent maximumExtent = {512, 512};
    FrameIndex frame = 0;
};

struct DocumentPreviewResult {
    CanvasRegion region; // Source document coordinates, including artboards.
    RasterLayer pixels; // Straight-alpha ARGB; no checkerboard or UI decoration.
    MediaIoResult result;
    [[nodiscard]] bool ok() const noexcept { return result.ok(); }
};

// Composite visible artwork at a bounded output size, preserving aspect and alpha.
// Synchronous and detached; applications should call these APIs on a worker.
IISHAREDCANVAS_EXPORT DocumentPreviewResult renderDocumentPreview(
    const Document &document, DocumentPreviewOptions options = {});

// Read a UTF-8 local IISC snapshot or SQLite working file, including committed WAL.
// Never open the source for authoring, migrate it, or persist a derived preview in it.
// Uses a private temporary snapshot for working files, including read-only inputs.
IISHAREDCANVAS_EXPORT DocumentPreviewResult readIiscPreview(
    const std::string &path, DocumentPreviewOptions options = {}, SerializationLimits limits = {});

} // namespace iiSharedCanvas
