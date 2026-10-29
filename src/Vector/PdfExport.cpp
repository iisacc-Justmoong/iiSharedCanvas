#include "Vector/PdfExport.h"

#include "Document/CanvasSampling.h"
#include "Native/Media_p.hpp"
#include "Native/VectorPdf_p.hpp"
#include "Render/FrameRenderer.h"
#include "Validation/Validation.h"

#include <algorithm>
#include <cmath>
#include <filesystem>
#include <limits>
#include <new>
#include <stdexcept>
#include <string_view>
#include <type_traits>
#include <utility>

namespace iiSharedCanvas {
namespace {
struct Failure { MediaIoCode code; std::string message; };
[[noreturn]] void fail(MediaIoCode code, std::string message) { throw Failure{code, std::move(message)}; }
void check(const MediaIoResult &result) { if (!result.ok()) fail(result.code, result.message); }

std::string number(double value)
{
    if (!std::isfinite(value)) fail(MediaIoCode::InvalidData, "PDF coordinates must be finite");
    return media_detail::pdfNumber(value);
}

class PdfOutput final {
public:
    explicit PdfOutput(std::uint64_t limit) : limit_(limit) { append("%PDF-1.4\n%\xe2\xe3\xcf\xd3\n"); }
    std::size_t reserve() { offsets_.push_back(0); return offsets_.size() - 1; }
    void append(std::string_view value)
    {
        if (bytes_.size() > limit_ || value.size() > limit_ - bytes_.size())
            throw std::length_error("PDF exceeds its output byte limit");
        bytes_.append(value);
    }
    void object(std::size_t id, std::string_view value)
    {
        begin(id); append(value); append("\nendobj\n");
    }
    void stream(std::size_t id, std::string_view dictionary, std::string_view value)
    {
        begin(id); append("<< "); append(dictionary); append(" /Length ");
        append(std::to_string(value.size())); append(" >>\nstream\n"); append(value); append("\nendstream\nendobj\n");
    }
    void finish(std::size_t root)
    {
        const auto xref = bytes_.size();
        append("xref\n0 "); append(std::to_string(offsets_.size())); append("\n0000000000 65535 f \n");
        for (std::size_t id = 1; id < offsets_.size(); ++id) {
            if (!offsets_[id]) throw std::logic_error("unwritten PDF object");
            const auto offset = std::to_string(offsets_[id]);
            if (offset.size() > 10) throw std::length_error("PDF cross-reference offset exceeds its field");
            append(std::string(10 - offset.size(), '0')); append(offset); append(" 00000 n \n");
        }
        append("trailer\n<< /Size "); append(std::to_string(offsets_.size())); append(" /Root ");
        append(std::to_string(root)); append(" 0 R >>\nstartxref\n"); append(std::to_string(xref)); append("\n%%EOF\n");
    }
    [[nodiscard]] const std::string &bytes() const { return bytes_; }
private:
    void begin(std::size_t id)
    {
        if (!id || id >= offsets_.size() || offsets_[id]) throw std::logic_error("invalid PDF object id");
        offsets_[id] = bytes_.size(); append(std::to_string(id)); append(" 0 obj\n");
    }
    std::uint64_t limit_;
    std::string bytes_;
    std::vector<std::size_t> offsets_{0};
};

class Page final {
public:
    Page(PdfOutput &output, const MediaLimits &limits) : output_(output), limits_(limits) {}
    void append(std::string_view text)
    {
        if (content_.size() > limits_.maxOutputBytes || text.size() > limits_.maxOutputBytes - content_.size())
            throw std::length_error("PDF page exceeds its output byte limit");
        content_.append(text);
    }
    void state(double fillAlpha, double strokeAlpha)
    {
        const auto id = output_.reserve();
        const auto name = "G" + std::to_string(states_.size());
        output_.object(id, "<< /Type /ExtGState /ca " + number(fillAlpha) + " /CA " + number(strokeAlpha) + " >>");
        states_.emplace_back(name, id);
        append("/"); append(name); append(" gs\n");
    }
    void image(const RasterLayer &pixels, double x, double y)
    {
        check(media_detail::checkRaster(pixels, limits_));
        const auto count = pixels.pixels.size();
        if (count > limits_.maxDecodedBytes / 4 || count > std::numeric_limits<std::size_t>::max() / 3)
            fail(MediaIoCode::LimitExceeded, "PDF image exceeds its decoded byte budget");
        std::string rgb; rgb.reserve(count * 3);
        std::string alpha; alpha.reserve(count);
        bool transparent = false;
        for (const auto value : pixels.pixels) {
            rgb.push_back(char((value >> 16) & 255)); rgb.push_back(char((value >> 8) & 255));
            rgb.push_back(char(value & 255)); alpha.push_back(char(value >> 24));
            transparent |= (value >> 24) != 255;
        }
        const auto dimensions = " /Width " + std::to_string(pixels.width) + " /Height " + std::to_string(pixels.height)
            + " /BitsPerComponent 8";
        std::string mask;
        if (transparent) {
            const auto id = output_.reserve();
            output_.stream(id, "/Type /XObject /Subtype /Image /ColorSpace /DeviceGray" + dimensions, alpha);
            mask = " /SMask " + std::to_string(id) + " 0 R";
        }
        const auto id = output_.reserve();
        const auto name = "Im" + std::to_string(images_.size());
        output_.stream(id, "/Type /XObject /Subtype /Image /ColorSpace /DeviceRGB" + dimensions + mask, rgb);
        images_.emplace_back(name, id);
        // PDF image samples start at the upper-left. Compensate for the page's
        // top-left canvas coordinate system without reversing the raster rows.
        append("q "); append(number(pixels.width)); append(" 0 0 "); append(number(-double(pixels.height)));
        append(" "); append(number(x)); append(" "); append(number(y + pixels.height));
        append(" cm /"); append(name); append(" Do Q\n");
    }
    void vector(const VectorAsset &asset)
    {
        append("1 J 1 j\n");
        const auto point = [&](Point p) { append(number(p.x)); append(" "); append(number(p.y)); };
        const auto color = [&](std::uint32_t value) {
            append(number(double((value >> 16) & 255) / 255)); append(" ");
            append(number(double((value >> 8) & 255) / 255)); append(" ");
            append(number(double(value & 255) / 255));
        };
        for (const auto &path : asset.paths) {
            if (!path.fill && !path.stroke) continue;
            state(path.fill ? double(path.fill->argb >> 24) / 255 : 1,
                  path.stroke ? double(path.stroke->paint.argb >> 24) / 255 : 1);
            if (path.fill) { color(path.fill->argb); append(" rg\n"); }
            if (path.stroke) { color(path.stroke->paint.argb); append(" RG "); append(number(path.stroke->width)); append(" w\n"); }
            Point current{}, first{};
            for (const auto &command : path.commands) {
                std::visit([&](const auto &value) {
                    using T = std::decay_t<decltype(value)>;
                    if constexpr (std::is_same_v<T, MoveTo>) { point(value.point); append(" m\n"); current = first = value.point; }
                    else if constexpr (std::is_same_v<T, LineTo>) { point(value.point); append(" l\n"); current = value.point; }
                    else if constexpr (std::is_same_v<T, QuadraticTo>) {
                        Point a{current.x + (value.control.x - current.x) * 2 / 3,
                                current.y + (value.control.y - current.y) * 2 / 3};
                        Point b{value.end.x + (value.control.x - value.end.x) * 2 / 3,
                                value.end.y + (value.control.y - value.end.y) * 2 / 3};
                        point(a); append(" "); point(b); append(" "); point(value.end); append(" c\n"); current = value.end;
                    } else if constexpr (std::is_same_v<T, CubicTo>) {
                        point(value.control1); append(" "); point(value.control2); append(" "); point(value.end);
                        append(" c\n"); current = value.end;
                    } else { static_assert(std::is_same_v<T, ClosePath>); append("h\n"); current = first; }
                }, command);
            }
            append(path.fill ? (path.stroke ? "B*\n" : "f*\n") : "S\n");
        }
    }
    std::string resources() const
    {
        std::string result = "<< /ExtGState << ";
        for (const auto &[name, id] : states_) result += "/" + name + " " + std::to_string(id) + " 0 R ";
        result += ">> /XObject << ";
        for (const auto &[name, id] : images_) result += "/" + name + " " + std::to_string(id) + " 0 R ";
        return result + ">> >>";
    }
    [[nodiscard]] const std::string &content() const { return content_; }
private:
    PdfOutput &output_;
    const MediaLimits &limits_;
    std::string content_;
    std::vector<std::pair<std::string, std::size_t>> states_, images_;
};

void warn(MediaIoResult &result, std::string message)
{
    if (std::find(result.warnings.begin(), result.warnings.end(), message) == result.warnings.end())
        result.warnings.push_back(std::move(message));
}
void transform(Page &page, const AffineTransform &t, CanvasOrigin origin)
{
    page.append(number(t.m11)); page.append(" "); page.append(number(t.m12)); page.append(" ");
    page.append(number(t.m21)); page.append(" "); page.append(number(t.m22)); page.append(" ");
    page.append(number(t.translationX - origin.x)); page.append(" "); page.append(number(t.translationY - origin.y));
    page.append(" cm\n");
}
}

MediaIoResult exportPdf(const Document &document, const std::string &path, const PdfExportOptions &options)
{
    try {
        std::filesystem::path destination;
        check(media_detail::checkDestination(path, options.overwrite, destination));
        check(media_detail::checkExtent(document.extent, options.limits));
        if (document.extent.width * 0.75 > 14400 || document.extent.height * 0.75 > 14400)
            fail(MediaIoCode::UnsupportedFeature, "PDF page dimensions exceed 14400 points");
        const auto validation = validate(document);
        if (!validation.ok()) fail(MediaIoCode::InvalidArgument, validation.issues.front().message);
        if (!document.artboards.empty())
            fail(MediaIoCode::UnsupportedFeature, "PDF artboard export is unavailable; select an artboard explicitly");
        const auto last = options.lastFrame.value_or(options.firstFrame);
        if (options.firstFrame > last || last >= document.timeline.frameCount
            || std::uint64_t(last) + 1 - options.firstFrame > options.limits.maxFrames)
            fail(MediaIoCode::InvalidArgument, "invalid PDF page/frame range");
        for (const auto &asset : document.assets) if (const auto *vector = std::get_if<VectorAsset>(&asset)) {
            check(media_detail::checkExtent(vector->viewport, options.limits));
            std::uint64_t commands = 0;
            for (const auto &item : vector->paths) {
                if (item.commands.size() > options.limits.maxVectorCommands - commands)
                    fail(MediaIoCode::LimitExceeded, "PDF vector command count exceeds the limit");
                commands += item.commands.size();
            }
        }
        MediaIoResult result;
        PdfOutput output(options.limits.maxOutputBytes);
        const auto root = output.reserve(), pages = output.reserve();
        std::vector<std::size_t> pageIds;
        for (std::uint64_t index = options.firstFrame; index <= last; ++index) {
            const auto frame = FrameIndex(index);
            Page page(output, options.limits);
            page.append("q\n0.75 0 0 -0.75 0 "); page.append(number(document.extent.height * 0.75)); page.append(" cm\n");
            bool flatten = false;
            for (const auto &layer : document.layers) {
                if (layerRole(layer) == LayerRole::ControlNet) continue;
                const auto sampled = sampleLayerAt(document, layer, frame);
                const auto &properties = layerProperties(layer);
                if (sampled.visible && (properties.blendMode != RasterBlendMode::SourceOver
                    || properties.appearance != LayerAppearance{})) flatten = true;
            }
            if (flatten) {
                if (!options.rasterizeUnsupportedBlending)
                    fail(MediaIoCode::UnsupportedFeature, "PDF requires explicit rasterization for this blending or appearance");
                const auto rendered = renderFrame(document, frame);
                if (!rendered.ok()) fail(MediaIoCode::InvalidData, rendered.message);
                page.image(rendered.pixels, 0, 0);
                warn(result, "PDF page rasterized to preserve canvas blending and appearance");
            } else for (std::size_t layerIndex = 0; layerIndex < document.layers.size(); ++layerIndex) {
                const auto &layer = document.layers[layerIndex];
                if (layerRole(layer) == LayerRole::ControlNet) continue;
                const auto sampled = sampleLayerAt(document, layer, frame);
                if (!sampled.visible || sampled.opacity <= 0) continue;
                const auto *asset = resolveAssetAt(document, layer, frame);
                if (!asset) fail(MediaIoCode::InvalidData, "cannot resolve PDF layer asset");
                page.append("q\n");
                if (std::holds_alternative<VectorAsset>(*asset) && sampled.opacity != 1) {
                    const auto rendered = renderFrameLayerTiles(document, frame, layerIndex,
                        {{canvasRegion(document), document.extent}});
                    if (!rendered.ok() || rendered.tiles.size() != 1)
                        fail(MediaIoCode::InvalidData, "cannot render translucent PDF vector group");
                    page.state(sampled.opacity, sampled.opacity);
                    page.image(rendered.tiles[0].pixels, 0, 0);
                    warn(result, "translucent vector layer rasterized to preserve isolated group opacity in PDF");
                } else {
                    transform(page, sampled.transform, canvasOrigin(document));
                    if (const auto *vector = std::get_if<VectorAsset>(asset)) page.vector(*vector);
                    else {
                        page.state(sampled.opacity, sampled.opacity);
                        if (const auto *raster = std::get_if<RasterAsset>(asset)) page.image(raster->pixels, 0, 0);
                        else if (const auto *video = std::get_if<VideoLayer>(&layer)) {
                            if (const auto *pixels = resolveVideoFrameAt(document, *video, frame)) page.image(*pixels, 0, 0);
                        } else if (const auto *chunks = std::get_if<ChunkedRasterAsset>(asset)) {
                            for (const auto &chunk : chunks->chunks)
                                page.image(chunk.pixels, double(chunk.column) * document.infiniteCanvas.chunkSize,
                                           double(chunk.row) * document.infiniteCanvas.chunkSize);
                        }
                    }
                }
                page.append("Q\n");
            }
            page.append("Q\n");
            const auto content = output.reserve(), pageId = output.reserve();
            output.stream(content, "", page.content());
            output.object(pageId, "<< /Type /Page /Parent " + std::to_string(pages) + " 0 R /MediaBox [0 0 "
                + number(document.extent.width * 0.75) + " " + number(document.extent.height * 0.75)
                + "] /Resources " + page.resources() + " /Contents " + std::to_string(content) + " 0 R >>");
            pageIds.push_back(pageId);
        }
        std::string kids;
        for (const auto id : pageIds) kids += std::to_string(id) + " 0 R ";
        output.object(pages, "<< /Type /Pages /Count " + std::to_string(pageIds.size()) + " /Kids [" + kids + "] >>");
        output.object(root, "<< /Type /Catalog /Pages " + std::to_string(pages) + " 0 R >>");
        output.finish(root);
        const auto &bytes = output.bytes();
        auto written = media_detail::writeFile(path,
            {reinterpret_cast<const std::uint8_t *>(bytes.data()), bytes.size()}, options.overwrite, options.limits);
        written.warnings = std::move(result.warnings);
        return written;
    } catch (const Failure &error) {
        return media_detail::error(error.code, error.message);
    } catch (const std::bad_alloc &) {
        return media_detail::error(MediaIoCode::LimitExceeded, "PDF allocation exceeds available memory");
    } catch (const std::length_error &error) {
        return media_detail::error(MediaIoCode::LimitExceeded, error.what());
    } catch (const std::exception &error) {
        return media_detail::error(MediaIoCode::IoError, error.what());
    }
}

} // namespace iiSharedCanvas
