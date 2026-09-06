#include <iiSharedCanvas.h>
#include <File/DocumentFile.h>

#include <QDir>
#include <QFile>
#include <QTemporaryDir>

#include <sqlite3.h>

#include <algorithm>
#include <cstdint>
#include <iostream>
#include <limits>
#include <span>
#include <string>
#include <vector>

namespace {
using namespace iiSharedCanvas;
int failures = 0;

void expect(bool condition, const std::string &message)
{
    if (!condition) {
        std::cerr << message << '\n';
        ++failures;
    }
}

Document audioDocument()
{
    Document document;
    document.extent = {1, 1};
    document.timeline = {{24, 1}, 48};
    AudioAsset asset;
    asset.id = "pcm-stereo";
    asset.samples.resize(48000 * 2 * 3);
    for (std::size_t index = 0; index < asset.samples.size(); ++index) {
        asset.samples[index] = static_cast<std::int16_t>(index % 32767);
    }
    asset.samples[0] = -32768;
    asset.samples[1] = 32767;
    document.audioAssets.push_back(std::move(asset));
    document.audioTracks.push_back({"dialogue", "대사", false, -3.0,
        {{"clip-1", "First take", "pcm-stereo", 4, 12, 123, -6.0, true},
         {"clip-2", "Disabled take", "pcm-stereo", 24, 12, 48000, 1.5, false}}});
    document.audioTracks.push_back({"music", "Music", true, -12.0,
        {{"clip-3", "Muted track", "pcm-stereo", 0, 48, 0, 0.0, true}}});
    return document;
}

void overwrite(std::vector<std::uint8_t> &bytes, std::size_t offset,
               std::uint64_t value, std::size_t width)
{
    for (std::size_t index = 0; index < width; ++index) {
        bytes[offset + index] = static_cast<std::uint8_t>(value >> (index * 8));
    }
}

void checksum(std::vector<std::uint8_t> &bytes)
{
    std::uint32_t crc = 0xffffffffU;
    for (const auto byte : std::span(bytes).subspan(IiscHeaderSize)) {
        crc ^= byte;
        for (int bit = 0; bit < 8; ++bit) {
            crc = (crc >> 1U) ^ (0xedb88320U & (0U - (crc & 1U)));
        }
    }
    overwrite(bytes, 24, crc ^ 0xffffffffU, 4);
}

std::string assetDigest(const std::string &path)
{
    sqlite3 *database = nullptr;
    sqlite3_stmt *query = nullptr;
    std::string result;
    if (sqlite3_open_v2(path.c_str(), &database, SQLITE_OPEN_READONLY, nullptr) == SQLITE_OK
        && sqlite3_prepare_v2(database,
            "SELECT hex(digest) FROM canvas_records WHERE kind=6 AND id='pcm-stereo'",
            -1, &query, nullptr) == SQLITE_OK
        && sqlite3_step(query) == SQLITE_ROW) {
        result = reinterpret_cast<const char *>(sqlite3_column_text(query, 0));
    }
    sqlite3_finalize(query);
    sqlite3_close(database);
    return result;
}

bool mutateRecords(const std::string &path, const char *sql)
{
    sqlite3 *database = nullptr;
    const bool opened = sqlite3_open_v2(path.c_str(), &database,
                                       SQLITE_OPEN_READWRITE, nullptr) == SQLITE_OK;
    const bool changed = opened && sqlite3_exec(database, sql, nullptr, nullptr, nullptr) == SQLITE_OK;
    sqlite3_close(database);
    return changed;
}
} // namespace

