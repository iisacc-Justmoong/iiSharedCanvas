#pragma once

#include "Document/Document.h"
#include "Export.h"

#include <string>
#include <vector>

namespace iiSharedCanvas {

enum class FrameRenderStatus {
    Success,
    InvalidDocument,
    FrameOutOfRange,
    InvalidRegion,
    AssetResolutionFailed,
};

struct FrameRenderResult {
    CanvasOrigin origin;
    RasterLayer pixels;
    FrameRenderStatus status = FrameRenderStatus::Success;
    std::string message;

    [[nodiscard]] bool ok() const noexcept
    {
        return status == FrameRenderStatus::Success;
    }
};

struct FrameRenderTileRequest {
    CanvasRegion region;
    CanvasExtent outputExtent;
};

struct FrameRenderTile {
    CanvasRegion region;
    RasterLayer pixels;
};

struct FrameTileRenderResult {
    std::vector<FrameRenderTile> tiles;
    FrameRenderStatus status = FrameRenderStatus::Success;
    std::string message;

    [[nodiscard]] bool ok() const noexcept
    {
        return status == FrameRenderStatus::Success;
    }
};

IISHAREDCANVAS_EXPORT FrameRenderResult renderFrame(const Document &document,
                                                     FrameIndex frame);
IISHAREDCANVAS_EXPORT FrameRenderResult renderFrameRegion(
    const Document &document,
    FrameIndex frame,
    CanvasRegion region,
    CanvasExtent outputExtent);
IISHAREDCANVAS_EXPORT FrameTileRenderResult renderFrameTiles(
    const Document &document,
    FrameIndex frame,
    const std::vector<FrameRenderTileRequest> &requests);

} // namespace iiSharedCanvas
