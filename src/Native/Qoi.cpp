#include "Native/Qoi_p.hpp"
#include "Native/Media_p.hpp"

#include <algorithm>
#include <array>
#include <limits>

namespace iiSharedCanvas::media_detail {
namespace {
std::uint32_t read32(std::span<const std::uint8_t> value, std::size_t pos)
{
    return std::uint32_t(value[pos]) << 24 | std::uint32_t(value[pos + 1]) << 16
        | std::uint32_t(value[pos + 2]) << 8 | value[pos + 3];
}

// QOI stores channels as unsigned bytes; arithmetic intentionally wraps.
std::uint32_t argb(int red, int green, int blue, int alpha)
{
    return std::uint32_t(std::uint8_t(alpha)) << 24 | std::uint32_t(std::uint8_t(red)) << 16
        | std::uint32_t(std::uint8_t(green)) << 8 | std::uint8_t(blue);
}

std::size_t slot(std::uint32_t color)
{
    return (((color >> 16) & 255) * 3 + ((color >> 8) & 255) * 5
            + (color & 255) * 7 + (color >> 24) * 11) % 64;
}

BitmapImportResult decodeFailure(MediaIoCode code, const char *message)
{
    BitmapImportResult result;
    result.result = error(code, message);
    return result;
}
} // namespace

BitmapImportResult decodeQoi(std::span<const std::uint8_t> bytes, const BitmapImportOptions &options)
{
    constexpr std::array<std::uint8_t, 8> endMarker{0, 0, 0, 0, 0, 0, 0, 1};
    if (bytes.size() > options.limits.maxInputBytes)
        return decodeFailure(MediaIoCode::LimitExceeded, "QOI exceeds the input byte limit");
    if (bytes.size() < 22 || bytes[0] != 'q' || bytes[1] != 'o' || bytes[2] != 'i' || bytes[3] != 'f'
        || (bytes[12] != 3 && bytes[12] != 4) || bytes[13] > 1
        || !std::equal(endMarker.begin(), endMarker.end(), bytes.end() - endMarker.size()))
        return decodeFailure(MediaIoCode::InvalidData, "Invalid QOI header or end marker");
    const auto width = read32(bytes, 4), height = read32(bytes, 8);
    if (!width || !height || width > std::numeric_limits<std::int32_t>::max()
        || height > std::numeric_limits<std::int32_t>::max())
        return decodeFailure(MediaIoCode::InvalidData, "Invalid QOI dimensions");
    if (auto valid = checkExtent({int(width), int(height)}, options.limits); !valid.ok()) {
        BitmapImportResult result; result.result = std::move(valid); return result;
    }
    const auto total = std::uint64_t(width) * height;
    if (total > std::numeric_limits<std::size_t>::max() / sizeof(std::uint32_t))
        return decodeFailure(MediaIoCode::LimitExceeded, "QOI pixels exceed addressable memory");
    BitmapImportResult result;
    result.asset.id = options.assetId;
    auto &raster = result.asset.pixels;
    raster.width = int(width); raster.height = int(height); raster.pixels.reserve(std::size_t(total));
    std::array<std::uint32_t, 64> index{};
    std::uint32_t previous = 0xff000000;
    std::size_t offset = 14;
    const auto end = bytes.size() - endMarker.size();
    while (raster.pixels.size() < total) {
        if (offset >= end) return decodeFailure(MediaIoCode::InvalidData, "Truncated QOI pixel data");
        const auto operation = bytes[offset++];
        if (operation == 0xfe || operation == 0xff) {
            const std::size_t channels = operation == 0xfe ? 3 : 4;
            if (channels > end - offset) return decodeFailure(MediaIoCode::InvalidData, "Truncated QOI color chunk");
            previous = argb(bytes[offset], bytes[offset + 1], bytes[offset + 2],
                            channels == 4 ? bytes[offset + 3] : int(previous >> 24));
            offset += channels;
        } else if ((operation & 0xc0) == 0x00) {
            previous = index[operation & 63];
        } else if ((operation & 0xc0) == 0x40) {
            previous = argb(int((previous >> 16) & 255) + ((operation >> 4) & 3) - 2,
                            int((previous >> 8) & 255) + ((operation >> 2) & 3) - 2,
                            int(previous & 255) + (operation & 3) - 2, int(previous >> 24));
        } else if ((operation & 0xc0) == 0x80) {
            if (offset >= end) return decodeFailure(MediaIoCode::InvalidData, "Truncated QOI luma chunk");
            const auto second = bytes[offset++];
            const int green = (operation & 63) - 32;
            previous = argb(int((previous >> 16) & 255) + green + ((second >> 4) & 15) - 8,
                            int((previous >> 8) & 255) + green,
                            int(previous & 255) + green + (second & 15) - 8, int(previous >> 24));
        } else {
            const auto run = std::size_t(operation & 63) + 1;
            if (run > total - raster.pixels.size())
                return decodeFailure(MediaIoCode::InvalidData, "QOI run exceeds its image extent");
            raster.pixels.insert(raster.pixels.end(), run, previous);
            index[slot(previous)] = previous;
            continue;
        }
        raster.pixels.push_back(previous);
        index[slot(previous)] = previous;
    }
    if (offset != end) return decodeFailure(MediaIoCode::InvalidData, "Extra QOI pixel data");
    result.format = "qoi";
    if (bytes[13] == 1) result.result.warnings.emplace_back("QOI marks its color values as linear; canvas pixels retain the encoded values");
    return result;
}

MediaBytesResult encodeQoi(const RasterLayer &pixels, const BitmapExportOptions &options)
{
    constexpr std::array<std::uint8_t, 8> endMarker{0, 0, 0, 0, 0, 0, 0, 1};
    if (auto valid = checkRaster(pixels, options.limits); !valid.ok()) return {{}, std::move(valid)};
    if (options.limits.maxOutputBytes < 22) return {{}, error(MediaIoCode::LimitExceeded, "QOI output exceeds its byte limit")};
    MediaBytesResult result;
    auto &out = result.bytes;
    const auto append = [&](std::uint8_t value) {
        if (out.size() >= options.limits.maxOutputBytes) return false;
        out.push_back(value); return true;
    };
    const auto append32 = [&](std::uint32_t value) {
        return append(std::uint8_t(value >> 24)) && append(std::uint8_t(value >> 16))
            && append(std::uint8_t(value >> 8)) && append(std::uint8_t(value));
    };
    if (!append('q') || !append('o') || !append('i') || !append('f')
        || !append32(std::uint32_t(pixels.width)) || !append32(std::uint32_t(pixels.height))
        || !append(4) || !append(0)) return {{}, error(MediaIoCode::LimitExceeded, "QOI output exceeds its byte limit")};
    std::uint32_t previous = 0xff000000;
    unsigned run = 0;
    for (std::size_t i = 0; i < pixels.pixels.size(); ++i) {
        const auto color = pixels.pixels[i];
        if (color == previous) {
            ++run;
            if ((run == 62 || i + 1 == pixels.pixels.size()) && !append(std::uint8_t(0xc0 | (run - 1))))
                return {{}, error(MediaIoCode::LimitExceeded, "QOI output exceeds its byte limit")};
            if (run == 62 || i + 1 == pixels.pixels.size()) run = 0;
            continue;
        }
        if (run && !append(std::uint8_t(0xc0 | (run - 1))))
            return {{}, error(MediaIoCode::LimitExceeded, "QOI output exceeds its byte limit")};
        run = 0;
        const bool sameAlpha = (previous >> 24) == (color >> 24);
        if (!append(sameAlpha ? 0xfe : 0xff) || !append(std::uint8_t(color >> 16))
            || !append(std::uint8_t(color >> 8)) || !append(std::uint8_t(color))
            || (!sameAlpha && !append(std::uint8_t(color >> 24))))
            return {{}, error(MediaIoCode::LimitExceeded, "QOI output exceeds its byte limit")};
        previous = color;
    }
    for (const auto byte : endMarker) if (!append(byte))
        return {{}, error(MediaIoCode::LimitExceeded, "QOI output exceeds its byte limit")};
    if (!options.text.empty()) result.result.warnings.emplace_back("QOI does not preserve text metadata");
    return result;
}

} // namespace iiSharedCanvas::media_detail