int main()
{
    const Document original = audioDocument();
    const auto encoded = encodeIisc(original);
    expect(encoded.ok(), "native audio fixture must encode: " + encoded.error.message);
    if (!encoded.ok()) return 1;
    const auto decoded = decodeIisc(encoded.bytes);
    expect(decoded.ok(), "native audio fixture must decode: " + decoded.error.message);
    expect(decoded.ok() && encodeIisc(decoded.document).bytes == encoded.bytes,
           "PCM samples, channel order, sample trims, clip intervals, mute, gain and enabled state must round-trip canonically");
    if (decoded.ok()) {
        expect(decoded.document.audioAssets.front().samples == original.audioAssets.front().samples,
               "signed interleaved PCM16 including both extrema must remain byte-exact");
    }
    Document withMetadata = original;
    StableDiffusionMetadata metadata;
    metadata.positivePrompt = "existing generation metadata";
    withMetadata.stableDiffusionMetadata = metadata;
    const auto metadataBytes = encodeIisc(withMetadata);
    const auto metadataRead = decodeIisc(metadataBytes.bytes);
    expect(metadataBytes.ok() && metadataRead.ok()
               && metadataRead.document.stableDiffusionMetadata == withMetadata.stableDiffusionMetadata
               && metadataRead.document.audioAssets == original.audioAssets
               && metadataRead.document.audioTracks == original.audioTracks,
           "the new audio tail must coexist with the complete existing metadata record");
    for (std::uint16_t minor = 0; minor < 4; ++minor) {
        Document legacy;
        legacy.extent = {1, 1};
        legacy.formatVersion = {1, minor};
        const auto bytes = encodeIisc(legacy);
        const auto read = decodeIisc(bytes.bytes);
        expect(bytes.ok() && read.ok() && read.document.audioAssets.empty()
                   && read.document.audioTracks.empty()
                   && encodeIisc(read.document).bytes == bytes.bytes,
               "legacy snapshots must decode with empty audio and preserve exact version bytes");
        Document unsupported = original;
        unsupported.formatVersion = {1, minor};
        expect(encodeIisc(unsupported).error.code == IiscErrorCode::InvalidDocument,
               "legacy encoders must reject audio instead of silently dropping it");
    }
    const auto limitCheck = [&](SerializationLimits limits) {
        expect(encodeIisc(original, limits).error.code == IiscErrorCode::LimitExceeded
                   && decodeIisc(encoded.bytes, limits).error.code == IiscErrorCode::LimitExceeded,
               "audio resource limits must reject oversized data symmetrically");
    };
    SerializationLimits limits;
    limits.maximumAudioAssets = 0;
    limitCheck(limits);
    limits = {};
    limits.maximumAudioTracks = 1;
    limitCheck(limits);
    limits = {};
    limits.maximumTotalAudioClips = 2;
    limitCheck(limits);
    limits = {};
    limits.maximumTotalAudioSamples = original.audioAssets.front().samples.size() - 1;
    limitCheck(limits);

    // Empty native visual/metadata prefix is 30 bytes for model 1.4.
    const std::size_t audioCountOffset = IiscHeaderSize + 30;
    const std::size_t sampleCountOffset = audioCountOffset + 4 + 4
        + original.audioAssets.front().id.size() + 4 + 2;
    auto malicious = encoded.bytes;
    overwrite(malicious, sampleCountOffset, std::numeric_limits<std::uint64_t>::max(), 8);
    checksum(malicious);
    expect(decodeIisc(malicious).error.code == IiscErrorCode::LimitExceeded,
           "malicious sample counts must fail before PCM allocation or multiplication");
    auto truncated = encoded.bytes;
    truncated.resize(sampleCountOffset + 8 + 3);
    overwrite(truncated, 16, truncated.size() - IiscHeaderSize, 8);
    checksum(truncated);
    expect(decodeIisc(truncated).error.code == IiscErrorCode::TruncatedData,
           "short PCM payloads must fail before allocating the declared buffer");
    auto invalidBoolean = encoded.bytes;
    invalidBoolean.back() = 2;
    checksum(invalidBoolean);
    expect(decodeIisc(invalidBoolean).error.code == IiscErrorCode::InvalidData,
           "audio booleans must reject noncanonical encodings");

    const QString outputRoot = QString::fromUtf8(IISHAREDCANVAS_TEST_OUTPUT_DIR);
    expect(QDir().mkpath(outputRoot), "working-file test output root must exist");
    QTemporaryDir directory(outputRoot + "/audio-persistence-XXXXXX");
    expect(directory.isValid(), "working-file fixture directory must exist");
    const std::string path = directory.filePath("audio.iisc").toStdString();
    DocumentFile file;
    expect(file.create(path, original).ok(), "working files must create complete native audio records");
    if (!file.isOpen()) return 1;
    const auto priorDigest = assetDigest(path);
    expect(!priorDigest.empty(), "PCM assets must own independent working-file records");
    expect(file.edit([](Document &draft) {
        draft.audioTracks.front().gainDb = -9.0;
        draft.audioTracks.front().clips.front().sourceOffsetSamples = 500;
        return true;
    }).ok(), "audio edits must commit through the normal working-file transaction");
    expect(file.lastWriteStatistics().recordsWritten == 1
               && file.lastWriteStatistics().payloadBytesWritten < 32
               && assetDigest(path) == priorDigest,
           "track-only edits must patch one record without rewriting PCM payloads");
    const auto committed = encodeIisc(*file.document()).bytes;
    const auto revision = file.revision();
    const auto unchanged = file.edit([](Document &) { return true; });
    expect(unchanged.ok() && !unchanged.changed && file.revision() == revision
               && file.lastWriteStatistics().recordsWritten == 0,
           "unchanged audio must perform no payload writes and preserve the revision");
    const auto rejected = file.edit([](Document &draft) {
        draft.audioTracks.front().clips.front().assetId = "missing";
        return true;
    });
    expect(!rejected.ok() && file.revision() == revision
               && encodeIisc(*file.document()).bytes == committed,
           "invalid audio references must preserve committed data and revision");
    expect(!file.edit([](Document &draft) {
        draft.audioAssets.front().samples[0] = 0;
        return false;
    }).ok() && file.revision() == revision,
           "rejected PCM edits must roll back without changing the revision");
    file.close();
    expect(file.open(path).ok() && encodeIisc(*file.document()).bytes == committed,
           "reopening must observe the accepted transaction and none of the rejected drafts");
    expect(file.edit([](Document &draft) {
        draft.audioAssets.front().samples[0] = -32767;
        return true;
    }).ok() && file.lastWriteStatistics().recordsWritten == 1
               && file.lastWriteStatistics().payloadBytesWritten <= 2,
           "a one-sample edit must incrementally patch its independent PCM record");
    const auto updated = encodeIisc(*file.document()).bytes;
    file.close();
    expect(file.open(path).ok() && encodeIisc(*file.document()).bytes == updated,
           "incremental signed PCM edits must survive reopening");
    file.close();
    limits = {};
    limits.maximumTotalAudioSamples = 1;
    expect(file.open(path, limits).code == DocumentFileCode::LimitExceeded,
           "working-file readers must enforce the same PCM allocation limit");
    const auto missingCounts = directory.filePath("missing-count.iisc");
    expect(QFile::copy(QString::fromStdString(path), missingCounts)
               && mutateRecords(missingCounts.toStdString(),
                                "DELETE FROM canvas_records WHERE kind=5")
               && file.open(missingCounts.toStdString()).code == DocumentFileCode::CorruptFile,
           "model 1.4 working files require audio count records and must not drop orphan PCM");
    const auto wrongIdentity = directory.filePath("wrong-id.iisc");
    expect(QFile::copy(QString::fromStdString(path), wrongIdentity)
               && mutateRecords(wrongIdentity.toStdString(),
                                "UPDATE canvas_records SET id='wrong' WHERE kind=6")
               && file.open(wrongIdentity.toStdString()).code == DocumentFileCode::CorruptFile,
           "audio record identities must match their checksummed PCM payload identities");
    return failures == 0 ? 0 : 1;
}
