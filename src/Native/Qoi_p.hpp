#pragma once

#include "Bitmap/BitmapCodec.h"

#include <span>

namespace iiSharedCanvas::media_detail {

// Memory-only QOI conversion. The caller retains all input ownership; results
// own their bytes and pixels. Limits and complete framing are checked before
// returning success. Calls access no global state and can run concurrently.
BitmapImportResult decodeQoi(std::span<const std::uint8_t> bytes, const BitmapImportOptions &options);
MediaBytesResult encodeQoi(const RasterLayer &pixels, const BitmapExportOptions &options);

} // namespace iiSharedCanvas::media_detail
