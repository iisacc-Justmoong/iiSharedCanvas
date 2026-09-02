#pragma once

#include "Document/Document.h"
#include "Export.h"

#include <cstddef>
#include <string>
#include <vector>

namespace iiSharedCanvas {

enum class FrameRenderStatus {
    Success,
    InvalidDocument,
    FrameOutOfRange,
    InvalidRegion,
    AssetResolutionFailed,
    LayerOutOfRange,
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

struct FrameLayerTileRenderResult {
    std::size_t layerIndex = 0;
    std::string layerId;
    bool visible = false; // Effective visibility at the requested frame.
    double opacity = 1.0;
    RasterBlendMode blendMode = RasterBlendMode::SourceOver;
    std::vector<FrameRenderTile> tiles;
    FrameRenderStatus status = FrameRenderStatus::Success;
    std::string message;

    [[nodiscard]] bool ok() const noexcept
    {
        return status == FrameRenderStatus::Success;
    }
};

struct FrameLayerBatchRenderResult {
    std::vector<FrameRenderTileRequest> requests;
    std::vector<FrameLayerTileRenderResult> layers;
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
IISHAREDCANVAS_EXPORT FrameLayerTileRenderResult renderFrameLayerTiles(
    const Document &document,
    FrameIndex frame,
    std::size_t layerIndex,
    const std::vector<FrameRenderTileRequest> &requests);
IISHAREDCANVAS_EXPORT FrameLayerBatchRenderResult renderFrameLayers(
    const Document &document,
    FrameIndex frame,
    const std::vector<FrameRenderTileRequest> &requests);
IISHAREDCANVAS_EXPORT FrameTileRenderResult composeFrameLayers(
    const FrameLayerBatchRenderResult &layers);

} // namespace iiSharedCanvas
