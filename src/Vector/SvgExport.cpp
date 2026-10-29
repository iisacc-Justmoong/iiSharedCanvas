#include "Vector/SvgExport.h"

#include "Native/Media_p.hpp"
#include "Validation/Validation.h"
#include <iiSDKCore/Core/Text/Number.h>
#include <zlib.h>

#include <array>
#include <limits>
#include <new>
#include <span>
#include <stdexcept>
#include <string_view>
#include <type_traits>
#include <utility>

namespace iiSharedCanvas {
namespace {
MediaIoResult validateAsset(const VectorAsset &asset, const MediaLimits &limits)
{
    auto extent = media_detail::checkExtent(asset.viewport, limits);
    if (!extent.ok()) return extent;
    std::uint64_t commands = 0;
    for (const auto &path : asset.paths) {
        if (path.commands.size() > limits.maxVectorCommands - commands)
            return media_detail::error(MediaIoCode::LimitExceeded, "vector command count exceeds the limit");
        commands += path.commands.size();
    }
    if (asset.id.size() > limits.maxDecodedBytes
        || asset.paths.size() > (limits.maxDecodedBytes - asset.id.size()) / sizeof(VectorPath))
        return media_detail::error(MediaIoCode::LimitExceeded, "vector validation exceeds the memory budget");
    const auto remaining = limits.maxDecodedBytes - asset.id.size()
        - std::uint64_t(asset.paths.size()) * sizeof(VectorPath);
    if (commands > remaining / sizeof(PathCommand))
        return media_detail::error(MediaIoCode::LimitExceeded, "vector validation exceeds the memory budget");
    Document document; document.extent = asset.viewport; document.assets.emplace_back(asset);
    const auto valid = validate(document);
    return valid.ok() ? MediaIoResult{} : media_detail::error(MediaIoCode::InvalidArgument, valid.issues.front().message);
}

class XmlOutput final {
public:
    explicit XmlOutput(std::uint64_t limit) : limit_(limit) {}
    void append(std::string_view text)
    {
        if (text.size() > limit_ - data_.size()) throw std::length_error("SVG exceeds its output byte limit");
        data_.append(text);
    }
    [[nodiscard]] const std::string &data() const { return data_; }
private:
    std::uint64_t limit_;
    std::string data_;
};

std::string number(double value) { return ii::core::format_decimal(value); }
void point(XmlOutput &xml, Point value)
{
    xml.append(number(value.x)); xml.append(" "); xml.append(number(value.y));
}
void pathCommands(XmlOutput &xml, const VectorPath &path)
{
    bool first = true;
    for (const auto &command : path.commands) {
        if (!first) xml.append(" ");
        first = false;
        std::visit([&](const auto &value) {
            using T = std::decay_t<decltype(value)>;
            if constexpr (std::is_same_v<T, MoveTo>) { xml.append("M "); point(xml, value.point); }
            else if constexpr (std::is_same_v<T, LineTo>) { xml.append("L "); point(xml, value.point); }
            else if constexpr (std::is_same_v<T, QuadraticTo>) {
                xml.append("Q "); point(xml, value.control); xml.append(" "); point(xml, value.end);
            } else if constexpr (std::is_same_v<T, CubicTo>) {
                xml.append("C "); point(xml, value.control1); xml.append(" "); point(xml, value.control2);
                xml.append(" "); point(xml, value.end);
            } else {
                static_assert(std::is_same_v<T, ClosePath>, "new path commands need an SVG encoding");
                xml.append("Z");
            }
        }, command);
    }
}
void paint(XmlOutput &xml, std::string_view name, const std::optional<SolidPaint> &value)
{
    xml.append(" "); xml.append(name); xml.append("=\"");
    if (!value) { xml.append("none\""); return; }
    constexpr char digits[] = "0123456789abcdef";
    std::array<char, 7> rgb{'#'};
    for (unsigned i = 0; i < 6; ++i) rgb[i + 1] = digits[(value->argb >> (20 - 4 * i)) & 15];
    xml.append({rgb.data(), rgb.size()}); xml.append("\"");
    const auto alpha = value->argb >> 24;
    if (alpha != 255) {
        xml.append(" "); xml.append(name); xml.append("-opacity=\""); xml.append(number(double(alpha) / 255)); xml.append("\"");
    }
}
MediaBytesResult gzip(std::span<const std::uint8_t> input, std::uint64_t limit)
{
    if (input.size() > std::numeric_limits<uInt>::max())
        return {{}, media_detail::error(MediaIoCode::LimitExceeded, "SVGZ input exceeds the compression API limit")};
    z_stream stream{};
    stream.next_in = const_cast<Bytef *>(input.data()); stream.avail_in = uInt(input.size());
    if (deflateInit2(&stream, Z_DEFAULT_COMPRESSION, Z_DEFLATED, 15 + 16, 8, Z_DEFAULT_STRATEGY) != Z_OK)
        return {{}, media_detail::error(MediaIoCode::IoError, "cannot initialize SVGZ compression")};
    struct End { z_stream &stream; ~End() { deflateEnd(&stream); } } end{stream};
    MediaBytesResult result; std::array<std::uint8_t, 16384> block{};
    int status = Z_OK;
    while (status == Z_OK) {
        stream.next_out = block.data(); stream.avail_out = uInt(block.size());
        status = deflate(&stream, Z_FINISH);
        const auto count = block.size() - stream.avail_out;
        if (count > limit - result.bytes.size())
            return {{}, media_detail::error(MediaIoCode::LimitExceeded, "SVGZ exceeds its output byte limit")};
        result.bytes.insert(result.bytes.end(), block.begin(), block.begin() + std::ptrdiff_t(count));
    }
    if (status != Z_STREAM_END || stream.avail_in)
        return {{}, media_detail::error(MediaIoCode::IoError, "cannot finish SVGZ compression")};
    return result;
}
}

MediaBytesResult encodeSvg(const VectorAsset &asset, const VectorExportOptions &options)
{
    try {
        auto validation = validateAsset(asset, options.limits);
        if (!validation.ok()) return {{}, std::move(validation)};
        XmlOutput xml(options.limits.maxOutputBytes);
        const auto width = std::to_string(asset.viewport.width), height = std::to_string(asset.viewport.height);
        xml.append("<?xml version=\"1.0\" encoding=\"UTF-8\"?>\n<svg xmlns=\"http://www.w3.org/2000/svg\" version=\"1.1\" width=\"");
        xml.append(width); xml.append("\" height=\""); xml.append(height); xml.append("\" viewBox=\"0 0 ");
        xml.append(width); xml.append(" "); xml.append(height); xml.append("\">\n");
        for (const auto &path : asset.paths) {
            xml.append("  <path d=\""); pathCommands(xml, path); xml.append("\" fill-rule=\"evenodd\"");
            paint(xml, "fill", path.fill);
            paint(xml, "stroke", path.stroke ? std::optional(path.stroke->paint) : std::nullopt);
            if (path.stroke) {
                xml.append(" stroke-width=\""); xml.append(number(path.stroke->width));
                xml.append("\" stroke-linecap=\"round\" stroke-linejoin=\"round\"");
            }
            xml.append("/>\n");
        }
        xml.append("</svg>\n");
        const auto &output = xml.data();
        const auto bytes = std::span(reinterpret_cast<const std::uint8_t *>(output.data()), output.size());
        if (options.compressed) return gzip(bytes, options.limits.maxOutputBytes);
        return {{bytes.begin(), bytes.end()}, {}};
    } catch (const std::bad_alloc &) {
        return {{}, media_detail::error(MediaIoCode::LimitExceeded, "SVG allocation exceeds the available budget")};
    } catch (const std::length_error &error) {
        return {{}, media_detail::error(MediaIoCode::LimitExceeded, error.what())};
    } catch (const std::exception &error) {
        return {{}, media_detail::error(MediaIoCode::InvalidArgument, error.what())};
    }
}

MediaIoResult exportSvg(const VectorAsset &asset, const std::string &path, const VectorExportOptions &options)
{
    std::filesystem::path destination;
    auto result = media_detail::checkDestination(path, options.overwrite, destination);
    if (!result.ok()) return result;
    auto encoded = encodeSvg(asset, options);
    if (!encoded.ok()) return std::move(encoded.result);
    return media_detail::writeFile(path, encoded.bytes, options.overwrite, options.limits);
}

} // namespace iiSharedCanvas
