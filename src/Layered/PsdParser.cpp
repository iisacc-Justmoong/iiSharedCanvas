#include "PsdParser_p.hpp"

#include <QByteArray>
#include <QColorSpace>

#include <zlib.h>

#include <algorithm>
#include <array>
#include <bit>
#include <limits>
#include <string_view>

namespace iiSharedCanvas::layered_detail {
namespace {

struct ParseFailure { MediaIoCode code; std::string message; };
[[noreturn]] void invalid(std::string message) { throw ParseFailure{MediaIoCode::InvalidData, std::move(message)}; }
[[noreturn]] void unsupported(std::string message) { throw ParseFailure{MediaIoCode::UnsupportedFeature, std::move(message)}; }
[[noreturn]] void limited(std::string message) { throw ParseFailure{MediaIoCode::LimitExceeded, std::move(message)}; }

// Every nested structure owns a bounded view. Length arithmetic never adds an
// untrusted length to a pointer, nor may one section read through its boundary.
class Reader {
public:
    explicit Reader(std::span<const std::uint8_t> bytes) : m_bytes(bytes) {}
    std::size_t remaining() const { return m_bytes.size(); }
    std::span<const std::uint8_t> take(std::size_t count)
    {
        if (count > m_bytes.size()) { invalid("PSD data is truncated or a section length is invalid"); }
        const auto result = m_bytes.first(count); m_bytes = m_bytes.subspan(count); return result;
    }
    std::uint8_t u8() { return take(1)[0]; }
    std::uint16_t u16()
    {
        const auto bytes = take(2); return std::uint16_t((std::uint16_t(bytes[0]) << 8) | bytes[1]);
    }
    std::uint32_t u32()
    {
        const auto bytes = take(4);
        return (std::uint32_t(bytes[0]) << 24) | (std::uint32_t(bytes[1]) << 16)
            | (std::uint32_t(bytes[2]) << 8) | bytes[3];
    }
    std::int16_t i16() { return std::bit_cast<std::int16_t>(u16()); }
    std::int32_t i32() { return std::bit_cast<std::int32_t>(u32()); }
    std::string_view key()
    {
        const auto bytes = take(4);
        return {reinterpret_cast<const char *>(bytes.data()), bytes.size()};
    }
    Reader section() { const auto size = u32(); return Reader(take(size)); }
    void zeroPadding(std::size_t maximum)
    {
        if (remaining() > maximum) { invalid("PSD section has unexpected trailing data"); }
        for (const auto byte : take(remaining())) {
            if (byte != 0) { invalid("PSD padding must be zero"); }
        }
    }
private:
    std::span<const std::uint8_t> m_bytes;
};

void warning(MediaIoResult &result, const std::string &message)
{
    if (std::find(result.warnings.begin(), result.warnings.end(), message) == result.warnings.end()) {
        result.warnings.push_back(message);
    }
}

void appendUtf8(std::string &out, std::uint32_t codepoint)
{
    if (codepoint <= 0x7f) { out.push_back(char(codepoint)); }
    else if (codepoint <= 0x7ff) {
        out.push_back(char(0xc0 | (codepoint >> 6))); out.push_back(char(0x80 | (codepoint & 0x3f)));
    } else if (codepoint <= 0xffff) {
        out.push_back(char(0xe0 | (codepoint >> 12))); out.push_back(char(0x80 | ((codepoint >> 6) & 0x3f)));
        out.push_back(char(0x80 | (codepoint & 0x3f)));
    } else {
        out.push_back(char(0xf0 | (codepoint >> 18))); out.push_back(char(0x80 | ((codepoint >> 12) & 0x3f)));
        out.push_back(char(0x80 | ((codepoint >> 6) & 0x3f))); out.push_back(char(0x80 | (codepoint & 0x3f)));
    }
}

std::string unicodeName(Reader &reader, std::uint64_t remainingBudget)
{
    const auto count = reader.u32();
    if (count > reader.remaining() / 2) { invalid("PSD Unicode layer name is truncated"); }
    // UTF-16 -> UTF-8 uses at most three bytes per code unit. Check before any
    // string allocation, including maliciously inflated name lengths.
    if (std::uint64_t(count) * 3 > remainingBudget) { limited("PSD layer names exceed the decoded-byte limit"); }
    std::string result;
    for (std::uint32_t index = 0; index < count; ++index) {
        std::uint32_t value = reader.u16();
        if (value >= 0xd800 && value <= 0xdbff) {
            if (++index >= count) { invalid("PSD layer name has an unmatched UTF-16 surrogate"); }
            const auto low = reader.u16();
            if (low < 0xdc00 || low > 0xdfff) { invalid("PSD layer name has an invalid UTF-16 surrogate pair"); }
            value = 0x10000 + ((value - 0xd800) << 10) + low - 0xdc00;
        } else if (value >= 0xdc00 && value <= 0xdfff) { invalid("PSD layer name has an unmatched UTF-16 surrogate"); }
        if (value == 0) {
            if (index + 1 != count) { invalid("PSD layer name contains an embedded null"); }
        } else { appendUtf8(result, value); }
    }
    reader.zeroPadding(2);
    return result;
}

struct Channel { std::int16_t id; std::uint32_t size; };
struct LayerRecord {
    std::int32_t x = 0, y = 0, width = 0, height = 0;
    std::string name;
    RasterBlendMode blend = RasterBlendMode::SourceOver;
    std::uint8_t opacity = 255;
    bool visible = true;
    std::vector<Channel> channels;
};

void additionalLayerData(Reader &extra, LayerRecord &record,
                         MediaIoResult &result, std::uint64_t nameBudget)
{
    bool unicode = false;
    while (extra.remaining() != 0) {
        if (extra.remaining() < 12) { extra.zeroPadding(3); break; }
        const auto signature = extra.key();
        if (signature != "8BIM" && signature != "8B64") { invalid("PSD layer information signature is invalid"); }
        const auto key = extra.key();
        const auto size = extra.u32();
        Reader payload(extra.take(size));
        if (size & 1U) { extra.take(1); }
        if (key == "luni") {
            if (unicode) { invalid("PSD layer has duplicate Unicode names"); }
            record.name = unicodeName(payload, nameBudget); unicode = true;
        } else if (key == "lyid" || key == "lspf" || key == "lyvr") {
            if (size != 4) { invalid("PSD editor metadata has an invalid size"); }
            warning(result, "PSD editor IDs, lock state and editor metadata are not retained");
        } else if (key == "lclr" || key == "fxrp") {
            if (size != (key == "lclr" ? 8U : 16U)) { invalid("PSD editor metadata has an invalid size"); }
            warning(result, "PSD editor IDs, lock state and editor metadata are not retained");
        } else if (key == "iOpa") {
            if (size != 1 && size != 4) { invalid("PSD fill opacity has an invalid size"); }
            if (payload.u8() != 255) { unsupported("PSD fill opacity is not supported"); }
            payload.zeroPadding(3);
        } else {
            unsupported("PSD layer feature is not supported: " + std::string(key));
        }
    }
    if (!unicode && std::any_of(record.name.begin(), record.name.end(), [](unsigned char c) { return c >= 128; })) {
        unsupported("PSD non-ASCII legacy names require a Unicode layer-name block");
    }
}

void imageResources(Reader &resources, const LayeredDocumentImportOptions &options, MediaIoResult &result)
{
    if (resources.remaining()) {
        warning(result, "PSD image resources are not retained in the imported canvas document");
    }
    while (resources.remaining()) {
        if (resources.key() != "8BIM") { invalid("PSD image resource signature is invalid"); }
        const auto id = resources.u16();
        const auto nameSize = resources.u8(); resources.take(nameSize);
        if ((nameSize + 1) & 1U) { resources.take(1); }
        const auto size = resources.u32(); const auto data = resources.take(size);
        if (size & 1U) { resources.take(1); }
        if (id == 1039) {
            if (size > options.limits.maxDecodedBytes || data.size() > std::size_t(std::numeric_limits<qsizetype>::max())) {
                limited("PSD ICC profile exceeds the decoded-byte limit");
            }
            const auto profile = QColorSpace::fromIccProfile(QByteArray::fromRawData(
                reinterpret_cast<const char *>(data.data()), qsizetype(data.size())));
            if (!profile.isValid()) { invalid("PSD embedded ICC profile is invalid"); }
            if (profile != QColorSpace(QColorSpace::SRgb)) { unsupported("PSD non-sRGB ICC profiles are not supported"); }
        } else if (id == 1041) {
            if (data.size() != 1 || data[0] > 1) { invalid("PSD ICC untagged-profile flag must be one Boolean byte"); }
            if (data[0] != 0) { unsupported("PSD intentionally untagged color data cannot be interpreted as sRGB"); }
        } else if (id == 1064) {
            Reader aspect(data);
            const auto version = aspect.u32();
            const auto high = aspect.u32(), low = aspect.u32();
            if ((version != 1 && version != 2) || aspect.remaining()) { invalid("PSD pixel aspect resource is invalid"); }
            if (high != 0x3ff00000 || low != 0) { unsupported("PSD non-square pixel aspect ratios are not supported"); }
        } else if (id == 1065 || id == 1075 || id == 1078) {
            unsupported("PSD layer comps and timeline metadata are not supported");
        }
    }
}

RasterBlendMode blendMode(std::string_view key)
{
    if (key == "norm") { return RasterBlendMode::SourceOver; }
    if (key == "mul ") { return RasterBlendMode::Multiply; }
    if (key == "scrn") { return RasterBlendMode::Screen; }
    if (key == "over") { return RasterBlendMode::Overlay; }
    unsupported("PSD blend mode is not supported: " + std::string(key));
}

void unpackRow(Reader &input, std::span<std::uint8_t> output, std::size_t width)
{
    std::size_t written = 0;
    while (input.remaining()) {
        const auto header = input.u8();
        if (header == 128) { continue; } // PackBits no-op.
        const auto count = header < 128 ? std::size_t(header) + 1 : 257U - header;
        if (count > width - written) { invalid("PSD PackBits run exceeds the row width"); }
        if (header < 128) {
            const auto bytes = input.take(count);
            if (!output.empty()) { std::copy(bytes.begin(), bytes.end(), output.begin() + std::ptrdiff_t(written)); }
        } else {
            const auto value = input.u8();
            if (!output.empty()) { std::fill_n(output.begin() + std::ptrdiff_t(written), count, value); }
        }
        written += count;
    }
    if (written != width) { invalid("PSD PackBits row is incomplete"); }
}

void decodeChannel(Reader &input, std::span<std::uint8_t> plane, std::int32_t width, std::int32_t height)
{
    const auto compression = input.u16();
    if (compression == 0) {
        if (input.remaining() != plane.size()) { invalid("PSD raw channel length does not match layer dimensions"); }
        const auto bytes = input.take(plane.size()); std::copy(bytes.begin(), bytes.end(), plane.begin());
    } else if (compression == 1) {
        Reader rowLengths(input.take(std::size_t(height) * 2));
        for (std::int32_t row = 0; row < height; ++row) {
            Reader packed(input.take(rowLengths.u16()));
            unpackRow(packed, plane.subspan(std::size_t(row) * width, std::size_t(width)), std::size_t(width));
        }
        if (input.remaining()) { invalid("PSD RLE channel has trailing bytes"); }
    } else if (compression == 2 || compression == 3) {
        const auto bytes = input.take(input.remaining());
        if (bytes.size() > std::numeric_limits<uLong>::max() || plane.size() > std::numeric_limits<uLongf>::max()) {
            limited("PSD ZIP channel exceeds zlib size limits");
        }
        uLong inputSize = uLong(bytes.size()); uLongf outputSize = uLongf(plane.size());
        const auto status = uncompress2(plane.data(), &outputSize, bytes.data(), &inputSize);
        if (status != Z_OK || inputSize != bytes.size() || outputSize != plane.size()) {
            invalid("PSD ZIP channel is corrupt, truncated or has an unexpected decoded length");
        }
        if (compression == 3) {
            for (std::int32_t row = 0; row < height; ++row) {
                for (std::int32_t column = 1; column < width; ++column) {
                    const auto index = std::size_t(row) * width + column;
                    plane[index] = std::uint8_t(plane[index] + plane[index - 1]);
                }
            }
        }
    } else { unsupported("PSD channel compression is not supported"); }
}

void validateMergedImage(Reader &file, std::uint32_t width, std::uint32_t height, std::uint16_t channels)
{
    // A merged preview is not used for conversion, but a present preview must
    // be complete. Validate in constant scratch space without allocating a
    // second canvas-sized raster. Maximize-compatibility-disabled files may
    // omit the preview section entirely.
    if (!file.remaining()) { return; }
    const auto compression = file.u16();
    const auto expectedBytes = std::uint64_t(width) * height * channels;
    if (compression == 0) {
        if (file.remaining() != expectedBytes) { invalid("PSD merged raw image is truncated or has trailing data"); }
        file.take(file.remaining());
    } else if (compression == 1) {
        const auto rowCount = std::size_t(height) * channels;
        Reader lengths(file.take(rowCount * 2));
        for (std::size_t row = 0; row < rowCount; ++row) {
            Reader packed(file.take(lengths.u16())); unpackRow(packed, {}, width);
        }
        if (file.remaining()) { invalid("PSD merged RLE image has trailing data"); }
    } else if (compression == 2 || compression == 3) {
        auto input = file.take(file.remaining());
        z_stream stream{};
        if (inflateInit(&stream) != Z_OK) { limited("PSD merged ZIP validation could not allocate zlib state"); }
        struct EndInflate { z_stream *stream; ~EndInflate() { inflateEnd(stream); } } cleanup{&stream};
        std::array<std::uint8_t, 4096> scratch;
        std::uint64_t decoded = 0;
        int status = Z_OK;
        while (status == Z_OK) {
            if (!stream.avail_in && !input.empty()) {
                const auto count = std::min<std::size_t>(input.size(), std::numeric_limits<uInt>::max());
                stream.next_in = const_cast<Bytef *>(input.data()); stream.avail_in = uInt(count);
                input = input.subspan(count);
            }
            stream.next_out = scratch.data(); stream.avail_out = uInt(scratch.size());
            status = inflate(&stream, Z_NO_FLUSH);
            const auto produced = scratch.size() - stream.avail_out;
            if (produced > expectedBytes - decoded) { invalid("PSD merged ZIP image exceeds the declared dimensions"); }
            decoded += produced;
        }
        if (status != Z_STREAM_END || !input.empty() || stream.avail_in || decoded != expectedBytes) {
            invalid("PSD merged ZIP image is corrupt, truncated or has trailing data");
        }
    } else { unsupported("PSD merged image compression is not supported"); }
}

Document parse(std::span<const std::uint8_t> bytes, const LayeredDocumentImportOptions &options,
               MediaIoResult &result)
{
    if (bytes.size() > options.limits.maxInputBytes) { limited("PSD input exceeds the encoded-byte limit"); }
    Reader file(bytes);
    if (file.key() != "8BPS") { invalid("PSD signature is invalid"); }
    if (file.u16() != 1) { unsupported("Only PSD version 1 is supported; PSB is not supported"); }
    for (const auto byte : file.take(6)) { if (byte != 0) { invalid("PSD reserved header bytes must be zero"); } }
    const auto compositeChannels = file.u16();
    const auto height = file.u32(), width = file.u32();
    if (compositeChannels < 3 || compositeChannels > 4) { unsupported("PSD extra document channels are not supported"); }
    if (!width || !height || width > 30000 || height > 30000) { invalid("PSD canvas dimensions are invalid"); }
    if (std::uint64_t(width) * height > options.limits.maxPixelsPerFrame) { limited("PSD canvas exceeds the pixel limit"); }
    const auto depth = file.u16(), mode = file.u16();
    if (depth != 8 || mode != 3) { unsupported("Only 8-bit RGB layered PSD documents are supported"); }
    if (file.u32() != 0) { invalid("RGB PSD color-mode data must be empty"); }
    auto resources = file.section(); imageResources(resources, options, result);
    auto layerAndMask = file.section();
    if (layerAndMask.remaining() == 0) { unsupported("PSD has no editable raster layers; composite fallback is disabled"); }
    auto info = layerAndMask.section();
    if (info.remaining() == 0) { unsupported("PSD has no editable raster layers; composite fallback is disabled"); }
    const auto signedCount = info.i16();
    const auto count = std::uint32_t(signedCount < 0 ? -std::int32_t(signedCount) : signedCount);
    if (!count) { unsupported("PSD has no editable raster layers; composite fallback is disabled"); }
    if (signedCount < 0 && compositeChannels != 4) {
        invalid("PSD negative layer count requires a merged transparency channel");
    }
    if (signedCount > 0 && compositeChannels == 4) {
        unsupported("PSD auxiliary document alpha channels are not supported");
    }
    if (count > options.maxLayers) { limited("PSD layer count exceeds the layer limit"); }
    // A minimal RGB record is 60 bytes before its channel streams. Bound the
    // record allocation by both the declared count and the enclosing bytes.
    if (count > info.remaining() / 60) { invalid("PSD layer count exceeds the available layer records"); }
    std::vector<LayerRecord> records; records.reserve(count);
    std::uint64_t rasterBytes = 0, largestPlane = 0, nameBytes = 0;
    for (std::uint32_t index = 0; index < count; ++index) {
        LayerRecord record;
        const auto top = info.i32(), left = info.i32(), bottom = info.i32(), right = info.i32();
        const auto layerWidth = std::int64_t(right) - left, layerHeight = std::int64_t(bottom) - top;
        if (layerWidth < 0 || layerHeight < 0) { invalid("PSD layer rectangle is inverted"); }
        if (!layerWidth || !layerHeight) { unsupported("PSD empty layers and non-pixel layers are not supported"); }
        if (layerWidth > std::numeric_limits<std::int32_t>::max() || layerHeight > std::numeric_limits<std::int32_t>::max()) {
            limited("PSD layer dimensions exceed the supported integer range");
        }
        const auto pixels = std::uint64_t(layerWidth) * std::uint64_t(layerHeight);
        if (pixels > options.limits.maxPixelsPerFrame || pixels > std::numeric_limits<std::size_t>::max() / 4) {
            limited("PSD layer exceeds the pixel limit");
        }
        if (pixels > (options.limits.maxDecodedBytes - rasterBytes) / 4) { limited("PSD layers exceed the decoded-byte limit"); }
        rasterBytes += pixels * 4; largestPlane = std::max(largestPlane, pixels);
        if (largestPlane > options.limits.maxDecodedBytes - rasterBytes
            || nameBytes > options.limits.maxDecodedBytes - rasterBytes - largestPlane) {
            limited("PSD raster pixels and channel scratch exceed the decoded-byte limit");
        }
        record.x = left; record.y = top; record.width = std::int32_t(layerWidth); record.height = std::int32_t(layerHeight);
        const auto channelCount = info.u16();
        if (channelCount != 3 && channelCount != 4) { unsupported("PSD layers require RGB and optional transparency channels only"); }
        std::array<bool, 4> seen{};
        for (std::uint16_t channel = 0; channel < channelCount; ++channel) {
            const auto id = info.i16(); const auto size = info.u32();
            if (id < -1 || id > 2) { unsupported("PSD masks and auxiliary layer channels are not supported"); }
            const auto slot = std::size_t(id + 1);
            if (seen[slot] || size < 2) { invalid("PSD channel IDs are duplicated or channel data is missing"); }
            seen[slot] = true; record.channels.push_back({id, size});
        }
        if (!seen[1] || !seen[2] || !seen[3]) { invalid("PSD layer is missing an RGB channel"); }
        if (info.key() != "8BIM") { invalid("PSD blend signature is invalid"); }
        record.blend = blendMode(info.key()); record.opacity = info.u8();
        if (info.u8() != 0) { unsupported("PSD clipping layers are not supported"); }
        const auto flags = info.u8(); record.visible = (flags & 2U) == 0;
        if ((flags & 0xe0U) || ((flags & 8U) && (flags & 16U))) { unsupported("PSD non-pixel or unknown layer flags are not supported"); }
        if (flags & 1U) { warning(result, "PSD editor IDs, lock state and editor metadata are not retained"); }
        if (info.u8() != 0) { invalid("PSD layer filler must be zero"); }
        auto extra = info.section();
        auto mask = extra.section();
        if (mask.remaining()) { unsupported("PSD layer masks are not supported"); }
        auto ranges = extra.section();
        if (ranges.remaining() % 8 != 0) { invalid("PSD blending ranges have an invalid size"); }
        while (ranges.remaining()) {
            if (ranges.u32() != 0x0000ffffU || ranges.u32() != 0x0000ffffU) {
                unsupported("PSD non-default blending ranges are not supported");
            }
        }
        const auto nameLength = extra.u8(); const auto name = extra.take(nameLength);
        if (nameLength > options.limits.maxDecodedBytes - rasterBytes - largestPlane - nameBytes) {
            limited("PSD layer names exceed the decoded-byte limit");
        }
        record.name.assign(reinterpret_cast<const char *>(name.data()), name.size());
        if (record.name.find('\0') != std::string::npos) { invalid("PSD legacy layer name contains a null"); }
        extra.take((4 - ((nameLength + 1) % 4)) % 4);
        additionalLayerData(extra, record, result,
                            options.limits.maxDecodedBytes - rasterBytes - largestPlane - nameBytes);
        nameBytes += record.name.size(); records.push_back(std::move(record));
    }
    // Validate every declared channel span before allocating a raster. Reading
    // through a truncated later layer must never yield a partial document.
    Reader channelCheck = info;
    for (const auto &record : records) {
        for (const auto channel : record.channels) { channelCheck.take(channel.size); }
    }
    channelCheck.zeroPadding(1);
    if (layerAndMask.remaining()) {
        auto globalMask = layerAndMask.section();
        if (globalMask.remaining()) { unsupported("PSD global masks are not supported"); }
        if (layerAndMask.remaining()) { unsupported("PSD global additional layer features are not supported"); }
    }
    validateMergedImage(file, width, height, compositeChannels);

    Document document; document.extent = {std::int32_t(width), std::int32_t(height)};
    document.assets.resize(count); document.layers.resize(count);
    for (std::size_t index = 0; index < records.size(); ++index) {
        auto &record = records[index];
        // PSD's binary layer list is already bottom-to-top, matching iisc.
        const auto destination = index;
        const auto suffix = std::to_string(destination);
        RasterAsset asset; asset.id = options.idPrefix + "-asset-" + suffix;
        asset.pixels = makeRasterLayer(record.width, record.height, 0xff000000U);
        std::vector<std::uint8_t> plane(std::size_t(record.width) * record.height);
        for (const auto channel : record.channels) {
            Reader encoded(info.take(channel.size)); decodeChannel(encoded, plane, record.width, record.height);
            const auto shift = channel.id == -1 ? 24 : 16 - channel.id * 8;
            const std::uint32_t clearMask = ~(0xffU << shift);
            for (std::size_t pixel = 0; pixel < plane.size(); ++pixel) {
                asset.pixels.pixels[pixel] = (asset.pixels.pixels[pixel] & clearMask)
                    | (std::uint32_t(plane[pixel]) << shift);
            }
        }
        BitmapLayer layer; layer.properties.id = options.idPrefix + "-layer-" + suffix;
        layer.properties.name = std::move(record.name); layer.properties.visible = record.visible;
        layer.properties.opacity = double(record.opacity) / 255.0; layer.properties.blendMode = record.blend;
        layer.properties.transform.translationX = record.x; layer.properties.transform.translationY = record.y;
        layer.source = StaticSource{asset.id};
        document.layers[destination] = std::move(layer); document.assets[destination] = std::move(asset);
    }
    return document;
}

} // namespace

LayeredDocumentImportResult decodePsd(std::span<const std::uint8_t> bytes,
                                     const LayeredDocumentImportOptions &options)
{
    LayeredDocumentImportResult result; result.format = "psd";
    try { result.document = parse(bytes, options, result.result); }
    catch (const ParseFailure &failure) {
        result.result = {failure.code, failure.message, {}};
    }
    return result;
}

} // namespace iiSharedCanvas::layered_detail
