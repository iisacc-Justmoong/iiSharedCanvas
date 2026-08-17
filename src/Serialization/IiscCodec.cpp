#include "Serialization/IiscCodec.h"

#include "Validation/Validation.h"

#include <algorithm>
#include <array>
#include <bit>
#include <cstddef>
#include <cstdint>
#include <limits>
#include <span>
#include <stdexcept>
#include <string>
#include <string_view>
#include <type_traits>
#include <utility>
#include <variant>
#include <vector>

namespace iiSharedCanvas {

namespace {

constexpr std::array<std::uint8_t, 8> ContainerMagic{
    'I', 'I', 'S', 'C', '\r', '\n', 0x1aU, '\n',
};
constexpr std::uint32_t ContainerFlags = 0;
constexpr std::uint32_t ContainerReserved = 0;

enum class RasterEncoding : std::uint8_t {
    RawArgb32 = 0,
    RunLengthArgb32 = 1,
};

struct DecodeFailure final : std::runtime_error {
    DecodeFailure(IiscErrorCode failureCode,
                  std::uint64_t failureOffset,
                  std::string failureMessage)
        : std::runtime_error(failureMessage),
          code(failureCode),
          offset(failureOffset)
    {
    }

    IiscErrorCode code;
    std::uint64_t offset;
};

class ByteWriter final {
public:
    void writeU8(std::uint8_t value)
    {
        m_bytes.push_back(value);
    }

    void writeU16(std::uint16_t value)
    {
        writeUnsigned(value);
    }

    void writeU32(std::uint32_t value)
    {
        writeUnsigned(value);
    }

    void writeU64(std::uint64_t value)
    {
        writeUnsigned(value);
    }

    void writeI32(std::int32_t value)
    {
        writeU32(std::bit_cast<std::uint32_t>(value));
    }

    void writeDouble(double value)
    {
        static_assert(sizeof(double) == sizeof(std::uint64_t));
        static_assert(std::numeric_limits<double>::is_iec559);
        writeU64(std::bit_cast<std::uint64_t>(value));
    }

    void writeBytes(std::span<const std::uint8_t> values)
    {
        m_bytes.insert(m_bytes.end(), values.begin(), values.end());
    }

    void writeString(const std::string &value)
    {
        writeU32(static_cast<std::uint32_t>(value.size()));
        writeBytes(std::span<const std::uint8_t>(
            reinterpret_cast<const std::uint8_t *>(value.data()), value.size()));
    }

    [[nodiscard]] const std::vector<std::uint8_t> &bytes() const noexcept
    {
        return m_bytes;
    }

    [[nodiscard]] std::vector<std::uint8_t> takeBytes() noexcept
    {
        return std::move(m_bytes);
    }

private:
    template <typename Unsigned>
    void writeUnsigned(Unsigned value)
    {
        static_assert(std::is_unsigned_v<Unsigned>);
        for (std::size_t index = 0; index < sizeof(Unsigned); ++index) {
            m_bytes.push_back(static_cast<std::uint8_t>(value & 0xffU));
            value >>= 8U;
        }
    }

    std::vector<std::uint8_t> m_bytes;
};

class ByteReader final {
public:
    explicit ByteReader(std::span<const std::uint8_t> bytes,
                        std::uint64_t baseOffset = 0) noexcept
        : m_bytes(bytes),
          m_baseOffset(baseOffset)
    {
    }

    [[nodiscard]] std::uint64_t offset() const noexcept
    {
        return m_baseOffset + m_position;
    }

    [[nodiscard]] std::uint64_t remaining() const noexcept
    {
        return m_bytes.size() - m_position;
    }

    [[nodiscard]] bool empty() const noexcept
    {
        return m_position == m_bytes.size();
    }

    std::uint8_t readU8()
    {
        require(1);
        return m_bytes[m_position++];
    }

    std::uint16_t readU16()
    {
        return readUnsigned<std::uint16_t>();
    }

    std::uint32_t readU32()
    {
        return readUnsigned<std::uint32_t>();
    }

    std::uint64_t readU64()
    {
        return readUnsigned<std::uint64_t>();
    }

    std::int32_t readI32()
    {
        return std::bit_cast<std::int32_t>(readU32());
    }

    double readDouble()
    {
        return std::bit_cast<double>(readU64());
    }

