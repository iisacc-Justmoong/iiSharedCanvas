#include "Native/WorkingFileTiles_p.hpp"
#include <iiFileProviderNative.h>
#include <algorithm>
#include <array>
#include <cstring>
#include <limits>
#include <map>
#include <set>

namespace iiSharedCanvas::detail {
namespace {
constexpr int TileSide = 64;
using Coordinate = std::pair<std::int64_t, std::int64_t>;
using Blob = sql::Blob;

[[noreturn]] void corrupt(const char *message)
{
    throw NativeStorageFailure(DocumentFileCode::CorruptFile, message);
}

Blob hash(std::span<const std::uint8_t> bytes)
{
    const auto text = iiFileProvider::native::sha256(std::string_view(
        reinterpret_cast<const char *>(bytes.data()), bytes.size()));
    const auto nibble = [](char c) { return c <= '9' ? c - '0' : c - 'a' + 10; };
    Blob result(32);
    for (std::size_t i = 0; i < result.size(); ++i)
        result[i] = std::uint8_t((nibble(text[i * 2]) << 4) | nibble(text[i * 2 + 1]));
    return result;
}

Blob tileHash(std::string_view asset, Coordinate key, std::span<const std::uint8_t> bytes)
{
    Blob identity{'I','I','C','T','I','L','E',2};
    const auto append = [&](std::uint64_t value) {
        for (unsigned i = 0; i < 8; ++i) identity.push_back(std::uint8_t(value >> (i * 8)));
    };
    append(asset.size()); identity.insert(identity.end(), asset.begin(), asset.end());
    append(std::uint64_t(key.first)); append(std::uint64_t(key.second)); append(bytes.size());
    const auto payload = hash(bytes);
    identity.insert(identity.end(), payload.begin(), payload.end());
    return hash(identity);
}

void within(std::uint64_t &total, std::uint64_t count, std::uint64_t maximum)
{
    if (total > maximum || count > maximum - total)
        throw NativeStorageFailure(DocumentFileCode::LimitExceeded,
                                   "working-file tiles exceed the configured byte limit");
    total += count;
}

struct StoredTile {
    std::int64_t rowId;
    std::size_t size;
    Blob digest;
};

void addDelta(std::uint64_t &total, std::uint64_t amount)
{
    if (amount > std::numeric_limits<std::uint64_t>::max() - total)
        throw NativeStorageFailure(DocumentFileCode::LimitExceeded, "working-file tile delta overflow");
    total += amount;
}

void writeAsset(NativeDatabase &database, const Asset &asset, int chunkSize,
                DocumentFileWriteStatistics &statistics, WorkingTileDelta *delta = nullptr)
{
    const std::string id = assetId(asset);
    std::map<Coordinate, StoredTile> previous;
    NativeStatement query(&database,
        "SELECT rowid,column,row,length(data),length(digest),digest FROM canvas_tiles WHERE asset=?");
    query.text(1, id);
    while (query.row()) {
        const auto length = query.integer(3);
        if ((length != 64 * 64 * 4 && length != 32 * 32 * 4) || query.integer(4) != 32)
            corrupt("invalid stored tile size");
        const auto digest = query.bytes(5);
        previous.emplace(Coordinate{query.integer(1), query.integer(2)},
                         StoredTile{query.integer(0), std::size_t(length), Blob(digest.begin(), digest.end())});
    }
    const auto writeRaster = [&](const RasterLayer &raster, std::int64_t originX,
                                  std::int64_t originY, int side) {
        std::array<std::uint8_t, TileSide * TileSide * 4> buffer{};
        for (std::int64_t y = 0; y < raster.height; y += side)
            for (std::int64_t x = 0; x < raster.width; x += side) {
                std::ranges::fill(buffer, 0);
                bool nonzero = false;
                for (int row = 0; row < side && y + row < raster.height; ++row)
                    for (int column = 0; column < side && x + column < raster.width; ++column) {
                        const auto pixel = raster.pixels[std::size_t(y + row) * raster.width + x + column];
                        nonzero |= pixel != 0;
                        const auto offset = (std::size_t(row) * side + column) * 4;
                        for (unsigned byte = 0; byte < 4; ++byte)
                            buffer[offset + byte] = std::uint8_t(pixel >> (byte * 8));
                    }
                const Coordinate key{originX / side + x / side, originY / side + y / side};
                auto old = previous.find(key);
                // An all-zero tile is absent. Hidden RGB in transparent pixels
                // remains data: only the complete ARGB word may be omitted.
                if (!nonzero)
                    continue;
                const std::span bytes(buffer.data(), std::size_t(side) * side * 4);
                const auto digest = tileHash(id, key, bytes);
                if (old == previous.end()) {
                    NativeStatement insert(&database,
                        "INSERT INTO canvas_tiles(asset,column,row,data,digest) VALUES(?,?,?,?,?)");
                    insert.text(1, id); insert.integer(2, key.first); insert.integer(3, key.second);
                    insert.bytes(4, bytes.data(), bytes.size());
                    insert.bytes(5, digest.data(), digest.size()); insert.done();
                    ++statistics.recordsWritten;
                    statistics.payloadBytesWritten += bytes.size();
                    if (delta) { addDelta(delta->addedBytes, bytes.size()); addDelta(delta->addedTiles, 1); }
                    continue;
                }
                if (old->second.digest != digest || old->second.size != bytes.size()) {
                    auto before = sqlValue(database.value.read_blob_range(
                        "canvas_tiles", "data", old->second.rowId, 0, old->second.size));
                    if (tileHash(id, key, before) != old->second.digest)
                        corrupt("stored tile checksum does not match");
                    if (before.size() == bytes.size()) {
                        const sql::BlobRangePatch patch{0, before, bytes};
                        const auto patched = sqlValue(database.value.compare_exchange_blob_ranges(
                            "canvas_tiles", "data", old->second.rowId, std::span(&patch, 1)));
                        statistics.payloadBytesWritten += patched.bytes_written;
                    }
                    NativeStatement update(&database, before.size() == bytes.size()
                        ? "UPDATE canvas_tiles SET digest=? WHERE rowid=?"
                        : "UPDATE canvas_tiles SET digest=?,data=? WHERE rowid=?");
                    update.bytes(1, digest.data(), digest.size());
                    const bool resized = before.size() != bytes.size();
                    if (resized) {
                        update.bytes(2, bytes.data(), bytes.size());
                        statistics.payloadBytesWritten += bytes.size();
                        if (delta) {
                            addDelta(delta->removedBytes, old->second.size);
                            addDelta(delta->addedBytes, bytes.size());
                        }
                    }
                    update.integer(resized ? 3 : 2, old->second.rowId); update.done();
                    ++statistics.recordsWritten;
                }
                previous.erase(old);
            }
    };
    if (const auto *raster = std::get_if<RasterAsset>(&asset))
        writeRaster(raster->pixels, 0, 0, TileSide);
    else if (const auto *chunked = std::get_if<ChunkedRasterAsset>(&asset))
        for (const auto &chunk : chunked->chunks)
            writeRaster(chunk.pixels, std::int64_t(chunk.column) * chunkSize,
                        std::int64_t(chunk.row) * chunkSize, std::min(TileSide, chunkSize));
    for (const auto &[key, old] : previous) {
        NativeStatement remove(&database, "DELETE FROM canvas_tiles WHERE rowid=?");
        remove.integer(1, old.rowId); remove.done(); ++statistics.recordsWritten;
        if (delta) { addDelta(delta->removedBytes, old.size); addDelta(delta->removedTiles, 1); }
    }
}
} // namespace

RasterLayer WorkingTileReader::read(std::string_view asset, std::int32_t width,
                                    std::int32_t height, std::optional<CanvasOrigin> chunk)
{
    if (chunk && (width < 32 || width > 4096 || width != height || (width & (width - 1))))
        corrupt("invalid tiled chunk geometry");
    const int side = chunk ? std::min(TileSide, width) : TileSide;
    const std::int64_t originX = chunk ? std::int64_t(chunk->x) * width : 0;
    const std::int64_t originY = chunk ? std::int64_t(chunk->y) * height : 0;
    if (chunk && (originX < INT32_MIN || originY < INT32_MIN ||
                  originX > INT32_MAX || originY > INT32_MAX))
        corrupt("tiled chunk origin is outside the coordinate domain");
    RasterLayer result = makeRasterLayer(width, height);
    NativeStatement query(&database_,
        "SELECT column,row,length(data),length(digest),data,digest FROM canvas_tiles "
        "WHERE asset=? AND column>=? AND column<? AND row>=? AND row<? ORDER BY row,column");
    query.text(1, std::string(asset));
    query.integer(2, originX / side); query.integer(3, originX / side + (std::int64_t(width) + side - 1) / side);
    query.integer(4, originY / side); query.integer(5, originY / side + (std::int64_t(height) + side - 1) / side);
    while (query.row()) {
        if (query.integer(2) != side * side * 4 || query.integer(3) != 32)
            corrupt("tile payload size does not match its geometry");
        within(bytes_, std::uint64_t(side) * side * 4, limits_.maximumContainerBytes);
        const auto bytes = query.bytes(4), digest = query.bytes(5);
        if (!std::ranges::equal(tileHash(asset, {query.integer(0), query.integer(1)}, bytes), digest))
            corrupt("tile checksum does not match");
        const auto x = query.integer(0) * side - originX, y = query.integer(1) * side - originY;
        bool nonzero = false;
        for (int row = 0; row < side; ++row)
            for (int column = 0; column < side; ++column) {
                const auto offset = (std::size_t(row) * side + column) * 4;
                std::uint32_t pixel = 0;
                for (unsigned byte = 0; byte < 4; ++byte)
                    pixel |= std::uint32_t(bytes[offset + byte]) << (byte * 8);
                nonzero |= pixel != 0;
                if (x + column >= width || y + row >= height) {
                    if (pixel) corrupt("nonzero tile padding lies outside its raster");
                } else
                    result.pixels[std::size_t(y + row) * width + x + column] = pixel;
            }
        if (!nonzero) corrupt("an all-zero tile must be omitted");
        ++rows_;
    }
    return result;
}

void WorkingTileReader::finish(std::uint64_t metadataBytes)
{
    if (database_.scalar("SELECT count(*) FROM canvas_tiles") != std::int64_t(rows_))
        corrupt("working file contains unreferenced tiles");
    if (database_.scalar("SELECT tile_count FROM canvas_tile_state WHERE singleton=1") != std::int64_t(rows_))
        corrupt("working file tile count does not match its manifest");
    within(bytes_, metadataBytes, limits_.maximumContainerBytes);
}

WorkingTileDelta writeWorkingAssetLayerTiles(NativeDatabase &database, const Document &document,
                                             std::string_view id, WorkingAssetTileAction action)
{
    if (!database.inTransaction())
        throw NativeStorageFailure(DocumentFileCode::EditRejected, "asset tiles require a transaction");
    if (action != WorkingAssetTileAction::Insert && action != WorkingAssetTileAction::Remove)
        throw NativeStorageFailure(DocumentFileCode::InvalidArgument, "invalid asset tile action");
    if (id.empty() || id.find('\0') != std::string_view::npos)
        throw NativeStorageFailure(DocumentFileCode::InvalidArgument, "invalid asset identifier");
    const bool inserting = action == WorkingAssetTileAction::Insert;
    const auto *asset = findAsset(document, std::string(id));
    if (inserting != (asset != nullptr))
        throw NativeStorageFailure(DocumentFileCode::Conflict, "asset tile operation does not match the document");
    if (inserting && std::holds_alternative<ChunkedRasterAsset>(*asset)) {
        const auto side = document.infiniteCanvas.chunkSize;
        if (side < 32 || side > 4096 || (side & (side - 1)))
            throw NativeStorageFailure(DocumentFileCode::InvalidDocument, "invalid chunk size");
    }
    const auto count = database.scalar("SELECT tile_count FROM canvas_tile_state WHERE singleton=1");
    if (count < 0) corrupt("invalid stored tile count");
    WorkingTileDelta delta;
    if (inserting) {
        NativeStatement existing(&database, "SELECT 1 FROM canvas_tiles WHERE asset=? LIMIT 1");
        existing.text(1, std::string(id));
        if (existing.row()) corrupt("inserted asset already has stored tiles");
        if (std::holds_alternative<RasterAsset>(*asset) || std::holds_alternative<ChunkedRasterAsset>(*asset))
            writeAsset(database, *asset, document.infiniteCanvas.chunkSize, delta.statistics, &delta);
    } else {
        NativeStatement existing(&database,
            "SELECT length(data),length(digest) FROM canvas_tiles WHERE asset=?");
        existing.text(1, std::string(id));
        while (existing.row()) {
            const auto length = existing.integer(0);
            if ((length != 64 * 64 * 4 && length != 32 * 32 * 4) || existing.integer(1) != 32)
                corrupt("invalid stored tile size");
            addDelta(delta.removedBytes, static_cast<std::uint64_t>(length));
            addDelta(delta.removedTiles, 1);
        }
        if (delta.removedTiles) {
            NativeStatement remove(&database, "DELETE FROM canvas_tiles WHERE asset=?");
            remove.text(1, std::string(id)); remove.done();
            delta.statistics.recordsWritten = delta.removedTiles;
        }
    }
    if (delta.removedTiles > static_cast<std::uint64_t>(count) ||
        delta.addedTiles > std::uint64_t(INT64_MAX) - (static_cast<std::uint64_t>(count) - delta.removedTiles))
        throw NativeStorageFailure(DocumentFileCode::LimitExceeded, "working-file tile count exceeds limit");
    if (delta.addedTiles || delta.removedTiles) {
        NativeStatement update(&database, "UPDATE canvas_tile_state SET tile_count=? WHERE singleton=1");
        update.integer(1, static_cast<std::int64_t>(static_cast<std::uint64_t>(count) - delta.removedTiles + delta.addedTiles));
        update.done();
    }
    return delta;
}

DocumentFileWriteStatistics writeWorkingTiles(NativeDatabase &database, const Document &document,
                                             const Document *previous, SerializationLimits limits,
                                             std::string_view forceAsset)
{
    DocumentFileWriteStatistics statistics;
    std::set<std::string> assets;
    for (const auto &asset : document.assets) {
        if (!std::holds_alternative<RasterAsset>(asset) && !std::holds_alternative<ChunkedRasterAsset>(asset))
            continue;
        assets.insert(assetId(asset));
        if (assetId(asset)==forceAsset || !workingAssetUnchanged(asset, previous))
            writeAsset(database, asset, document.infiniteCanvas.chunkSize, statistics);
    }
    std::vector<std::string> removed;
    NativeStatement query(&database, "SELECT DISTINCT asset FROM canvas_tiles");
    while (query.row()) {
        auto id = query.text(0);
        if (!assets.contains(id)) removed.push_back(std::move(id));
    }
    for (const auto &id : removed) {
        NativeStatement count(&database, "SELECT count(*) FROM canvas_tiles WHERE asset=?");
        count.text(1, id); if (!count.row()) corrupt("missing tile count");
        statistics.recordsWritten += count.integer(0);
        NativeStatement remove(&database, "DELETE FROM canvas_tiles WHERE asset=?");
        remove.text(1, id); remove.done();
    }
    const auto tiles = database.scalar("SELECT coalesce(sum(length(data)),0) FROM canvas_tiles");
    const auto metadata = database.scalar("SELECT coalesce(sum(length(data)),0) FROM canvas_records");
    if (tiles < 0 || metadata < 0) corrupt("negative working-file payload size");
    std::uint64_t total = IiscHeaderSize - 4;
    within(total, std::uint64_t(tiles), limits.maximumContainerBytes);
    within(total, std::uint64_t(metadata), limits.maximumContainerBytes);
    const auto count = database.scalar("SELECT count(*) FROM canvas_tiles");
    if (database.scalar("SELECT tile_count FROM canvas_tile_state WHERE singleton=1") != count) {
        NativeStatement update(&database, "UPDATE canvas_tile_state SET tile_count=? WHERE singleton=1");
        update.integer(1, count); update.done();
    }
    return statistics;
}

WorkingTileDelta writeWorkingTilePatches(NativeDatabase &database, const PreparedBitmapChange &change)
{
    struct Segment {
        std::int32_t x, y;
        std::size_t count;
        const std::byte *data;
    };
    const int side = change.chunks ? std::min(TileSide, change.chunkSize) : TileSide;
    if (side != 32 && side != TileSide) corrupt("invalid tile size for prepared bitmap edit");
    const auto floor = [side](std::int64_t value) {
        return value / side - (value % side < 0);
    };
    std::map<Coordinate, std::vector<Segment>> affected;
    for (const auto &patch : change.patches) {
        if (patch.plane == "chunks" || patch.plane == "extent" || patch.plane == "canvas") continue;
        if (patch.plane != "pixels" || patch.sampleBytes != 4 ||
            patch.bounds.width <= 0 || patch.bounds.height <= 0 ||
            std::uint64_t(patch.bounds.width) * patch.bounds.height * 4 != patch.data.size())
            corrupt("invalid prepared bitmap pixel span");
        for (int row = 0; row < patch.bounds.height; ++row) {
            const auto y = std::int64_t(patch.bounds.origin.y) + row;
            for (int x = 0; x < patch.bounds.width;) {
                const auto worldX = std::int64_t(patch.bounds.origin.x) + x;
                const auto column = floor(worldX), tileRow = floor(y);
                const auto localX = worldX - column * side;
                const int count = std::min(patch.bounds.width - x, int(side - localX));
                if (worldX < INT32_MIN || worldX > INT32_MAX || y < INT32_MIN || y > INT32_MAX)
                    corrupt("prepared bitmap coordinate is outside the supported range");
                affected[{column, tileRow}].push_back({std::int32_t(worldX), std::int32_t(y),
                    std::size_t(count), patch.data.data() +
                        (std::size_t(row) * patch.bounds.width + x) * 4});
                x += count;
            }
        }
    }
    const std::string assetId(change.assetId);
    WorkingTileDelta result;
    for (const auto &[key, segments] : affected) {
        const std::size_t length = std::size_t(side) * side * 4;
        sql::Blob before(length, 0);
        bool exists = false;
        std::int64_t rowId = 0;
        {
            NativeStatement query(&database,
                "SELECT rowid,length(data),length(digest),data,digest FROM canvas_tiles "
                "WHERE asset=? AND column=? AND row=?");
            query.text(1, assetId); query.integer(2, key.first); query.integer(3, key.second);
            if (query.row()) {
                exists = true; rowId = query.integer(0);
                if (query.integer(1) != std::int64_t(length) || query.integer(2) != 32)
                    corrupt("stored tile has an invalid size");
                const auto data = query.bytes(3), expected = query.bytes(4);
                if (!std::ranges::equal(tileHash(assetId, key, data), expected))
                    corrupt("stored tile checksum does not match");
                std::ranges::copy(data, before.begin());
                if (query.row()) corrupt("duplicate stored tile");
            }
        }
        auto after = before;
        for (const auto &segment : segments) {
            const auto localX = std::int64_t(segment.x) - key.first * side;
            const auto localY = std::int64_t(segment.y) - key.second * side;
            for (std::size_t x = 0; x < segment.count; ++x) {
                const auto offset = (std::size_t(localY) * side + std::size_t(localX) + x) * 4;
                std::uint32_t oldPixel = 0, nextPixel = 0;
                for (unsigned byte = 0; byte < 4; ++byte)
                    oldPixel |= std::uint32_t(before[offset + byte]) << (byte * 8);
                if (oldPixel != change.readPixel(change.readerContext,
                        {std::int32_t(std::int64_t(segment.x) + std::int64_t(x)), segment.y}))
                    throw NativeStorageFailure(DocumentFileCode::Conflict,
                                               "stored and in-memory bitmap pixels differ");
                std::memcpy(&nextPixel, segment.data + x * 4, 4);
                for (unsigned byte = 0; byte < 4; ++byte)
                    after[offset + byte] = std::uint8_t(nextPixel >> (byte * 8));
            }
        }
        if (after == before) continue;
        const bool empty = std::ranges::all_of(after, [](auto byte) { return byte == 0; });
        if (empty) {
            if (exists) {
                NativeStatement remove(&database, "DELETE FROM canvas_tiles WHERE rowid=?");
                remove.integer(1, rowId); remove.done();
                result.removedBytes += length; ++result.removedTiles; ++result.statistics.recordsWritten;
            }
            continue;
        }
        const auto digest = tileHash(assetId, key, after);
        if (!exists) {
            NativeStatement insert(&database,
                "INSERT INTO canvas_tiles(asset,column,row,data,digest) VALUES(?,?,?,?,?)");
            insert.text(1, assetId); insert.integer(2, key.first); insert.integer(3, key.second);
            insert.bytes(4, after.data(), after.size()); insert.bytes(5, digest.data(), digest.size());
            insert.done();
            result.addedBytes += length; ++result.addedTiles;
            result.statistics.payloadBytesWritten += length;
        } else {
            const sql::BlobRangePatch range{0, before, after};
            const auto written = sqlValue(database.value.compare_exchange_blob_ranges(
                "canvas_tiles", "data", rowId, std::span(&range, 1)));
            NativeStatement update(&database, "UPDATE canvas_tiles SET digest=? WHERE rowid=?");
            update.bytes(1, digest.data(), digest.size()); update.integer(2, rowId); update.done();
            result.statistics.payloadBytesWritten += written.bytes_written;
        }
        ++result.statistics.recordsWritten;
    }
    for (const auto &presence : change.presence) {
        if (!presence.before || presence.after) continue;
        const auto tilesPerChunk = change.chunkSize / side;
        const auto column = std::int64_t(presence.column) * tilesPerChunk;
        const auto row = std::int64_t(presence.row) * tilesPerChunk;
        NativeStatement remaining(&database,
            "SELECT 1 FROM canvas_tiles WHERE asset=? AND column>=? AND column<? "
            "AND row>=? AND row<? LIMIT 1");
        remaining.text(1, assetId); remaining.integer(2, column);
        remaining.integer(3, column + tilesPerChunk); remaining.integer(4, row);
        remaining.integer(5, row + tilesPerChunk);
        if (remaining.row())
            throw NativeStorageFailure(DocumentFileCode::Conflict,
                                       "chunk removal would discard other stored pixels");
    }
    return result;
}
} // namespace iiSharedCanvas::detail
