#include "Document/DocumentInfoSnapshot.h"
#include "File/DocumentFile.h"

namespace iiSharedCanvas {
// Copy only small metadata and IDs; neither raster payloads nor owned child
// containers are traversed or shared with the returned value.
DocumentInfoSnapshot DocumentInfoSnapshotBuilder::build(const Document &document, const DocumentHistory *history)
{
    DocumentInfoSnapshot out;
    out.format = document.formatVersion; out.extent = document.extent;
    out.canvasMode = document.canvasMode; out.timeline = document.timeline;
    out.pixelAspectRatio = document.pixelAspectRatio;
    out.authorshipRevision = document.authorship.revision();
    out.authorship = authorshipDump(document.authorship);
    out.stableDiffusionMetadata = document.stableDiffusionMetadata;
    out.publicationMetadata = document.publicationMetadata;
    if (history && history->document() == &document) out.history = history->statistics();
    out.assetCount = document.assets.size(); out.layerCount = document.layers.size();
    out.assets.reserve(out.assetCount); out.layers.reserve(out.layerCount);
    for (const auto &asset : document.assets) {
        DocumentAssetInfo info{assetId(asset), contentKind(asset)};
        if (const auto *linked = std::get_if<LinkedCanvasAsset>(&asset)) {
            info.linkedFrame = linked->frame; info.linkedBytes = linked->document.size();
        }
        out.assets.push_back(std::move(info));
    }
    for (const auto &layer : document.layers) {
        const auto &p = layerProperties(layer);
        out.layers.push_back({p.id, p.name, p.visible, p.opacity});
    }
    return out;
}

// Storage revision belongs to the file, while authorship belongs to the model.
std::optional<DocumentInfoSnapshot> DocumentInfoSnapshotBuilder::build(const DocumentFile &file, const DocumentHistory *history)
{
    if (!file.isOpen() || !file.document()) return std::nullopt;
    auto out = build(*file.document(), history);
    out.storageRevision = file.revision();
    out.storageVersion = file.storageVersion(); out.filePath = file.filePath();
    return out;
}
} // namespace iiSharedCanvas
