#pragma once

#include "Media/MediaIo.h"

#include <optional>
#include <string>

namespace iiSharedCanvas {

struct PdfExportOptions {
    FrameIndex firstFrame = 0;
    std::optional<FrameIndex> lastFrame; // Inclusive; absent means one page.
    bool rasterizeUnsupportedBlending = false;
    bool overwrite = false;
    MediaLimits limits;
};

IISHAREDCANVAS_EXPORT MediaIoResult exportPdf(
    const Document &document, const std::string &path, const PdfExportOptions &options = {});

} // namespace iiSharedCanvas