    std::span<const std::uint8_t> readBytes(std::uint64_t count)
    {
        if (count > std::numeric_limits<std::size_t>::max()) {
            fail(IiscErrorCode::LimitExceeded, "byte range exceeds the platform address space");
        }
        require(static_cast<std::size_t>(count));
        const auto result = m_bytes.subspan(m_position, static_cast<std::size_t>(count));
        m_position += static_cast<std::size_t>(count);
        return result;
    }

    [[noreturn]] void fail(IiscErrorCode code, std::string message) const
    {
        throw DecodeFailure(code, offset(), std::move(message));
    }

private:
    template <typename Unsigned>
    Unsigned readUnsigned()
    {
        static_assert(std::is_unsigned_v<Unsigned>);
        require(sizeof(Unsigned));
        Unsigned value = 0;
        for (std::size_t index = 0; index < sizeof(Unsigned); ++index) {
            value |= static_cast<Unsigned>(m_bytes[m_position++]) << (index * 8U);
        }
        return value;
    }

    void require(std::size_t count) const
    {
        if (count > m_bytes.size() - m_position) {
            fail(IiscErrorCode::TruncatedData, "container ended before the declared value");
        }
    }

    std::span<const std::uint8_t> m_bytes;
    std::size_t m_position = 0;
    std::uint64_t m_baseOffset = 0;
};

struct LimitTotals {
    std::uint64_t rasterPixels = 0;
    std::uint64_t vectorPaths = 0;
    std::uint64_t pathCommands = 0;
    std::uint64_t keyframes = 0;
    std::uint64_t stringBytes = 0;
};

IiscError makeError(IiscErrorCode code, std::uint64_t offset, std::string message)
{
    return {code, offset, std::move(message)};
}

bool addWithin(std::uint64_t &total, std::uint64_t amount, std::uint64_t maximum) noexcept
{
    if (amount > maximum || total > maximum - amount) {
        return false;
    }
    total += amount;
    return true;
}

bool pixelCountWithin(std::int32_t width,
                      std::int32_t height,
                      std::uint64_t maximum,
                      std::uint64_t &pixelCount) noexcept
{
    if (width <= 0 || height <= 0) {
        return false;
    }
    pixelCount = static_cast<std::uint64_t>(width) * static_cast<std::uint64_t>(height);
    return pixelCount <= maximum;
}

bool isContinuationByte(std::uint8_t value) noexcept
{
    return (value & 0xc0U) == 0x80U;
}

bool isValidUtf8(std::string_view value) noexcept
{
    const auto *bytes = reinterpret_cast<const std::uint8_t *>(value.data());
    std::size_t index = 0;
    while (index < value.size()) {
        const std::uint8_t first = bytes[index++];
        if (first <= 0x7fU) {
            continue;
        }
        if (first >= 0xc2U && first <= 0xdfU) {
            if (index >= value.size() || !isContinuationByte(bytes[index++])) {
                return false;
            }
            continue;
        }
        if (first >= 0xe0U && first <= 0xefU) {
            if (index + 1 >= value.size()) {
                return false;
            }
            const std::uint8_t second = bytes[index++];
            const std::uint8_t third = bytes[index++];
            if (!isContinuationByte(third)
                || (first == 0xe0U && (second < 0xa0U || second > 0xbfU))
                || (first == 0xedU && (second < 0x80U || second > 0x9fU))
                || (first != 0xe0U && first != 0xedU && !isContinuationByte(second))) {
                return false;
            }
            continue;
        }
        if (first >= 0xf0U && first <= 0xf4U) {
            if (index + 2 >= value.size()) {
                return false;
            }
            const std::uint8_t second = bytes[index++];
            const std::uint8_t third = bytes[index++];
            const std::uint8_t fourth = bytes[index++];
            if (!isContinuationByte(third) || !isContinuationByte(fourth)
                || (first == 0xf0U && (second < 0x90U || second > 0xbfU))
                || (first == 0xf4U && (second < 0x80U || second > 0x8fU))
                || (first != 0xf0U && first != 0xf4U && !isContinuationByte(second))) {
                return false;
            }
            continue;
        }
        return false;
    }
    return true;
}

IiscError trackString(const std::string &value,
                      const SerializationLimits &limits,
                      LimitTotals &totals)
{
    if (!isValidUtf8(value)) {
        return makeError(IiscErrorCode::InvalidDocument, 0,
                         "document strings must contain canonical UTF-8");
    }
    const std::uint64_t size = value.size();
    if (size > std::numeric_limits<std::uint32_t>::max()
        || size > limits.maximumStringBytes
        || !addWithin(totals.stringBytes, size, limits.maximumTotalStringBytes)) {
        return makeError(IiscErrorCode::LimitExceeded, 0,
                         "serialized string bytes exceed the configured limits");
    }
    return {};
}

IiscError checkDocumentLimits(const Document &document,
                              const SerializationLimits &limits)
{
    if (document.assets.size() > limits.maximumAssets
        || document.assets.size() > std::numeric_limits<std::uint32_t>::max()) {
        return makeError(IiscErrorCode::LimitExceeded, 0,
                         "asset count exceeds the configured limit");
    }
    if (document.layers.size() > limits.maximumLayers
        || document.layers.size() > std::numeric_limits<std::uint32_t>::max()) {
        return makeError(IiscErrorCode::LimitExceeded, 0,
                         "layer count exceeds the configured limit");
    }

    std::uint64_t canvasPixels = 0;
    if (!pixelCountWithin(document.extent.width,
                          document.extent.height,
                          limits.maximumCanvasPixels,
                          canvasPixels)) {
        return makeError(IiscErrorCode::LimitExceeded, 0,
                         "canvas pixel count exceeds the configured limit");
    }

    LimitTotals totals;
    for (const Asset &asset : document.assets) {
        if (IiscError error = trackString(assetId(asset), limits, totals);
            error.code != IiscErrorCode::None) {
            return error;
        }
        if (const auto *raster = std::get_if<RasterAsset>(&asset)) {
            const std::uint64_t pixels = raster->pixels.pixels.size();
            if (!addWithin(totals.rasterPixels,
                           pixels,
                           limits.maximumTotalRasterPixels)) {
                return makeError(IiscErrorCode::LimitExceeded, 0,
                                 "raster pixel count exceeds the configured limit");
            }
            continue;
        }

        const VectorAsset &vector = std::get<VectorAsset>(asset);
        std::uint64_t viewportPixels = 0;
        if (!pixelCountWithin(vector.viewport.width,
                              vector.viewport.height,
                              limits.maximumCanvasPixels,
                              viewportPixels)) {
            return makeError(IiscErrorCode::LimitExceeded, 0,
                             "vector viewport exceeds the configured canvas limit");
        }
        if (!addWithin(totals.vectorPaths,
                       vector.paths.size(),
                       limits.maximumTotalVectorPaths)) {
            return makeError(IiscErrorCode::LimitExceeded, 0,
                             "vector path count exceeds the configured limit");
        }
        for (const VectorPath &path : vector.paths) {
            if (path.commands.size() > std::numeric_limits<std::uint32_t>::max()
                || !addWithin(totals.pathCommands,
                              path.commands.size(),
                              limits.maximumTotalPathCommands)) {
                return makeError(IiscErrorCode::LimitExceeded, 0,
                                 "vector command count exceeds the configured limit");
            }
        }
    }

    for (const Layer &layer : document.layers) {
        for (const std::string *value : {&layer.id, &layer.name}) {
            if (IiscError error = trackString(*value, limits, totals);
                error.code != IiscErrorCode::None) {
                return error;
            }
        }
        if (const auto *source = std::get_if<StaticSource>(&layer.source)) {
            if (IiscError error = trackString(source->assetId, limits, totals);
                error.code != IiscErrorCode::None) {
                return error;
            }
            continue;
        }
        const auto &source = std::get<KeyframedSource>(layer.source);
        if (source.keyframes.size() > std::numeric_limits<std::uint32_t>::max()
            || !addWithin(totals.keyframes,
                          source.keyframes.size(),
                          limits.maximumTotalKeyframes)) {
            return makeError(IiscErrorCode::LimitExceeded, 0,
                             "keyframe count exceeds the configured limit");
        }
        for (const Keyframe &keyframe : source.keyframes) {
            if (IiscError error = trackString(keyframe.assetId, limits, totals);
                error.code != IiscErrorCode::None) {
                return error;
            }
        }
    }
    return {};
}

std::uint32_t crc32(std::span<const std::uint8_t> bytes) noexcept
{
    std::uint32_t crc = 0xffffffffU;
    for (const std::uint8_t byte : bytes) {
        crc ^= byte;
        for (int bit = 0; bit < 8; ++bit) {
            const std::uint32_t mask = 0U - (crc & 1U);
            crc = (crc >> 1U) ^ (0xedb88320U & mask);
        }
    }
    return crc ^ 0xffffffffU;
}

void writePoint(ByteWriter &writer, Point point)
{
    writer.writeDouble(point.x);
    writer.writeDouble(point.y);
}

void writePathCommand(ByteWriter &writer, const PathCommand &command)
{
    std::visit([&](const auto &value) {
        using Command = std::decay_t<decltype(value)>;
        if constexpr (std::is_same_v<Command, MoveTo>) {
            writer.writeU8(0);
            writePoint(writer, value.point);
        } else if constexpr (std::is_same_v<Command, LineTo>) {
            writer.writeU8(1);
            writePoint(writer, value.point);
        } else if constexpr (std::is_same_v<Command, QuadraticTo>) {
            writer.writeU8(2);
            writePoint(writer, value.control);
            writePoint(writer, value.end);
        } else if constexpr (std::is_same_v<Command, CubicTo>) {
            writer.writeU8(3);
            writePoint(writer, value.control1);
            writePoint(writer, value.control2);
            writePoint(writer, value.end);
        } else {
            writer.writeU8(4);
        }
    }, command);
}

std::uint8_t encodedBlendMode(RasterBlendMode mode)
{
    switch (mode) {
    case RasterBlendMode::SourceOver:
        return 0;
    case RasterBlendMode::Multiply:
        return 1;
    case RasterBlendMode::Screen:
        return 2;
    case RasterBlendMode::Overlay:
        return 3;
    case RasterBlendMode::DestinationOut:
        break;
    }
    return 0;
}

void writeRaster(ByteWriter &writer, const RasterLayer &raster)
{
    writer.writeI32(raster.width);
    writer.writeI32(raster.height);
    writer.writeU64(raster.pixels.size());

    std::uint64_t runCount = 0;
    std::uint32_t priorPixel = 0;
    std::uint32_t priorRunLength = 0;
    for (const std::uint32_t pixel : raster.pixels) {
        if (priorRunLength == 0 || pixel != priorPixel
            || priorRunLength == std::numeric_limits<std::uint32_t>::max()) {
            ++runCount;
            priorPixel = pixel;
            priorRunLength = 1;
        } else {
            ++priorRunLength;
        }
    }

    const std::uint64_t rawBytes = static_cast<std::uint64_t>(raster.pixels.size()) * 4U;
    const std::uint64_t runBytes = runCount * 8U;
    if (runBytes < rawBytes) {
        writer.writeU8(static_cast<std::uint8_t>(RasterEncoding::RunLengthArgb32));
        writer.writeU64(runBytes);
        std::uint32_t runPixel = raster.pixels.front();
        std::uint32_t runLength = 0;
        const auto flushRun = [&] {
            writer.writeU32(runLength);
            writer.writeU32(runPixel);
        };
        for (const std::uint32_t pixel : raster.pixels) {
            if (runLength > 0 && (pixel != runPixel
                                  || runLength == std::numeric_limits<std::uint32_t>::max())) {
                flushRun();
                runPixel = pixel;
                runLength = 0;
            }
            ++runLength;
        }
        if (runLength > 0) {
            flushRun();
        }
        return;
    }

    writer.writeU8(static_cast<std::uint8_t>(RasterEncoding::RawArgb32));
    writer.writeU64(rawBytes);
    for (const std::uint32_t pixel : raster.pixels) {
        writer.writeU32(pixel);
    }
}

void writeVector(ByteWriter &writer, const VectorAsset &vector)
{
    writer.writeI32(vector.viewport.width);
    writer.writeI32(vector.viewport.height);
    writer.writeU32(static_cast<std::uint32_t>(vector.paths.size()));
    for (const VectorPath &path : vector.paths) {
        writer.writeU32(static_cast<std::uint32_t>(path.commands.size()));
        for (const PathCommand &command : path.commands) {
            writePathCommand(writer, command);
        }
        writer.writeU8(path.fill ? 1U : 0U);
        if (path.fill) {
            writer.writeU32(path.fill->argb);
        }
        writer.writeU8(path.stroke ? 1U : 0U);
        if (path.stroke) {
            writer.writeU32(path.stroke->paint.argb);
            writer.writeDouble(path.stroke->width);
        }
    }
}

void writePayload(ByteWriter &writer, const Document &document)
{
    writer.writeI32(document.extent.width);
    writer.writeI32(document.extent.height);
    writer.writeU32(document.timeline.frameRate.numerator);
    writer.writeU32(document.timeline.frameRate.denominator);
    writer.writeU32(document.timeline.frameCount);

    writer.writeU32(static_cast<std::uint32_t>(document.assets.size()));
    for (const Asset &asset : document.assets) {
        writer.writeU8(std::holds_alternative<RasterAsset>(asset) ? 0U : 1U);
        writer.writeString(assetId(asset));
        if (const auto *raster = std::get_if<RasterAsset>(&asset)) {
            writeRaster(writer, raster->pixels);
        } else {
            writeVector(writer, std::get<VectorAsset>(asset));
        }
    }

    writer.writeU32(static_cast<std::uint32_t>(document.layers.size()));
    for (const Layer &layer : document.layers) {
        writer.writeString(layer.id);
        writer.writeString(layer.name);
        writer.writeU8(layer.visible ? 1U : 0U);
        writer.writeDouble(layer.opacity);
        writer.writeDouble(layer.transform.m11);
        writer.writeDouble(layer.transform.m12);
        writer.writeDouble(layer.transform.m21);
        writer.writeDouble(layer.transform.m22);
        writer.writeDouble(layer.transform.translationX);
        writer.writeDouble(layer.transform.translationY);
        writer.writeU8(encodedBlendMode(layer.blendMode));
        if (const auto *source = std::get_if<StaticSource>(&layer.source)) {
            writer.writeU8(0);
            writer.writeString(source->assetId);
            continue;
        }
        const auto &source = std::get<KeyframedSource>(layer.source);
        writer.writeU8(1);
        writer.writeU8(source.kind == ContentKind::Raster ? 0U : 1U);
        writer.writeU32(static_cast<std::uint32_t>(source.keyframes.size()));
        for (const Keyframe &keyframe : source.keyframes) {
            writer.writeU32(keyframe.frame);
            writer.writeString(keyframe.assetId);
        }
    }
}

class DocumentReader final {
public:
    DocumentReader(ByteReader &reader, const SerializationLimits &limits)
        : m_reader(reader),
          m_limits(limits)
    {
    }

