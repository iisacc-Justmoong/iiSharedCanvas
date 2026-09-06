#include "Serialization/IiscCodec.h"
#include "Serialization/DocumentRecords_p.hpp"

#include "Validation/Validation.h"

#include <algorithm>
#include <array>
#include <bit>
#include <cstddef>
#include <cstdint>
#include <limits>
#include <optional>
#include <span>
#include <stdexcept>
#include <string>
#include <string_view>
#include <type_traits>
#include <unordered_map>
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
        return std::exchange(m_bytes, {});
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
    std::uint64_t rasterChunks = 0;
    std::uint64_t vectorPaths = 0;
    std::uint64_t pathCommands = 0;
    std::uint64_t keyframes = 0;
    std::uint64_t stringBytes = 0;
    std::uint64_t metadataEntries = 0;
    std::uint64_t audioSamples = 0;
    std::uint64_t audioClips = 0;
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
                      LimitTotals &totals,
                      std::optional<std::uint32_t> maximumStringBytes = std::nullopt)
{
    if (!isValidUtf8(value)) {
        return makeError(IiscErrorCode::InvalidDocument, 0,
                         "document strings must contain canonical UTF-8");
    }
    const std::uint64_t size = value.size();
    const std::uint32_t individualMaximum =
        maximumStringBytes.value_or(limits.maximumStringBytes);
    if (size > std::numeric_limits<std::uint32_t>::max()
        || size > individualMaximum
        || !addWithin(totals.stringBytes, size, limits.maximumTotalStringBytes)) {
        return makeError(IiscErrorCode::LimitExceeded, 0,
                         "document string bytes exceed the configured limits");
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
    if (document.frames.size() > limits.maximumFrames) {
        return makeError(IiscErrorCode::LimitExceeded, 0,
                         "owned frame count exceeds the configured limit");
    }
    if (document.audioAssets.size() > limits.maximumAudioAssets
        || document.audioTracks.size() > limits.maximumAudioTracks) {
        return makeError(IiscErrorCode::LimitExceeded, 0,
                         "audio asset or track count exceeds the configured limit");
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
    for (const AudioAsset &asset : document.audioAssets) {
        if (IiscError error = trackString(asset.id, limits, totals);
            error.code != IiscErrorCode::None) {
            return error;
        }
        if (!addWithin(totals.audioSamples, asset.samples.size(),
                       std::min(limits.maximumTotalAudioSamples,
                                limits.maximumContainerBytes / 2))) {
            return makeError(IiscErrorCode::LimitExceeded, 0,
                             "audio sample count exceeds the configured limit");
        }
    }
    for (const AudioTrackLayer &track : document.audioTracks) {
        for (const std::string *value : {&track.id, &track.name}) {
            if (IiscError error = trackString(*value, limits, totals);
                error.code != IiscErrorCode::None) {
                return error;
            }
        }
        if (track.clips.size() > std::numeric_limits<std::uint32_t>::max()
            || !addWithin(totals.audioClips, track.clips.size(),
                          limits.maximumTotalAudioClips)) {
            return makeError(IiscErrorCode::LimitExceeded, 0,
                             "audio clip count exceeds the configured limit");
        }
        for (const AudioClip &clip : track.clips) {
            for (const std::string *value : {&clip.id, &clip.name, &clip.assetId}) {
                if (IiscError error = trackString(*value, limits, totals);
                    error.code != IiscErrorCode::None) {
                    return error;
                }
            }
        }
    }
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

        if (const auto *chunked = std::get_if<ChunkedRasterAsset>(&asset)) {
            if (!addWithin(totals.rasterChunks,
                           chunked->chunks.size(),
                           limits.maximumRasterChunks)) {
                return makeError(IiscErrorCode::LimitExceeded, 0,
                                 "raster chunk count exceeds the configured limit");
            }
            for (const RasterChunk &chunk : chunked->chunks) {
                if (!addWithin(totals.rasterPixels,
                               chunk.pixels.pixels.size(),
                               limits.maximumTotalRasterPixels)) {
                    return makeError(IiscErrorCode::LimitExceeded, 0,
                                     "raster pixel count exceeds the configured limit");
                }
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

    std::unordered_map<std::string_view, std::uint64_t> keyframeCountsByLayer;
    keyframeCountsByLayer.reserve(document.layers.size());
    for (const Layer &layer : document.layers) {
        const LayerProperties &properties = layerProperties(layer);
        const LayerSource &sourceValue = layerSource(layer);
        for (const std::string *value : {&properties.id, &properties.name}) {
            if (IiscError error = trackString(*value, limits, totals);
                error.code != IiscErrorCode::None) {
                return error;
            }
        }
        if (const auto *source = std::get_if<StaticSource>(&sourceValue)) {
            if (IiscError error = trackString(source->assetId, limits, totals);
                error.code != IiscErrorCode::None) {
                return error;
            }
            continue;
        }
        keyframeCountsByLayer.emplace(properties.id, 0);
    }

    for (const Frame &frame : document.frames) {
        if (!addWithin(totals.keyframes,
                       frame.keyframes.size(),
                       limits.maximumTotalKeyframes)) {
            return makeError(IiscErrorCode::LimitExceeded, 0,
                             "keyframe count exceeds the configured limit");
        }
        for (const Keyframe &keyframe : frame.keyframes) {
            const auto layer = keyframeCountsByLayer.find(keyframe.layerId);
            if (layer == keyframeCountsByLayer.end()) {
                return makeError(IiscErrorCode::InvalidDocument, 0,
                                 "frame keyframe references a non-keyframed layer");
            }
            if (layer->second == std::numeric_limits<std::uint32_t>::max()) {
                return makeError(IiscErrorCode::LimitExceeded, 0,
                                 "per-layer keyframe count exceeds the canonical format");
            }
            ++layer->second;
            if (IiscError error = trackString(keyframe.layerId, limits, totals);
                error.code != IiscErrorCode::None) {
                return error;
            }
            if (IiscError error = trackString(keyframe.assetId, limits, totals);
                error.code != IiscErrorCode::None) {
                return error;
            }
        }
    }

    if (document.stableDiffusionMetadata) {
        const StableDiffusionMetadata &metadata = *document.stableDiffusionMetadata;
        const auto trackMetadataString = [&](const std::string &value) {
            return trackString(value,
                               limits,
                               totals,
                               limits.maximumMetadataStringBytes);
        };
        const auto trackMetadataCount = [&](std::size_t count) {
            if (count > std::numeric_limits<std::uint32_t>::max()
                || !addWithin(totals.metadataEntries,
                              count,
                              limits.maximumMetadataEntries)) {
                return makeError(IiscErrorCode::LimitExceeded, 0,
                                 "generation metadata entry count exceeds the configured limit");
            }
            return IiscError{};
        };

        for (const std::string *value : {
                 &metadata.positivePrompt,
                 &metadata.negativePrompt,
                 &metadata.software,
                 &metadata.softwareVersion,
                 &metadata.createdAt,
                 &metadata.generationParametersText,
                 &metadata.comfyUi.promptJson,
                 &metadata.comfyUi.workflowJson,
             }) {
            if (IiscError error = trackMetadataString(*value);
                error.code != IiscErrorCode::None) {
                return error;
            }
        }

        for (std::size_t count : {
                 metadata.samplingPasses.size(),
                 metadata.models.size(),
                 metadata.loras.size(),
                 metadata.comfyUi.extraPngInfo.size(),
                 metadata.extraParameters.size(),
             }) {
            if (IiscError error = trackMetadataCount(count);
                error.code != IiscErrorCode::None) {
                return error;
            }
        }
        for (const StableDiffusionSamplingPass &pass : metadata.samplingPasses) {
            for (const std::string *value : {
                     &pass.nodeId, &pass.samplerName, &pass.scheduler,
                 }) {
                if (IiscError error = trackMetadataString(*value);
                    error.code != IiscErrorCode::None) {
                    return error;
                }
            }
        }
        for (const StableDiffusionModelResource &model : metadata.models) {
            for (const std::string *value : {
                     &model.role, &model.name, &model.hash, &model.hashType,
                     &model.uri,
                 }) {
                if (IiscError error = trackMetadataString(*value);
                    error.code != IiscErrorCode::None) {
                    return error;
                }
            }
        }
        for (const StableDiffusionLora &lora : metadata.loras) {
            for (const std::string *value : {&lora.name, &lora.hash}) {
                if (IiscError error = trackMetadataString(*value);
                    error.code != IiscErrorCode::None) {
                    return error;
                }
            }
        }
        for (const StableDiffusionMetadataEntry &entry
             : metadata.comfyUi.extraPngInfo) {
            for (const std::string *value : {&entry.key, &entry.value}) {
                if (IiscError error = trackMetadataString(*value);
                    error.code != IiscErrorCode::None) {
                    return error;
                }
            }
        }
        for (const StableDiffusionMetadataEntry &entry : metadata.extraParameters) {
            for (const std::string *value : {&entry.key, &entry.value}) {
                if (IiscError error = trackMetadataString(*value);
                    error.code != IiscErrorCode::None) {
                    return error;
                }
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

void writeRaster(ByteWriter &writer, const RasterLayer &raster, bool compress = true)
{
    writer.writeI32(raster.width);
    writer.writeI32(raster.height);
    writer.writeU64(raster.pixels.size());

    std::uint64_t runCount = 0;
    std::uint32_t priorPixel = 0;
    std::uint32_t priorRunLength = 0;
    if (compress) {
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
    }

    const std::uint64_t rawBytes = static_cast<std::uint64_t>(raster.pixels.size()) * 4U;
    const std::uint64_t runBytes = runCount * 8U;
    if (compress && runBytes < rawBytes) {
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

void writeChunkedRaster(ByteWriter &writer, const ChunkedRasterAsset &chunked,
                        bool compress = true)
{
    writer.writeU32(static_cast<std::uint32_t>(chunked.chunks.size()));
    for (const RasterChunk &chunk : chunked.chunks) {
        writer.writeI32(chunk.column);
        writer.writeI32(chunk.row);
        writeRaster(writer, chunk.pixels, compress);
    }
}

void writeOptionalU32(ByteWriter &writer,
                      const std::optional<std::uint32_t> &value)
{
    writer.writeU8(value ? 1U : 0U);
    if (value) {
        writer.writeU32(*value);
    }
}

void writeOptionalU64(ByteWriter &writer,
                      const std::optional<std::uint64_t> &value)
{
    writer.writeU8(value ? 1U : 0U);
    if (value) {
        writer.writeU64(*value);
    }
}

void writeOptionalDouble(ByteWriter &writer,
                         const std::optional<double> &value)
{
    writer.writeU8(value ? 1U : 0U);
    if (value) {
        writer.writeDouble(*value);
    }
}

void writeStableDiffusionMetadata(ByteWriter &writer,
                                  const StableDiffusionMetadata &metadata)
{
    writer.writeString(metadata.positivePrompt);
    writer.writeString(metadata.negativePrompt);
    writer.writeU8(metadata.outputExtent ? 1U : 0U);
    if (metadata.outputExtent) {
        writer.writeU32(metadata.outputExtent->width);
        writer.writeU32(metadata.outputExtent->height);
    }
    writeOptionalU32(writer, metadata.batchSize);
    writeOptionalU32(writer, metadata.clipSkip);

    writer.writeU32(static_cast<std::uint32_t>(metadata.samplingPasses.size()));
    for (const StableDiffusionSamplingPass &pass : metadata.samplingPasses) {
        writer.writeString(pass.nodeId);
        writeOptionalU64(writer, pass.seed);
        writeOptionalU32(writer, pass.steps);
        writeOptionalDouble(writer, pass.cfgScale);
        writer.writeString(pass.samplerName);
        writer.writeString(pass.scheduler);
        writeOptionalDouble(writer, pass.denoiseStrength);
        writeOptionalU32(writer, pass.startStep);
        writeOptionalU32(writer, pass.endStep);
    }

    writer.writeU32(static_cast<std::uint32_t>(metadata.models.size()));
    for (const StableDiffusionModelResource &model : metadata.models) {
        writer.writeString(model.role);
        writer.writeString(model.name);
        writer.writeString(model.hash);
        writer.writeString(model.hashType);
        writer.writeString(model.uri);
    }

    writer.writeU32(static_cast<std::uint32_t>(metadata.loras.size()));
    for (const StableDiffusionLora &lora : metadata.loras) {
        writer.writeString(lora.name);
        writer.writeString(lora.hash);
        writer.writeDouble(lora.modelStrength);
        writer.writeDouble(lora.clipStrength);
    }

    writer.writeString(metadata.software);
    writer.writeString(metadata.softwareVersion);
    writer.writeString(metadata.createdAt);
    writer.writeString(metadata.generationParametersText);
    writer.writeString(metadata.comfyUi.promptJson);
    writer.writeString(metadata.comfyUi.workflowJson);

    writer.writeU32(static_cast<std::uint32_t>(
        metadata.comfyUi.extraPngInfo.size()));
    for (const StableDiffusionMetadataEntry &entry
         : metadata.comfyUi.extraPngInfo) {
        writer.writeString(entry.key);
        writer.writeString(entry.value);
    }

    writer.writeU32(static_cast<std::uint32_t>(metadata.extraParameters.size()));
    for (const StableDiffusionMetadataEntry &entry : metadata.extraParameters) {
        writer.writeString(entry.key);
        writer.writeString(entry.value);
    }
}

bool sameRasterPixels(const RasterLayer &left, const RasterLayer &right)
{
    return left.width == right.width && left.height == right.height
        && left.pixels == right.pixels;
}

bool unchangedRasterAsset(const Asset &asset, const Document *previous)
{
    const Asset *prior = previous ? findAsset(*previous, assetId(asset)) : nullptr;
    if (!prior || prior->index() != asset.index()) {
        return false;
    }
    if (const auto *raster = std::get_if<RasterAsset>(&asset)) {
        return sameRasterPixels(raster->pixels, std::get<RasterAsset>(*prior).pixels);
    }
    if (const auto *chunked = std::get_if<ChunkedRasterAsset>(&asset)) {
        const auto &priorChunks = std::get<ChunkedRasterAsset>(*prior).chunks;
        return chunked->chunks.size() == priorChunks.size()
            && std::equal(chunked->chunks.begin(), chunked->chunks.end(),
                          priorChunks.begin(), [](const RasterChunk &left, const RasterChunk &right) {
                return left.column == right.column && left.row == right.row
                    && sameRasterPixels(left.pixels, right.pixels);
            });
    }
    return false;
}

void writePayload(ByteWriter &writer, const Document &document,
                  std::vector<detail::DocumentRecord> *records = nullptr,
                  const Document *previous = nullptr)
{
    const auto record = [&](detail::RecordKind kind, std::string id = {},
                            std::uint32_t position = 0) {
        if (records) {
            records->push_back({kind, std::move(id), position, writer.takeBytes()});
        }
    };
    if (records) {
        writer.writeU16(document.formatVersion.major);
        writer.writeU16(document.formatVersion.minor);
        writer.writeI32(document.infiniteCanvas.chunkSize);
    }
    writer.writeI32(document.extent.width);
    writer.writeI32(document.extent.height);
    if (document.formatVersion.minor >= 1) {
        writer.writeU8(document.canvasMode == CanvasMode::Infinite ? 1U : 0U);
        if (document.canvasMode == CanvasMode::Infinite) {
            writer.writeI32(document.infiniteCanvas.origin.x);
            writer.writeI32(document.infiniteCanvas.origin.y);
            writer.writeI32(document.infiniteCanvas.chunkSize);
        }
    }
    writer.writeU32(document.timeline.frameRate.numerator);
    writer.writeU32(document.timeline.frameRate.denominator);
    writer.writeU32(document.timeline.frameCount);

    writer.writeU32(static_cast<std::uint32_t>(document.assets.size()));
    record(detail::RecordKind::Header);
    std::uint32_t assetPosition = 0;
    for (const Asset &asset : document.assets) {
        if (records && unchangedRasterAsset(asset, previous)) {
            records->push_back({detail::RecordKind::Asset, assetId(asset), assetPosition++, std::nullopt});
            continue;
        }
        const std::uint8_t kind = std::holds_alternative<RasterAsset>(asset)
            ? 0U
            : (std::holds_alternative<VectorAsset>(asset) ? 1U : 2U);
        writer.writeU8(kind);
        writer.writeString(assetId(asset));
        if (const auto *raster = std::get_if<RasterAsset>(&asset)) {
            writeRaster(writer, raster->pixels, !records);
        } else if (const auto *vector = std::get_if<VectorAsset>(&asset)) {
            writeVector(writer, *vector);
        } else {
            writeChunkedRaster(writer, std::get<ChunkedRasterAsset>(asset), !records);
        }
        record(detail::RecordKind::Asset, assetId(asset), assetPosition++);
    }

    using ProjectedKeyframe = std::pair<FrameIndex, const Keyframe *>;
    std::vector<std::vector<ProjectedKeyframe>> keyframesByLayer(
        document.layers.size());
    std::unordered_map<std::string_view, std::size_t> layerIndices;
    layerIndices.reserve(document.layers.size());
    for (std::size_t index = 0; index < document.layers.size(); ++index) {
        if (std::holds_alternative<KeyframedSource>(
                layerSource(document.layers[index]))) {
            layerIndices.emplace(layerProperties(document.layers[index]).id, index);
        }
    }
    for (const Frame &frame : document.frames) {
        for (const Keyframe &keyframe : frame.keyframes) {
            const auto layer = layerIndices.find(keyframe.layerId);
            if (layer != layerIndices.end()) {
                keyframesByLayer[layer->second].emplace_back(frame.index,
                                                             &keyframe);
            }
        }
    }

    writer.writeU32(static_cast<std::uint32_t>(document.layers.size()));
    record(detail::RecordKind::LayerCount);
    for (std::size_t layerIndex = 0;
         layerIndex < document.layers.size();
         ++layerIndex) {
        const Layer &layer = document.layers[layerIndex];
        const LayerProperties &properties = layerProperties(layer);
        const LayerSource &sourceValue = layerSource(layer);
        writer.writeString(properties.id);
        writer.writeString(properties.name);
        writer.writeU8(properties.visible ? 1U : 0U);
        writer.writeDouble(properties.opacity);
        writer.writeDouble(properties.transform.m11);
        writer.writeDouble(properties.transform.m12);
        writer.writeDouble(properties.transform.m21);
        writer.writeDouble(properties.transform.m22);
        writer.writeDouble(properties.transform.translationX);
        writer.writeDouble(properties.transform.translationY);
        writer.writeU8(encodedBlendMode(properties.blendMode));
        if (const auto *source = std::get_if<StaticSource>(&sourceValue)) {
            writer.writeU8(0);
            writer.writeString(source->assetId);
        } else {
            const auto &keyframes = keyframesByLayer[layerIndex];
            writer.writeU8(1);
            writer.writeU8(contentKind(layer) == ContentKind::Raster ? 0U : 1U);
            writer.writeU32(static_cast<std::uint32_t>(keyframes.size()));
            for (const auto &[frame, keyframe] : keyframes) {
                writer.writeU32(frame);
                writer.writeString(keyframe->assetId);
            }
        }
        if (document.formatVersion.minor >= 3) {
            writer.writeU8(properties.frameRange ? 1U : 0U);
            if (properties.frameRange) {
                writer.writeU32(properties.frameRange->firstFrame);
                writer.writeU32(properties.frameRange->lastFrame);
            }
        }
        record(detail::RecordKind::Layer, properties.id, static_cast<std::uint32_t>(layerIndex));
    }
    if (document.formatVersion.minor >= 2) {
        writer.writeU8(document.stableDiffusionMetadata ? 1U : 0U);
        if (document.stableDiffusionMetadata) {
            writeStableDiffusionMetadata(writer,
                                         *document.stableDiffusionMetadata);
        }
    }
    record(detail::RecordKind::Metadata);
    if (document.formatVersion.minor >= 4) {
        writer.writeU32(static_cast<std::uint32_t>(document.audioAssets.size()));
        record(detail::RecordKind::AudioAssetCount);
        for (std::size_t index = 0; index < document.audioAssets.size(); ++index) {
            const AudioAsset &asset = document.audioAssets[index];
            const AudioAsset *prior = previous ? findAudioAsset(*previous, asset.id) : nullptr;
            if (records && prior && *prior == asset) {
                records->push_back({detail::RecordKind::AudioAsset, asset.id,
                                   static_cast<std::uint32_t>(index), std::nullopt});
                continue;
            }
            writer.writeString(asset.id);
            writer.writeU32(asset.sampleRate);
            writer.writeU16(asset.channelCount);
            writer.writeU64(asset.samples.size());
            for (const std::int16_t sample : asset.samples) {
                writer.writeU16(std::bit_cast<std::uint16_t>(sample));
            }
            record(detail::RecordKind::AudioAsset, asset.id,
                   static_cast<std::uint32_t>(index));
        }
        writer.writeU32(static_cast<std::uint32_t>(document.audioTracks.size()));
        record(detail::RecordKind::AudioTrackCount);
        for (std::size_t index = 0; index < document.audioTracks.size(); ++index) {
            const AudioTrackLayer &track = document.audioTracks[index];
            writer.writeString(track.id);
            writer.writeString(track.name);
            writer.writeU8(track.muted ? 1U : 0U);
            writer.writeDouble(track.gainDb);
            writer.writeU32(static_cast<std::uint32_t>(track.clips.size()));
            for (const AudioClip &clip : track.clips) {
                writer.writeString(clip.id);
                writer.writeString(clip.name);
                writer.writeString(clip.assetId);
                writer.writeU32(clip.startFrame);
                writer.writeU32(clip.durationFrames);
                writer.writeU64(clip.sourceOffsetSamples);
                writer.writeDouble(clip.gainDb);
                writer.writeU8(clip.enabled ? 1U : 0U);
            }
            record(detail::RecordKind::AudioTrack, track.id,
                   static_cast<std::uint32_t>(index));
        }
    }
}

class DocumentReader final {
    struct PendingKeyframe {
        FrameIndex frame = 0;
        std::size_t layerIndex = 0;
        std::string assetId;
    };

public:
    DocumentReader(ByteReader &reader, const SerializationLimits &limits,
                   bool workingFile = false)
        : m_reader(reader),
          m_limits(limits),
          m_workingFile(workingFile)
    {
    }

    Document read(FormatVersion version)
    {
        Document document;
        document.formatVersion = version;
        document.extent = {m_reader.readI32(), m_reader.readI32()};
        if (version.minor >= 1) {
            const std::uint8_t canvasMode = m_reader.readU8();
            if (canvasMode == 1U) {
                document.canvasMode = CanvasMode::Infinite;
                document.infiniteCanvas.origin = {m_reader.readI32(), m_reader.readI32()};
                document.infiniteCanvas.chunkSize = m_reader.readI32();
            } else if (canvasMode != 0U) {
                m_reader.fail(IiscErrorCode::InvalidData, "unknown canvas mode tag");
            }
        }
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
            } else if (kind == 2 && version.minor >= 1) {
                document.assets.emplace_back(readChunkedRaster(id));
            } else {
                m_reader.fail(IiscErrorCode::InvalidData, "unknown asset kind tag");
            }
        }

        const std::uint32_t layerCount = limitedCount(m_limits.maximumLayers, "layer");
        document.layers.reserve(layerCount);
        for (std::uint32_t index = 0; index < layerCount; ++index) {
            document.layers.push_back(readLayer(document, index, version));
        }
        std::sort(m_pendingKeyframes.begin(), m_pendingKeyframes.end(),
                  [&document](const PendingKeyframe &left,
                              const PendingKeyframe &right) {
                      if (left.frame != right.frame) {
                          return left.frame < right.frame;
                      }
                      return layerProperties(document.layers[left.layerIndex]).id
                          < layerProperties(document.layers[right.layerIndex]).id;
                  });
        std::size_t ownedFrameCount = 0;
        std::optional<FrameIndex> priorFrame;
        for (const PendingKeyframe &pending : m_pendingKeyframes) {
            if (!priorFrame || *priorFrame != pending.frame) {
                ++ownedFrameCount;
                priorFrame = pending.frame;
            }
        }
        if (ownedFrameCount > m_limits.maximumFrames) {
            m_reader.fail(IiscErrorCode::LimitExceeded,
                          "owned frame count exceeds the configured limit");
        }
        document.frames.reserve(ownedFrameCount);
        for (PendingKeyframe &pending : m_pendingKeyframes) {
            if (document.frames.empty()
                || document.frames.back().index != pending.frame) {
                document.frames.push_back(Frame{pending.frame, {}});
            }
            document.frames.back().keyframes.push_back({
                layerProperties(document.layers[pending.layerIndex]).id,
                std::move(pending.assetId),
            });
        }
        std::vector<PendingKeyframe>().swap(m_pendingKeyframes);
        if (version.minor >= 2 && readBoolean()) {
            document.stableDiffusionMetadata = readStableDiffusionMetadata();
        }
        if (version.minor >= 4) {
            readAudio(document);
        }
        return document;
    }

private:
    void requireCollectionBytes(std::uint64_t count, std::uint64_t minimumBytes)
    {
        if (count > m_reader.remaining() / minimumBytes) {
            m_reader.fail(IiscErrorCode::TruncatedData,
                          "audio collection cannot fit its declared payload");
        }
    }

    void readAudio(Document &document)
    {
        const auto assetCount = limitedCount(m_limits.maximumAudioAssets, "audio asset");
        requireCollectionBytes(assetCount, 18);
        document.audioAssets.reserve(assetCount);
        for (std::uint32_t index = 0; index < assetCount; ++index) {
            AudioAsset asset;
            asset.id = readString();
            asset.sampleRate = m_reader.readU32();
            asset.channelCount = m_reader.readU16();
            const auto count = m_reader.readU64();
            addTotal(m_totals.audioSamples, count,
                     std::min(m_limits.maximumTotalAudioSamples,
                              m_limits.maximumContainerBytes / 2), "audio sample");
            requireCollectionBytes(count, sizeof(std::int16_t));
            if (count > asset.samples.max_size()) {
                m_reader.fail(IiscErrorCode::LimitExceeded,
                              "audio sample count exceeds the platform address space");
            }
            asset.samples.reserve(static_cast<std::size_t>(count));
            for (std::uint64_t sample = 0; sample < count; ++sample) {
                asset.samples.push_back(std::bit_cast<std::int16_t>(m_reader.readU16()));
            }
            document.audioAssets.push_back(std::move(asset));
        }
        const auto trackCount = limitedCount(m_limits.maximumAudioTracks, "audio track");
        requireCollectionBytes(trackCount, 21);
        document.audioTracks.reserve(trackCount);
        for (std::uint32_t index = 0; index < trackCount; ++index) {
            AudioTrackLayer track;
            track.id = readString();
            track.name = readString();
            track.muted = readBoolean();
            track.gainDb = m_reader.readDouble();
            const auto clipCount = m_reader.readU32();
            addTotal(m_totals.audioClips, clipCount,
                     m_limits.maximumTotalAudioClips, "audio clip");
            requireCollectionBytes(clipCount, 37);
            track.clips.reserve(clipCount);
            for (std::uint32_t clipIndex = 0; clipIndex < clipCount; ++clipIndex) {
                AudioClip clip;
                clip.id = readString();
                clip.name = readString();
                clip.assetId = readString();
                clip.startFrame = m_reader.readU32();
                clip.durationFrames = m_reader.readU32();
                clip.sourceOffsetSamples = m_reader.readU64();
                clip.gainDb = m_reader.readDouble();
                clip.enabled = readBoolean();
                track.clips.push_back(std::move(clip));
            }
            document.audioTracks.push_back(std::move(track));
        }
    }

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

    std::string readStringWithLimit(std::uint32_t maximum,
                                    const char *kind)
    {
        const std::uint32_t size = m_reader.readU32();
        if (size > maximum) {
            m_reader.fail(IiscErrorCode::LimitExceeded,
                          std::string(kind) + " length exceeds the configured limit");
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

    std::string readString()
    {
        return readStringWithLimit(m_limits.maximumStringBytes, "string");
    }

    std::string readMetadataString()
    {
        return readStringWithLimit(m_limits.maximumMetadataStringBytes,
                                   "generation metadata string");
    }

    bool readBoolean()
    {
        const std::uint8_t value = m_reader.readU8();
        if (value > 1U) {
            m_reader.fail(IiscErrorCode::InvalidData, "boolean value must be zero or one");
        }
        return value != 0;
    }

    std::optional<std::uint32_t> readOptionalU32()
    {
        if (!readBoolean()) {
            return std::nullopt;
        }
        return m_reader.readU32();
    }

    std::optional<std::uint64_t> readOptionalU64()
    {
        if (!readBoolean()) {
            return std::nullopt;
        }
        return m_reader.readU64();
    }

    std::optional<double> readOptionalDouble()
    {
        if (!readBoolean()) {
            return std::nullopt;
        }
        return m_reader.readDouble();
    }

    std::uint32_t readMetadataCount(const char *kind)
    {
        const std::uint32_t count = limitedCount(m_limits.maximumMetadataEntries,
                                                 kind);
        addTotal(m_totals.metadataEntries,
                 count,
                 m_limits.maximumMetadataEntries,
                 "generation metadata entry");
        return count;
    }

    StableDiffusionMetadata readStableDiffusionMetadata()
    {
        StableDiffusionMetadata metadata;
        metadata.positivePrompt = readMetadataString();
        metadata.negativePrompt = readMetadataString();
        if (readBoolean()) {
            metadata.outputExtent = StableDiffusionImageExtent{
                m_reader.readU32(), m_reader.readU32(),
            };
        }
        metadata.batchSize = readOptionalU32();
        metadata.clipSkip = readOptionalU32();

        const std::uint32_t samplingPassCount =
            readMetadataCount("generation sampling pass");
        metadata.samplingPasses.reserve(samplingPassCount);
        for (std::uint32_t index = 0; index < samplingPassCount; ++index) {
            StableDiffusionSamplingPass pass;
            pass.nodeId = readMetadataString();
            pass.seed = readOptionalU64();
            pass.steps = readOptionalU32();
            pass.cfgScale = readOptionalDouble();
            pass.samplerName = readMetadataString();
            pass.scheduler = readMetadataString();
            pass.denoiseStrength = readOptionalDouble();
            pass.startStep = readOptionalU32();
            pass.endStep = readOptionalU32();
            metadata.samplingPasses.push_back(std::move(pass));
        }

        const std::uint32_t modelCount =
            readMetadataCount("generation model resource");
        metadata.models.reserve(modelCount);
        for (std::uint32_t index = 0; index < modelCount; ++index) {
            metadata.models.push_back({
                readMetadataString(),
                readMetadataString(),
                readMetadataString(),
                readMetadataString(),
                readMetadataString(),
            });
        }

        const std::uint32_t loraCount = readMetadataCount("generation LoRA");
        metadata.loras.reserve(loraCount);
        for (std::uint32_t index = 0; index < loraCount; ++index) {
            metadata.loras.push_back({
                readMetadataString(),
                readMetadataString(),
                m_reader.readDouble(),
                m_reader.readDouble(),
            });
        }

        metadata.software = readMetadataString();
        metadata.softwareVersion = readMetadataString();
        metadata.createdAt = readMetadataString();
        metadata.generationParametersText = readMetadataString();
        metadata.comfyUi.promptJson = readMetadataString();
        metadata.comfyUi.workflowJson = readMetadataString();

        const std::uint32_t comfyExtraCount =
            readMetadataCount("ComfyUI extension metadata");
        metadata.comfyUi.extraPngInfo.reserve(comfyExtraCount);
        for (std::uint32_t index = 0; index < comfyExtraCount; ++index) {
            metadata.comfyUi.extraPngInfo.push_back({
                readMetadataString(), readMetadataString(),
            });
        }

        const std::uint32_t extraParameterCount =
            readMetadataCount("generation extra parameter");
        metadata.extraParameters.reserve(extraParameterCount);
        for (std::uint32_t index = 0; index < extraParameterCount; ++index) {
            metadata.extraParameters.push_back({
                readMetadataString(), readMetadataString(),
            });
        }
        return metadata;
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
        if (m_workingFile && encoding != RasterEncoding::RawArgb32) {
            m_reader.fail(IiscErrorCode::InvalidData, "working-file rasters must use fixed raw storage");
        }
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
            if (!m_workingFile && runCount * 8U < rawByteCount) {
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

    ChunkedRasterAsset readChunkedRaster(std::string id)
    {
        ChunkedRasterAsset chunked;
        chunked.id = std::move(id);
        const std::uint32_t chunkCount = limitedCount(m_limits.maximumRasterChunks,
                                                       "raster chunk");
        addTotal(m_totals.rasterChunks,
                 chunkCount,
                 m_limits.maximumRasterChunks,
                 "raster chunk");
        chunked.chunks.reserve(chunkCount);
        for (std::uint32_t index = 0; index < chunkCount; ++index) {
            chunked.chunks.push_back({m_reader.readI32(), m_reader.readI32(), readRaster()});
        }
        return chunked;
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

    Layer readLayer(const Document &document,
                    std::size_t layerIndex,
                    FormatVersion version)
    {
        LayerProperties properties;
        properties.id = readString();
        properties.name = readString();
        properties.visible = readBoolean();
        properties.opacity = m_reader.readDouble();
        properties.transform.m11 = m_reader.readDouble();
        properties.transform.m12 = m_reader.readDouble();
        properties.transform.m21 = m_reader.readDouble();
        properties.transform.m22 = m_reader.readDouble();
        properties.transform.translationX = m_reader.readDouble();
        properties.transform.translationY = m_reader.readDouble();
        properties.blendMode = readBlendMode();

        LayerSource source;
        ContentKind kind = ContentKind::Raster;
        const std::uint8_t sourceKind = m_reader.readU8();
        if (sourceKind == 0) {
            StaticSource staticSource{readString()};
            const Asset *asset = findAsset(document, staticSource.assetId);
            if (!asset) {
                m_reader.fail(IiscErrorCode::InvalidData,
                              "static layer source references an unknown asset id");
            }
            kind = contentKind(*asset);
            source = std::move(staticSource);
        } else if (sourceKind == 1) {
            KeyframedSource keyframed;
            kind = readContentKind();
            const std::uint32_t keyframeCount = m_reader.readU32();
            addTotal(m_totals.keyframes,
                     keyframeCount,
                     m_limits.maximumTotalKeyframes,
                     "keyframe");
            if (keyframeCount > m_pendingKeyframes.max_size()
                    - m_pendingKeyframes.size()) {
                m_reader.fail(IiscErrorCode::LimitExceeded,
                              "keyframe count exceeds the platform address space");
            }
            keyframed.frameIndices.reserve(keyframeCount);
            std::optional<FrameIndex> priorFrame;
            for (std::uint32_t index = 0; index < keyframeCount; ++index) {
                const FrameIndex frame = m_reader.readU32();
                if (priorFrame && frame <= *priorFrame) {
                    m_reader.fail(IiscErrorCode::InvalidData,
                                  "layer keyframe positions must be strictly increasing");
                }
                priorFrame = frame;
                keyframed.frameIndices.push_back(frame);
                addTotal(m_totals.stringBytes,
                         properties.id.size(),
                         m_limits.maximumTotalStringBytes,
                         "frame-owned layer-id byte");
                m_pendingKeyframes.push_back({
                    frame,
                    layerIndex,
                    readString(),
                });
            }
            source = std::move(keyframed);
        } else {
            m_reader.fail(IiscErrorCode::InvalidData, "unknown layer source tag");
        }
        if (version.minor >= 3 && readBoolean()) {
            properties.frameRange = LayerFrameRange{
                m_reader.readU32(),
                m_reader.readU32(),
            };
        }
        if (kind == ContentKind::Raster) {
            return BitmapLayer{std::move(properties), std::move(source)};
        }
        return VectorLayer{std::move(properties), std::move(source)};
    }

    ByteReader &m_reader;
    const SerializationLimits &m_limits;
    bool m_workingFile = false;
    LimitTotals m_totals;
    std::vector<PendingKeyframe> m_pendingKeyframes;
};

} // namespace

detail::RecordEncodeResult detail::encodeDocumentRecords(
    const Document &document, const Document *previous, SerializationLimits limits)
{
    const ValidationResult validation = validate(document);
    if (!validation.ok()) {
        return {{}, makeError(IiscErrorCode::InvalidDocument, 0,
                             validation.issues.front().path + ": "
                                 + validation.issues.front().message)};
    }
    // Working files deliberately do not RLE-compress pixels.
    limits.maximumTotalRasterPixels = std::min(limits.maximumTotalRasterPixels,
                                                limits.maximumContainerBytes / 4);
    if (IiscError error = checkDocumentLimits(document, limits);
        error.code != IiscErrorCode::None) {
        return {{}, std::move(error)};
    }
    RecordEncodeResult result;
    ByteWriter writer;
    writePayload(writer, document, &result.records, previous);
    return result;
}

IiscDecodeResult detail::decodeDocumentRecords(
    const std::vector<DocumentRecord> &records, SerializationLimits limits)
{
    try {
        const auto invalid = [](const char *message) {
            throw DecodeFailure(IiscErrorCode::InvalidData, 0, message);
        };
        if (records.size() < 3
            || records.size() > static_cast<std::uint64_t>(limits.maximumAssets)
                                  + limits.maximumLayers + limits.maximumAudioAssets
                                  + limits.maximumAudioTracks + 5) {
            invalid("working-file record count is invalid");
        }
        std::size_t assetCount = 0;
        std::size_t layerCount = 0;
        std::size_t audioAssetCount = 0;
        std::size_t audioTrackCount = 0;
        bool sawLayerCount = false;
        bool sawMetadata = false;
        bool sawAudioAssetCount = false;
        bool sawAudioTrackCount = false;
        ByteWriter writer;
        std::uint64_t total = IiscHeaderSize - 4;
        for (std::size_t index = 0; index < records.size(); ++index) {
            const DocumentRecord &record = records[index];
            if (index > 0 && static_cast<int>(record.kind)
                                 < static_cast<int>(records[index - 1].kind)) {
                invalid("working-file record kinds are not in canonical order");
            }
            bool valid = false;
            switch (record.kind) {
            case RecordKind::Header:
                valid = index == 0 && record.id.empty() && record.position == 0;
                break;
            case RecordKind::Asset:
                valid = index > 0 && !sawLayerCount && record.position == assetCount++;
                break;
            case RecordKind::LayerCount:
                valid = index > 0 && !sawLayerCount && record.id.empty() && record.position == 0;
                sawLayerCount = true;
                break;
            case RecordKind::Layer:
                valid = sawLayerCount && !sawMetadata && record.position == layerCount++;
                break;
            case RecordKind::Metadata:
                valid = sawLayerCount && !sawMetadata
                    && record.id.empty() && record.position == 0;
                sawMetadata = true;
                break;
            case RecordKind::AudioAssetCount:
                valid = sawMetadata && !sawAudioAssetCount
                    && record.id.empty() && record.position == 0;
                sawAudioAssetCount = true;
                break;
            case RecordKind::AudioAsset:
                valid = sawAudioAssetCount && !sawAudioTrackCount
                    && record.position == audioAssetCount++;
                break;
            case RecordKind::AudioTrackCount:
                valid = sawAudioAssetCount && !sawAudioTrackCount
                    && record.id.empty() && record.position == 0;
                sawAudioTrackCount = true;
                break;
            case RecordKind::AudioTrack:
                valid = sawAudioTrackCount && record.position == audioTrackCount++;
                break;
            }
            if (!valid || !record.data) {
                invalid("working-file record layout is invalid");
            }
            if (!addWithin(total, record.data->size(), limits.maximumContainerBytes)) {
                throw DecodeFailure(IiscErrorCode::LimitExceeded, 0,
                                    "working-file payload exceeds the configured byte limit");
            }
            writer.writeBytes(*record.data);
        }
        if (records.front().kind != RecordKind::Header
            || !sawMetadata || !sawLayerCount) {
            invalid("working-file singleton records are missing");
        }
        ByteReader bytes(writer.bytes());
        const FormatVersion version{bytes.readU16(), bytes.readU16()};
        if (version.major != CurrentFormatMajor || version.minor > CurrentFormatMinor) {
            throw DecodeFailure(IiscErrorCode::UnsupportedVersion, 0,
                                "working-file document version is not supported");
        }
        if ((version.minor >= 4) != (sawAudioAssetCount && sawAudioTrackCount)
            || (version.minor < 4 && (sawAudioAssetCount || sawAudioTrackCount))) {
            invalid("working-file audio records do not match the document version");
        }
        const auto storedChunkSize = bytes.readI32();
        DocumentReader reader(bytes, limits, true);
        Document document = reader.read(version);
        if (document.canvasMode == CanvasMode::Infinite
            && document.infiniteCanvas.chunkSize != storedChunkSize) {
            invalid("working-file chunk size does not match its canvas geometry");
        }
        document.infiniteCanvas.chunkSize = storedChunkSize;
        if (!bytes.empty() || document.assets.size() != assetCount
            || document.layers.size() != layerCount
            || document.audioAssets.size() != audioAssetCount
            || document.audioTracks.size() != audioTrackCount) {
            invalid("working-file records do not match the document counts");
        }
        for (const DocumentRecord &record : records) {
            if (record.kind == RecordKind::Asset
                && record.id != assetId(document.assets[record.position])) {
                invalid("working-file asset identity does not match its payload");
            }
            if (record.kind == RecordKind::Layer
                && record.id != layerProperties(document.layers[record.position]).id) {
                invalid("working-file layer identity does not match its payload");
            }
            if (record.kind == RecordKind::AudioAsset
                && record.id != document.audioAssets[record.position].id) {
                invalid("working-file audio asset identity does not match its payload");
            }
            if (record.kind == RecordKind::AudioTrack
                && record.id != document.audioTracks[record.position].id) {
                invalid("working-file audio track identity does not match its payload");
            }
        }
        const ValidationResult validation = validate(document);
        if (!validation.ok()) {
            throw DecodeFailure(IiscErrorCode::InvalidData, 0,
                                validation.issues.front().path + ": "
                                    + validation.issues.front().message);
        }
        return {std::move(document), {}};
    } catch (const DecodeFailure &failure) {
        return {{}, makeError(failure.code, failure.offset, failure.what())};
    }
}

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
