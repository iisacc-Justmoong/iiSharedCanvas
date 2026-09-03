#pragma once

#include "Document/Document.h"
#include "Export.h"

#include <cstddef>
#include <cstdint>
#include <functional>
#include <limits>
#include <string>
#include <vector>

namespace iiSharedCanvas {

class DocumentFile;

inline constexpr std::size_t AppendDocumentIndex =
    std::numeric_limits<std::size_t>::max();

enum class DocumentEditCode {
    None,
    NotBound,
    InvalidDocument,
    InvalidArgument,
    DuplicateAssetId,
    AssetNotFound,
    AssetKindMismatch,
    AssetReferenced,
    DuplicateLayerId,
    LayerNotFound,
    IndexOutOfRange,
    SourceNotKeyframed,
    KeyframeNotFound,
    DuplicateKeyframe,
    ValidationRejected,
    PersistenceFailed,
};

struct DocumentEditResult {
    DocumentEditCode code = DocumentEditCode::None;
    bool changed = false;
    std::string path;
    std::string message;

    [[nodiscard]] bool ok() const noexcept
    {
        return code == DocumentEditCode::None;
    }
};

struct KeyframePlacement {
    FrameIndex frame = 0;
    std::string assetId;
};

class IISHAREDCANVAS_EXPORT DocumentEditor final {
public:
    DocumentEditor() = default;
    explicit DocumentEditor(Document &document);
    explicit DocumentEditor(DocumentFile &file);

    DocumentEditResult bind(Document &document);
    DocumentEditResult bind(DocumentFile &file);
    void unbind() noexcept;
    [[nodiscard]] bool isBound() const noexcept;
    [[nodiscard]] Document *document() noexcept;
    [[nodiscard]] const Document *document() const noexcept;
    [[nodiscard]] std::uint64_t revision() const noexcept;
    [[nodiscard]] const DocumentEditResult &lastResult() const noexcept;

    DocumentEditResult setCanvasExtent(CanvasExtent extent);
    DocumentEditResult ensureInfiniteCanvasRegion(CanvasRegion region);
    DocumentEditResult setFrameRate(FrameRate frameRate);
    DocumentEditResult setFrameCount(FrameIndex frameCount);
    DocumentEditResult setStableDiffusionMetadata(StableDiffusionMetadata metadata);
    DocumentEditResult clearStableDiffusionMetadata();

    DocumentEditResult insertRasterAsset(std::string id,
                                         RasterLayer pixels,
                                         std::size_t index = AppendDocumentIndex);
    DocumentEditResult insertVectorAsset(std::string id,
                                         CanvasExtent viewport,
                                         std::vector<VectorPath> paths,
                                         std::size_t index = AppendDocumentIndex);
    DocumentEditResult replaceRasterPixels(const std::string &assetId,
                                           RasterLayer pixels);
    DocumentEditResult replaceVectorData(const std::string &assetId,
                                         CanvasExtent viewport,
                                         std::vector<VectorPath> paths);
    DocumentEditResult renameAsset(const std::string &assetId,
                                   std::string replacementId);
    DocumentEditResult moveAsset(const std::string &assetId,
                                 std::size_t destinationIndex);
    DocumentEditResult removeAsset(const std::string &assetId);

    DocumentEditResult insertLayer(Layer layer,
                                   std::size_t index = AppendDocumentIndex);
    DocumentEditResult insertKeyframedLayer(
        Layer layer,
        std::vector<KeyframePlacement> keyframes,
        std::size_t index = AppendDocumentIndex);
    DocumentEditResult replaceLayer(const std::string &layerId, Layer layer);
    DocumentEditResult renameLayer(const std::string &layerId,
                                   std::string replacementId);
    DocumentEditResult setLayerName(const std::string &layerId, std::string name);
    DocumentEditResult setLayerVisible(const std::string &layerId, bool visible);
    DocumentEditResult setLayerOpacity(const std::string &layerId, double opacity);
    DocumentEditResult setLayerTransform(const std::string &layerId,
                                         AffineTransform transform);
    DocumentEditResult setLayerBlendMode(const std::string &layerId,
                                         RasterBlendMode blendMode);
    DocumentEditResult setLayerFrameRange(
        const std::string &layerId,
        std::optional<LayerFrameRange> frameRange);
    DocumentEditResult setStaticSource(const std::string &layerId,
                                       std::string assetId);
    DocumentEditResult setKeyframedSource(const std::string &layerId,
                                          std::vector<KeyframePlacement> keyframes);
    DocumentEditResult moveLayer(const std::string &layerId,
                                 std::size_t destinationIndex);
    DocumentEditResult removeLayer(const std::string &layerId);

    DocumentEditResult insertKeyframe(const std::string &layerId,
                                      FrameIndex frame,
                                      std::string assetId);
    DocumentEditResult setKeyframeAsset(const std::string &layerId,
                                        FrameIndex frame,
                                        std::string assetId);
    DocumentEditResult moveKeyframe(const std::string &layerId,
                                    FrameIndex frame,
                                    FrameIndex destinationFrame);
    DocumentEditResult removeKeyframe(const std::string &layerId,
                                      FrameIndex frame);

    DocumentEditResult insertVectorPath(const std::string &assetId,
                                        VectorPath path,
                                        std::size_t index = AppendDocumentIndex);
    DocumentEditResult replaceVectorPath(const std::string &assetId,
                                         std::size_t index,
                                         VectorPath path);
    DocumentEditResult moveVectorPath(const std::string &assetId,
                                      std::size_t index,
                                      std::size_t destinationIndex);
    DocumentEditResult removeVectorPath(const std::string &assetId,
                                        std::size_t index);

private:
    DocumentEditResult editFile(const std::function<DocumentEditResult(DocumentEditor &)> &edit);
    [[nodiscard]] DocumentEditResult reject(DocumentEditCode code,
                                            std::string path,
                                            std::string message);
    [[nodiscard]] DocumentEditResult unchanged();
    [[nodiscard]] DocumentEditResult applied();
    [[nodiscard]] bool requireValidDocument();

    Document *m_document = nullptr;
    DocumentFile *m_file = nullptr;
    std::uint64_t m_fileGeneration = 0;
    std::uint64_t m_revision = 0;
    DocumentEditResult m_lastResult;
};

} // namespace iiSharedCanvas
