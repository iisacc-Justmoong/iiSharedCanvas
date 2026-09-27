#include "ControlNet/SemanticSegment.h"
#include "Document/Document.h"
#include "Validation/Validation.h"

#include <algorithm>
#include <cmath>
#include <unordered_map>
#include <unordered_set>

namespace iiSharedCanvas {

bool isSemanticMaskAsset(const Document &document, const std::string &id) noexcept
{
    for (const auto &base : document.layers) {
        const auto *layer = std::get_if<SemanticSegmentLayer>(&base);
        if (!layer) { continue; }
        if (const auto *source = std::get_if<StaticSource>(&layer->source)) {
            if (source->assetId == id) { return true; }
        } else {
            for (const auto frame : std::get<KeyframedSource>(layer->source).frameIndices) {
                const auto *key = findKeyframe(document, layer->properties.id, frame);
                if (key && key->assetId == id) { return true; }
            }
        }
    }
    return false;
}

SemanticSegmentLayer *findSemanticSegmentLayer(Document &document, const std::string &id) noexcept
{
    auto *layer = findLayer(document, id);
    return layer ? std::get_if<SemanticSegmentLayer>(layer) : nullptr;
}

const SemanticSegmentLayer *findSemanticSegmentLayer(const Document &document, const std::string &id) noexcept
{
    const auto *layer = findLayer(document, id);
    return layer ? std::get_if<SemanticSegmentLayer>(layer) : nullptr;
}

const SemanticClass *findSemanticClass(const SemanticSegmentation &value, std::uint32_t id) noexcept
{
    const auto found = std::find_if(value.taxonomy.classes.begin(), value.taxonomy.classes.end(),
        [id](const auto &entry) { return entry.id == id; });
    return found == value.taxonomy.classes.end() ? nullptr : &*found;
}

const SemanticRegion *findSemanticRegion(const SemanticSegmentation &value, std::uint32_t id) noexcept
{
    const auto found = std::find_if(value.regions.begin(), value.regions.end(),
        [id](const auto &entry) { return entry.id == id; });
    return found == value.regions.end() ? nullptr : &*found;
}

std::vector<SemanticIssue> validateSemanticSegment(const Document &document, const SemanticSegmentLayer &layer)
{
    std::vector<SemanticIssue> issues;
    const auto issue = [&](std::string path, std::string message) {
        issues.push_back({std::move(path), std::move(message)});
    };
    const auto opaque = [](std::uint32_t color) { return (color >> 24) == 255; };
    const auto &settings = layer.control;
    const auto &value = layer.segmentation;
    if (document.formatVersion.minor < 7) { issue("", "semantic layers require format 1.7"); }
    if (!std::isfinite(settings.conditioningScale) || settings.conditioningScale < 0
        || !std::isfinite(settings.guidanceStart) || !std::isfinite(settings.guidanceEnd)
        || settings.guidanceStart < 0 || settings.guidanceEnd > 1 || settings.guidanceStart >= settings.guidanceEnd) {
        issue("control", "conditioning scale must be finite and nonnegative; guidance must satisfy 0 <= start < end <= 1");
    }
    if (value.taxonomy.id.empty() || value.taxonomy.classes.empty()
        || value.taxonomy.classes.size() > 65536 || value.regions.size() > 1048576) {
        issue("segmentation", "a named nonempty taxonomy requires at most 65536 classes and 1048576 regions");
        return issues;
    }
    if (!opaque(value.voidMaskColor) || !opaque(value.voidControlColor)) {
        issue("segmentation", "void colors must be opaque ARGB");
    }
    std::unordered_map<std::uint32_t, const SemanticClass *> classes;
    std::unordered_set<std::string> keys;
    for (const auto &entry : value.taxonomy.classes) {
        if (!classes.emplace(entry.id, &entry).second || entry.key.empty() || entry.name.empty()
            || !keys.insert(entry.key).second || !opaque(entry.controlColor)) {
            issue("segmentation.taxonomy.classes", "class ids and keys must be unique, names nonempty and control colors opaque");
        }
        std::unordered_set<std::string> aliases;
        for (const auto &alias : entry.aliases) {
            if (alias.empty() || !aliases.insert(alias).second) { issue("segmentation.taxonomy.classes.aliases", "aliases must be nonempty and unique per class"); }
        }
    }
    std::unordered_map<std::uint32_t, unsigned> states;
    for (const auto &[id, entry] : classes) {
        if (states[id] == 2) { continue; }
        std::vector<std::uint32_t> path;
        auto current = entry;
        while (current) {
            auto &state = states[current->id];
            if (state == 1) { issue("segmentation.taxonomy.classes.parentId", "class parent hierarchy contains a cycle"); break; }
            if (state == 2) { break; }
            state = 1; path.push_back(current->id);
            if (!current->parentId) { break; }
            const auto parent = classes.find(*current->parentId);
            if (parent == classes.end()) { issue("segmentation.taxonomy.classes.parentId", "class parent is missing"); break; }
            current = parent->second;
        }
        for (const auto visited : path) { states[visited] = 2; }
    }
    std::unordered_set<std::uint32_t> ids;
    std::unordered_set<std::uint32_t> colors;
    for (const auto &region : value.regions) {
        if (!region.id || !ids.insert(region.id).second || !classes.contains(region.classId)
            || !opaque(region.maskColor) || region.maskColor == value.voidMaskColor
            || !colors.insert(region.maskColor).second) {
            issue("segmentation.regions", "regions require unique nonzero ids, unique opaque non-void mask colors and existing classes");
        }
        if ((region.instanceId && *region.instanceId == 0)
            || (region.confidence && (!std::isfinite(*region.confidence) || *region.confidence < 0 || *region.confidence > 1))
            || static_cast<unsigned>(region.origin) > static_cast<unsigned>(SemanticOrigin::Model)) {
            issue("segmentation.regions", "instance ids must be nonzero, confidence within [0,1], and origin supported");
        }
        std::unordered_set<std::string> attributes;
        for (const auto &attribute : region.attributes) {
            if (attribute.key.empty() || !attributes.insert(attribute.key).second) {
                issue("segmentation.regions.attributes", "attribute keys must be nonempty and unique per region");
            }
        }
    }
    std::unordered_set<std::string> assets;
    if (const auto *source = std::get_if<StaticSource>(&layer.source)) {
        assets.insert(source->assetId);
    } else {
        for (const auto frame : std::get<KeyframedSource>(layer.source).frameIndices) {
            const auto *key = findKeyframe(document, layer.properties.id, frame);
            if (!key) { issue("source", "semantic frame content is missing"); }
            else { assets.insert(key->assetId); }
        }
    }
    std::optional<CanvasExtent> extent;
    for (const auto &id : assets) {
        const auto *asset = findRasterAsset(document, id);
        if (!asset || asset->pixels.width <= 0 || asset->pixels.height <= 0
            || std::uint64_t(asset->pixels.width) * asset->pixels.height != asset->pixels.pixels.size()) {
            issue("source", "semantic sources require valid dense RasterAsset masks"); continue;
        }
        if (extent && (extent->width != asset->pixels.width || extent->height != asset->pixels.height)) {
            issue("source", "all semantic frame masks must share one extent");
        }
        extent = CanvasExtent{asset->pixels.width, asset->pixels.height};
        if (std::any_of(asset->pixels.pixels.begin(), asset->pixels.pixels.end(), [&](auto color) {
                return color != value.voidMaskColor && !colors.contains(color);
            })) {
            issue("source", "mask contains an unmapped color; antialiasing, transparency and approximate palette matching are forbidden");
        }
    }
    return issues;
}

SemanticControlMapResult renderSemanticControlMap(const Document &document, const std::string &id,
                                                  std::uint32_t frame, std::uint64_t maxPixels)
{
    const auto failure = [](std::string message) { SemanticControlMapResult result; result.message = std::move(message); return result; };
    const auto validation = validate(document);
    if (!validation.ok()) { return failure(validation.issues.front().path + ": " + validation.issues.front().message); }
    const auto *base = findLayer(document, id);
    const auto *layer = base ? std::get_if<SemanticSegmentLayer>(base) : nullptr;
    if (!layer) { return failure("semantic layer not found"); }
    if (!layer->control.enabled) { return failure("ControlNet layer is disabled"); }
    const auto *asset = resolveAssetAt(document, *base, frame);
    const auto *mask = asset ? std::get_if<RasterAsset>(asset) : nullptr;
    if (!mask) { return failure("semantic source is outside its frame range or timeline"); }
    if (mask->pixels.pixels.size() > maxPixels) { return failure("semantic output exceeds maxPixels"); }
    SemanticControlMapResult result;
    result.pixels = makeRasterLayer(mask->pixels.width, mask->pixels.height, layer->segmentation.voidControlColor);
    result.classIds.resize(mask->pixels.pixels.size());
    result.regionIds.resize(mask->pixels.pixels.size());
    result.validPixels.resize(mask->pixels.pixels.size());
    std::unordered_map<std::uint32_t, const SemanticClass *> classes;
    for (const auto &entry : layer->segmentation.taxonomy.classes) { classes.emplace(entry.id, &entry); }
    std::unordered_map<std::uint32_t, std::size_t> colors;
    for (std::size_t i = 0; i < layer->segmentation.regions.size(); ++i) {
        const auto &region = layer->segmentation.regions[i];
        colors.emplace(region.maskColor, i);
        result.regions.push_back({region.id, region.classId});
    }
    for (std::size_t i = 0; i < mask->pixels.pixels.size(); ++i) {
        const auto color = mask->pixels.pixels[i];
        if (color == layer->segmentation.voidMaskColor) { continue; }
        const auto index = colors.at(color);
        const auto &region = layer->segmentation.regions[index];
        result.pixels.pixels[i] = classes.at(region.classId)->controlColor;
        result.classIds[i] = region.classId; result.regionIds[i] = region.id; result.validPixels[i] = 1;
        auto &geometry = result.regions[index];
        const auto x = static_cast<std::int32_t>(i % mask->pixels.width);
        const auto y = static_cast<std::int32_t>(i / mask->pixels.width);
        if (geometry.pixelCount == 0) { geometry.bounds = {x,y,1,1}; }
        else {
            const auto right = std::max(geometry.bounds.x + geometry.bounds.width, x + 1);
            const auto bottom = std::max(geometry.bounds.y + geometry.bounds.height, y + 1);
            geometry.bounds.x = std::min(geometry.bounds.x,x); geometry.bounds.y = std::min(geometry.bounds.y,y);
            geometry.bounds.width = right - geometry.bounds.x; geometry.bounds.height = bottom - geometry.bounds.y;
        }
        ++geometry.pixelCount; geometry.centroid.x += x + 0.5; geometry.centroid.y += y + 0.5;
    }
    for (auto &geometry : result.regions) {
        if (geometry.pixelCount) { geometry.centroid.x /= geometry.pixelCount; geometry.centroid.y /= geometry.pixelCount; }
    }
    return result;
}

} // namespace iiSharedCanvas
