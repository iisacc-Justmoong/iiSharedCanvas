// Included by IiscCodec.cpp. These records use the same field encoders and
// readers as .iisc without reserializing or reconstructing the document.
namespace {
constexpr std::array<std::uint8_t, 4> EditMagic{'I', 'S', 'E', '1'};
constexpr int EditTileSize = 64;
constexpr std::uint8_t RasterTilesRecord = 32;
constexpr std::uint32_t RemovedRecord = std::numeric_limits<std::uint32_t>::max();

struct EditRecord {
    std::uint8_t kind;
    std::string_view id;
    std::uint32_t position;
    std::span<const std::uint8_t> data;
};

void writeEditRecord(ByteWriter &writer, const EditRecord &record)
{
    writer.writeU8(record.kind);
    writer.writeU32(record.position);
    writer.writeU32(static_cast<std::uint32_t>(record.id.size()));
    writer.writeBytes({reinterpret_cast<const std::uint8_t *>(record.id.data()), record.id.size()});
    writer.writeU64(record.data.size());
    writer.writeBytes(record.data);
}

bool sameExtent(const RasterLayer &a, const RasterLayer &b)
{
    return a.width == b.width && a.height == b.height;
}

void writeRasterEdit(ByteWriter &writer, const RasterLayer &before, const RasterLayer &after,
                     std::optional<CanvasRegion> dirty = {})
{
    ByteWriter tiles;
    std::uint32_t count = 0;
    const auto tileStart = [](int coordinate) { return std::max(0, coordinate) / EditTileSize * EditTileSize; };
    const auto clippedEnd = [](int extent, int origin, int size) {
        return static_cast<int>(std::clamp(std::int64_t(origin) + size, std::int64_t(0), std::int64_t(extent)));
    };
    const int left = dirty ? tileStart(dirty->origin.x) : 0, top = dirty ? tileStart(dirty->origin.y) : 0;
    const int right = dirty ? clippedEnd(after.width, dirty->origin.x, dirty->extent.width) : after.width;
    const int bottom = dirty ? clippedEnd(after.height, dirty->origin.y, dirty->extent.height) : after.height;
    for (int y = top; y < bottom; y += EditTileSize) {
        for (int x = left; x < right; x += EditTileSize) {
            const int width = std::min(EditTileSize, after.width - x);
            const int height = std::min(EditTileSize, after.height - y);
            const auto at = [&](int row) { return std::size_t(y + row) * after.width + x; };
            bool changed = false;
            for (int row = 0; row < height && !changed; ++row) {
                const auto offset = at(row);
                changed = !std::equal(after.pixels.data() + offset, after.pixels.data() + offset + width,
                                      before.pixels.data() + offset);
            }
            if (!changed) continue;
            RasterLayer tile{width, height, {}};
            tile.pixels.reserve(std::size_t(width) * height);
            for (int row = 0; row < height; ++row)
                tile.pixels.insert(tile.pixels.end(), after.pixels.data() + at(row), after.pixels.data() + at(row) + width);
            tiles.writeI32(x); tiles.writeI32(y);
            writeRaster(tiles, tile);
            ++count;
        }
    }
    writer.writeU32(count);
    writer.writeBytes(tiles.bytes());
}

auto chunkCoordinate(const RasterChunk &chunk) { return std::pair{chunk.row, chunk.column}; }

void writeChunkEdit(ByteWriter &writer, const ChunkedRasterAsset &before, const ChunkedRasterAsset &after)
{
    ByteWriter chunks;
    std::uint32_t count = 0;
    auto old = before.chunks.begin(), next = after.chunks.begin();
    while (old != before.chunks.end() || next != after.chunks.end()) {
        const auto emit = [&](const RasterChunk &chunk, std::uint8_t operation) {
            chunks.writeI32(chunk.column); chunks.writeI32(chunk.row); chunks.writeU8(operation);
            ++count;
        };
        if (next == after.chunks.end() || (old != before.chunks.end() && chunkCoordinate(*old) < chunkCoordinate(*next))) {
            emit(*old++, 0);
        } else if (old == before.chunks.end() || chunkCoordinate(*next) < chunkCoordinate(*old)) {
            emit(*next, 1);
            writeRaster(chunks, next++->pixels);
        } else {
            if (!sameRasterPixels(old->pixels, next->pixels)) {
                emit(*next, 2);
                writeRasterEdit(chunks, old->pixels, next->pixels);
            }
            ++old; ++next;
        }
    }
    writer.writeU32(count);
    writer.writeBytes(chunks.bytes());
}

IiscEncodeResult finishEdit(const Document &before, const Document &after, const ByteWriter &payload,
                            std::uint32_t count, const SerializationLimits &limits)
{
    if (payload.bytes().size() + 20 > limits.maximumContainerBytes)
        return {{}, makeError(IiscErrorCode::LimitExceeded, 0, "edit exceeds its byte budget")};
    ByteWriter result;
    result.writeBytes(EditMagic);
    result.writeU16(before.formatVersion.major); result.writeU16(before.formatVersion.minor);
    result.writeU16(after.formatVersion.major); result.writeU16(after.formatVersion.minor);
    result.writeU32(crc32(payload.bytes()));
    result.writeU32(count);
    result.writeBytes(payload.bytes());
    return {result.takeBytes(), {}};
}

using RecordKey = std::pair<detail::RecordKind, std::string_view>;
bool isCollection(detail::RecordKind kind)
{
    using enum detail::RecordKind;
    return kind == Asset || kind == Layer || kind == AudioAsset || kind == AudioTrack || kind == Artboard;
}
bool isSeparateCount(detail::RecordKind kind)
{
    using enum detail::RecordKind;
    return kind == LayerCount || kind == AudioAssetCount || kind == AudioTrackCount || kind == ArtboardCount;
}

class EditTransaction final {
public:
    ~EditTransaction() {
        for (auto i = rollback_.rbegin(); i != rollback_.rend(); ++i) (*i)();
    }
    void onRollback(std::function<void()> action) { rollback_.push_back(std::move(action)); }
    template<typename T> void replace(T &value, T next) {
        auto previous = std::make_shared<T>(std::move(next));
        onRollback([&value, previous] { std::swap(value, *previous); });
        std::swap(value, *previous);
    }
    void finish() { rollback_.clear(); }
private:
    std::vector<std::function<void()>> rollback_;
};

void requireEdit(bool condition, const char *message)
{
    if (!condition) throw DecodeFailure(IiscErrorCode::InvalidData, 0, message);
}

std::vector<EditRecord> readEditRecords(ByteReader &reader, const SerializationLimits &limits)
{
    const auto count = reader.readU32();
    const auto maximum = std::uint64_t(limits.maximumAssets) * 2 + limits.maximumLayers +
                         limits.maximumAudioAssets + limits.maximumAudioTracks + limits.maximumArtboards + 8;
    requireEdit(count <= maximum && count <= reader.remaining() / 17, "invalid edit record count");
    std::vector<EditRecord> result;
    result.reserve(count);
    std::set<std::pair<unsigned, std::string_view>> used;
    for (std::uint32_t i = 0; i < count; ++i) {
        const auto kind = reader.readU8();
        const auto position = reader.readU32();
        const auto length = reader.readU32();
        requireEdit(length <= limits.maximumStringBytes, "edit record identity is too long");
        const auto rawId = reader.readBytes(length);
        const std::string_view id(reinterpret_cast<const char *>(rawId.data()), rawId.size());
        const auto data = reader.readBytes(reader.readU64());
        requireEdit(used.emplace(kind, id).second, "duplicate edit record");
        if (kind == RasterTilesRecord) {
            requireEdit(!id.empty() && position == 0 && !data.empty(), "invalid raster edit record");
        } else {
            requireEdit(kind <= static_cast<std::uint8_t>(detail::RecordKind::PublicationMetadata), "unknown edit record");
            const auto recordKind = static_cast<detail::RecordKind>(kind);
            requireEdit(!isSeparateCount(recordKind), "collection count is not an edit record");
            if (isCollection(recordKind))
                requireEdit(!id.empty() && (position != RemovedRecord || data.empty()), "invalid object edit record");
            else
                requireEdit(id.empty() && position == 0 && (!data.empty() || recordKind == detail::RecordKind::Metadata),
                            "invalid document edit record");
        }
        result.push_back({kind, id, position, data});
    }
    return result;
}

template<typename T, typename Identity, typename Read>
void editCollection(EditTransaction &transaction, std::vector<T> &values,
                    std::span<const EditRecord> edits, std::uint32_t maximum,
                    Identity identity, Read read)
{
    if (edits.empty()) return;
    const auto absent = values.size();
    std::map<std::string_view, std::size_t> prior;
    for (std::size_t i = 0; i < values.size(); ++i) prior.emplace(identity(values[i]), i);
    struct Placement {
        std::size_t original;
        std::uint32_t position;
        std::optional<T> data;
    };
    std::vector<Placement> changes;
    std::size_t size = values.size();
    std::vector<bool> moved(values.size());
    bool sameOrder = true;
    for (const auto &edit : edits) {
        const auto found = prior.find(edit.id);
        const auto original = found == prior.end() ? absent : found->second;
        if (original != absent) moved[original] = true;
        if (edit.position == RemovedRecord) {
            requireEdit(original != absent, "edit removes an unknown object");
            --size;
            sameOrder = false;
            continue;
        }
        requireEdit(original != absent || !edit.data.empty(), "new edit object has no data");
        if (original == absent) ++size;
        std::optional<T> value;
        if (!edit.data.empty()) {
            value = read(edit);
            requireEdit(identity(*value) == edit.id, "edit object identity does not match its payload");
        }
        sameOrder &= original != absent && edit.position == original;
        changes.push_back({original, edit.position, std::move(value)});
    }
    requireEdit(size <= maximum, "edit collection exceeds its object limit");
    if (sameOrder) {
        for (auto &change : changes)
            if (change.data) transaction.replace(values[change.original], std::move(*change.data));
        return;
    }
    struct Reordering {
        std::vector<T> originals;
        std::vector<std::pair<std::size_t, std::size_t>> retained;
    };
    auto rollback = std::make_shared<Reordering>();
    rollback->retained.reserve(size);
    std::vector<std::optional<T>> updated(size);
    std::vector<std::size_t> slots(size, absent);
    for (auto &change : changes) {
        requireEdit(change.position < size && !updated[change.position] && slots[change.position] == absent,
                    "edit collection contains an invalid object position");
        if (change.data) updated[change.position] = std::move(change.data);
        else slots[change.position] = change.original;
    }
    std::size_t original = 0;
    std::vector<T> reordered;
    reordered.reserve(size);
    for (std::size_t i = 0; i < size; ++i) {
        if (updated[i]) continue;
        if (slots[i] == absent) {
            while (original < absent && moved[original]) ++original;
            requireEdit(original != absent, "edit collection is missing a retained object");
            slots[i] = original++;
        }
        rollback->retained.emplace_back(slots[i], i);
    }
    transaction.onRollback([&values, rollback] {
        for (const auto &[old, current] : rollback->retained)
            rollback->originals[old] = std::move(values[current]);
        values.swap(rollback->originals);
    });
    static_assert(std::is_nothrow_move_constructible_v<T> && std::is_nothrow_move_assignable_v<T>);
    for (std::size_t i = 0; i < size; ++i)
        reordered.push_back(updated[i] ? std::move(*updated[i]) : std::move(values[slots[i]]));
    rollback->originals.swap(values);
    values.swap(reordered);
}

void applyRasterEdit(EditTransaction &transaction, RasterLayer &raster, ByteReader &reader,
                     DocumentReader &decoder)
{
    const auto count = reader.readU32();
    requireEdit(count <= reader.remaining() / 33, "invalid raster tile count");
    std::pair<int, int> last{-1, -1};
    for (std::uint32_t i = 0; i < count; ++i) {
        const int x = reader.readI32(), y = reader.readI32();
        requireEdit(x >= 0 && y >= 0 && x < raster.width && y < raster.height &&
                    x % EditTileSize == 0 && y % EditTileSize == 0 && last < std::pair{y, x},
                    "invalid raster edit tile position");
        last = {y, x};
        auto data = std::make_shared<RasterLayer>(decoder.readEditTile(
            std::min(EditTileSize, raster.width - x), std::min(EditTileSize, raster.height - y)));
        const auto exchange = [&raster, data, x, y] {
            for (int row = 0; row < data->height; ++row) {
                auto *first = data->pixels.data() + std::size_t(row) * data->width;
                auto *target = raster.pixels.data() + std::size_t(y + row) * raster.width + x;
                std::swap_ranges(first, first + data->width, target);
            }
        };
        transaction.onRollback(exchange);
        exchange();
    }
}

void applyChunkEdit(EditTransaction &transaction, ChunkedRasterAsset &asset, int extent,
                    ByteReader &reader, DocumentReader &decoder)
{
    const auto count = reader.readU32();
    requireEdit(count <= reader.remaining() / 9, "invalid raster chunk edit count");
    std::optional<std::pair<int, int>> last;
    // Keep existing chunk allocations available to the tile rollback. Structural
    // edits are applied together after all existing chunks have been patched.
    std::map<std::pair<int, int>, std::optional<RasterChunk>> structure;
    for (std::uint32_t i = 0; i < count; ++i) {
        const int column = reader.readI32(), row = reader.readI32();
        const auto key = std::pair{row, column};
        requireEdit(!last || *last < key, "raster chunk edits are not ordered");
        last = key;
        const auto it = std::ranges::lower_bound(asset.chunks, key, {}, chunkCoordinate);
        const bool exists = it != asset.chunks.end() && chunkCoordinate(*it) == key;
        const auto operation = reader.readU8();
        requireEdit(operation <= 2 && (operation == 1) != exists, "invalid raster chunk presence change");
        if (operation == 0) {
            structure.emplace(key, std::nullopt);
        } else if (operation == 1) {
            auto pixels = decoder.readEditTile(extent, extent);
            structure.emplace(key, RasterChunk{column, row, std::move(pixels)});
        } else {
            applyRasterEdit(transaction, it->pixels, reader, decoder);
        }
    }
    if (structure.empty()) return;
    struct RetainedChunks {
        std::vector<RasterChunk> old;
        std::vector<std::pair<std::size_t, std::size_t>> moved;
    };
    auto retained = std::make_shared<RetainedChunks>();
    retained->moved.reserve(asset.chunks.size());
    std::vector<RasterChunk> combined;
    combined.reserve(asset.chunks.size() + structure.size());
    transaction.onRollback([&asset, retained] {
        for (const auto &[old, current] : retained->moved)
            retained->old[old] = std::move(asset.chunks[current]);
        asset.chunks.swap(retained->old);
    });
    auto chunk = asset.chunks.begin();
    const auto retain = [&] {
        retained->moved.emplace_back(std::size_t(chunk - asset.chunks.begin()), combined.size());
        combined.push_back(std::move(*chunk++));
    };
    for (auto &[key, addition] : structure) {
        while (chunk != asset.chunks.end() && chunkCoordinate(*chunk) < key) retain();
        if (addition) combined.push_back(std::move(*addition));
        else ++chunk;
    }
    while (chunk != asset.chunks.end()) retain();
    retained->old.swap(asset.chunks);
    asset.chunks.swap(combined);
}
} // namespace

