#pragma once

#include "Serialization/IiscCodec.h"

#include <optional>

namespace iiSharedCanvas::detail {

// Private working-file records reuse the versioned .iisc field encodings.
// Raster payloads are raw ARGB32 so one changed pixel never shifts later bytes.
enum class RecordKind : int { Header, Asset, LayerCount, Layer, Metadata };

struct DocumentRecord {
    RecordKind kind;
    std::string id;
    std::uint32_t position = 0;
    // An absent payload means the corresponding prior asset is unchanged.
    std::optional<std::vector<std::uint8_t>> data;
};

struct RecordEncodeResult {
    std::vector<DocumentRecord> records;
    IiscError error;
};

RecordEncodeResult encodeDocumentRecords(const Document &document,
                                         const Document *previous,
                                         SerializationLimits limits);
IiscDecodeResult decodeDocumentRecords(const std::vector<DocumentRecord> &records,
                                       SerializationLimits limits);

} // namespace iiSharedCanvas::detail
