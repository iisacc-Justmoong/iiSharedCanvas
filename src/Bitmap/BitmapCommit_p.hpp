#pragma once

#include "Document/Document.h"
#include <History/RasterEditHistory.h>
#include <cstdint>
#include <optional>
#include <span>
#include <string_view>

namespace iiSharedCanvas::detail {

struct BitmapChunkPresence {
    std::int32_t column, row;
    bool before, after;
};

// Borrowed data and reader are valid only for the synchronous writer call.
// The port commits durably before the journal publishes its prepared memory.
struct PreparedBitmapChange {
    Document *document;
    std::string_view assetId;
    const RasterAsset *raster;
    const ChunkedRasterAsset *chunks;
    int chunkSize;
    std::span<const RasterHistoryPatch> patches;
    std::span<const BitmapChunkPresence> presence;
    const DocumentAuthorship *authorship;
    std::optional<CanvasRegion> canvasAfter;
    const void *readerContext;
    std::uint32_t (*readPixel)(const void *, DevicePixelPoint);
};

struct BitmapCommitPort {
    void *context;
    bool (*commit)(void *, const PreparedBitmapChange &, RasterHistoryCode &, bool &);
    bool useDraftFallback = false;
    // Optional exact metadata for publishing an already-authored history.
    const DocumentAuthorship *authorshipOverride = nullptr;
};

} // namespace iiSharedCanvas::detail
