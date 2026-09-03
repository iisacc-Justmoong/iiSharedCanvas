#include "PsdWriter_p.hpp"

#include "Media/MediaIo_p.hpp"
#include "Render/FrameRenderer.h"
#include "Validation/Validation.h"
#include "Vector/SvgParser_p.hpp"

#include <QPageLayout>
#include <QPageSize>
#include <QGuiApplication>
#include <QColorSpace>
#include <QPainter>
#include <QPdfWriter>
#include <QUuid>

#include <algorithm>
#include <array>
#include <bit>
#include <cmath>
#include <limits>
#include <new>
#include <stdexcept>
#include <string_view>

namespace iiSharedCanvas {
namespace {
using namespace media_detail;
constexpr std::uint64_t MaximumPsdBytes = 0x7fffffffULL;

struct ExportFailure { MediaIoCode code; std::string message; };
[[noreturn]] void fail(MediaIoCode code, std::string message) { throw ExportFailure{code, std::move(message)}; }
void checked(MediaIoResult result) { if (!result.ok()) { fail(result.code, std::move(result.message)); } }
void warn(MediaIoResult &result, const std::string &message)
{
    if (std::find(result.warnings.begin(), result.warnings.end(), message) == result.warnings.end()) {
        result.warnings.push_back(message);
    }
}

class Writer {
public:
    explicit Writer(std::uint64_t limit) : m_limit(std::min(limit, MaximumPsdBytes)) {}
    std::vector<std::uint8_t> bytes;
    std::size_t size() const { return bytes.size(); }
    std::uint64_t remaining() const { return m_limit - bytes.size(); }
    void reserveMore(std::size_t count)
    {
        if (count > remaining()) { fail(MediaIoCode::LimitExceeded, "PSD output exceeds the byte limit or PSD v1 size limit"); }
    }
    void u8(std::uint8_t value) { reserveMore(1); bytes.push_back(value); }
    void u16(std::uint16_t value) { u8(std::uint8_t(value >> 8)); u8(std::uint8_t(value)); }
    void u32(std::uint32_t value) { u16(std::uint16_t(value >> 16)); u16(std::uint16_t(value)); }
    void u64(std::uint64_t value) { u32(std::uint32_t(value >> 32)); u32(std::uint32_t(value)); }
    void f64(double value) { u64(std::bit_cast<std::uint64_t>(value)); }
    void data(std::span<const std::uint8_t> value)
    {
        reserveMore(value.size()); bytes.insert(bytes.end(), value.begin(), value.end());
    }
    void text(std::string_view value)
    {
        data({reinterpret_cast<const std::uint8_t *>(value.data()), value.size()});
    }
    void zeros(std::size_t count) { reserveMore(count); bytes.insert(bytes.end(), count, 0); }
    void patch32(std::size_t offset, std::uint64_t value)
    {
        if (offset > size() || size() - offset < 4 || value > std::numeric_limits<std::uint32_t>::max()) {
            fail(MediaIoCode::LimitExceeded, "PSD section exceeds its 32-bit length field");
        }
        for (unsigned index = 0; index < 4; ++index) { bytes[offset + index] = std::uint8_t(value >> (24 - index * 8)); }
    }
    void patch64(std::size_t offset, std::uint64_t value)
    {
        patch32(offset, value >> 32); patch32(offset + 4, value & 0xffffffffU);
    }
    std::size_t begin32() { const auto offset = size(); u32(0); return offset; }
    void end32(std::size_t offset) { patch32(offset, size() - offset - 4); }
    void unicode(const QString &value)
    {
        if (std::uint64_t(value.size()) > std::numeric_limits<std::uint32_t>::max()) {
            fail(MediaIoCode::LimitExceeded, "PSD Unicode text exceeds its length field");
        }
        u32(std::uint32_t(value.size()));
        for (const auto character : value) { u16(character.unicode()); }
    }
    std::size_t beginTag(std::string_view key) { text("8BIM"); text(key); return begin32(); }
    void endTag(std::size_t offset, std::size_t alignment = 4)
    {
        const auto length = size() - offset - 4;
        // Photoshop's typed tagged-block payloads include their padding in
        // the declared length (including the placed/smart descriptors).
        zeros((alignment - length % alignment) % alignment); end32(offset);
    }
private:
    std::uint64_t m_limit;
};

void charge(std::uint64_t &total, std::uint64_t amount, std::uint64_t limit, const char *message)
{
    if (total > limit || amount > limit - total) { fail(MediaIoCode::LimitExceeded, message); }
    total += amount;
}

std::string_view psdBlend(RasterBlendMode mode)
{
    switch (mode) {
    case RasterBlendMode::SourceOver: return "norm";
    case RasterBlendMode::Multiply: return "mul ";
    case RasterBlendMode::Screen: return "scrn";
    case RasterBlendMode::Overlay: return "over";
    default: fail(MediaIoCode::UnsupportedFeature, "PSD export does not support the layer blend mode");
    }
}

bool nativeBitmap(const Asset *asset, const LayerProperties &properties, CanvasOrigin origin)
{
    if (!asset || !std::holds_alternative<RasterAsset>(*asset)) { return false; }
    const auto &t = properties.transform;
    if (t.m11 != 1 || t.m12 != 0 || t.m21 != 0 || t.m22 != 1
        || std::floor(t.translationX) != t.translationX || std::floor(t.translationY) != t.translationY) { return false; }
    const auto &pixels = std::get<RasterAsset>(*asset).pixels;
    const double left = t.translationX - origin.x, top = t.translationY - origin.y;
    return left >= std::numeric_limits<std::int32_t>::min() && top >= std::numeric_limits<std::int32_t>::min()
        && left + pixels.width <= std::numeric_limits<std::int32_t>::max()
        && top + pixels.height <= std::numeric_limits<std::int32_t>::max();
}

struct ExportLayer {
    const Layer *layer = nullptr;
    const Asset *asset = nullptr;
    QString name;
    std::int32_t left = 0, top = 0, width = 1, height = 1;
    std::uint8_t opacity = 255;
    bool visible = true, directBitmap = false;
    RasterBlendMode blend = RasterBlendMode::SourceOver;
    QByteArray pdf;
    QRectF pdfBounds;
    std::string uniqueId;
};

QRectF vectorBounds(const VectorAsset &vector)
{
    QRectF bounds(0, 0, vector.viewport.width, vector.viewport.height);
    for (const auto &path : vector.paths) {
        auto pathBounds = vector_detail::painterPath(path).controlPointRect();
        if (path.stroke) {
            const auto half = path.stroke->width / 2;
            pathBounds.adjust(-half, -half, half, half);
        }
        bounds = bounds.united(pathBounds);
    }
    const auto left = std::floor(bounds.left()), top = std::floor(bounds.top());
    const auto right = std::ceil(bounds.right()), bottom = std::ceil(bounds.bottom());
    if (!std::isfinite(left) || !std::isfinite(top) || !std::isfinite(right) || !std::isfinite(bottom)
        || right - left > 14400 || bottom - top > 14400 || right <= left || bottom <= top) {
        fail(MediaIoCode::UnsupportedFeature, "PSD Smart Object PDF bounds must be finite and at most 14400 points per dimension");
    }
    return {left, top, right - left, bottom - top};
}

std::array<double, 8> smartQuad(const ExportLayer &layer, CanvasOrigin origin)
{
    const auto &t = layerProperties(*layer.layer).transform;
    const auto determinant = t.m11 * t.m22 - t.m12 * t.m21;
    if (!std::isfinite(determinant) || determinant == 0) {
        fail(MediaIoCode::UnsupportedFeature, "PSD Smart Objects require a finite non-degenerate affine transform");
    }
    const auto &r = layer.pdfBounds;
    const std::array<QPointF, 4> corners{r.topLeft(), r.topRight(), r.bottomRight(), r.bottomLeft()};
    std::array<double, 8> result{};
    for (std::size_t index = 0; index < corners.size(); ++index) {
        const auto &point = corners[index];
        result[index * 2] = t.m11 * point.x() + t.m21 * point.y() + t.translationX - origin.x;
        result[index * 2 + 1] = t.m12 * point.x() + t.m22 * point.y() + t.translationY - origin.y;
        if (!std::isfinite(result[index * 2]) || !std::isfinite(result[index * 2 + 1])) {
            fail(MediaIoCode::UnsupportedFeature, "PSD Smart Object placement is outside finite coordinates");
        }
    }
    return result;
}

void makeVectorPdf(ExportLayer &layer, std::size_t index, std::uint64_t byteLimit)
{
    if (!qobject_cast<QGuiApplication *>(QCoreApplication::instance())) {
        fail(MediaIoCode::InvalidArgument, "PSD vector PDF export requires an initialized QGuiApplication");
    }
    BoundedBuffer buffer(&layer.pdf, byteLimit);
    if (!buffer.open(QIODevice::WriteOnly)) { fail(MediaIoCode::IoError, "PSD Smart Object PDF buffer could not be opened"); }
    {
        QPdfWriter pdf(&buffer); pdf.setResolution(72); pdf.setCreator(QStringLiteral("iiSharedCanvas"));
        const QPageSize page(layer.pdfBounds.size(), QPageSize::Point, QString(), QPageSize::ExactMatch);
        if (!page.isValid() || page.size(QPageSize::Point) != layer.pdfBounds.size()
            || !pdf.setPageSize(page) || !pdf.setPageMargins(QMarginsF(), QPageLayout::Point)) {
            fail(MediaIoCode::UnsupportedFeature, "PSD Smart Object PDF page cannot represent the complete vector bounds");
        }
        QPainter painter(&pdf);
        if (!painter.isActive()) { fail(MediaIoCode::IoError, "PSD Smart Object PDF painter could not be initialized"); }
        painter.setRenderHint(QPainter::Antialiasing, true);
        painter.translate(-layer.pdfBounds.left(), -layer.pdfBounds.top());
        // Layer opacity and its affine transform belong to PSD layer/object
        // metadata. Only each individual path's own paint alpha is in the PDF.
        for (const auto &path : std::get<VectorAsset>(*layer.asset).paths) {
            painter.setBrush(path.fill ? QBrush(QColor::fromRgba(path.fill->argb)) : Qt::NoBrush);
            painter.setPen(path.stroke ? QPen(QColor::fromRgba(path.stroke->paint.argb), path.stroke->width,
                                             Qt::SolidLine, Qt::RoundCap, Qt::RoundJoin) : Qt::NoPen);
            painter.drawPath(vector_detail::painterPath(path));
            if (buffer.exceeded) { fail(MediaIoCode::LimitExceeded, "PSD embedded PDF exceeds the byte limit"); }
        }
        painter.end();
    }
    if (buffer.exceeded || !layer.pdf.startsWith("%PDF-")) {
        fail(buffer.exceeded ? MediaIoCode::LimitExceeded : MediaIoCode::IoError, "PSD embedded PDF is incomplete or exceeded its byte limit");
    }
    const QUuid namespaceId(QStringLiteral("{b2f82714-2825-56d5-bcce-5b0e3d5c2a97}"));
    layer.uniqueId = QUuid::createUuidV5(namespaceId,
        QByteArray::fromStdString(std::to_string(index) + ":" + layerProperties(*layer.layer).id))
        .toString(QUuid::WithoutBraces).toStdString();
}

void descriptorKey(Writer &out, std::string_view key)
{
    out.u32(key.size() == 4 ? 0 : std::uint32_t(key.size())); out.text(key);
}
void descriptor(Writer &out, std::string_view classId, std::uint32_t count)
{
    out.u32(1); out.u16(0); descriptorKey(out, classId); out.u32(count);
}
void entry(Writer &out, std::string_view key, std::string_view type) { descriptorKey(out, key); out.text(type); }
void longEntry(Writer &out, std::string_view key, std::int32_t value) { entry(out, key, "long"); out.u32(std::uint32_t(value)); }
void doubleEntry(Writer &out, std::string_view key, double value) { entry(out, key, "doub"); out.f64(value); }
void enumEntry(Writer &out, std::string_view key, std::string_view type, std::string_view value)
{
    entry(out, key, "enum"); descriptorKey(out, type); descriptorKey(out, value);
}
void textEntry(Writer &out, std::string_view key, const std::string &value)
{
    entry(out, key, "TEXT"); out.unicode(QString::fromStdString(value) + QChar(0));
}
void warpDescriptor(Writer &out, const QRectF &bounds)
{
    descriptor(out, "warp", 8);
    enumEntry(out, "warpStyle", "warpStyle", "warpNone");
    doubleEntry(out, "warpValue", 0); doubleEntry(out, "warpPerspective", 0); doubleEntry(out, "warpPerspectiveOther", 0);
    enumEntry(out, "warpRotate", "Ornt", "Hrzn");
    entry(out, "bounds", "Objc"); descriptor(out, "Rctn", 4);
    doubleEntry(out, "Top ", 0); doubleEntry(out, "Left", 0);
    doubleEntry(out, "Btom", bounds.height()); doubleEntry(out, "Rght", bounds.width());
    longEntry(out, "uOrder", 4); longEntry(out, "vOrder", 4);
}

void smartTags(Writer &out, const ExportLayer &layer, CanvasOrigin origin)
{
    const auto quad = smartQuad(layer, origin);
    const auto placed = out.beginTag("PlLd");
    out.text("plcL"); out.u32(3); out.u8(std::uint8_t(layer.uniqueId.size())); out.text(layer.uniqueId);
    out.u32(1); out.u32(1); out.u32(16); out.u32(1); // Page, count, antialias, vector source.
    for (const auto value : quad) { out.f64(value); }
    out.u32(0); out.u32(16); warpDescriptor(out, layer.pdfBounds);
    out.endTag(placed);

    const auto smart = out.beginTag("SoLd"); out.text("soLD"); out.u32(4); out.u32(16);
    descriptor(out, "null", 17);
    textEntry(out, "Idnt", layer.uniqueId); textEntry(out, "placed", layer.uniqueId);
    // PDF MediaBox crop retains the complete page used by Sz, warp and quad;
    // bounding-box cropping would move content when the Smart Object is edited.
    longEntry(out, "PgNm", 1); longEntry(out, "totalPages", 1); longEntry(out, "Crop", 3);
    for (const auto key : {"frameStep", "duration"}) {
        entry(out, key, "Objc"); descriptor(out, "null", 2);
        longEntry(out, "numerator", 0); longEntry(out, "denominator", 600);
    }
    longEntry(out, "frameCount", 1); longEntry(out, "Annt", 16); longEntry(out, "Type", 1);
    for (const auto key : {"Trnf", "nonAffineTransform"}) {
        entry(out, key, "VlLs"); out.u32(8);
        for (const auto value : quad) { out.text("doub"); out.f64(value); }
    }
    entry(out, "warp", "Objc"); warpDescriptor(out, layer.pdfBounds);
    entry(out, "Sz  ", "Objc"); descriptor(out, "Pnt ", 2);
    doubleEntry(out, "Wdth", layer.pdfBounds.width()); doubleEntry(out, "Hght", layer.pdfBounds.height());
    entry(out, "Rslt", "UntF"); out.text("#Rsl"); out.f64(72);
    longEntry(out, "comp", -1);
    entry(out, "compInfo", "Objc"); descriptor(out, "null", 2);
    longEntry(out, "compID", -1); longEntry(out, "originalCompID", -1);
    out.endTag(smart);
}

void linkedPdfs(Writer &out, const std::vector<ExportLayer> &layers)
{
    if (std::none_of(layers.begin(), layers.end(), [](const auto &layer) { return !layer.pdf.isEmpty(); })) { return; }
    const auto tag = out.beginTag("lnk2");
    for (std::size_t index = 0; index < layers.size(); ++index) {
        const auto &layer = layers[index]; if (layer.pdf.isEmpty()) { continue; }
        const auto size = out.size(); out.u64(0); const auto body = out.size();
        out.text("liFD"); out.u32(2); out.u8(std::uint8_t(layer.uniqueId.size())); out.text(layer.uniqueId);
        // Adobe's linked-file Unicode filename includes a terminating code
        // unit in its count; omitting it truncates the last character in PS.
        out.unicode(QStringLiteral("vector-%1.pdf").arg(index) + QChar(0)); out.text("PDF "); out.u32(0);
        out.u64(std::uint64_t(layer.pdf.size())); out.u8(0); // Embedded payload; no external/open-file descriptor.
        out.data({reinterpret_cast<const std::uint8_t *>(layer.pdf.constData()), std::size_t(layer.pdf.size())});
        const auto length = out.size() - body; out.patch64(size, length); out.zeros((4 - length % 4) % 4);
    }
    out.endTag(tag, 4);
}

std::vector<ExportLayer> preflight(const Document &document, const PsdExportOptions &options, MediaIoResult &result,
                                  std::uint64_t &workingBytes)
{
    const auto validation = validate(document);
    if (!validation.ok()) { fail(MediaIoCode::InvalidArgument, validation.issues.front().message); }
    if (document.extent.width > 30000 || document.extent.height > 30000) {
        fail(MediaIoCode::UnsupportedFeature, "PSD v1 supports canvas dimensions up to 30000 pixels; PSB export is unavailable");
    }
    checked(checkExtent(document.extent, options.limits));
    if (!options.limits.maxFrames) { fail(MediaIoCode::LimitExceeded, "PSD frame 0 exceeds the frame limit"); }
    const auto origin = canvasOrigin(document);
    const auto outputLimit = std::min(options.limits.maxOutputBytes, MaximumPsdBytes);
    std::vector<ExportLayer> layers;
    std::uint64_t metadataBytes = 0, commandCount = 0, maxAssetBytes = 0, maxVectorScratch = 0, maxIdBytes = 0;
    const auto canvasBytes = std::uint64_t(document.extent.width) * document.extent.height * 4;
    std::uint64_t minimumOutput = 64;
    if (document.timeline.frameCount != 1 || !document.frames.empty()) {
        warn(result, "PSD exports frame 0 only; native timeline and later frames are omitted");
    }
    if (document.canvasMode == CanvasMode::Infinite) {
        warn(result, "PSD exports the fixed canvasRegion viewport of an infinite canvas");
    }
    if (document.stableDiffusionMetadata) { warn(result, "PSD does not retain native Stable Diffusion metadata"); }
    for (const auto &layer : document.layers) {
        const auto &properties = layerProperties(layer);
        if (properties.name.size() > options.limits.maxDecodedBytes / 4) {
            fail(MediaIoCode::LimitExceeded, "PSD layer names exceed the decoded-byte budget");
        }
        charge(metadataBytes, std::uint64_t(properties.name.size()) * 4, options.limits.maxDecodedBytes,
               "PSD layer names exceed the decoded-byte budget");
        if (properties.name.find('\0') != std::string::npos
            || QString::fromUtf8(properties.name).toUtf8().toStdString() != properties.name) {
            fail(MediaIoCode::InvalidArgument, "PSD layer names must be canonical UTF-8 without embedded nulls");
        }
        if (!layerExistsAt(document, layer, 0)) {
            warn(result, "PSD omits layers outside frame 0 because of their native frame range"); continue;
        }
        if (layers.size() >= options.maxLayers || layers.size() >= 32767) {
            fail(MediaIoCode::LimitExceeded, "PSD layer count exceeds its configured or signed 16-bit limit");
        }
        // Include the old and new backing arrays during vector growth; charge
        // before constructing/pushing any per-layer output metadata.
        charge(metadataBytes, 3 * sizeof(ExportLayer), options.limits.maxDecodedBytes,
               "PSD layer records exceed the decoded-byte budget");
        const auto *asset = resolveAssetAt(document, layer, 0);
        if (!asset) { fail(MediaIoCode::InvalidData, "PSD cannot resolve a layer asset at frame 0"); }
        std::uint64_t idBytes = 0;
        for (const auto size : {properties.id.size(), assetId(*asset).size()}) {
            if (size > options.limits.maxDecodedBytes / 4) { fail(MediaIoCode::LimitExceeded, "PSD copied IDs exceed the decoded-byte budget"); }
            charge(idBytes, std::uint64_t(size) * 4, options.limits.maxDecodedBytes, "PSD copied IDs exceed the decoded-byte budget");
        }
        maxIdBytes = std::max(maxIdBytes, idBytes);
        (void)psdBlend(properties.blendMode);
        ExportLayer output; output.layer = &layer; output.asset = asset;
        output.name = QString::fromUtf8(properties.name); output.visible = properties.visible;
        output.opacity = std::uint8_t(std::lround(properties.opacity * 255)); output.blend = properties.blendMode;
        if (double(output.opacity) / 255 != properties.opacity) { warn(result, "PSD layer opacity is quantized to 8-bit precision"); }
        output.directBitmap = nativeBitmap(asset, properties, origin);
        output.width = document.extent.width; output.height = document.extent.height;
        std::uint64_t assetBytes = 0, vectorScratch = 0;
        if (const auto *raster = std::get_if<RasterAsset>(asset)) {
            checked(checkRaster(raster->pixels, options.limits)); assetBytes = std::uint64_t(raster->pixels.pixels.size()) * 4;
            if (output.directBitmap) {
                output.width = raster->pixels.width; output.height = raster->pixels.height;
                output.left = std::int32_t(properties.transform.translationX - origin.x);
                output.top = std::int32_t(properties.transform.translationY - origin.y);
            } else { warn(result, "PSD bitmap transforms are baked into canvasRegion-clipped cached pixels"); }
        } else if (const auto *chunks = std::get_if<ChunkedRasterAsset>(asset)) {
            for (const auto &chunk : chunks->chunks) {
                checked(checkRaster(chunk.pixels, options.limits));
                charge(assetBytes, std::uint64_t(chunk.pixels.pixels.size()) * 4, options.limits.maxDecodedBytes,
                       "PSD chunked raster source exceeds the decoded-byte budget");
                // Native renderLayerRegion retains a canvas-sized piece per
                // chunk before composition. Count all chunks conservatively.
                charge(vectorScratch, canvasBytes * 2, options.limits.maxDecodedBytes,
                       "PSD chunk render pieces exceed the decoded-byte budget");
            }
            warn(result, "PSD chunked bitmap storage and transforms are baked into canvasRegion-clipped cached pixels");
        } else {
            const auto &vector = std::get<VectorAsset>(*asset);
            for (const auto &path : vector.paths) {
                charge(commandCount, path.commands.size(), options.limits.maxVectorCommands, "PSD vector commands exceed the command limit");
                charge(assetBytes, std::uint64_t(path.commands.size()) * sizeof(PathCommand), options.limits.maxDecodedBytes,
                       "PSD vector source exceeds the decoded-byte budget");
                for (const auto &command : path.commands) {
                    const bool curved = std::holds_alternative<QuadraticTo>(command) || std::holds_alternative<CubicTo>(command);
                    charge(vectorScratch, curved ? 32768 : 128, options.limits.maxDecodedBytes,
                           "PSD vector rasterization scratch exceeds the decoded-byte budget");
                }
            }
            output.pdfBounds = vectorBounds(vector);
            (void)smartQuad(output, origin);
            warn(result, "PSD vector layers use embedded editable PDF Smart Objects and canvasRegion-clipped raster caches");
        }
        maxAssetBytes = std::max(maxAssetBytes, assetBytes); maxVectorScratch = std::max(maxVectorScratch, vectorScratch);
        const auto pixelBytes = std::uint64_t(output.width) * output.height * 4;
        charge(minimumOutput, pixelBytes + 96 + std::uint64_t(output.name.size()) * 2,
               outputLimit, "PSD raw channels exceed the output-byte budget");
        layers.push_back(std::move(output));
    }
    if (layers.empty()) {
        if (!options.maxLayers) { fail(MediaIoCode::LimitExceeded, "PSD empty-frame placeholder exceeds the layer-count limit"); }
        charge(metadataBytes, 3 * sizeof(ExportLayer) + 4 * sizeof("(Empty frame 0)"), options.limits.maxDecodedBytes,
               "PSD placeholder metadata exceeds the decoded-byte budget");
        ExportLayer placeholder; placeholder.name = QStringLiteral("(Empty frame 0)"); layers.push_back(std::move(placeholder));
        warn(result, "PSD uses a transparent placeholder layer for an empty frame 0; the native document is unchanged");
    }
    charge(minimumOutput, canvasBytes, outputLimit, "PSD merged preview exceeds the output-byte budget");
    workingBytes = 0;
    charge(workingBytes, canvasBytes * 8, options.limits.maxDecodedBytes, "PSD render scratch exceeds the decoded-byte budget");
    charge(workingBytes, maxAssetBytes, options.limits.maxDecodedBytes, "PSD isolated asset copy exceeds the decoded-byte budget");
    charge(workingBytes, maxVectorScratch, options.limits.maxDecodedBytes, "PSD vector render scratch exceeds the decoded-byte budget");
    charge(workingBytes, metadataBytes, options.limits.maxDecodedBytes, "PSD layer/name storage exceeds the decoded-byte budget");
    charge(workingBytes, maxIdBytes, options.limits.maxDecodedBytes, "PSD copied/hash IDs exceed the decoded-byte budget");
    return layers;
}

RasterLayer layerCache(const Document &document, const ExportLayer &layer)
{
    if (!layer.layer) { return makeRasterLayer(document.extent.width, document.extent.height, 0); }
    Document isolated; isolated.extent = document.extent; isolated.canvasMode = document.canvasMode;
    isolated.infiniteCanvas = document.infiniteCanvas;
    isolated.assets.push_back(*layer.asset);
    isolated.layers.emplace_back(contentKind(*layer.layer) == ContentKind::Vector ? Layer(VectorLayer{}) : Layer(BitmapLayer{}));
    auto &properties = layerProperties(isolated.layers[0]);
    properties = layerProperties(*layer.layer);
    properties.visible = true; properties.opacity = 1; properties.blendMode = RasterBlendMode::SourceOver;
    properties.frameRange.reset(); layerSource(isolated.layers[0]) = StaticSource{assetId(*layer.asset)};
    auto rendered = renderFrameLayerTiles(isolated, 0, 0, {{canvasRegion(isolated), isolated.extent}});
    if (!rendered.ok() || rendered.tiles.size() != 1) { fail(MediaIoCode::InvalidData, "PSD layer raster cache could not be rendered: " + rendered.message); }
    return std::move(rendered.tiles[0].pixels);
}

void mergeLayer(RasterLayer &merged, RasterLayer cache, const ExportLayer &layer, CanvasRegion region)
{
    FrameLayerBatchRenderResult batch; batch.requests.push_back({region, region.extent});
    FrameLayerTileRenderResult base; base.visible = true; base.tiles.push_back({region, std::move(merged)});
    FrameLayerTileRenderResult overlay; overlay.layerIndex = 1; overlay.visible = true; overlay.opacity = double(layer.opacity) / 255;
    overlay.blendMode = layer.blend; overlay.tiles.push_back({region, std::move(cache)});
    batch.layers.push_back(std::move(base)); batch.layers.push_back(std::move(overlay));
    auto composed = composeFrameLayers(batch);
    if (!composed.ok() || composed.tiles.size() != 1) { fail(MediaIoCode::InvalidData, "PSD merged preview could not be composed: " + composed.message); }
    merged = std::move(composed.tiles[0].pixels);
}

void rawChannels(Writer &out, const RasterLayer &raster, bool perChannelCompression)
{
    for (const int shift : {16, 8, 0, 24}) {
        if (perChannelCompression) { out.u16(0); }
        out.reserveMore(raster.pixels.size());
        for (const auto pixel : raster.pixels) { out.bytes.push_back(std::uint8_t(pixel >> shift)); }
    }
}

void imageResources(Writer &out)
{
    const auto resources = out.begin32();
    out.text("8BIM"); out.u16(1005); out.u16(0); out.u32(16);
    out.u32(72U << 16); out.u16(1); out.u16(1);
    out.u32(72U << 16); out.u16(1); out.u16(1);
    const auto profile = QColorSpace(QColorSpace::SRgb).iccProfile();
    if (profile.isEmpty()) { fail(MediaIoCode::IoError, "PSD sRGB profile could not be created"); }
    out.text("8BIM"); out.u16(1039); out.u16(0); out.u32(std::uint32_t(profile.size()));
    out.data({reinterpret_cast<const std::uint8_t *>(profile.constData()), std::size_t(profile.size())});
    if (profile.size() & 1) { out.u8(0); }
    out.end32(resources);
}

MediaBytesResult encode(const Document &document, const PsdExportOptions &options)
{
    MediaBytesResult result;
    std::uint64_t workingBytes = 0;
    auto layers = preflight(document, options, result.result, workingBytes);
    std::uint64_t pdfBytes = 0;
    for (std::size_t index = 0; index < layers.size(); ++index) {
        auto &layer = layers[index];
        if (layer.asset && std::holds_alternative<VectorAsset>(*layer.asset)) {
            const auto limit = std::min(options.limits.maxOutputBytes, options.limits.maxDecodedBytes - workingBytes);
            makeVectorPdf(layer, index, limit - pdfBytes);
            charge(pdfBytes, std::uint64_t(layer.pdf.size()), limit, "PSD embedded PDFs exceed their aggregate byte budget");
        }
    }
    Writer out(options.limits.maxOutputBytes);
    out.text("8BPS"); out.u16(1); out.zeros(6); out.u16(4);
    out.u32(std::uint32_t(document.extent.height)); out.u32(std::uint32_t(document.extent.width)); out.u16(8); out.u16(3);
    out.u32(0); imageResources(out);
    const auto layerSection = out.begin32(), layerInfo = out.begin32();
    out.u16(std::uint16_t(-std::int32_t(layers.size())));
    for (auto &layer : layers) {
        out.u32(std::uint32_t(layer.top)); out.u32(std::uint32_t(layer.left));
        out.u32(std::uint32_t(std::int64_t(layer.top) + layer.height));
        out.u32(std::uint32_t(std::int64_t(layer.left) + layer.width)); out.u16(4);
        const auto pixels = std::uint64_t(layer.width) * layer.height;
        if (pixels + 2 > std::numeric_limits<std::uint32_t>::max()) { fail(MediaIoCode::LimitExceeded, "PSD layer channel exceeds its length field"); }
        for (const int id : {0, 1, 2, -1}) { out.u16(std::uint16_t(id)); out.u32(std::uint32_t(pixels + 2)); }
        out.text("8BIM"); out.text(psdBlend(layer.blend)); out.u8(layer.opacity); out.u8(0);
        out.u8(layer.visible ? 0 : 2); out.u8(0);
        const auto extra = out.begin32(); out.u32(0); out.u32(0); out.zeros(4);
        const auto name = out.beginTag("luni"); out.unicode(layer.name); out.endTag(name);
        if (layer.asset && std::holds_alternative<VectorAsset>(*layer.asset)) {
            smartTags(out, layer, canvasOrigin(document));
        }
        out.end32(extra);
    }
    auto merged = makeRasterLayer(document.extent.width, document.extent.height, 0);
    for (const auto &layer : layers) {
        RasterLayer cache;
        if (layer.directBitmap) {
            rawChannels(out, std::get<RasterAsset>(*layer.asset).pixels, true);
        } else if (!layer.layer) {
            rawChannels(out, makeRasterLayer(1, 1, 0), true);
        } else {
            cache = layerCache(document, layer); rawChannels(out, cache, true);
        }
        if (layer.visible && layer.opacity && layer.layer) {
            if (cache.pixels.empty()) { cache = layerCache(document, layer); }
            mergeLayer(merged, std::move(cache), layer, canvasRegion(document));
        }
    }
    if ((out.size() - layerInfo - 4) & 1U) { out.u8(0); }
    out.end32(layerInfo); out.u32(0); linkedPdfs(out, layers); out.end32(layerSection);
    out.u16(0); rawChannels(out, merged, false);
    result.bytes = std::move(out.bytes); return result;
}
} // namespace

MediaBytesResult encodePsd(const Document &document, const PsdExportOptions &options)
{
    try { return encode(document, options); }
    catch (const ExportFailure &failure) { return {{}, {failure.code, failure.message, {}}}; }
    catch (const std::bad_alloc &) { return {{}, {MediaIoCode::LimitExceeded, "PSD allocation exceeded available memory", {}}}; }
    catch (const std::length_error &) { return {{}, {MediaIoCode::LimitExceeded, "PSD allocation exceeded a container size limit", {}}}; }
}

MediaIoResult exportPsd(const Document &document, const std::string &path, const PsdExportOptions &options)
{
    QString absolute;
    auto destination = checkDestination(path, options.overwrite, absolute);
    if (!destination.ok()) { return destination; }
    auto encoded = encodePsd(document, options);
    if (!encoded.ok()) { return std::move(encoded.result); }
    auto written = writeFile(path, encoded.bytes, options.overwrite, options.limits);
    written.warnings = std::move(encoded.result.warnings); return written;
}
} // namespace iiSharedCanvas