    Document read(FormatVersion version)
    {
        Document document;
        document.formatVersion = version;
        document.extent = {m_reader.readI32(), m_reader.readI32()};
        document.timeline.frameRate = {m_reader.readU32(), m_reader.readU32()};
        document.timeline.frameCount = m_reader.readU32();

        std::uint64_t canvasPixels = 0;
        if (!pixelCountWithin(document.extent.width,
                              document.extent.height,
                              m_limits.maximumCanvasPixels,
                              canvasPixels)) {
            m_reader.fail(IiscErrorCode::LimitExceeded,
                          "decoded canvas pixel count exceeds the configured limit");
        }

        const std::uint32_t assetCount = limitedCount(m_limits.maximumAssets, "asset");
        document.assets.reserve(assetCount);
        for (std::uint32_t index = 0; index < assetCount; ++index) {
            const std::uint8_t kind = m_reader.readU8();
            const std::string id = readString();
            if (kind == 0) {
                document.assets.emplace_back(RasterAsset{id, readRaster()});
            } else if (kind == 1) {
                document.assets.emplace_back(readVector(id));
            } else {
                m_reader.fail(IiscErrorCode::InvalidData, "unknown asset kind tag");
            }
        }

        const std::uint32_t layerCount = limitedCount(m_limits.maximumLayers, "layer");
        document.layers.reserve(layerCount);
        for (std::uint32_t index = 0; index < layerCount; ++index) {
            document.layers.push_back(readLayer());
        }
        return document;
    }

private:
    std::uint32_t limitedCount(std::uint32_t maximum, const char *kind)
    {
        const std::uint32_t count = m_reader.readU32();
        if (count > maximum) {
            m_reader.fail(IiscErrorCode::LimitExceeded,
                          std::string(kind) + " count exceeds the configured limit");
        }
        return count;
    }

