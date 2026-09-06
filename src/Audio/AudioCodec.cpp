#include "AudioCodec.h"
#include "Media/MediaIo_p.hpp"

#include <bit>
#include <cstring>
#include <limits>
#include <new>
#include <stdexcept>

namespace iiSharedCanvas {
namespace {
struct Failure { MediaIoCode code; const char *message; };
[[noreturn]] void fail(MediaIoCode code, const char *message) { throw Failure{code, message}; }
std::uint16_t u16(std::span<const std::uint8_t> b, std::size_t p)
{ return std::uint16_t(b[p]) | (std::uint16_t(b[p + 1]) << 8); }
std::uint32_t u32(std::span<const std::uint8_t> b, std::size_t p)
{ return std::uint32_t(u16(b, p)) | (std::uint32_t(u16(b, p + 2)) << 16); }
bool tag(std::span<const std::uint8_t> b, std::size_t p, const char *s)
{ return b.size() - p >= 4 && std::memcmp(b.data() + p, s, 4) == 0; }
void put16(std::vector<std::uint8_t> &b, std::uint16_t n)
{ b.push_back(n & 255); b.push_back(n >> 8); }
void put32(std::vector<std::uint8_t> &b, std::uint32_t n)
{ put16(b, n & 65535); put16(b, n >> 16); }
void putTag(std::vector<std::uint8_t> &b, const char *s) { b.insert(b.end(), s, s + 4); }
bool validFormat(std::uint32_t rate, std::uint16_t channels)
{ return rate >= 8000 && rate <= 192000 && (channels == 1 || channels == 2); }
}

AudioImportResult decodeAudioWav(std::span<const std::uint8_t> bytes, const AudioImportOptions &options)
{
    AudioImportResult result;
    try {
        if (options.assetId.empty()) { fail(MediaIoCode::InvalidArgument, "audio asset id must not be empty"); }
        if (bytes.size() > options.limits.maxInputBytes) { fail(MediaIoCode::LimitExceeded, "WAV exceeds input budget"); }
        if (bytes.size() < 12 || !tag(bytes, 0, "RIFF") || !tag(bytes, 8, "WAVE")) {
            fail(MediaIoCode::UnsupportedFormat, "expected a RIFF/WAVE PCM16 file");
        }
        if (std::uint64_t(u32(bytes, 4)) + 8 != bytes.size()) { fail(MediaIoCode::InvalidData, "invalid RIFF byte count"); }
        bool haveFormat = false, haveData = false, omittedMetadata = false;
        std::uint32_t rate = 0; std::uint16_t channels = 0;
        std::span<const std::uint8_t> pcm;
        for (std::size_t p = 12; p < bytes.size();) {
            if (bytes.size() - p < 8) { fail(MediaIoCode::InvalidData, "truncated WAV chunk header"); }
            const auto size = u32(bytes, p + 4);
            const auto paddedSize = std::uint64_t(size) + (size & 1);
            if (paddedSize > bytes.size() - p - 8) { fail(MediaIoCode::InvalidData, "truncated WAV chunk payload"); }
            if (tag(bytes, p, "fmt ")) {
                if (haveFormat || size < 16) { fail(MediaIoCode::InvalidData, "invalid or duplicate WAV format chunk"); }
                const auto f = bytes.subspan(p + 8, size);
                if (u16(f, 0) != 1 || u16(f, 14) != 16) { fail(MediaIoCode::UnsupportedFeature, "only uncompressed signed PCM16 WAV is supported"); }
                channels = u16(f, 2); rate = u32(f, 4);
                if (!validFormat(rate, channels)) { fail(MediaIoCode::UnsupportedFeature, "WAV requires mono/stereo and 8000..192000 Hz"); }
                if ((size != 16 && (size != 18 || u16(f, 16) != 0))
                    || u16(f, 12) != channels * 2 || u32(f, 8) != rate * channels * 2) {
                    fail(MediaIoCode::InvalidData, "inconsistent PCM format or alignment");
                }
                haveFormat = true;
            } else if (tag(bytes, p, "data")) {
                if (haveData || !haveFormat) { fail(MediaIoCode::InvalidData, "WAV must have one data chunk after its format"); }
                pcm = bytes.subspan(p + 8, size); haveData = true;
            } else { omittedMetadata = true; }
            p += 8 + static_cast<std::size_t>(paddedSize);
        }
        if (!haveFormat || !haveData || pcm.empty() || pcm.size() % (channels * 2) != 0) {
            fail(MediaIoCode::InvalidData, "WAV has no complete PCM sample frames");
        }
        if (pcm.size() > options.limits.maxDecodedBytes) { fail(MediaIoCode::LimitExceeded, "PCM exceeds decoded audio budget"); }
        AudioAsset asset; asset.id = options.assetId; asset.sampleRate = rate; asset.channelCount = channels;
        asset.samples.resize(pcm.size() / 2);
        for (std::size_t i = 0; i < asset.samples.size(); ++i) { asset.samples[i] = std::bit_cast<std::int16_t>(u16(pcm, i * 2)); }
        result.asset = std::move(asset);
        if (omittedMetadata) { result.result.warnings.push_back("WAV ancillary chunks are not stored; PCM samples and format are preserved"); }
    } catch (const Failure &e) { result.result = {e.code, e.message, {}}; }
    catch (const std::bad_alloc &) { result.result = {MediaIoCode::LimitExceeded, "audio allocation failed", {}}; }
    catch (const std::length_error &) { result.result = {MediaIoCode::LimitExceeded, "audio allocation is too large", {}}; }
    return result;
}

AudioImportResult importAudioWav(const std::string &path, const AudioImportOptions &options)
{
    auto bytes = media_detail::readFile(path, options.limits);
    if (!bytes.ok()) { return {{}, std::move(bytes.result)}; }
    return decodeAudioWav(bytes.bytes, options);
}

MediaBytesResult encodeAudioWav(const AudioAsset &asset, const MediaLimits &limits)
{
    MediaBytesResult result;
    try {
        if (!validFormat(asset.sampleRate, asset.channelCount) || asset.samples.empty()
            || asset.samples.size() % asset.channelCount != 0) {
            fail(MediaIoCode::InvalidArgument, "audio requires complete mono/stereo PCM16 frames at 8000..192000 Hz");
        }
        if (asset.samples.size() > (std::numeric_limits<std::uint32_t>::max() - 36ULL) / 2
            || limits.maxOutputBytes < 44 || asset.samples.size() > (limits.maxOutputBytes - 44) / 2
            || limits.maxDecodedBytes < 44 || asset.samples.size() > (limits.maxDecodedBytes - 44) / 2) {
            fail(MediaIoCode::LimitExceeded, "WAV exceeds RIFF or configured byte limit");
        }
        const auto dataBytes = static_cast<std::uint32_t>(asset.samples.size() * 2);
        auto &b = result.bytes; b.reserve(std::size_t(dataBytes) + 44);
        putTag(b, "RIFF"); put32(b, dataBytes + 36); putTag(b, "WAVE");
        putTag(b, "fmt "); put32(b, 16); put16(b, 1); put16(b, asset.channelCount);
        put32(b, asset.sampleRate); put32(b, asset.sampleRate * asset.channelCount * 2);
        put16(b, asset.channelCount * 2); put16(b, 16); putTag(b, "data"); put32(b, dataBytes);
        for (const auto sample : asset.samples) { put16(b, std::bit_cast<std::uint16_t>(sample)); }
    } catch (const Failure &e) { result = {{}, {e.code, e.message, {}}}; }
    catch (const std::bad_alloc &) { result = {{}, {MediaIoCode::LimitExceeded, "audio allocation failed", {}}}; }
    catch (const std::length_error &) { result = {{}, {MediaIoCode::LimitExceeded, "audio allocation is too large", {}}}; }
    return result;
}

MediaIoResult exportAudioWav(const AudioAsset &asset, const std::string &path, const MediaLimits &limits)
{
    const auto encoded = encodeAudioWav(asset, limits);
    if (!encoded.ok()) { return encoded.result; }
    return media_detail::writeFile(path, encoded.bytes, false, limits);
}
} // namespace iiSharedCanvas
