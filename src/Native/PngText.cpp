#include "Native/PngText_p.hpp"
#include "Native/Media_p.hpp"

#include <zlib.h>

#include <algorithm>
#include <array>
#include <limits>
#include <set>
#include <string_view>

namespace iiSharedCanvas::media_detail {
namespace {

MediaIoResult invalid(std::string message)
{
    return error(MediaIoCode::InvalidData, std::move(message));
}

std::uint32_t be32(std::span<const std::uint8_t> bytes, std::size_t offset)
{
    return (std::uint32_t(bytes[offset]) << 24) | (std::uint32_t(bytes[offset + 1]) << 16)
        | (std::uint32_t(bytes[offset + 2]) << 8) | bytes[offset + 3];
}

void append32(std::vector<std::uint8_t> &out, std::uint32_t value)
{
    out.push_back(std::uint8_t(value >> 24)); out.push_back(std::uint8_t(value >> 16));
    out.push_back(std::uint8_t(value >> 8)); out.push_back(std::uint8_t(value));
}

std::uint32_t checksum(std::span<const std::uint8_t> bytes)
{
    auto result = crc32(0L, Z_NULL, 0);
    while (!bytes.empty()) {
        const auto count = std::min<std::size_t>(bytes.size(), 65536);
        result = crc32(result, bytes.data(), uInt(count));
        bytes = bytes.subspan(count);
    }
    return std::uint32_t(result);
}

// PNG keywords are Latin-1; the public interface consistently uses UTF-8.
std::string latin1ToUtf8(std::span<const std::uint8_t> value)
{
    std::string result;
    for (const auto byte : value) {
        if (byte < 0x80) result.push_back(char(byte));
        else { result.push_back(char(0xc0 | (byte >> 6))); result.push_back(char(0x80 | (byte & 0x3f))); }
    }
    return result;
}

bool keyword(std::string_view utf8, std::string &latin)
{
    if (!validText(utf8)) return false;
    latin.clear();
    for (std::size_t i = 0; i < utf8.size();) {
        const auto first = static_cast<unsigned char>(utf8[i++]);
        unsigned value = first;
        if (first >= 0x80) {
            if (first != 0xc2 && first != 0xc3) return false;
            if (i == utf8.size()) return false;
            value = ((first & 0x1f) << 6) | (static_cast<unsigned char>(utf8[i++]) & 0x3f);
        }
        if (value < 32 || (value >= 127 && value <= 160)) return false;
        latin.push_back(char(value));
    }
    return !latin.empty() && latin.size() <= 79 && latin.front() != ' '
        && latin.back() != ' ' && latin.find("  ") == latin.npos;
}

MediaIoResult inflateText(std::span<const std::uint8_t> input, std::uint64_t limit, std::string &output)
{
    if (input.size() > std::numeric_limits<uInt>::max()) return error(MediaIoCode::LimitExceeded, "PNG compressed text is too large");
    z_stream stream{};
    if (inflateInit(&stream) != Z_OK) return invalid("Cannot initialize PNG text decompression");
    struct Guard { z_stream *stream; ~Guard() { inflateEnd(stream); } } guard{&stream};
    stream.next_in = const_cast<Bytef *>(input.data());
    stream.avail_in = uInt(input.size());
    std::array<char, 4096> buffer{};
    for (;;) {
        stream.next_out = reinterpret_cast<Bytef *>(buffer.data());
        stream.avail_out = uInt(buffer.size());
        const auto status = inflate(&stream, Z_NO_FLUSH);
        const auto count = buffer.size() - stream.avail_out;
        if (count > limit || output.size() > limit - count)
            return error(MediaIoCode::LimitExceeded, "PNG text exceeds the decoded byte limit");
        output.append(buffer.data(), count);
        if (status == Z_STREAM_END)
            return stream.avail_in == 0 ? MediaIoResult{} : invalid("Trailing compressed PNG text data");
        if (status != Z_OK || (!stream.avail_in && !count)) return invalid("Malformed compressed PNG text");
    }
}

MediaIoResult decodeText(std::uint32_t type, std::span<const std::uint8_t> payload,
                         std::uint64_t limit, MediaTextEntry &entry)
{
    const auto separator = std::find(payload.begin(), payload.end(), std::uint8_t(0));
    if (separator == payload.end()) return invalid("PNG text keyword is unterminated");
    const auto keySize = std::size_t(separator - payload.begin());
    entry.key = latin1ToUtf8(payload.first(keySize));
    std::string checked;
    if (!keyword(entry.key, checked)) return invalid("Invalid PNG text keyword");
    payload = payload.subspan(keySize + 1);
    std::string value;
    constexpr std::uint32_t text = 0x74455874, zipped = 0x7a545874, international = 0x69545874;
    if (type == text) {
        if (std::find(payload.begin(), payload.end(), 0) != payload.end()) return invalid("PNG text contains NUL");
        if (payload.size() > limit) return error(MediaIoCode::LimitExceeded, "PNG text exceeds the decoded byte limit");
        value = latin1ToUtf8(payload);
    } else if (type == zipped) {
        if (payload.empty() || payload.front() != 0) return invalid("Unsupported PNG text compression");
        std::string raw;
        auto result = inflateText(payload.subspan(1), limit, raw);
        if (!result.ok()) return result;
        if (raw.find('\0') != raw.npos) return invalid("PNG text contains NUL");
        value = latin1ToUtf8(std::span(reinterpret_cast<const std::uint8_t *>(raw.data()), raw.size()));
    } else if (type == international) {
        if (payload.size() < 4 || payload[0] > 1 || payload[1] != 0) return invalid("Invalid PNG international text header");
        const auto compressed = payload[0] == 1;
        payload = payload.subspan(2);
        for (int field = 0; field < 2; ++field) {
            const auto end = std::find(payload.begin(), payload.end(), std::uint8_t(0));
            if (end == payload.end()) return invalid("PNG international text header is unterminated");
            if (field == 1) {
                const std::string_view translated(reinterpret_cast<const char *>(payload.data()), std::size_t(end - payload.begin()));
                if (!validText(translated)) return invalid("PNG translated keyword is not UTF-8");
            }
            payload = payload.subspan(std::size_t(end - payload.begin()) + 1);
        }
        if (compressed) {
            auto result = inflateText(payload, limit, value);
            if (!result.ok()) return result;
        } else {
            if (payload.size() > limit) return error(MediaIoCode::LimitExceeded, "PNG text exceeds the decoded byte limit");
            value.assign(reinterpret_cast<const char *>(payload.data()), payload.size());
        }
        if (!validText(value)) return invalid("PNG international text is not UTF-8");
    }
    if (value.size() > limit) return error(MediaIoCode::LimitExceeded, "PNG text exceeds the decoded byte limit");
    entry.value = std::move(value);
    return {};
}

} // namespace

PngTextResult readPngText(std::span<const std::uint8_t> bytes, std::uint64_t maxDecodedBytes)
{
    constexpr std::array<std::uint8_t, 8> signature{137, 80, 78, 71, 13, 10, 26, 10};
    PngTextResult result;
    const auto fail = [&](MediaIoResult failure) { return PngTextResult{{}, std::move(failure)}; };
    if (bytes.size() < signature.size() || !std::equal(signature.begin(), signature.end(), bytes.begin()))
        return fail(invalid("Invalid PNG signature"));
    bytes = bytes.subspan(signature.size());
    bool header = false, pixels = false;
    std::uint64_t textBytes = 0;
    while (!bytes.empty()) {
        if (bytes.size() < 12) return fail(invalid("Truncated PNG chunk"));
        const auto size = std::size_t(be32(bytes, 0));
        const auto type = be32(bytes, 4);
        if (size > bytes.size() - 12 || (!header && (type != 0x49484452 || size != 13)))
            return fail(invalid("Invalid PNG chunk length or header"));
        if (type == 0x49484452) {
            if (header) return fail(invalid("Duplicate PNG header"));
            header = true;
        }
        if (checksum(bytes.subspan(4, size + 4)) != be32(bytes, size + 8)) return fail(invalid("PNG chunk CRC mismatch"));
        if (type == 0x49444154) pixels = true;
        if (type == 0x74455874 || type == 0x7a545874 || type == 0x69545874) {
            MediaTextEntry entry;
            auto parsed = decodeText(type, bytes.subspan(8, size), maxDecodedBytes - textBytes, entry);
            if (!parsed.ok()) return fail(std::move(parsed));
            if (entry.key.size() > maxDecodedBytes - textBytes ||
                entry.value.size() > maxDecodedBytes - textBytes - entry.key.size())
                return fail(error(MediaIoCode::LimitExceeded, "PNG metadata exceeds the decoded byte limit"));
            textBytes += entry.key.size() + entry.value.size();
            const auto existing = std::find_if(result.text.begin(), result.text.end(), [&](const auto &item) { return item.key == entry.key; });
            if (existing == result.text.end()) result.text.push_back(std::move(entry));
            else existing->value = std::move(entry.value);
        }
        if (type == 0x49454e44) {
            if (size != 0 || !pixels || bytes.size() != 12) return fail(invalid("Invalid PNG end"));
            return result;
        }
        bytes = bytes.subspan(size + 12);
    }
    return fail(invalid("Missing PNG end"));
}

MediaIoResult validateTextEntries(std::span<const MediaTextEntry> entries, bool png)
{
    std::set<std::string_view> keys;
    for (const auto &entry : entries) {
        if (entry.key.empty() || !validText(entry.key) || !validText(entry.value) || !keys.insert(entry.key).second)
            return error(MediaIoCode::InvalidArgument, "Metadata requires unique nonempty UTF-8 keys and UTF-8 values");
        std::string latin;
        if (png && !keyword(entry.key, latin)) return error(MediaIoCode::InvalidArgument, "Invalid PNG Latin-1 keyword");
    }
    return {};
}

MediaBytesResult writePngText(std::vector<std::uint8_t> bytes, std::span<const MediaTextEntry> entries,
                              std::uint64_t maxOutputBytes)
{
    if (auto valid = validateTextEntries(entries, true); !valid.ok()) return {{}, std::move(valid)};
    if (bytes.size() < 12 || bytes.size() > maxOutputBytes) return {{}, error(MediaIoCode::LimitExceeded, "PNG exceeds the output byte limit")};
    std::vector<std::uint8_t> chunks;
    for (const auto &entry : entries) {
        std::string key;
        (void)keyword(entry.key, key);
        const auto length = std::uint64_t(key.size()) + 5 + entry.value.size();
        if (length > std::numeric_limits<std::uint32_t>::max() || length + 12 > maxOutputBytes - bytes.size()
            || chunks.size() > maxOutputBytes - bytes.size() - length - 12)
            return {{}, error(MediaIoCode::LimitExceeded, "PNG metadata exceeds the output byte limit")};
        const auto begin = chunks.size();
        append32(chunks, std::uint32_t(length));
        chunks.insert(chunks.end(), {'i', 'T', 'X', 't'});
        chunks.insert(chunks.end(), key.begin(), key.end());
        chunks.insert(chunks.end(), 5, 0); // key terminator, flag, method, language, translated keyword
        chunks.insert(chunks.end(), entry.value.begin(), entry.value.end());
        append32(chunks, checksum(std::span(chunks).subspan(begin + 4)));
    }
    bytes.insert(bytes.end() - 12, chunks.begin(), chunks.end());
    return {std::move(bytes), {}};
}

} // namespace iiSharedCanvas::media_detail