    void addTotal(std::uint64_t &total,
                  std::uint64_t amount,
                  std::uint64_t maximum,
                  const char *kind)
    {
        if (!addWithin(total, amount, maximum)) {
            m_reader.fail(IiscErrorCode::LimitExceeded,
                          std::string(kind) + " count exceeds the configured limit");
        }
    }

    std::string readString()
    {
        const std::uint32_t size = m_reader.readU32();
        if (size > m_limits.maximumStringBytes) {
            m_reader.fail(IiscErrorCode::LimitExceeded,
                          "string length exceeds the configured limit");
        }
        addTotal(m_totals.stringBytes,
                 size,
                 m_limits.maximumTotalStringBytes,
                 "string byte");
        const std::span<const std::uint8_t> bytes = m_reader.readBytes(size);
        std::string value{reinterpret_cast<const char *>(bytes.data()), bytes.size()};
        if (!isValidUtf8(value)) {
            m_reader.fail(IiscErrorCode::InvalidData,
                          "serialized strings must contain canonical UTF-8");
        }
        return value;
    }

    bool readBoolean()
    {
        const std::uint8_t value = m_reader.readU8();
        if (value > 1U) {
            m_reader.fail(IiscErrorCode::InvalidData, "boolean value must be zero or one");
        }
        return value != 0;
    }

