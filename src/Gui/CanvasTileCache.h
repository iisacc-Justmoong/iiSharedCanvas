#pragma once

#include "Gui/CanvasPresentation.h"

#include <cstddef>
#include <cstdint>
#include <span>
#include <vector>

namespace iiSharedCanvas {

// Immutable image storage may be shared by successive cache snapshots. The
// owner serializes publication and retains the snapshot for any active draw.
struct CanvasCachedTile {
    CanvasRegion region;
    std::shared_ptr<const ii::gui::RasterImage> image;
};
struct CanvasTileCache {
    std::uint64_t revision = 0;
    std::vector<CanvasCachedTile> tiles; // Oldest first, newest last.
};

// Plans complete rerenders for every cached tile intersecting caller-supplied
// document-space damage. Uncached visible regions are scheduled separately by
// the owner; no document, editor, scheduler or widget is accessed.
class CanvasDirtyTilePlanner final {
public:
    // The cache revision must match expectedRevision. Output requests retain
    // the cached pixel extents and requested sampling, in cache order. Invalid
    // geometry, overlap or an excessive request count fails without mutation.
    [[nodiscard]] ii::core::Result<std::vector<FrameRenderTileRequest>> execute(
        const CanvasTileCache &snapshot, std::uint64_t expectedRevision,
        std::span<const CanvasRegion> damaged, RasterSampling sampling,
        std::size_t maximumRequests) const;
};

// Produces a detached cache generation without changing the supplied snapshot.
// The caller supplies all invalidated document areas and serializes publication.
// Independent calls are thread-safe and no document or widget is accessed.
class CanvasTileCacheUpdater final {
public:
    // expectedRevision must equal the snapshot revision; nextRevision cannot
    // decrease. Intersecting old tiles are removed in their entirety before
    // updates are inserted; callers rerender the complete removed coverage.
    // New overlapping tiles replace earlier entries. The oldest entries are
    // evicted until both explicit budgets hold. Invalid or stale input returns
    // an error and leaves every input and prior shared image unchanged.
    [[nodiscard]] ii::core::Result<CanvasTileCache> execute(const CanvasTileCache &snapshot,
        std::uint64_t expectedRevision, std::uint64_t nextRevision,
        std::span<const CanvasRegion> invalidated, std::span<const FrameRenderTile> updates,
        std::size_t maximumTiles, std::uint64_t maximumPixels) const;
};

// Builds iiGUI image runs from immutable cached tiles. They retain the same
// image identities across updates, enabling the iiGUI renderer to reuse them.
// Actual redraw and upload scheduling remain the host renderer's responsibility.
class CanvasTilePresenter final {
public:
    // query.revision must match the cache. Supports axis-aligned pan and zoom;
    // rotation and reflection return
    // unsupported so the caller can choose the full-frame presentation path.
    // The returned drawing owns shared references, never the input cache.
    [[nodiscard]] ii::core::Result<std::shared_ptr<const ii::gui::WidgetDrawing>> execute(
        const CanvasTileCache &cache, const ii::gui::EditorCanvasQuery &query) const;
};

} // namespace iiSharedCanvas
