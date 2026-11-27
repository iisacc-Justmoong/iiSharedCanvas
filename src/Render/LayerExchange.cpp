#include "Render/LayerExchange.h"

#include "Bitmap/BitmapCodec.h"
#include "Render/FrameRenderer.h"
#include "Validation/Validation.h"
#include "Vector/SvgExport.h"

#include <iiSDKCore/Core/Text/Number.h>

#include <algorithm>
#include <array>
#include <cmath>
#include <cstdint>
#include <limits>
#include <new>
#include <stdexcept>
#include <unordered_map>
#include <unordered_set>

namespace iiSharedCanvas {
namespace {

bool validProperties(const LayerProperties &p)
{
    const auto &t = p.transform;
    return std::isfinite(p.opacity) && p.opacity >= 0 && p.opacity <= 1 &&
        std::isfinite(t.m11) && std::isfinite(t.m12) && std::isfinite(t.m21) && std::isfinite(t.m22) &&
        std::isfinite(t.translationX) && std::isfinite(t.translationY) &&
        static_cast<unsigned>(p.blendMode) <= static_cast<unsigned>(RasterBlendMode::Dissolve);
}

bool activeMask(const std::optional<LayerMask> &mask) { return mask && mask->enabled; }

// Each item represents a distinct reason that a standalone layer cannot be
// reproduced with only CSS declarations or a plain vector path group.
std::vector<std::string> appearanceLosses(const LayerProperties &p)
{
    std::vector<std::string> losses;
    const auto &a = p.appearance;
    if (activeMask(a.rasterMask) || activeMask(a.vectorMask) || activeMask(a.filterMask) ||
        (a.maskStackEnabled && std::any_of(a.maskStack.begin(), a.maskStack.end(),
            [](const LayerMaskEntry &entry) { return entry.mask.enabled; }))) losses.emplace_back("layer masks");
    if (std::any_of(a.filters.begin(), a.filters.end(), [](const LayerFilter &filter) { return filter.enabled; }))
        losses.emplace_back("layer filters");
    if (std::any_of(a.styles.effects.begin(), a.styles.effects.end(), [](const LayerStyle &style) { return style.enabled; }))
        losses.emplace_back("layer styles");
    if (a.fillOpacity != 1) losses.emplace_back("independent fill opacity");
    if (a.clippedToBelow) losses.emplace_back("clipping to a lower layer");
    if (a.blendIf) losses.emplace_back("conditional blending");
    if (p.artboardId) losses.emplace_back("artboard clipping and placement");
    if (p.frameRange || !p.motion.empty()) losses.emplace_back("timeline-dependent visibility or motion");
    return losses;
}

std::string number(double value) { return ii::core::format_decimal(value); }

std::string matrix(const AffineTransform &t, std::string_view delimiter)
{
    const std::array values{t.m11, t.m12, t.m21, t.m22, t.translationX, t.translationY};
    std::string result = "matrix(";
    for (std::size_t i = 0; i < values.size(); ++i) {
        if (i) result += delimiter;
        result += number(values[i]);
    }
    return result + ")";
}

std::string color(std::uint32_t argb)
{
    constexpr char hex[] = "0123456789abcdef";
    std::string result = "#";
    for (const auto shift : {16, 8, 0, 24}) {
        const auto component = (argb >> shift) & 255U;
        result.push_back(hex[component >> 4]); result.push_back(hex[component & 15U]);
    }
    return result;
}

const char *cssBlend(RasterBlendMode mode)
{
    switch (mode) {
    case RasterBlendMode::SourceOver: return "normal";
    case RasterBlendMode::Multiply: return "multiply";
    case RasterBlendMode::Screen: return "screen";
    case RasterBlendMode::Overlay: return "overlay";
    case RasterBlendMode::Darken: return "darken";
    case RasterBlendMode::Lighten: return "lighten";
    case RasterBlendMode::ColorBurn: return "color-burn";
    case RasterBlendMode::ColorDodge: return "color-dodge";
    case RasterBlendMode::SoftLight: return "soft-light";
    case RasterBlendMode::HardLight: return "hard-light";
    case RasterBlendMode::Difference: return "difference";
    case RasterBlendMode::Exclusion: return "exclusion";
    case RasterBlendMode::Hue: return "hue";
    case RasterBlendMode::Saturation: return "saturation";
    case RasterBlendMode::Color: return "color";
    case RasterBlendMode::Luminosity: return "luminosity";
    default: return nullptr;
    }
}

class BoundedText final {
public:
    explicit BoundedText(std::uint64_t limit) : limit_(limit) {}
    void append(std::string_view part) {
        if (part.size() > limit_ - text_.size()) throw std::length_error("SVG output exceeds its byte budget");
        text_.append(part);
    }
    [[nodiscard]] std::string take() && { return std::move(text_); }
private:
    std::uint64_t limit_;
    std::string text_;
};

// Stream base64 into the bounded result instead of allocating a second copy.
void appendBase64(BoundedText &output, std::span<const std::uint8_t> bytes)
{
    constexpr char digits[] = "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";
    for (std::size_t i = 0; i < bytes.size(); i += 3) {
        const auto remaining = bytes.size() - i;
        const std::uint32_t value = (std::uint32_t(bytes[i]) << 16) |
            (remaining > 1 ? std::uint32_t(bytes[i + 1]) << 8 : 0) |
            (remaining > 2 ? bytes[i + 2] : 0);
        const std::array chars{digits[(value >> 18) & 63U], digits[(value >> 12) & 63U],
            remaining > 1 ? digits[(value >> 6) & 63U] : '=', remaining > 2 ? digits[value & 63U] : '='};
        output.append({chars.data(), chars.size()});
    }
}

} // namespace

LayerCssResult LayerCssEncoder::operator()(const LayerProperties &properties, CanvasExtent extent,
    const FillLayerContent *fill, const LayerGroupTree *groups) const
{
    if (!validProperties(properties) || extent.width <= 0 || extent.height <= 0 ||
        (fill && (fill->extent != extent || static_cast<unsigned>(fill->kind) > static_cast<unsigned>(FillLayerKind::Pattern))))
        return {{}, {}, "invalid CSS layer properties or dimensions"};
    try {
        LayerCssResult result;
        result.losses = appearanceLosses(properties);
        if (groups && !groups->groupOf(properties.id).empty())
            result.losses.emplace_back("enclosing group visibility and composition");
        result.declarations = "position: absolute;\nleft: 0;\ntop: 0;\nwidth: " + std::to_string(extent.width) +
            "px;\nheight: " + std::to_string(extent.height) + "px;\ntransform-origin: 0 0;\ntransform: " +
            matrix(properties.transform, ", ") + ";\nopacity: " + number(properties.opacity) +
            ";\nvisibility: " + (properties.visible ? "visible" : "hidden") + ";\n";
        if (const auto *blend = cssBlend(properties.blendMode)) {
            result.declarations += std::string("mix-blend-mode: ") + blend + ";\n";
            if (properties.blendMode != RasterBlendMode::SourceOver)
                result.losses.emplace_back("blending depends on the surrounding CSS stacking context");
        } else result.losses.emplace_back("blend mode has no CSS equivalent");
        if (!fill) result.losses.emplace_back("layer content was not supplied");
        else if (fill->kind == FillLayerKind::Solid) result.declarations += "background-color: " + color(fill->color) + ";\n";
        else result.losses.emplace_back("gradient or pattern content requires a separate projection");
        return result;
    } catch (const std::bad_alloc &) {
        return {{}, {}, "insufficient memory for CSS output"};
    }
}

SelectedLayerSvgResult SelectedLayerSvgProjection::operator()(const Document &document, FrameIndex frame,
    std::span<const std::string_view> selectedLayerIds, CanvasRegion region,
    const RasterLayer *fallback, const MediaLimits &limits, std::span<const SvgLayerRasterFallback> layerFallbacks) const
{
    const auto failure = [](std::string message) { return SelectedLayerSvgResult{{}, false, {}, std::move(message)}; };
    if (frame >= document.timeline.frameCount || selectedLayerIds.empty() ||
        region.extent.width <= 0 || region.extent.height <= 0 ||
        std::int64_t(region.origin.x) + region.extent.width > std::numeric_limits<std::int32_t>::max() ||
        std::int64_t(region.origin.y) + region.extent.height > std::numeric_limits<std::int32_t>::max())
        return failure("invalid selected frame or SVG region");
    const auto pixelLimit = static_cast<std::size_t>(std::min<std::uint64_t>(limits.maxPixelsPerFrame,
        std::numeric_limits<std::size_t>::max()));
    const auto display = displayPixelExtent(region.extent, document.pixelAspectRatio, pixelLimit);
    if (!display) return failure("invalid pixel aspect ratio or SVG pixel budget");
    try {
        const auto validity = validate(document);
        if (!validity.ok()) return failure("invalid SVG source document: " + validity.issues.front().message);
        std::unordered_set<std::string_view> selected;
        for (const auto id : selectedLayerIds) {
            const auto index=layerIndex(document,std::string(id));
            if (id.empty() || !selected.insert(id).second || !index)
                return failure("selected layer identifiers must be distinct and present");
            if (layerRole(document.layers[*index])!=LayerRole::Artwork)
                return failure("only artwork layers can be projected");
        }
        // A clipped layer must keep its original direct-group/artboard base;
        // another selected layer farther below must not silently replace it.
        for (std::size_t i=0;i<document.layers.size();++i) {
            const auto &properties=layerProperties(document.layers[i]);
            if (!selected.contains(properties.id) || !properties.appearance.clippedToBelow) continue;
            const auto group=document.relations.groups.groupOf(properties.id);
            const LayerProperties *base=nullptr;
            for (std::size_t j=i;j>0;--j) {
                const auto &candidate=document.layers[j-1];
                const auto &p=layerProperties(candidate);
                if (layerRole(candidate)==LayerRole::Artwork && p.artboardId==properties.artboardId &&
                    document.relations.groups.groupOf(p.id)==group && !p.appearance.clippedToBelow) {
                    base=&p; break;
                }
            }
            if (!base || !selected.contains(base->id))
                return failure("selected clipped layer requires its original base");
        }
        const auto visible=[&](const Layer &layer) {
            const auto &p=layerProperties(layer);
            if (!p.visible || !layerExistsAt(document,layer,frame)) return false;
            if (p.artboardId) {
                const auto board=std::find_if(document.artboards.begin(),document.artboards.end(),
                    [&](const Artboard &candidate) { return candidate.id==*p.artboardId; });
                if (board==document.artboards.end() || !board->visible) return false;
            }
            auto group=document.relations.groups.groupOf(p.id);
            std::size_t depth=0;
            while (!group.empty()) {
                if (++depth>document.relations.groups.groups().size()) return false;
                const auto *parent=document.relations.groups.find(group);
                if (!parent || !parent->visible) return false;
                group=parent->parentId;
            }
            return true;
        };

        const auto aligned=[&](const RasterLayer *image) {
            return image && image->width==region.extent.width && image->height==region.extent.height &&
                std::uint64_t(image->width)*image->height==image->pixels.size();
        };
        std::unordered_map<std::string_view,const RasterLayer *> isolated;
        for (const auto &item:layerFallbacks)
            if (!selected.contains(item.layerId) || !aligned(item.pixels) ||
                !isolated.emplace(item.layerId,item.pixels).second)
                return failure("isolated layer fallback identifiers or geometry are invalid");

        struct Entry { const VectorAsset *asset; const LayerProperties *properties; const RasterLayer *raster; };
        std::vector<Entry> entries;
        std::vector<std::string> rasterizedLayers;
        bool needsWholeRaster = false;
        for (const auto &layer : document.layers) {
            const auto &properties = layerProperties(layer);
            if (!selected.contains(properties.id)) continue;
            if (!validProperties(properties)) return failure("invalid selected layer properties");
            if (!visible(layer)) continue;
            const auto *resolved=resolveAssetAt(document,layer,frame);
            const bool backdropDependent=properties.blendMode!=RasterBlendMode::SourceOver ||
                properties.appearance.clippedToBelow || properties.appearance.blendIf || properties.artboardId ||
                !document.relations.groups.groupOf(properties.id).empty() ||
                (resolved && std::holds_alternative<AdjustmentLayerContent>(*resolved));
            if (backdropDependent) { needsWholeRaster=true; break; }
            const auto *vectorLayer = std::get_if<StaticVectorLayer>(&layer);
            const VectorAsset *asset=nullptr;
            if (vectorLayer) {
                asset=findVectorAsset(document,vectorLayer->content.assetId);
                if (!asset) return failure("selected vector asset is missing");
            }
            if (asset && !asset->shape && appearanceLosses(properties).empty())
                entries.push_back({asset,&properties,nullptr});
            else if (const auto found=isolated.find(properties.id); found!=isolated.end()) {
                entries.push_back({nullptr,&properties,found->second});
                rasterizedLayers.push_back(properties.id);
            } else { needsWholeRaster=true; break; }
        }

        BoundedText output(limits.maxOutputBytes);
        output.append("<?xml version=\"1.0\" encoding=\"UTF-8\"?>\n<svg xmlns=\"http://www.w3.org/2000/svg\" width=\"");
        output.append(std::to_string(display->width)); output.append("\" height=\"");
        output.append(std::to_string(display->height)); output.append("\" viewBox=\"");
        output.append(std::to_string(region.origin.x)); output.append(" "); output.append(std::to_string(region.origin.y));
        output.append(" "); output.append(std::to_string(region.extent.width)); output.append(" ");
        output.append(std::to_string(region.extent.height)); output.append("\" preserveAspectRatio=\"none\">\n");

        const auto appendImage=[&](const RasterLayer &image) -> std::string {
            BitmapExportOptions options; options.limits = limits; options.extendedCodecs = false;
            const auto png = encodeBitmap(image, options);
            if (!png.ok()) return png.result.message.empty() ? "PNG encoding failed" : png.result.message;
            output.append("<image x=\""); output.append(std::to_string(region.origin.x)); output.append("\" y=\"");
            output.append(std::to_string(region.origin.y)); output.append("\" width=\"");
            output.append(std::to_string(region.extent.width)); output.append("\" height=\"");
            output.append(std::to_string(region.extent.height)); output.append("\" href=\"data:image/png;base64,");
            appendBase64(output, png.bytes); output.append("\"/>\n");
            return {};
        };
        if (needsWholeRaster) {
            if (!aligned(fallback)) return failure("selected composition requires an aligned fallback raster");
            if (const auto error=appendImage(*fallback); !error.empty()) return failure(error);
            output.append("</svg>\n");
            rasterizedLayers.clear();
            for (const auto &layer:document.layers) {
                const auto &properties=layerProperties(layer);
                if (selected.contains(properties.id) && visible(layer)) rasterizedLayers.push_back(properties.id);
            }
            return {std::move(output).take(),true,{"selected composition was embedded as raster pixels"},{},std::move(rasterizedLayers)};
        }

        for (const auto &entry : entries) {
            if (entry.raster) {
                if (const auto error=appendImage(*entry.raster); !error.empty()) return failure(error);
                continue;
            }
            VectorExportOptions options; options.limits = limits;
            const auto svg = encodeSvg(*entry.asset, options);
            if (!svg.ok()) return failure(svg.result.message);
            const std::string_view data(reinterpret_cast<const char *>(svg.bytes.data()), svg.bytes.size());
            const auto start = data.find("<svg ");
            if (start == std::string_view::npos) return failure("vector encoder returned invalid SVG");
            output.append("<g transform=\""); output.append(matrix(entry.properties->transform, " "));
            output.append("\" opacity=\""); output.append(number(entry.properties->opacity)); output.append("\">\n");
            output.append(data.substr(start)); output.append("</g>\n");
        }
        output.append("</svg>\n");
        const bool rasterized=!rasterizedLayers.empty();
        std::vector<std::string> warnings;
        if (rasterized) warnings.emplace_back("some selected layers were embedded as raster pixels");
        return {std::move(output).take(),rasterized,std::move(warnings),{},std::move(rasterizedLayers)};
    } catch (const std::bad_alloc &) {
        return failure("insufficient memory for SVG projection");
    } catch (const std::length_error &error) {
        return failure(error.what());
    }
}
} // namespace iiSharedCanvas
