#pragma once

#include "Media/MediaIo.h"

#include <span>

namespace iiSharedCanvas::media_detail {

// All results own their data. These synchronous helpers access only the
// supplied bytes and limits; they do not access files or shared state.
struct PngTextResult {
    std::vector<MediaTextEntry> text;
    MediaIoResult result;
};

// Validate complete PNG framing and extract tEXt, zTXt and iTXt. Each chunk's
// CRC and the cumulative expanded text budget are checked before publication.
PngTextResult readPngText(std::span<const std::uint8_t> bytes, std::uint64_t maxDecodedBytes);

// Validate unique UTF-8 keys and values; PNG additionally requires a
// standards-compliant, losslessly representable Latin-1 keyword.
MediaIoResult validateTextEntries(std::span<const MediaTextEntry> entries, bool png);

// Append validated iTXt chunks before IEND without changing image pixels.
// The input vector is consumed and the output budget includes all chunks.
MediaBytesResult writePngText(std::vector<std::uint8_t> bytes,
                             std::span<const MediaTextEntry> entries,
                             std::uint64_t maxOutputBytes);

} // namespace iiSharedCanvas::media_detail