    Point readPoint()
    {
        return {m_reader.readDouble(), m_reader.readDouble()};
    }

    PathCommand readPathCommand()
    {
        switch (m_reader.readU8()) {
        case 0:
            return MoveTo{readPoint()};
        case 1:
            return LineTo{readPoint()};
        case 2:
            return QuadraticTo{readPoint(), readPoint()};
        case 3:
            return CubicTo{readPoint(), readPoint(), readPoint()};
        case 4:
            return ClosePath{};
        default:
            m_reader.fail(IiscErrorCode::InvalidData, "unknown vector path command tag");
        }
    }

    RasterLayer readRaster()
    {
        const std::int32_t width = m_reader.readI32();
        const std::int32_t height = m_reader.readI32();
        const std::uint64_t declaredPixels = m_reader.readU64();
        std::uint64_t expectedPixels = 0;
        if (!pixelCountWithin(width,
                              height,
                              m_limits.maximumTotalRasterPixels,
                              expectedPixels)
            || declaredPixels != expectedPixels) {
            m_reader.fail(IiscErrorCode::InvalidData,
                          "raster dimensions do not match the declared pixel count");
        }
        addTotal(m_totals.rasterPixels,
                 expectedPixels,
                 m_limits.maximumTotalRasterPixels,
                 "raster pixel");

        const RasterEncoding encoding = static_cast<RasterEncoding>(m_reader.readU8());
        const std::uint64_t encodedByteCount = m_reader.readU64();
        const std::uint64_t encodedOffset = m_reader.offset();
        ByteReader encoded(m_reader.readBytes(encodedByteCount), encodedOffset);

        const std::uint64_t rawByteCount = expectedPixels * 4U;
        if (encoding == RasterEncoding::RawArgb32) {
            if (encodedByteCount != rawByteCount) {
                encoded.fail(IiscErrorCode::InvalidData,
                             "raw raster byte count does not match its pixel count");
            }
        } else if (encoding == RasterEncoding::RunLengthArgb32) {
            if (encodedByteCount % 8U != 0 || encodedByteCount >= rawByteCount) {
                encoded.fail(IiscErrorCode::InvalidData,
                             "run-length raster encoding is not canonical");
            }
        } else {
            encoded.fail(IiscErrorCode::InvalidData, "unknown raster encoding tag");
        }

        RasterLayer raster;
        raster.width = width;
        raster.height = height;
        raster.pixels.resize(static_cast<std::size_t>(expectedPixels));
        if (encoding == RasterEncoding::RawArgb32) {
            std::uint64_t runCount = 0;
            std::uint32_t priorPixel = 0;
            for (std::uint32_t &pixel : raster.pixels) {
                pixel = encoded.readU32();
                if (runCount == 0 || pixel != priorPixel) {
                    ++runCount;
                    priorPixel = pixel;
                }
            }
            if (runCount * 8U < rawByteCount) {
                encoded.fail(IiscErrorCode::InvalidData,
                             "raw raster must use the smaller canonical run-length encoding");
            }
        } else if (encoding == RasterEncoding::RunLengthArgb32) {
            std::uint64_t position = 0;
            bool hasPriorRun = false;
            std::uint32_t priorRunPixel = 0;
            while (!encoded.empty()) {
                const std::uint32_t length = encoded.readU32();
                const std::uint32_t pixel = encoded.readU32();
                if (length == 0 || length > expectedPixels - position) {
                    encoded.fail(IiscErrorCode::InvalidData,
                                 "run-length raster records exceed the pixel count");
                }
                if (hasPriorRun && pixel == priorRunPixel) {
                    encoded.fail(IiscErrorCode::InvalidData,
                                 "adjacent run-length records are not canonical");
                }
                std::fill_n(raster.pixels.begin() + static_cast<std::ptrdiff_t>(position),
                            length,
                            pixel);
                position += length;
                priorRunPixel = pixel;
                hasPriorRun = true;
            }
            if (position != expectedPixels) {
                encoded.fail(IiscErrorCode::InvalidData,
                             "run-length raster records do not fill the declared pixels");
            }
        }
        if (!encoded.empty()) {
            encoded.fail(IiscErrorCode::TrailingData,
                         "raster record contains unconsumed encoded bytes");
        }
        return raster;
    }

