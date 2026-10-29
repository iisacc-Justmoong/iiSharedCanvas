#pragma once

#include "Media/MediaIo.h"

namespace iiSharedCanvas {

struct VectorExportOptions {
    bool compressed = false; // gzip SVGZ, not a proprietary wrapper.
    bool overwrite = false;
    MediaLimits limits;
};

IISHAREDCANVAS_EXPORT MediaBytesResult encodeSvg(
    const VectorAsset &asset, const VectorExportOptions &options = {});
IISHAREDCANVAS_EXPORT MediaIoResult exportSvg(
    const VectorAsset &asset, const std::string &path, const VectorExportOptions &options = {});

} // namespace iiSharedCanvas
