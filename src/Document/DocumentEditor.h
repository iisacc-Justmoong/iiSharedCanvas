#pragma once

#include "Document/Document.h"
#include "ControlNet/ControlNetParameters.h"
#include "iiSharedCanvas/Export.h"

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
    DuplicateAudioClipId,
    AudioClipNotFound,
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

    DocumentEditResult setFileAuthor(const iiFileProvider::FileAuthor &author);
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

    DocumentEditResult insertVideoAsset(VideoAsset asset, std::size_t index = AppendDocumentIndex);
    DocumentEditResult replaceVideoAsset(const std::string &assetId, VideoAsset asset);
    DocumentEditResult setVideoPlayback(const std::string &layerId, VideoPlayback playback);
    DocumentEditResult setLayerMotion(const std::string &layerId, std::vector<MotionKeyframe> keyframes);

    DocumentEditResult insertAudioAsset(AudioAsset asset,
                                       std::size_t index = AppendDocumentIndex);
    // Replacement preserves the stable id. Remove rejects any referencing clip.
    DocumentEditResult replaceAudioAsset(const std::string &assetId, AudioAsset asset);
    DocumentEditResult removeAudioAsset(const std::string &assetId);
    DocumentEditResult insertAudioTrack(AudioTrackLayer track,
                                       std::size_t index = AppendDocumentIndex);
    DocumentEditResult replaceAudioTrack(const std::string &trackId, AudioTrackLayer track);
    DocumentEditResult moveAudioTrack(const std::string &trackId, std::size_t destinationIndex);
    DocumentEditResult removeAudioTrack(const std::string &trackId);
    // Insert/replace canonicalize clip start order; overlapping clips are rejected.
    DocumentEditResult insertAudioClip(const std::string &trackId, AudioClip clip);
    DocumentEditResult replaceAudioClip(const std::string &trackId,
                                       const std::string &clipId, AudioClip clip);
    DocumentEditResult removeAudioClip(const std::string &trackId, const std::string &clipId);

    // Dedicated ControlNet content uses validated identity masks and semantic definitions.
    DocumentEditResult insertMlsdAsset(MlsdAsset asset, std::size_t index = AppendDocumentIndex);
    DocumentEditResult replaceMlsdAsset(const std::string &assetId, MlsdAsset asset);
    DocumentEditResult insertMlsdLayer(MlsdLayer layer, std::vector<KeyframePlacement> keyframes = {}, std::size_t index = AppendDocumentIndex);
    DocumentEditResult setMlsdSegment(const std::string &assetId, const std::string &segmentId, MlsdSegment segment);
    DocumentEditResult insertCannyAsset(CannyAsset asset, std::size_t index = AppendDocumentIndex);
    DocumentEditResult replaceCannyAsset(const std::string &assetId, CannyAsset asset);
    DocumentEditResult insertCannyLayer(CannyLayer layer, std::vector<KeyframePlacement> keyframes = {}, std::size_t index = AppendDocumentIndex);
    DocumentEditResult setCannySample(const std::string &assetId, std::int32_t x, std::int32_t y, std::uint8_t value);
    DocumentEditResult insertScribbleAsset(ScribbleAsset asset, std::size_t index = AppendDocumentIndex);
    DocumentEditResult replaceScribbleAsset(const std::string &assetId, ScribbleAsset asset);
    DocumentEditResult insertScribbleLayer(ScribbleLayer layer, std::vector<KeyframePlacement> keyframes = {}, std::size_t index = AppendDocumentIndex);
    DocumentEditResult setScribbleSample(const std::string &assetId, std::int32_t x, std::int32_t y, std::uint8_t value);
    DocumentEditResult insertLineArtAsset(LineArtAsset asset, std::size_t index = AppendDocumentIndex);
    DocumentEditResult replaceLineArtAsset(const std::string &assetId, LineArtAsset asset);
    DocumentEditResult insertLineArtLayer(LineArtLayer layer, std::vector<KeyframePlacement> keyframes = {}, std::size_t index = AppendDocumentIndex);
    DocumentEditResult setLineArtSample(const std::string &assetId, std::int32_t x, std::int32_t y, double value);
    DocumentEditResult insertNormalMapAsset(NormalMapAsset asset, std::size_t index = AppendDocumentIndex);
    DocumentEditResult replaceNormalMapAsset(const std::string &assetId, NormalMapAsset asset);
    DocumentEditResult insertNormalMapLayer(NormalMapLayer layer, std::vector<KeyframePlacement> keyframes = {}, std::size_t index = AppendDocumentIndex);
    DocumentEditResult setNormalMapSample(const std::string &assetId, std::int32_t x, std::int32_t y, NormalMapSample sample);
    DocumentEditResult insertShuffleAsset(ShuffleAsset asset, std::size_t index = AppendDocumentIndex);
    DocumentEditResult replaceShuffleAsset(const std::string &assetId, ShuffleAsset asset);
    DocumentEditResult insertShuffleLayer(ShuffleLayer layer, std::vector<KeyframePlacement> keyframes = {}, std::size_t index = AppendDocumentIndex);
    DocumentEditResult setShuffleSample(const std::string &assetId, std::int32_t x, std::int32_t y, ShuffleColor sample);
    DocumentEditResult insertTileAsset(TileAsset asset, std::size_t index = AppendDocumentIndex);
    DocumentEditResult replaceTileAsset(const std::string &assetId, TileAsset asset);
    DocumentEditResult insertTileLayer(TileLayer layer, std::vector<KeyframePlacement> keyframes = {}, std::size_t index = AppendDocumentIndex);
    DocumentEditResult setTileSample(const std::string &assetId, std::int32_t x, std::int32_t y, TileColor sample);
    DocumentEditResult setReferenceSettings(const std::string &layerId, ReferenceSettings settings);
    DocumentEditResult insertReferenceAsset(ReferenceAsset asset, std::size_t index = AppendDocumentIndex);
    DocumentEditResult replaceReferenceAsset(const std::string &assetId, ReferenceAsset asset);
    DocumentEditResult insertReferenceLayer(ReferenceLayer layer, std::vector<KeyframePlacement> keyframes = {}, std::size_t index = AppendDocumentIndex);
    DocumentEditResult setReferenceSample(const std::string &assetId, std::int32_t x, std::int32_t y, ReferenceColor sample);
    DocumentEditResult insertIpAdapterAsset(IpAdapterAsset asset, std::size_t index = AppendDocumentIndex);
    DocumentEditResult replaceIpAdapterAsset(const std::string &assetId, IpAdapterAsset asset);
    DocumentEditResult insertIpAdapterLayer(IpAdapterLayer layer, std::vector<KeyframePlacement> keyframes = {}, std::size_t index = AppendDocumentIndex);
    DocumentEditResult setIpAdapterValue(const std::string &assetId, IpAdapterBranch branch,
        std::uint32_t token, std::uint32_t channel, float value);
    DocumentEditResult insertDepthAsset(DepthAsset asset, std::size_t index = AppendDocumentIndex);
    DocumentEditResult replaceDepthAsset(const std::string &assetId, DepthAsset asset);
    DocumentEditResult insertDepthLayer(DepthLayer layer, std::vector<KeyframePlacement> keyframes = {}, std::size_t index = AppendDocumentIndex);
    DocumentEditResult setDepthSample(const std::string &assetId, std::int32_t x, std::int32_t y, double value);
    DocumentEditResult insertPoseAsset(PoseAsset asset, std::size_t index = AppendDocumentIndex);
    DocumentEditResult replacePoseAsset(const std::string &assetId, PoseAsset asset);
    DocumentEditResult insertPoseLayer(PoseLayer layer, std::vector<KeyframePlacement> keyframes = {}, std::size_t index = AppendDocumentIndex);
    DocumentEditResult setPoseAnchor(const std::string &assetId, const std::string &personId,
        PoseGroup group, std::uint32_t index, PoseAnchor anchor);
    DocumentEditResult setPoseExpressionWeight(const std::string &assetId, const std::string &personId,
        const std::string &expressionId, double weight);

    DocumentEditResult insertSemanticSegmentLayer(
        SemanticSegmentLayer layer, std::vector<KeyframePlacement> keyframes = {},
        std::size_t index = AppendDocumentIndex);
    DocumentEditResult setSemanticSegmentation(const std::string &layerId, SemanticSegmentation segmentation);
    [[nodiscard]] ControlNetParametersResult controlNetParameters(const std::string &layerId) const;
    // Atomically replace detailed parameters for the layer and every source state.
    // Stable ids, concrete types and source references must remain unchanged.
    // Full replacement: reacquire a fresh snapshot after other edits. Shared assets
    // are changed for every owner; the complete document must remain valid.
    DocumentEditResult setControlNetParameters(const std::string &layerId, ControlNetParameters parameters);
    DocumentEditResult patchControlNetSettings(const std::string &layerId, ControlNetSettingsPatch patch);
    DocumentEditResult setControlNetSettings(const std::string &layerId, ControlNetSettings settings);

    // Static keeps one content asset; dynamic uses frame-zero-first hold sampling.
    DocumentEditResult insertStaticLayer(
        LayerProperties properties, LayerRepresentation representation,
        std::string assetId, std::size_t index = AppendDocumentIndex);
    DocumentEditResult insertDynamicLayer(
        LayerProperties properties, LayerRepresentation representation,
        std::vector<KeyframePlacement> keyframes,
        std::size_t index = AppendDocumentIndex);

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
    [[nodiscard]] DocumentEditResult applied(bool recordAuthorship = true);
    [[nodiscard]] bool requireValidDocument();

    Document *m_document = nullptr;
    DocumentFile *m_file = nullptr;
    std::uint64_t m_fileGeneration = 0;
    std::uint64_t m_revision = 0;
    DocumentEditResult m_lastResult;
};

} // namespace iiSharedCanvas
