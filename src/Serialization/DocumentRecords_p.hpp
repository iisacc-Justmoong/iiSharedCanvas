#pragma once

#include "Serialization/IiscCodec.h"

#include <optional>
#include <functional>

namespace iiSharedCanvas::detail {

// Private working-file records reuse the versioned .iisc field encodings.
// Raster payloads are raw ARGB32 so one changed pixel never shifts later bytes.
enum class RecordKind : int {
    Header, Asset, LayerCount, Layer, Metadata,
    AudioAssetCount, AudioAsset, AudioTrackCount, AudioTrack, Authorship,
    ArtboardCount, Artboard, PublicationMetadata,
};

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

// Storage v2 keeps raw bitmap pixels in independently checked tiles. Video
// frames remain inline. This callback materializes only a validated descriptor.
using WorkingRasterReader = std::function<RasterLayer(
    std::string_view, std::int32_t, std::int32_t, std::optional<CanvasOrigin>)>;
bool workingAssetUnchanged(const Asset &, const Document *previous);

struct WorkingResourceTotals {
    std::uint64_t rasterPixels = 0, rasterChunks = 0, stringBytes = 0, authorshipBytes = 0;
};
struct WorkingResourceResult { WorkingResourceTotals totals; IiscError error; };
WorkingResourceResult workingResourceTotals(const Document &, SerializationLimits);
std::vector<std::uint8_t> encodeWorkingAuthorship(const std::string &json);
std::vector<std::uint8_t> encodeWorkingHeader(const Document &document,
                                               std::optional<CanvasRegion> canvas = {});
std::vector<std::uint8_t> encodeWorkingChunkDescriptor(
    std::string_view assetId, std::span<const CanvasOrigin> chunks, int chunkSize);

RecordEncodeResult encodeDocumentRecords(const Document &document,
                                         const Document *previous,
                                         SerializationLimits limits, bool separateRasters = false,
                                         std::string_view forceAsset = {});
IiscDecodeResult decodeDocumentRecords(const std::vector<DocumentRecord> &records,
                                       SerializationLimits limits,
                                       const WorkingRasterReader &rasterReader = {});

} // namespace iiSharedCanvas::detail