IiscEncodeResult encodeIiscEdit(const Document &before, const Document &after, SerializationLimits limits)
{
    const auto validation = validate(after);
    if (!validation.ok())
        return {{}, makeError(IiscErrorCode::InvalidDocument, 0, validation.issues.front().message)};
    if (auto error = checkDocumentLimits(after, limits); error.code != IiscErrorCode::None)
        return {{}, std::move(error)};
    using enum detail::RecordKind;
    std::set<std::string_view> tiled;
    std::map<std::string_view, std::vector<std::uint8_t>> rasterEdits;
    for (const auto &asset : after.assets) {
        const auto *old = findAsset(before, assetId(asset));
        if (!old || old->index() != asset.index()) continue;
        ByteWriter writer;
        if (const auto *dense = std::get_if<RasterAsset>(&asset)) {
            const auto &prior = std::get<RasterAsset>(*old).pixels;
            if (!sameExtent(prior, dense->pixels)) continue;
            tiled.insert(assetId(asset));
            if (!sameRasterPixels(prior, dense->pixels)) {
                writer.writeU8(0);
                writeRasterEdit(writer, prior, dense->pixels);
            }
        } else if (const auto *sparse = std::get_if<ChunkedRasterAsset>(&asset)) {
            if (before.infiniteCanvas.chunkSize != after.infiniteCanvas.chunkSize) continue;
            tiled.insert(assetId(asset));
            if (!unchangedAsset(asset, &before)) {
                writer.writeU8(1);
                writer.writeI32(after.infiniteCanvas.chunkSize);
                writeChunkEdit(writer, std::get<ChunkedRasterAsset>(*old), *sparse);
            }
        }
        if (writer.bytes().size() > sizeof(std::uint32_t)) rasterEdits.emplace(assetId(asset), writer.takeBytes());
    }
    std::vector<detail::DocumentRecord> source, target;
    ByteWriter recordWriter;
    writePayload(recordWriter, before, &source, &before, false, {}, nullptr, true);
    writePayload(recordWriter, after, &target, &before, false, {}, &tiled, true);
    std::map<RecordKey, const detail::DocumentRecord *> previous;
    for (const auto &item : source) previous.emplace(RecordKey{item.kind, item.id}, &item);
    ByteWriter payload;
    std::uint32_t count = 0;
    const auto append = [&](EditRecord record) { writeEditRecord(payload, record); ++count; };
    for (const auto &item : target) {
        auto found = previous.find({item.kind, item.id});
        const bool differs = item.data && (found == previous.end() || found->second->data != item.data);
        if (!isSeparateCount(item.kind) &&
            (differs || found == previous.end() || found->second->position != item.position)) {
            append({static_cast<std::uint8_t>(item.kind), item.id, item.position,
                    differs ? std::span<const std::uint8_t>(*item.data) : std::span<const std::uint8_t>{}});
        }
        if (found != previous.end()) previous.erase(found);
    }
    for (const auto &[key, item] : previous)
        if (isCollection(item->kind))
            append({static_cast<std::uint8_t>(item->kind), item->id, RemovedRecord, {}});
    for (const auto &[id, bytes] : rasterEdits) append({RasterTilesRecord, id, 0, bytes});
    return finishEdit(before, after, payload, count, limits);
}

