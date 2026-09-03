#include "VectorCodec.h"

#include "Media/MediaIo_p.hpp"
#include "Render/FrameRenderer.h"
#include "Validation/Validation.h"
#include "SvgParser_p.hpp"

#include <QBuffer>
#include <QImageReader>
#include <QPageLayout>
#include <QPageSize>
#include <QPainter>
#include <QPdfWriter>
#include <QRegularExpression>
#include <QXmlStreamReader>
#include <QXmlStreamWriter>

#include <zlib.h>

#include <array>
#include <limits>

namespace iiSharedCanvas {
namespace {
using namespace media_detail;
using namespace vector_detail;

MediaBytesResult gzipBytes(std::span<const std::uint8_t> input, bool compress, std::uint64_t limit)
{
    MediaBytesResult result;
    if (input.size() > std::numeric_limits<uInt>::max()) {
        result.result = error(MediaIoCode::LimitExceeded, "SVGZ input exceeds the compression API limit");
        return result;
    }
    z_stream stream{};
    stream.next_in = const_cast<Bytef *>(input.data());
    stream.avail_in = uInt(input.size());
    const auto initialized = compress ? deflateInit2(&stream, Z_DEFAULT_COMPRESSION, Z_DEFLATED, 15 + 16, 8, Z_DEFAULT_STRATEGY)
                                     : inflateInit2(&stream, 15 + 16);
    if (initialized != Z_OK) {
        result.result = error(MediaIoCode::IoError, "cannot initialize SVGZ compression");
        return result;
    }
    struct EndStream {
        z_stream &stream; bool compress;
        ~EndStream() { if (compress) { deflateEnd(&stream); } else { inflateEnd(&stream); } }
    } end{stream, compress};
    std::array<std::uint8_t, 16 * 1024> block{};
    int status = Z_OK;
    while (status == Z_OK) {
        stream.next_out = block.data();
        stream.avail_out = uInt(block.size());
        status = compress ? deflate(&stream, Z_FINISH) : inflate(&stream, Z_NO_FLUSH);
        const auto size = block.size() - stream.avail_out;
        if (size > limit - result.bytes.size()) {
            result.result = error(MediaIoCode::LimitExceeded, "SVGZ expansion/output exceeds the byte limit");
            result.bytes.clear();
            return result;
        }
        result.bytes.insert(result.bytes.end(), block.begin(), block.begin() + std::ptrdiff_t(size));
    }
    if (status != Z_STREAM_END || stream.avail_in != 0) {
        result.bytes.clear();
        result.result = error(MediaIoCode::InvalidData, "corrupt, truncated, or concatenated SVGZ stream");
    }
    return result;
}

MediaIoResult validateVector(const VectorAsset &asset, const MediaLimits &limits)
{
    auto result = checkExtent(asset.viewport, limits);
    if (!result.ok()) { return result; }
    std::uint64_t commands = 0;
    for (const auto &path : asset.paths) {
        commands += path.commands.size();
        if (commands > limits.maxVectorCommands) { return error(MediaIoCode::LimitExceeded, "vector command count exceeds the limit"); }
    }
    Document document;
    document.extent = asset.viewport;
    document.assets.emplace_back(asset);
    const auto validation = validate(document);
    return validation.ok() ? MediaIoResult{} : MediaIoResult{MediaIoCode::InvalidArgument, validation.issues.front().message, {}};
}

QString number(double value) { return QString::number(value, 'g', 17); }
QString point(Point value) { return number(value.x) + ' ' + number(value.y); }

QString pathText(const VectorPath &path)
{
    QString text;
    for (const auto &command : path.commands) {
        std::visit([&](const auto &value) {
            using T = std::decay_t<decltype(value)>;
            if (!text.isEmpty()) { text += ' '; }
            if constexpr (std::is_same_v<T, MoveTo>) { text += "M " + point(value.point); }
            else if constexpr (std::is_same_v<T, LineTo>) { text += "L " + point(value.point); }
            else if constexpr (std::is_same_v<T, QuadraticTo>) { text += "Q " + point(value.control) + ' ' + point(value.end); }
            else if constexpr (std::is_same_v<T, CubicTo>) { text += "C " + point(value.control1) + ' ' + point(value.control2) + ' ' + point(value.end); }
            else { text += 'Z'; }
        }, command);
    }
    return text;
}

void paintAttribute(QXmlStreamWriter &xml, const QString &key, std::optional<SolidPaint> paint)
{
    if (!paint) { xml.writeAttribute(key, "none"); return; }
    xml.writeAttribute(key, QString("#%1").arg(paint->argb & 0x00ffffffU, 6, 16, QChar('0')));
    if ((paint->argb >> 24) != 255) { xml.writeAttribute(key + "-opacity", number(double(paint->argb >> 24) / 255)); }
}

MediaIoResult safeRasterSvg(const QByteArray &bytes, const MediaLimits &limits)
{
    QXmlStreamReader xml(bytes);
    std::uint32_t depth = 0;
    const QRegularExpression externalUrl("url\\(\\s*['\"]?(?!#)[^\\s'\"]", QRegularExpression::CaseInsensitiveOption);
    while (!xml.atEnd()) {
        const auto token = xml.readNext();
        if (token == QXmlStreamReader::DTD || token == QXmlStreamReader::EntityReference
            || token == QXmlStreamReader::ProcessingInstruction) {
            return error(MediaIoCode::UnsupportedFeature, "SVG entities and processing instructions are disabled");
        }
        if (token == QXmlStreamReader::StartElement) {
            if (++depth > limits.maxXmlDepth) { return error(MediaIoCode::LimitExceeded, "SVG nesting exceeds depth limit"); }
            const auto name = xml.name();
            if (name == u"script" || name == u"foreignObject" || name.toString().startsWith("animate") || name == u"set") {
                return error(MediaIoCode::UnsupportedFeature, "active or animated SVG is not a static raster import");
            }
            for (const auto &attribute : xml.attributes()) {
                if (attribute.name().toString().startsWith("on", Qt::CaseInsensitive)
                    || (attribute.name() == u"href" && !attribute.value().startsWith('#'))
                    || externalUrl.match(attribute.value().toString()).hasMatch()) {
                    return error(MediaIoCode::UnsupportedFeature, "external SVG resources are disabled");
                }
            }
        } else if (token == QXmlStreamReader::EndElement) { --depth; }
        else if (token == QXmlStreamReader::Characters
                 && (xml.text().contains(u"@import") || externalUrl.match(xml.text().toString()).hasMatch())) {
            return error(MediaIoCode::UnsupportedFeature, "external SVG stylesheets are disabled");
        }
    }
    return xml.hasError() ? error(MediaIoCode::InvalidData, xml.errorString()) : MediaIoResult{};
}
}

VectorImportResult decodeSvg(std::span<const std::uint8_t> bytes, const VectorImportOptions &options)
{
    if (bytes.size() > options.limits.maxInputBytes || bytes.size() > std::uint64_t(std::numeric_limits<qsizetype>::max())) {
        return {{}, error(MediaIoCode::LimitExceeded, "SVG input exceeds the byte limit")};
    }
    if (options.assetId.empty() || options.assetId.find('\0') != std::string::npos
        || QString::fromUtf8(options.assetId).toUtf8().toStdString() != options.assetId || bytes.empty()) {
        return {{}, error(MediaIoCode::InvalidArgument, "SVG requires bytes and a valid UTF-8 asset id")};
    }
    MediaBytesResult decompressed;
    if (bytes.size() >= 2 && bytes[0] == 0x1f && bytes[1] == 0x8b) {
        decompressed = gzipBytes(bytes, false, std::min(options.limits.maxInputBytes, options.limits.maxDecodedBytes));
        if (!decompressed.ok()) { return {{}, std::move(decompressed.result)}; }
        bytes = decompressed.bytes;
    }
    return parseSvg(QByteArray::fromRawData(reinterpret_cast<const char *>(bytes.data()), qsizetype(bytes.size())), options);
}

VectorImportResult importSvg(const std::string &path, const VectorImportOptions &options)
{
    auto input = readFile(path, options.limits);
    if (!input.ok()) { return {{}, std::move(input.result)}; }
    return decodeSvg(input.bytes, options);
}

MediaBytesResult encodeSvg(const VectorAsset &asset, const VectorExportOptions &options)
{
    MediaBytesResult result;
    result.result = validateVector(asset, options.limits);
    if (!result.ok()) { return result; }
    QByteArray output;
    BoundedBuffer buffer(&output, options.limits.maxOutputBytes);
    buffer.open(QIODevice::WriteOnly);
    QXmlStreamWriter xml(&buffer);
    xml.setAutoFormatting(true);
    xml.writeStartDocument();
    xml.writeStartElement("svg");
    xml.writeDefaultNamespace("http://www.w3.org/2000/svg");
    xml.writeAttribute("version", "1.1");
    xml.writeAttribute("width", QString::number(asset.viewport.width));
    xml.writeAttribute("height", QString::number(asset.viewport.height));
    xml.writeAttribute("viewBox", "0 0 " + QString::number(asset.viewport.width) + ' ' + QString::number(asset.viewport.height));
    for (const auto &path : asset.paths) {
        xml.writeStartElement("path");
        xml.writeAttribute("d", pathText(path));
        xml.writeAttribute("fill-rule", "evenodd");
        paintAttribute(xml, "fill", path.fill);
        paintAttribute(xml, "stroke", path.stroke ? std::optional(path.stroke->paint) : std::nullopt);
        if (path.stroke) {
            xml.writeAttribute("stroke-width", number(path.stroke->width));
            xml.writeAttribute("stroke-linecap", "round");
            xml.writeAttribute("stroke-linejoin", "round");
        }
        xml.writeEndElement();
        if (xml.hasError()) { break; }
    }
    xml.writeEndElement();
    xml.writeEndDocument();
    if (xml.hasError() || buffer.exceeded) {
        result.result = error(buffer.exceeded ? MediaIoCode::LimitExceeded : MediaIoCode::IoError, "SVG encoding failed or exceeded output limit");
        return result;
    }
    result.bytes.assign(output.begin(), output.end());
    return options.compressed ? gzipBytes(result.bytes, true, options.limits.maxOutputBytes) : result;
}

MediaIoResult exportSvg(const VectorAsset &asset, const std::string &path, const VectorExportOptions &options)
{
    QString absolute;
    auto result = checkDestination(path, options.overwrite, absolute);
    if (!result.ok()) { return result; }
    auto encoded = encodeSvg(asset, options);
    if (!encoded.ok()) { return encoded.result; }
    return writeFile(path, encoded.bytes, options.overwrite, options.limits);
}

MediaIoResult exportPdf(const Document &document, const std::string &path, const PdfExportOptions &options)
{
    QString absolute;
    auto result = checkDestination(path, options.overwrite, absolute);
    if (!result.ok()) { return result; }
    result = checkExtent(document.extent, options.limits);
    if (!result.ok()) { return result; }
    const auto validation = validate(document);
    if (!validation.ok()) { return {MediaIoCode::InvalidArgument, validation.issues.front().message, {}}; }
    const auto last = options.lastFrame.value_or(options.firstFrame);
    if (options.firstFrame > last || last >= document.timeline.frameCount
        || std::uint64_t(last) + 1 - options.firstFrame > options.limits.maxFrames) {
        return error(MediaIoCode::InvalidArgument, "invalid PDF page/frame range");
    }
    for (const auto &asset : document.assets) {
        if (const auto *vector = std::get_if<VectorAsset>(&asset)) {
            result = validateVector(*vector, options.limits);
            if (!result.ok()) { return result; }
        }
    }
    QByteArray output;
    BoundedBuffer buffer(&output, options.limits.maxOutputBytes);
    buffer.open(QIODevice::WriteOnly);
    {
        QPdfWriter writer(&buffer);
        writer.setResolution(96);
        writer.setCreator("iiSharedCanvas");
        writer.setPageSize(QPageSize(QSizeF(document.extent.width * 72.0 / 96, document.extent.height * 72.0 / 96), QPageSize::Point));
        writer.setPageMargins(QMarginsF(0, 0, 0, 0), QPageLayout::Point);
        QPainter painter(&writer);
        if (!painter.isActive()) { return error(MediaIoCode::IoError, "cannot initialize PDF painter"); }
        painter.setRenderHint(QPainter::Antialiasing, true);
        for (std::uint64_t frame = options.firstFrame; frame <= last; ++frame) {
            if (frame != options.firstFrame && !writer.newPage()) { return error(MediaIoCode::IoError, "cannot append PDF page"); }
            bool rasterizeFrame = false;
            for (const auto &layer : document.layers) {
                const auto &properties = layerProperties(layer);
                if (properties.visible && layerExistsAt(document, layer, FrameIndex(frame))
                    && properties.blendMode != RasterBlendMode::SourceOver) { rasterizeFrame = true; }
            }
            if (rasterizeFrame && !options.rasterizeUnsupportedBlending) {
                return error(MediaIoCode::UnsupportedFeature, "PDF cannot preserve this blend mode; explicit frame rasterization is required");
            }
            if (rasterizeFrame) {
                const auto rendered = renderFrame(document, FrameIndex(frame));
                if (!rendered.ok()) { return {MediaIoCode::InvalidData, rendered.message, {}}; }
                painter.drawImage(0, 0, imageFromRaster(rendered.pixels));
                result.warnings.emplace_back("PDF page rasterized to preserve canvas blend modes");
                continue;
            }
            for (std::size_t index = 0; index < document.layers.size(); ++index) {
                const auto &layer = document.layers[index];
                const auto &properties = layerProperties(layer);
                if (!properties.visible || properties.opacity <= 0 || !layerExistsAt(document, layer, FrameIndex(frame))) { continue; }
                const auto *asset = resolveAssetAt(document, layer, FrameIndex(frame));
                if (!asset) { return error(MediaIoCode::InvalidData, "cannot resolve PDF layer asset"); }
                painter.save();
                painter.setOpacity(properties.opacity);
                const auto *vector = std::get_if<VectorAsset>(asset);
                if (vector && properties.opacity != 1) {
                    const auto rendered = renderFrameLayerTiles(document, FrameIndex(frame), index,
                                                               {{canvasRegion(document), document.extent}});
                    if (!rendered.ok() || rendered.tiles.size() != 1) {
                        return error(MediaIoCode::InvalidData, "cannot render translucent PDF vector group");
                    }
                    painter.drawImage(0, 0, imageFromRaster(rendered.tiles[0].pixels));
                    result.warnings.emplace_back("translucent vector layer rasterized to preserve isolated group opacity in PDF");
                } else {
                    const auto &t = properties.transform;
                    const auto origin = canvasOrigin(document);
                    painter.setTransform(QTransform(t.m11, t.m12, t.m21, t.m22, t.translationX - origin.x, t.translationY - origin.y));
                    if (vector) {
                        for (const auto &path : vector->paths) {
                            painter.setBrush(path.fill ? QBrush(QColor::fromRgba(path.fill->argb)) : Qt::NoBrush);
                            painter.setPen(path.stroke ? QPen(QColor::fromRgba(path.stroke->paint.argb), path.stroke->width,
                                                            Qt::SolidLine, Qt::RoundCap, Qt::RoundJoin) : Qt::NoPen);
                            painter.drawPath(painterPath(path));
                        }
                    } else if (const auto *raster = std::get_if<RasterAsset>(asset)) {
                        painter.drawImage(0, 0, imageFromRaster(raster->pixels));
                    } else if (const auto *chunks = std::get_if<ChunkedRasterAsset>(asset)) {
                        for (const auto &chunk : chunks->chunks) {
                            painter.drawImage(QPointF(double(chunk.column) * document.infiniteCanvas.chunkSize,
                                                     double(chunk.row) * document.infiniteCanvas.chunkSize), imageFromRaster(chunk.pixels));
                        }
                    }
                }
                painter.restore();
            }
            if (buffer.exceeded) { return error(MediaIoCode::LimitExceeded, "PDF output exceeds byte limit"); }
        }
        painter.end();
    }
    if (buffer.exceeded) { return error(MediaIoCode::LimitExceeded, "PDF output exceeds byte limit"); }
    auto written = writeFile(path, {reinterpret_cast<const std::uint8_t *>(output.constData()), std::size_t(output.size())},
                              options.overwrite, options.limits);
    written.warnings = std::move(result.warnings);
    return written;
}

BitmapImportResult rasterizeVectorFile(const std::string &path, const RasterizedVectorImportOptions &options)
{
    BitmapImportResult result;
    result.result = checkExtent(options.outputExtent, options.limits);
    if (!result.ok()) { return result; }
    if (options.assetId.empty() || options.assetId.find('\0') != std::string::npos
        || QString::fromUtf8(options.assetId).toUtf8().toStdString() != options.assetId
        || options.page >= options.limits.maxFrames || options.page > std::uint32_t(std::numeric_limits<int>::max())) {
        result.result = error(MediaIoCode::InvalidArgument, "rasterized import requires an asset id and bounded page index");
        return result;
    }
    auto input = readFile(path, options.limits);
    if (!input.ok()) { result.result = std::move(input.result); return result; }
    if (input.bytes.size() >= 2 && input.bytes[0] == 0x1f && input.bytes[1] == 0x8b) {
        input = gzipBytes(input.bytes, false, std::min(options.limits.maxInputBytes, options.limits.maxDecodedBytes));
        if (!input.ok()) { result.result = std::move(input.result); return result; }
    }
    if (input.bytes.size() > std::uint64_t(std::numeric_limits<qsizetype>::max())) {
        result.result = error(MediaIoCode::LimitExceeded, "vector raster input exceeds the image plugin byte limit");
        return result;
    }
    QByteArray data = QByteArray::fromRawData(reinterpret_cast<const char *>(input.bytes.data()), qsizetype(input.bytes.size()));
    QBuffer buffer(&data);
    buffer.open(QIODevice::ReadOnly);
    QImageReader reader(&buffer);
    reader.setDecideFormatFromContent(true);
    const auto format = reader.format();
    if (format != "svg" && format != "svgz" && format != "pdf") {
        result.result = error(MediaIoCode::UnsupportedFormat, "a Qt SVG/PDF image plugin is required for rasterization");
        return result;
    }
    if (format == "svg" || format == "svgz") {
        result.result = safeRasterSvg(data, options.limits);
        if (!result.ok()) { return result; }
    }
    if (options.page != 0 && !reader.jumpToImage(int(options.page))) {
        result.result = error(MediaIoCode::InvalidArgument, "requested vector page is unavailable");
        return result;
    }
    reader.setScaledSize(QSize(options.outputExtent.width, options.outputExtent.height));
    const auto image = reader.read();
    if (image.isNull()) { result.result = error(MediaIoCode::InvalidData, reader.errorString()); return result; }
    if (image.width() != options.outputExtent.width || image.height() != options.outputExtent.height) {
        result.result = error(MediaIoCode::UnsupportedFeature, "vector plugin did not honor requested raster dimensions");
        return result;
    }
    result.asset = {options.assetId, rasterFromImage(image, result.result)};
    if (!result.ok()) { result.asset = {}; return result; }
    result.format = format.toStdString();
    result.result.warnings.emplace_back("vector document rasterized: paths, text, layers and editability are not retained");
    return result;
}

} // namespace iiSharedCanvas
