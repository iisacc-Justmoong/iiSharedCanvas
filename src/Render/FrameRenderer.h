#pragma once

#include "Document/Document.h"
#include "Export.h"

#include <string>

namespace iiSharedCanvas {

enum class FrameRenderStatus {
    Success,
    InvalidDocument,
    FrameOutOfRange,
    AssetResolutionFailed,
};

struct FrameRenderResult {
    RasterLayer pixels;
    FrameRenderStatus status = FrameRenderStatus::Success;
    std::string message;

    [[nodiscard]] bool ok() const noexcept
    {
        return status == FrameRenderStatus::Success;
    }
};

IISHAREDCANVAS_EXPORT FrameRenderResult renderFrame(const Document &document,
                                                     FrameIndex frame);

} // namespace iiSharedCanvas
