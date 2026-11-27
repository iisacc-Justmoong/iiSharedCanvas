#include "Document/ArtboardEditor_p.hpp"
#include "Document/CanvasAffine_p.hpp"
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
AffineTransform boardTransform(const Artboard *board) {
    if (!board) return {};
    if (board->geometry) return board->geometry->transform;
    AffineTransform t; t.translationX=board->region.origin.x; t.translationY=board->region.origin.y; return t;
}
AffineTransform compose(const AffineTransform &a,const AffineTransform &b) {
    return {a.m11*b.m11+a.m21*b.m12,a.m12*b.m11+a.m22*b.m12,
            a.m11*b.m21+a.m21*b.m22,a.m12*b.m21+a.m22*b.m22,
            a.m11*b.translationX+a.m21*b.translationY+a.translationX,
            a.m12*b.translationX+a.m22*b.translationY+a.translationY};
}
AffineTransform inverse(const AffineTransform &t) {
    const auto d=t.m11*t.m22-t.m21*t.m12;
    AffineTransform result{t.m22/d,-t.m12/d,-t.m21/d,t.m11/d};
    result.translationX=-result.m11*t.translationX-result.m21*t.translationY;
    result.translationY=-result.m12*t.translationX-result.m22*t.translationY;
    return result;
}
// Rebase independent masks as well as the layer: linked masks already use the
// layer's local coordinates and therefore follow the rebased layer transform.
void rebase(LayerProperties &p,const AffineTransform &map) {
    p.transform=compose(map,p.transform);
    const auto shift=[&](LayerMask &mask) { if (!mask.linkedToLayer) mask.transform=compose(map,mask.transform); };
    if (p.appearance.rasterMask) shift(*p.appearance.rasterMask);
    if (p.appearance.vectorMask) shift(*p.appearance.vectorMask);
    if (p.appearance.filterMask) shift(*p.appearance.filterMask);
    for (auto &entry:p.appearance.maskStack) shift(entry.mask);
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
    auto priorRelations = draft.relations;
    auto priorPublication = draft.publicationMetadata;
    const auto priorVersion = draft.formatVersion;
    const auto priorAssetCount = draft.assets.size();
    const auto restore = [&]() {
        draft.artboards = std::move(priorArtboards);
        draft.layers = std::move(priorLayers);
        draft.frames = std::move(priorFrames);
        draft.relations = std::move(priorRelations);
        draft.publicationMetadata = std::move(priorPublication);
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
        if (a->geometry) return DocumentEditResult{DocumentEditCode::InvalidArgument,false,"artboards",
            "an oriented artboard requires an explicit geometry edit"};
        a->region = region; return changed();
    });
}

DocumentEditResult DocumentEditor::setArtboardGeometry(const std::string &id,std::optional<ArtboardGeometry> geometry)
{
    return editArtboards([&](Document &d) {
        auto *board=findArtboard(d,id); if (!board) return missing();
        if (board->geometry==geometry) return DocumentEditResult{};
        for (const auto &layer:d.layers) {
            const auto &p=layerProperties(layer);
            if (p.artboardId==id && hasLayerLock(d.relations.locks.effective(d,p.id),LayerLock::Transform))
                return DocumentEditResult{DocumentEditCode::LayerLocked,false,"artboards","an owned layer transform is locked"};
        }
        if (geometry) {
            const auto bounds=canvas_affine_detail::bounds({{},geometry->extent},geometry->transform);
            if (!bounds) return DocumentEditResult{DocumentEditCode::InvalidArgument,false,"artboards","invalid oriented geometry"};
            board->region=*bounds;
        }
        board->geometry=std::move(geometry); return changed();
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
        const auto lock=d.relations.locks.effective(d,layerId);
        if (hasLayerLock(lock,LayerLock::Structure) || hasLayerLock(lock,LayerLock::Transform))
            return DocumentEditResult{DocumentEditCode::LayerLocked,false,"layers","layer ownership or transform is locked"};
        const auto *prior = p.artboardId ? findArtboard(d, *p.artboardId) : nullptr;
        rebase(p,compose(inverse(boardTransform(target)),boardTransform(prior)));
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
        for (const auto &layer:d.layers) {
            const auto &p=layerProperties(layer);
            if (p.artboardId==id && !d.relations.groups.groupOf(p.id).empty())
                return DocumentEditResult{DocumentEditCode::ValidationRejected,false,"artboards",
                    "duplicating grouped artboards is not supported"};
        }
        const auto position = index == AppendDocumentIndex ? d.artboards.size() : index;
        if (position > d.artboards.size()) return badIndex();
        Artboard copy = *source;
        if (copy.geometry) {
            copy.geometry->transform.translationX+=double(origin.x)-source->region.origin.x;
            copy.geometry->transform.translationY+=double(origin.y)-source->region.origin.y;
        }
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
        const auto toWorld = boardTransform(a);
        for (const auto &layer:d.layers) {
            const auto &p=layerProperties(layer); if (p.artboardId!=id) continue;
            const auto lock=d.relations.locks.effective(d,p.id);
            if (hasLayerLock(lock,LayerLock::Structure) ||
                (removal==ArtboardRemoval::KeepLayers && hasLayerLock(lock,LayerLock::Transform)))
                return DocumentEditResult{DocumentEditCode::LayerLocked,false,"layers","an affected layer is locked"};
        }
        if (removal == ArtboardRemoval::KeepLayers) {
            for (auto &layer : d.layers) {
                auto &p = layerProperties(layer);
                if (p.artboardId != id) continue;
                rebase(p,toWorld);
                p.artboardId.reset();
            }
        } else {
            for (const auto &layer:d.layers)
                if (layerProperties(layer).artboardId==id) d.relations.removeLayer(layerProperties(layer).id);
            std::erase_if(d.layers, [&](const Layer &layer) { return layerProperties(layer).artboardId == id; });
            for (auto &frame : d.frames)
                std::erase_if(frame.keyframes, [&](const Keyframe &key) { return !findLayer(d, key.layerId); });
            std::erase_if(d.frames, [](const Frame &frame) { return frame.keyframes.empty(); });
        }
        std::erase_if(d.artboards, [&](const Artboard &value) { return value.id == id; });
        if (d.publicationMetadata) {
            auto &metadata = *d.publicationMetadata;
            std::erase_if(metadata.canvases,[&](const auto &canvas) { return canvas.artboardId == id; });
            if (metadata.coverArtboardId == id)
                metadata.coverArtboardId = metadata.canvases.empty() ? std::string{} : metadata.canvases.front().artboardId;
        }
        return changed();
    });
}
} // namespace iiSharedCanvas