IiscEncodeResult encodeIiscBitmapEdit(const Document &before, const Document &after, const std::string &id,
                                     std::optional<CanvasRegion> dirty, SerializationLimits limits)
{
    const auto *old = findAsset(before, id), *next = findAsset(after, id);
    if (!old || !next || old->index() != next->index() || before.formatVersion.major != after.formatVersion.major ||
        before.formatVersion.minor != after.formatVersion.minor)
        return encodeIiscEdit(before, after, limits);

    ByteWriter pixels;
    if (const auto *dense = std::get_if<RasterAsset>(next)) {
        const auto &prior = std::get<RasterAsset>(*old).pixels;
        if (!sameExtent(prior, dense->pixels)) return encodeIiscEdit(before, after, limits);
        pixels.writeU8(0);
        writeRasterEdit(pixels, prior, dense->pixels, dirty);
    } else if (const auto *chunked = std::get_if<ChunkedRasterAsset>(next)) {
        if (before.infiniteCanvas.chunkSize != after.infiniteCanvas.chunkSize)
            return encodeIiscEdit(before, after, limits);
        pixels.writeU8(1);
        pixels.writeI32(after.infiniteCanvas.chunkSize);
        writeChunkEdit(pixels, std::get<ChunkedRasterAsset>(*old), *chunked);
    } else return encodeIiscEdit(before, after, limits);

    ByteWriter payload;
    std::uint32_t count = 0;
    if (pixels.bytes().size() > (pixels.bytes().front() == 0 ? 5U : 9U)) {
        writeEditRecord(payload, {RasterTilesRecord, id, 0, pixels.bytes()});
        ++count;
    }
    if (before.authorship.dump() != after.authorship.dump()) {
        ByteWriter authorship;
        authorship.writeString(authorshipDump(after.authorship));
        writeEditRecord(payload, {static_cast<std::uint8_t>(detail::RecordKind::Authorship), {}, 0, authorship.bytes()});
        ++count;
    }
    return finishEdit(before, after, payload, count, limits);
}

