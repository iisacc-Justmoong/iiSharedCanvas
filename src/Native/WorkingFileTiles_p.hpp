#pragma once

#include "Native/Storage_p.hpp"
#include "Bitmap/BitmapCommit_p.hpp"
#include "Serialization/DocumentRecords_p.hpp"

namespace iiSharedCanvas::detail {

inline constexpr const char *WorkingTileStateSchema =
    "CREATE TABLE canvas_tile_state (singleton INTEGER PRIMARY KEY CHECK(singleton=1), "
    "tile_count INTEGER NOT NULL CHECK(tile_count>=0))";

inline constexpr const char *WorkingTileSchema =
    "CREATE TABLE canvas_tiles (asset TEXT NOT NULL, column INTEGER NOT NULL, "
    "row INTEGER NOT NULL, data BLOB NOT NULL, digest BLOB NOT NULL, "
    "PRIMARY KEY(asset,column,row))";

// Only the private working format separates editable bitmaps. Canonical IISC
// snapshots, video frames and non-bitmap assets retain their own encodings.
class WorkingTileReader final {
public:
    WorkingTileReader(NativeDatabase &database, SerializationLimits limits)
        : database_(database), limits_(limits) {}
    RasterLayer read(std::string_view asset, std::int32_t width, std::int32_t height,
                     std::optional<CanvasOrigin> chunk);
    void finish(std::uint64_t metadataBytes);
private:
    NativeDatabase &database_;
    SerializationLimits limits_;
    std::uint64_t rows_ = 0, bytes_ = 0;
};

DocumentFileWriteStatistics writeWorkingTiles(NativeDatabase &, const Document &,
                                             const Document *previous,
                                             SerializationLimits, std::string_view forceAsset = {});

struct WorkingTileDelta {
    DocumentFileWriteStatistics statistics;
    std::uint64_t addedBytes = 0, removedBytes = 0;
    std::uint64_t addedTiles = 0, removedTiles = 0;
};

// A private asset/layer edit has exactly one inserted or removed asset. The
// caller owns a write transaction and must validate the returned byte delta
// against its cached payload limit before publishing that transaction.
enum class WorkingAssetTileAction { Insert, Remove };
WorkingTileDelta writeWorkingAssetLayerTiles(NativeDatabase &, const Document &,
                                             std::string_view assetId, WorkingAssetTileAction);

// The caller owns an outer transaction and publishes memory only after commit.
// Only tiles intersecting the prepared pixel spans are materialized.
WorkingTileDelta writeWorkingTilePatches(NativeDatabase &, const PreparedBitmapChange &);

} // namespace iiSharedCanvas::detail