    VectorAsset readVector(std::string id)
    {
        VectorAsset vector;
        vector.id = std::move(id);
        vector.viewport = {m_reader.readI32(), m_reader.readI32()};
        std::uint64_t viewportPixels = 0;
        if (!pixelCountWithin(vector.viewport.width,
                              vector.viewport.height,
                              m_limits.maximumCanvasPixels,
                              viewportPixels)) {
            m_reader.fail(IiscErrorCode::LimitExceeded,
                          "vector viewport exceeds the configured canvas limit");
        }

        const std::uint32_t pathCount = m_reader.readU32();
        addTotal(m_totals.vectorPaths,
                 pathCount,
                 m_limits.maximumTotalVectorPaths,
                 "vector path");
        vector.paths.reserve(pathCount);
        for (std::uint32_t pathIndex = 0; pathIndex < pathCount; ++pathIndex) {
            VectorPath path;
            const std::uint32_t commandCount = m_reader.readU32();
            addTotal(m_totals.pathCommands,
                     commandCount,
                     m_limits.maximumTotalPathCommands,
                     "vector command");
            path.commands.reserve(commandCount);
            for (std::uint32_t commandIndex = 0;
                 commandIndex < commandCount;
                 ++commandIndex) {
                path.commands.push_back(readPathCommand());
            }
            if (readBoolean()) {
                path.fill = SolidPaint{m_reader.readU32()};
            }
            if (readBoolean()) {
                path.stroke = StrokeStyle{SolidPaint{m_reader.readU32()},
                                          m_reader.readDouble()};
            }
            vector.paths.push_back(std::move(path));
        }
        return vector;
    }