IiscError applyIiscEdit(Document &document, std::span<const std::uint8_t> bytes, SerializationLimits limits,
                        const std::function<bool(const Document &)> &accept)
{
    try {
        if (bytes.size() > limits.maximumContainerBytes)
            return makeError(IiscErrorCode::LimitExceeded, 0, "edit exceeds its byte budget");
        ByteReader reader(bytes);
        requireEdit(std::ranges::equal(reader.readBytes(EditMagic.size()), EditMagic), "unknown edit format");
        const FormatVersion before{reader.readU16(), reader.readU16()};
        const FormatVersion after{reader.readU16(), reader.readU16()};
        requireEdit(before.major == document.formatVersion.major && before.minor == document.formatVersion.minor &&
                    after.major == CurrentFormatMajor && after.minor <= CurrentFormatMinor, "edit format version mismatch");
        const auto checksum = reader.readU32();
        requireEdit(bytes.size() >= 20 && crc32(bytes.subspan(20)) == checksum, "invalid edit checksum");
        const auto records = readEditRecords(reader, limits);
        requireEdit(reader.empty(), "edit has trailing data");

        using enum detail::RecordKind;
        const auto each = [&](detail::RecordKind kind) {
            std::vector<EditRecord> result;
            for (const auto &record : records)
                if (record.kind == static_cast<std::uint8_t>(kind)) result.push_back(record);
            return result;
        };
        EditTransaction transaction;
        const auto decode = [&](const EditRecord &record, const auto &read) {
            ByteReader recordReader(record.data);
            DocumentReader decoder(recordReader, limits);
            auto result = read(decoder);
            requireEdit(recordReader.empty(), "edit record has trailing data");
            return result;
        };
        const auto header = each(Header);
        requireEdit(!header.empty() || before.minor == after.minor, "version change is missing its header");
        if (!header.empty()) {
            ByteReader info(header.front().data);
            requireEdit(info.readU16() == after.major && info.readU16() == after.minor, "edit header version mismatch");
            const int chunkSize = info.readI32();
            Document result;
            DocumentReader decoder(info, limits);
            decoder.readHeader(result, after);
            info.readU32(); // Asset count is determined by insertion and removal events.
            requireEdit(info.empty() && (result.canvasMode != CanvasMode::Infinite ||
                        chunkSize == result.infiniteCanvas.chunkSize), "invalid edit canvas geometry");
            result.infiniteCanvas.chunkSize = chunkSize;
            transaction.replace(document.extent, result.extent);
            transaction.replace(document.canvasMode, result.canvasMode);
            transaction.replace(document.infiniteCanvas, result.infiniteCanvas);
            transaction.replace(document.timeline, result.timeline);
        }
        transaction.replace(document.formatVersion, after);
        editCollection(transaction, document.assets, each(Asset), limits.maximumAssets,
            &assetId, [&](const auto &record) { return decode(record, [&](auto &d) { return d.readAsset(after); }); });
        for (const auto &record : records) {
            if (record.kind != RasterTilesRecord) continue;
            auto *asset = findAsset(document, std::string(record.id));
            requireEdit(asset, "raster edit asset does not exist");
            ByteReader rasterReader(record.data);
            DocumentReader decoder(rasterReader, limits);
            const auto kind = rasterReader.readU8();
            if (auto *dense = std::get_if<RasterAsset>(asset); dense && kind == 0) {
                applyRasterEdit(transaction, dense->pixels, rasterReader, decoder);
            } else if (auto *sparse = std::get_if<ChunkedRasterAsset>(asset); sparse && kind == 1) {
                const int size = rasterReader.readI32();
                requireEdit(size == document.infiniteCanvas.chunkSize, "raster edit chunk size does not match");
                applyChunkEdit(transaction, *sparse, size, rasterReader, decoder);
            } else requireEdit(false, "raster edit asset type does not match");
            requireEdit(rasterReader.empty(), "raster edit record has trailing data");
        }

        auto layers = each(Layer);
        std::set<std::string_view> changedSources;
        std::map<FrameIndex, std::vector<Keyframe>> keys;
        for (const auto &record : layers)
            if (record.position == RemovedRecord || !record.data.empty()) changedSources.insert(record.id);
        editCollection(transaction, document.layers, layers, limits.maximumLayers,
            [](const auto &layer) -> const std::string & { return layerProperties(layer).id; },
            [&](const auto &record) { return decode(record, [&](auto &d) { return d.readEditedLayer(document, after, keys); }); });
        if (!changedSources.empty()) {
            // Preserve the frame index and key storage for property-only edits.
            bool sameKeys = true;
            for (const auto &frame : document.frames) {
                const auto next = keys.find(frame.index);
                const auto expected = next == keys.end() ? 0 : next->second.size();
                std::size_t retained = 0;
                for (const auto &key : frame.keyframes) {
                    if (!changedSources.contains(key.layerId)) continue;
                    ++retained;
                    sameKeys &= next != keys.end() && std::ranges::any_of(next->second, [&](const Keyframe &item) {
                        return item.layerId == key.layerId && item.assetId == key.assetId;
                    });
                }
                sameKeys &= retained == expected;
                if (!sameKeys) break;
            }
            if (sameKeys) for (const auto &[index, entries] : keys)
                sameKeys &= std::ranges::binary_search(document.frames, index, {}, &Frame::index);
            if (!sameKeys) {
                auto frames = document.frames;
                for (auto &frame : frames) {
                    const auto next = keys.find(frame.index);
                    if (next == keys.end() && !std::ranges::any_of(frame.keyframes, [&](const auto &key) {
                            return changedSources.contains(key.layerId);
                        })) continue;
                    std::erase_if(frame.keyframes, [&](const auto &key) { return changedSources.contains(key.layerId); });
                    if (next != keys.end()) {
                        std::ranges::move(next->second, std::back_inserter(frame.keyframes));
                        keys.erase(next);
                        std::ranges::sort(frame.keyframes, {}, &Keyframe::layerId);
                    }
                }
                std::erase_if(frames, [](const auto &frame) { return frame.keyframes.empty(); });
                for (auto &[frame, values] : keys) {
                    std::ranges::sort(values, {}, &Keyframe::layerId);
                    frames.push_back({frame, std::move(values)});
                }
                std::ranges::sort(frames, {}, &Frame::index);
                transaction.replace(document.frames, std::move(frames));
            }
        }
        const auto optional = [&](detail::RecordKind kind, auto &value, const auto &read) {
            const auto found = each(kind);
            if (!found.empty()) transaction.replace(value, decode(found.front(), read));
        };
        optional(Metadata, document.stableDiffusionMetadata, [&](auto &d) { return d.readOptionalMetadata(after); });
        optional(Authorship, document.authorship, [](auto &d) { return d.readAuthorship(); });
        if (after.minor < 5 && before.minor >= 5)
            transaction.replace(document.authorship, DocumentAuthorship{});
        optional(PublicationMetadata, document.publicationMetadata, [&](auto &d) { return d.readOptionalPublication(after); });
        if (after.minor < 22 && before.minor >= 22)
            transaction.replace(document.publicationMetadata, std::optional<iiSharedCanvas::PublicationMetadata>{});
        editCollection(transaction, document.audioAssets, each(AudioAsset), limits.maximumAudioAssets,
            [](const iiSharedCanvas::AudioAsset &asset) -> const std::string & { return asset.id; },
            [&](const auto &record) { return decode(record, [](auto &d) { return d.readAudioAsset(); }); });
        editCollection(transaction, document.audioTracks, each(AudioTrack), limits.maximumAudioTracks,
            [](const auto &track) -> const std::string & { return track.id; },
            [&](const auto &record) { return decode(record, [](auto &d) { return d.readAudioTrack(); }); });
        editCollection(transaction, document.artboards, each(Artboard), limits.maximumArtboards,
            [](const auto &board) -> const std::string & { return board.id; },
            [&](const auto &record) { return decode(record, [](auto &d) { return d.readArtboard(); }); });

        const auto validation = validate(document);
        if (!validation.ok()) return makeError(IiscErrorCode::InvalidDocument, 0, validation.issues.front().message);
        if (auto error = checkDocumentLimits(document, limits); error.code != IiscErrorCode::None) return error;
        if (accept && !accept(document)) return makeError(IiscErrorCode::InvalidDocument, 0, "edit was rejected");
        transaction.finish();
        return {};
    } catch (const DecodeFailure &failure) {
        return makeError(failure.code, failure.offset, failure.what());
    } catch (const std::exception &failure) {
        return makeError(IiscErrorCode::InvalidDocument, 0, failure.what());
    }
}

