#include "Document/ArtboardEditor_p.hpp"
#include "Validation/Validation.h"

#include <algorithm>
#include <unordered_map>
#include <utility>

namespace iiSharedCanvas {
namespace artboard_detail {
DocumentEditResult changed() { return {DocumentEditCode::None, true, {}, {}}; }
DocumentEditResult missing() {
    return {DocumentEditCode::ArtboardNotFound, false, "artboards", "artboard id was not found"};
}
DocumentEditResult duplicate() {
    return {DocumentEditCode::DuplicateArtboardId, false, "artboards", "artboard id already exists"};
}
DocumentEditResult badIndex() {
    return {DocumentEditCode::IndexOutOfRange, false, "artboards", "artboard index is outside the collection"};
}
}

using namespace artboard_detail;

DocumentEditResult DocumentEditor::editArtboards(const std::function<DocumentEditResult(Document &)> &edit)
{
    if (m_file) return editFile([&](DocumentEditor &working) { return working.editArtboards(edit); });
    if (!requireValidDocument()) return m_lastResult;
    // These operations edit ownership/geometry/keyframes and only append sources.
    // Preserve rollback without copying every existing raster/video/audio payload.
    Document &draft = *m_document;
    auto priorArtboards = draft.artboards;
    auto priorLayers = draft.layers;
    auto priorFrames = draft.frames;
    const auto priorVersion = draft.formatVersion;
    const auto priorAssetCount = draft.assets.size();
    const auto restore = [&]() {
        draft.artboards = std::move(priorArtboards);
        draft.layers = std::move(priorLayers);
        draft.frames = std::move(priorFrames);
        draft.formatVersion = priorVersion;
        draft.assets.resize(priorAssetCount);
    };
    const auto result = edit(draft);
    if (!result.ok()) { restore(); return reject(result.code, result.path, result.message); }
    if (!result.changed) return unchanged();
    draft.formatVersion.minor = CurrentFormatMinor;
    const auto validation = validate(draft);
    if (!validation.ok()) {
        const auto &issue = validation.issues.front();
        restore();
        return reject(DocumentEditCode::ValidationRejected, issue.path, issue.message);
    }
    return applied();
}

DocumentEditResult DocumentEditor::insertArtboard(Artboard artboard, std::size_t index)
{
    return editArtboards([&](Document &d) {
        if (findArtboard(d, artboard.id)) return duplicate();
        const auto position = index == AppendDocumentIndex ? d.artboards.size() : index;
        if (position > d.artboards.size()) return badIndex();
        d.artboards.insert(d.artboards.begin() + std::ptrdiff_t(position), std::move(artboard));
        return changed();
    });
}

DocumentEditResult DocumentEditor::setArtboardName(const std::string &id, std::string name)
{
    return editArtboards([&](Document &d) {
        auto *a = findArtboard(d, id); if (!a) return missing();
        if (a->name == name) return DocumentEditResult{};
        a->name = std::move(name); return changed();
    });
}

DocumentEditResult DocumentEditor::setArtboardRegion(const std::string &id, CanvasRegion region)
{
    return editArtboards([&](Document &d) {
        auto *a = findArtboard(d, id); if (!a) return missing();
        if (a->region == region) return DocumentEditResult{};
        a->region = region; return changed();
    });
}

DocumentEditResult DocumentEditor::setArtboardBackground(const std::string &id, std::uint32_t argb)
{
    return editArtboards([&](Document &d) {
        auto *a = findArtboard(d, id); if (!a) return missing();
        if (a->backgroundArgb == argb) return DocumentEditResult{};
        a->backgroundArgb = argb; return changed();
    });
}

DocumentEditResult DocumentEditor::setArtboardVisible(const std::string &id, bool visible)
{
    return editArtboards([&](Document &d) {
        auto *a = findArtboard(d, id); if (!a) return missing();
        if (a->visible == visible) return DocumentEditResult{};
        a->visible = visible; return changed();
    });
}

DocumentEditResult DocumentEditor::moveArtboard(const std::string &id, std::size_t destinationIndex)
{
    return editArtboards([&](Document &d) {
        const auto *a = findArtboard(d, id); if (!a) return missing();
        if (destinationIndex >= d.artboards.size()) return badIndex();
        const auto index = std::size_t(a - d.artboards.data());
        if (index == destinationIndex) return DocumentEditResult{};
        Artboard value = std::move(d.artboards[index]);
        d.artboards.erase(d.artboards.begin() + std::ptrdiff_t(index));
        d.artboards.insert(d.artboards.begin() + std::ptrdiff_t(destinationIndex), std::move(value));
        return changed();
    });
}

DocumentEditResult DocumentEditor::setLayerArtboard(const std::string &layerId,
                                                    std::optional<std::string> artboardId)
{
    return editArtboards([&](Document &d) {
        auto *layer = findLayer(d, layerId);
        if (!layer) return DocumentEditResult{DocumentEditCode::LayerNotFound, false, "layers", "layer id was not found"};
        const auto *target = artboardId ? findArtboard(d, *artboardId) : nullptr;
        if (artboardId && !target) return missing();
        auto &p = layerProperties(*layer);
        if (p.artboardId == artboardId) return DocumentEditResult{};
        const auto *prior = p.artboardId ? findArtboard(d, *p.artboardId) : nullptr;
        p.transform.translationX += double(prior ? prior->region.origin.x : 0) - double(target ? target->region.origin.x : 0);
        p.transform.translationY += double(prior ? prior->region.origin.y : 0) - double(target ? target->region.origin.y : 0);
        p.artboardId = std::move(artboardId);
        return changed();
    });
}

DocumentEditResult DocumentEditor::duplicateArtboard(const std::string &id, std::string replacementId,
                                                      CanvasOrigin origin, std::size_t index)
{
    return editArtboards([&](Document &d) {
        const auto *source = findArtboard(d, id); if (!source) return missing();
        if (findArtboard(d, replacementId)) return duplicate();
        const auto position = index == AppendDocumentIndex ? d.artboards.size() : index;
        if (position > d.artboards.size()) return badIndex();
        Artboard copy = *source;
        copy.id = replacementId; copy.region.origin = origin;
        std::unordered_map<std::string, std::string> ids;
        std::unordered_map<std::string, std::string> assetIds;
        // Clone each referenced source once, preserving sharing within the new board.
        for (const auto &layer : d.layers) {
            if (layerProperties(layer).artboardId != id) continue;
            const auto add = [&](const std::string &assetId) {
                assetIds.emplace(assetId, replacementId + ":asset:" + assetId);
            };
            if (const auto *staticSource = staticLayerSource(layer)) add(staticSource->assetId);
            else for (const auto &frame : d.frames)
                if (const auto *key = findKeyframe(frame, layerProperties(layer).id)) add(key->assetId);
        }
        const auto assetCount = d.assets.size();
        for (std::size_t i = 0; i < assetCount; ++i) {
            const auto found = assetIds.find(assetId(d.assets[i]));
            if (found == assetIds.end()) continue;
            if (findAsset(d, found->second))
                return DocumentEditResult{DocumentEditCode::DuplicateAssetId, false, "assets", "duplicated asset id already exists"};
            Asset clone = d.assets[i];
            std::visit([&](auto &value) { value.id = found->second; }, clone);
            d.assets.push_back(std::move(clone));
        }
        const auto layerCount = d.layers.size();
        for (std::size_t i = 0; i < layerCount; ++i) {
            const auto &p = layerProperties(d.layers[i]);
            if (p.artboardId != id) continue;
            const auto newId = replacementId + ":" + p.id;
            if (findLayer(d, newId) || findAudioTrack(d, newId))
                return DocumentEditResult{DocumentEditCode::DuplicateLayerId, false, "layers", "duplicated layer id already exists"};
            ids.emplace(p.id, newId);
            Layer clone = d.layers[i];
            layerProperties(clone).id = newId;
            layerProperties(clone).artboardId = replacementId;
            if (auto *staticSource = staticLayerSource(clone))
                staticSource->assetId = assetIds.at(staticSource->assetId);
            d.layers.push_back(std::move(clone));
        }
        for (auto &frame : d.frames) {
            const auto keyCount = frame.keyframes.size();
            for (std::size_t i = 0; i < keyCount; ++i) {
                const auto found = ids.find(frame.keyframes[i].layerId);
                if (found != ids.end()) frame.keyframes.push_back({found->second, assetIds.at(frame.keyframes[i].assetId)});
            }
            std::sort(frame.keyframes.begin(), frame.keyframes.end(),
                      [](const Keyframe &a, const Keyframe &b) { return a.layerId < b.layerId; });
        }
        d.artboards.insert(d.artboards.begin() + std::ptrdiff_t(position), std::move(copy));
        return changed();
    });
}

DocumentEditResult DocumentEditor::removeArtboard(const std::string &id, ArtboardRemoval removal)
{
    return editArtboards([&](Document &d) {
        const auto *a = findArtboard(d, id); if (!a) return missing();
        if (removal != ArtboardRemoval::KeepLayers && removal != ArtboardRemoval::DeleteLayers)
            return DocumentEditResult{DocumentEditCode::InvalidArgument, false, "artboards", "unknown removal policy"};
        const auto origin = a->region.origin;
        if (removal == ArtboardRemoval::KeepLayers) {
            for (auto &layer : d.layers) {
                auto &p = layerProperties(layer);
                if (p.artboardId != id) continue;
                p.transform.translationX += origin.x; p.transform.translationY += origin.y;
                p.artboardId.reset();
            }
        } else {
            std::erase_if(d.layers, [&](const Layer &layer) { return layerProperties(layer).artboardId == id; });
            for (auto &frame : d.frames)
                std::erase_if(frame.keyframes, [&](const Keyframe &key) { return !findLayer(d, key.layerId); });
            std::erase_if(d.frames, [](const Frame &frame) { return frame.keyframes.empty(); });
        }
        std::erase_if(d.artboards, [&](const Artboard &value) { return value.id == id; });
        return changed();
    });
}
} // namespace iiSharedCanvas
