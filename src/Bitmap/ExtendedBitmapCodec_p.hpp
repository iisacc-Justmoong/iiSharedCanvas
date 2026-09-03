#pragma once

#include "BitmapCodec.h"

#include <QString>

namespace iiSharedCanvas::bitmap_detail {
std::vector<MediaFormatCapability> extendedFormats(const MediaBackendOptions &backend);
bool isExtendedFormat(const QString &name);
QString detectExtendedFormat(std::span<const std::uint8_t> bytes);
BitmapImportResult decodeExtended(std::span<const std::uint8_t> bytes, const QString &format,
                                  const BitmapImportOptions &options);
MediaBytesResult encodeExtended(const RasterLayer &pixels, const QString &format,
                                const BitmapExportOptions &options);
}