    RasterBlendMode readBlendMode()
    {
        switch (m_reader.readU8()) {
        case 0:
            return RasterBlendMode::SourceOver;
        case 1:
            return RasterBlendMode::Multiply;
        case 2:
            return RasterBlendMode::Screen;
        case 3:
            return RasterBlendMode::Overlay;
        default:
            m_reader.fail(IiscErrorCode::InvalidData, "unknown layer blend mode tag");
        }
    }

    ContentKind readContentKind()
    {
        switch (m_reader.readU8()) {
        case 0:
            return ContentKind::Raster;
        case 1:
            return ContentKind::Vector;
        default:
            m_reader.fail(IiscErrorCode::InvalidData, "unknown content kind tag");
        }
    }

    Layer readLayer()
    {
        Layer layer;
        layer.id = readString();
        layer.name = readString();
        layer.visible = readBoolean();
        layer.opacity = m_reader.readDouble();
        layer.transform.m11 = m_reader.readDouble();
        layer.transform.m12 = m_reader.readDouble();
        layer.transform.m21 = m_reader.readDouble();
        layer.transform.m22 = m_reader.readDouble();
        layer.transform.translationX = m_reader.readDouble();
        layer.transform.translationY = m_reader.readDouble();
        layer.blendMode = readBlendMode();

        const std::uint8_t sourceKind = m_reader.readU8();
        if (sourceKind == 0) {
            layer.source = StaticSource{readString()};
        } else if (sourceKind == 1) {
            KeyframedSource source;
            source.kind = readContentKind();
            const std::uint32_t keyframeCount = m_reader.readU32();
            addTotal(m_totals.keyframes,
                     keyframeCount,
                     m_limits.maximumTotalKeyframes,
                     "keyframe");
            source.keyframes.reserve(keyframeCount);
            for (std::uint32_t index = 0; index < keyframeCount; ++index) {
                source.keyframes.push_back({m_reader.readU32(), readString()});
            }
            layer.source = std::move(source);
        } else {
            m_reader.fail(IiscErrorCode::InvalidData, "unknown layer source tag");
        }
        return layer;
    }

