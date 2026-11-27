#pragma once
#include "Document/DocumentHistory.h"
#include <optional>

namespace iiSharedCanvas {
class DocumentFile;
// Small owned summaries deliberately exclude raster and embedded document data.
// An asset's stable identifier and public content kind, with no payload copy.
struct DocumentAssetInfo {
    std::string id;
    ContentKind kind;
    // Present only for an owned linked canvas; no document bytes are copied.
    std::optional<FrameIndex> linkedFrame;
    std::size_t linkedBytes = 0;
};
// A detached summary in document layer order; it contains no borrowed views.
struct DocumentLayerInfo { std::string id, name; bool visible; double opacity; };
// Values remain valid independently of later edits and owner destruction.
struct DocumentInfoSnapshot {
    FormatVersion format;
    CanvasExtent extent;
    CanvasMode canvasMode;
    Timeline timeline;
    DocumentPixelAspectRatio pixelAspectRatio;
    std::uint64_t authorshipRevision = 0;
    std::string authorship;
    std::optional<StableDiffusionMetadata> stableDiffusionMetadata;
    std::optional<PublicationMetadata> publicationMetadata;
    std::optional<std::uint64_t> storageRevision;
    std::optional<std::uint32_t> storageVersion;
    std::optional<std::string> filePath;
    std::optional<DocumentHistoryStatistics> history;
    std::size_t assetCount = 0, layerCount = 0;
    std::vector<DocumentAssetInfo> assets;
    std::vector<DocumentLayerInfo> layers;
};

// Collects detached values without changing the source, owner or history.
class IISHAREDCANVAS_EXPORT DocumentInfoSnapshotBuilder final {
public:
    // A history bound to a different document is ignored rather than misreported.
    [[nodiscard]] static DocumentInfoSnapshot build(const Document &, const DocumentHistory *history = nullptr);
    // Returns nullopt for a closed file. Revisions describe separate domains.
    [[nodiscard]] static std::optional<DocumentInfoSnapshot> build(const DocumentFile &, const DocumentHistory *history = nullptr);
};
} // namespace iiSharedCanvas
