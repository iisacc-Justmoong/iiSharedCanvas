#pragma once

#include "LayeredDocumentCodec.h"

namespace iiSharedCanvas::layered_detail {

LayeredDocumentImportResult decodeOpenRaster(
    std::span<const std::uint8_t> bytes,
    const LayeredDocumentImportOptions &options);

} // namespace iiSharedCanvas::layered_detail