    ByteReader &m_reader;
    const SerializationLimits &m_limits;
    LimitTotals m_totals;
};

} // namespace

IiscEncodeResult encodeIisc(const Document &document, SerializationLimits limits)
{
    const ValidationResult validation = validate(document);
    if (!validation.ok()) {
        return {{}, makeError(IiscErrorCode::InvalidDocument,
                              0,
                              validation.issues.front().path + ": "
                                  + validation.issues.front().message)};
    }
    if (IiscError error = checkDocumentLimits(document, limits);
        error.code != IiscErrorCode::None) {
        return {{}, std::move(error)};
    }

    ByteWriter payloadWriter;
    writePayload(payloadWriter, document);
    std::vector<std::uint8_t> payload = payloadWriter.takeBytes();
    if (payload.size() > std::numeric_limits<std::uint64_t>::max() - IiscHeaderSize
        || payload.size() + IiscHeaderSize > limits.maximumContainerBytes) {
        return {{}, makeError(IiscErrorCode::LimitExceeded,
                              0,
                              "encoded container exceeds the configured byte limit")};
    }

    ByteWriter container;
    container.writeBytes(ContainerMagic);
    container.writeU16(document.formatVersion.major);
    container.writeU16(document.formatVersion.minor);
    container.writeU32(ContainerFlags);
    container.writeU64(payload.size());
    container.writeU32(crc32(payload));
    container.writeU32(ContainerReserved);
    container.writeBytes(payload);
    return {container.takeBytes(), {}};
}

IiscDecodeResult decodeIisc(std::span<const std::uint8_t> bytes,
                            SerializationLimits limits)
{
    if (bytes.size() > limits.maximumContainerBytes) {
        return {{}, makeError(IiscErrorCode::LimitExceeded,
                              0,
                              "container exceeds the configured byte limit")};
    }

    try {
        ByteReader container(bytes);
        const std::span<const std::uint8_t> magic = container.readBytes(ContainerMagic.size());
        if (!std::equal(magic.begin(), magic.end(), ContainerMagic.begin())) {
            container.fail(IiscErrorCode::InvalidHeader, "container magic is not .iisc");
        }

        const FormatVersion version{container.readU16(), container.readU16()};
        const std::uint32_t flags = container.readU32();
        const std::uint64_t payloadSize = container.readU64();
        const std::uint32_t expectedChecksum = container.readU32();
        const std::uint32_t reserved = container.readU32();
        if (version.major != CurrentFormatMajor || version.minor > CurrentFormatMinor) {
            container.fail(IiscErrorCode::UnsupportedVersion,
                           "container version is newer than this reader");
        }
        if (flags != ContainerFlags || reserved != ContainerReserved) {
            container.fail(IiscErrorCode::InvalidHeader,
                           "container header flags or reserved bytes are not canonical");
        }
        if (payloadSize > container.remaining()) {
            container.fail(IiscErrorCode::TruncatedData,
                           "declared payload extends beyond the container");
        }
        if (payloadSize < container.remaining()) {
            container.fail(IiscErrorCode::TrailingData,
                           "container has bytes after its declared payload");
        }

        const std::uint64_t payloadOffset = container.offset();
        const std::span<const std::uint8_t> payload = container.readBytes(payloadSize);
        if (crc32(payload) != expectedChecksum) {
            throw DecodeFailure(IiscErrorCode::ChecksumMismatch,
                                payloadOffset,
                                "container payload checksum does not match");
        }

        ByteReader payloadReader(payload, payloadOffset);
        DocumentReader reader(payloadReader, limits);
        Document document = reader.read(version);
        if (!payloadReader.empty()) {
            payloadReader.fail(IiscErrorCode::TrailingData,
                               "payload has unconsumed canonical data");
        }
        const ValidationResult validation = validate(document);
        if (!validation.ok()) {
            payloadReader.fail(IiscErrorCode::InvalidData,
                               validation.issues.front().path + ": "
                                   + validation.issues.front().message);
        }
        return {std::move(document), {}};
    } catch (const DecodeFailure &failure) {
        return {{}, makeError(failure.code, failure.offset, failure.what())};
    }
}

} // namespace iiSharedCanvas