IiscError describeIiscEdit(std::span<const std::uint8_t> bytes, IiscEditImpact &impact, SerializationLimits limits)
{
    try {
        if (bytes.size() > limits.maximumContainerBytes)
            return makeError(IiscErrorCode::LimitExceeded, 0, "edit exceeds its byte budget");
        ByteReader reader(bytes);
        requireEdit(std::ranges::equal(reader.readBytes(EditMagic.size()), EditMagic), "unknown edit format");
        reader.readBytes(8); // Base and target model versions.
        const auto checksum = reader.readU32();
        requireEdit(bytes.size() >= 20 && crc32(bytes.subspan(20)) == checksum, "invalid edit checksum");
        const auto records = readEditRecords(reader, limits);
        requireEdit(reader.empty(), "edit has trailing data");
        IiscEditImpact result;
        result.changed = !records.empty();
        using enum detail::RecordKind;
        for (const auto &record : records) {
            const auto kind = static_cast<detail::RecordKind>(record.kind);
            if (kind == Header) result.header = true;
            if (kind == Layer) result.layers.emplace_back(record.id);
            if (kind == Asset && (!record.data.empty() || record.position == RemovedRecord))
                result.assets.emplace_back(record.id);
            if (kind == AudioTrack) result.audioTracks.emplace_back(record.id);
            if (kind == Artboard) result.artboards.emplace_back(record.id);
            if (record.kind != RasterTilesRecord) continue;

            ByteReader raster(record.data);
            IiscRasterChange pixels{std::string(record.id), {}};
            const auto add = [&](std::int64_t x, std::int64_t y, int w, int h) {
                requireEdit(x >= INT32_MIN && x <= INT32_MAX && y >= INT32_MIN && y <= INT32_MAX &&
                            w > 0 && h > 0, "invalid raster edit bounds");
                pixels.regions.push_back({{static_cast<int>(x), static_cast<int>(y)}, {w,h}});
            };
            const auto rasterExtent = [&]() {
                const int w = raster.readI32(), h = raster.readI32();
                raster.readBytes(9); // Pixel count and encoding tag.
                raster.readBytes(raster.readU64());
                return CanvasExtent{w,h};
            };
            const auto tiles = [&](std::int64_t left, std::int64_t top) {
                const auto count = raster.readU32();
                requireEdit(count <= raster.remaining() / 33, "invalid raster tile count");
                for (std::uint32_t i = 0; i < count; ++i) {
                    const int x = raster.readI32(), y = raster.readI32();
                    const auto extent = rasterExtent();
                    add(left+x, top+y, extent.width, extent.height);
                }
            };
            const auto storage = raster.readU8();
            if (storage == 0) {
                tiles(0, 0);
            } else {
                requireEdit(storage == 1, "unknown raster edit storage");
                const int size = raster.readI32();
                requireEdit(size > 0, "invalid raster edit chunk size");
                const auto count = raster.readU32();
                requireEdit(count <= raster.remaining() / 9, "invalid raster chunk edit count");
                for (std::uint32_t i = 0; i < count; ++i) {
                    const auto left = std::int64_t(raster.readI32()) * size;
                    const auto top = std::int64_t(raster.readI32()) * size;
                    const auto operation = raster.readU8();
                    if (operation == 2) tiles(left, top);
                    else {
                        requireEdit(operation <= 1, "invalid raster chunk edit operation");
                        if (operation == 1) rasterExtent();
                        add(left, top, size, size);
                    }
                }
            }
            requireEdit(raster.empty(), "raster edit record has trailing data");
            result.pixels.push_back(std::move(pixels));
        }
        impact = std::move(result);
        return {};
    } catch (const DecodeFailure &failure) {
        return makeError(failure.code, failure.offset, failure.what());
    } catch (const std::exception &failure) {
        return makeError(IiscErrorCode::InvalidDocument, 0, failure.what());
    }
}
