#include "Document/DocumentEditor.h"
#include "File/DocumentFile.h"

#include "Validation/Validation.h"

#include <algorithm>
#include <optional>
#include <type_traits>
#include <utility>

namespace iiSharedCanvas {
namespace {

std::optional<ValidationIssue> firstValidationIssue(const Document &document)
{
    ValidationResult result = validate(document);
    if (result.ok()) {
        return std::nullopt;
    }
    return std::move(result.issues.front());
}

std::size_t insertionIndex(std::size_t requested, std::size_t size) noexcept
{
    return requested == AppendDocumentIndex ? size : requested;
}

std::int64_t floorToMultiple(std::int64_t value, std::int64_t step) noexcept
{
    const std::int64_t remainder = value % step;
    return remainder < 0 ? value - remainder - step : value - remainder;
}

std::int64_t ceilToMultiple(std::int64_t value, std::int64_t step) noexcept
{
    const std::int64_t remainder = value % step;
    if (remainder == 0) {
        return value;
    }
    return remainder > 0 ? value - remainder + step : value - remainder;
}

template<typename Value>
void moveElement(std::vector<Value> &values,
                 std::size_t sourceIndex,
                 std::size_t destinationIndex)
{
    if (sourceIndex < destinationIndex) {
        std::rotate(values.begin() + static_cast<std::ptrdiff_t>(sourceIndex),
                    values.begin() + static_cast<std::ptrdiff_t>(sourceIndex + 1),
                    values.begin() + static_cast<std::ptrdiff_t>(destinationIndex + 1));
    } else if (sourceIndex > destinationIndex) {
        std::rotate(values.begin() + static_cast<std::ptrdiff_t>(destinationIndex),
                    values.begin() + static_cast<std::ptrdiff_t>(sourceIndex),
                    values.begin() + static_cast<std::ptrdiff_t>(sourceIndex + 1));
    }
}

void setAssetIdentifier(Asset &asset, std::string identifier)
{
    std::visit([&identifier](auto &value) {
        value.id = std::move(identifier);
    }, asset);
}

void replaceAssetReferences(Document &document,
                            const std::string &from,
                            const std::string &to)
{
    for (Layer &layer : document.layers) {
        LayerSource &sourceValue = layerSource(layer);
        if (auto *source = std::get_if<StaticSource>(&sourceValue)) {
            if (source->assetId == from) {
                source->assetId = to;
            }
        }
    }
    for (Frame &frame : document.frames) {
        for (Keyframe &keyframe : frame.keyframes) {
            if (keyframe.assetId == from) {
                keyframe.assetId = to;
            }
        }
    }
}

bool sameRaster(const RasterLayer &first, const RasterLayer &second)
{
    return first.width == second.width
        && first.height == second.height
        && first.pixels == second.pixels;
}

bool sameTransform(const AffineTransform &first,
                   const AffineTransform &second) noexcept
{
    return first.m11 == second.m11
        && first.m12 == second.m12
        && first.m21 == second.m21
        && first.m22 == second.m22
        && first.translationX == second.translationX
        && first.translationY == second.translationY;
}

std::vector<KeyframePlacement> keyframePlacements(const Document &document,
                                                  const std::string &layerId)
{
    std::vector<KeyframePlacement> placements;
    for (const Frame &frame : document.frames) {
        if (const Keyframe *keyframe = findKeyframe(frame, layerId)) {
            placements.push_back({frame.index, keyframe->assetId});
        }
    }
    return placements;
}

bool sameKeyframes(const std::vector<KeyframePlacement> &first,
                   const std::vector<KeyframePlacement> &second)
{
    return first.size() == second.size()
        && std::equal(first.begin(), first.end(), second.begin(),
                      [](const KeyframePlacement &left,
                         const KeyframePlacement &right) {
                          return left.frame == right.frame
                              && left.assetId == right.assetId;
                      });
}

void removeLayerKeyframes(Document &document, const std::string &layerId)
{
    for (Frame &frame : document.frames) {
        std::erase_if(frame.keyframes, [&layerId](const Keyframe &keyframe) {
            return keyframe.layerId == layerId;
        });
    }
    std::erase_if(document.frames, [](const Frame &frame) {
        return frame.keyframes.empty();
    });
}

void rebuildLayerFrameIndex(Document &document, const std::string &layerId)
{
    Layer *layer = findLayer(document, layerId);
    if (!layer) {
        return;
    }
    auto *source = std::get_if<KeyframedSource>(&layerSource(*layer));
    if (!source) {
        return;
    }
    source->frameIndices.clear();
    source->frameIndices.reserve(document.frames.size());
    for (const Frame &frame : document.frames) {
        if (findKeyframe(frame, layerId)) {
            source->frameIndices.push_back(frame.index);
        }
    }
}

void renameLayerKeyframes(Document &document,
                          const std::string &from,
                          const std::string &to)
{
    for (Frame &frame : document.frames) {
        bool renamed = false;
        for (Keyframe &keyframe : frame.keyframes) {
            if (keyframe.layerId == from) {
                keyframe.layerId = to;
                renamed = true;
            }
        }
        if (renamed) {
            std::sort(frame.keyframes.begin(), frame.keyframes.end(),
                      [](const Keyframe &left, const Keyframe &right) {
                          return left.layerId < right.layerId;
                      });
        }
    }
}

void insertFrameKeyframe(Document &document,
                         FrameIndex frameIndexValue,
                         Keyframe keyframe)
{
    const auto framePosition = std::lower_bound(
        document.frames.begin(), document.frames.end(), frameIndexValue,
        [](const Frame &frame, FrameIndex index) {
            return frame.index < index;
        });
    if (framePosition != document.frames.end()
        && framePosition->index == frameIndexValue) {
        const auto keyframePosition = std::lower_bound(
            framePosition->keyframes.begin(),
            framePosition->keyframes.end(),
            keyframe.layerId,
            [](const Keyframe &value, const std::string &layerId) {
                return value.layerId < layerId;
            });
        framePosition->keyframes.insert(keyframePosition, std::move(keyframe));
        return;
    }
    document.frames.insert(
        framePosition,
        Frame{frameIndexValue, {std::move(keyframe)}});
}

bool eraseFrameKeyframe(Document &document,
                        FrameIndex frameIndexValue,
                        const std::string &layerId)
{
    const std::optional<std::size_t> ownerPosition = frameIndex(
        document, frameIndexValue);
    if (!ownerPosition) {
        return false;
    }
    Frame &owner = document.frames[*ownerPosition];
    const std::optional<std::size_t> position = keyframeIndex(owner, layerId);
    if (!position) {
        return false;
    }
    owner.keyframes.erase(
        owner.keyframes.begin() + static_cast<std::ptrdiff_t>(*position));
    if (owner.keyframes.empty()) {
        document.frames.erase(
            document.frames.begin() + static_cast<std::ptrdiff_t>(*ownerPosition));
    }
    return true;
}

DocumentEditCode codeForValidationIssue(const ValidationIssue &issue) noexcept
{
    switch (issue.code) {
    case ValidationCode::ContentKindMismatch:
        return DocumentEditCode::AssetKindMismatch;
    case ValidationCode::DuplicateAssetId:
        return DocumentEditCode::DuplicateAssetId;
    case ValidationCode::DuplicateLayerId:
        return DocumentEditCode::DuplicateLayerId;
    case ValidationCode::DuplicateAudioClipId:
        return DocumentEditCode::DuplicateAudioClipId;
    default:
        return DocumentEditCode::ValidationRejected;
    }
}

} // namespace

DocumentEditor::DocumentEditor(Document &value)
{
    bind(value);
}

DocumentEditor::DocumentEditor(DocumentFile &file)
{
    bind(file);
}

DocumentEditResult DocumentEditor::bind(DocumentFile &file)
{
    if (!file.isOpen()) {
        return reject(DocumentEditCode::NotBound, "file", "no working file is open");
    }
    const auto result = bind(*file.boundDocument());
    if (result.ok()) {
        m_file = &file;
        m_fileGeneration = file.bindingGeneration();
    }
    return result;
}

DocumentEditResult DocumentEditor::bind(Document &value)
{
    if (const std::optional<ValidationIssue> issue = firstValidationIssue(value)) {
        return reject(DocumentEditCode::InvalidDocument,
                      issue->path,
                      issue->message);
    }
    m_document = &value;
    m_file = nullptr;
    m_fileGeneration = 0;
    m_revision = 0;
    return unchanged();
}

void DocumentEditor::unbind() noexcept
{
    m_document = nullptr;
    m_file = nullptr;
    m_fileGeneration = 0;
    m_revision = 0;
    m_lastResult = {};
}

bool DocumentEditor::isBound() const noexcept
{
    return m_document != nullptr
        && (!m_file || (m_file->isOpen() && m_fileGeneration == m_file->bindingGeneration()));
}

Document *DocumentEditor::document() noexcept
{
    return m_file ? nullptr : m_document;
}

const Document *DocumentEditor::document() const noexcept
{
    return isBound() ? m_document : nullptr;
}

std::uint64_t DocumentEditor::revision() const noexcept
{
    return m_revision;
}

const DocumentEditResult &DocumentEditor::lastResult() const noexcept
{
    return m_lastResult;
}

DocumentEditResult DocumentEditor::setCanvasExtent(CanvasExtent extent)
{
    if (m_file) {
        return editFile([&](DocumentEditor &editor) {
            return editor.setCanvasExtent(extent);
        });
    }
    if (!requireValidDocument()) {
        return m_lastResult;
    }
    if (m_document->extent.width == extent.width
        && m_document->extent.height == extent.height) {
        return unchanged();
    }

    const CanvasExtent prior = m_document->extent;
    m_document->extent = extent;
    if (const std::optional<ValidationIssue> issue = firstValidationIssue(*m_document)) {
        m_document->extent = prior;
        return reject(DocumentEditCode::ValidationRejected, issue->path, issue->message);
    }
    return applied();
}

DocumentEditResult DocumentEditor::ensureInfiniteCanvasRegion(CanvasRegion region)
{
    if (m_file) {
        return editFile([&](DocumentEditor &editor) {
            return editor.ensureInfiniteCanvasRegion(region);
        });
    }
    if (!requireValidDocument()) {
        return m_lastResult;
    }
    if (m_document->canvasMode != CanvasMode::Infinite) {
        return reject(DocumentEditCode::InvalidArgument, "canvasMode",
                      "canvas region growth requires an infinite canvas");
    }
    if (region.extent.width <= 0 || region.extent.height <= 0) {
        return reject(DocumentEditCode::InvalidArgument, "region.extent",
                      "requested canvas region must have a positive extent");
    }

    const std::int64_t requestedRight = static_cast<std::int64_t>(region.origin.x)
        + region.extent.width;
    const std::int64_t requestedBottom = static_cast<std::int64_t>(region.origin.y)
        + region.extent.height;
    const CanvasOrigin currentOrigin = m_document->infiniteCanvas.origin;
    const std::int64_t currentRight = static_cast<std::int64_t>(currentOrigin.x)
        + m_document->extent.width;
    const std::int64_t currentBottom = static_cast<std::int64_t>(currentOrigin.y)
        + m_document->extent.height;
    const std::int64_t chunkSize = m_document->infiniteCanvas.chunkSize;

    const std::int64_t nextLeft = floorToMultiple(
        std::min<std::int64_t>(currentOrigin.x, region.origin.x), chunkSize);
    const std::int64_t nextTop = floorToMultiple(
        std::min<std::int64_t>(currentOrigin.y, region.origin.y), chunkSize);
    const std::int64_t nextRight = ceilToMultiple(
        std::max(currentRight, requestedRight), chunkSize);
    const std::int64_t nextBottom = ceilToMultiple(
        std::max(currentBottom, requestedBottom), chunkSize);
    const std::int64_t nextWidth = nextRight - nextLeft;
    const std::int64_t nextHeight = nextBottom - nextTop;
    if (nextLeft < std::numeric_limits<std::int32_t>::min()
        || nextTop < std::numeric_limits<std::int32_t>::min()
        || nextRight > std::numeric_limits<std::int32_t>::max()
        || nextBottom > std::numeric_limits<std::int32_t>::max()
        || nextWidth <= 0
        || nextHeight <= 0
        || nextWidth > std::numeric_limits<std::int32_t>::max()
        || nextHeight > std::numeric_limits<std::int32_t>::max()) {
        return reject(DocumentEditCode::InvalidArgument, "region",
                      "requested canvas region exceeds the supported coordinate range");
    }

    if (nextLeft == currentOrigin.x
        && nextTop == currentOrigin.y
        && nextWidth == m_document->extent.width
        && nextHeight == m_document->extent.height) {
        return unchanged();
    }

    const InfiniteCanvas priorCanvas = m_document->infiniteCanvas;
    const CanvasExtent priorExtent = m_document->extent;
    m_document->infiniteCanvas.origin = {
        static_cast<std::int32_t>(nextLeft),
        static_cast<std::int32_t>(nextTop),
    };
    m_document->extent = {
        static_cast<std::int32_t>(nextWidth),
        static_cast<std::int32_t>(nextHeight),
    };
    if (const std::optional<ValidationIssue> issue = firstValidationIssue(*m_document)) {
        m_document->infiniteCanvas = priorCanvas;
        m_document->extent = priorExtent;
        return reject(DocumentEditCode::ValidationRejected, issue->path, issue->message);
    }
    return applied();
}

DocumentEditResult DocumentEditor::setFrameRate(FrameRate frameRate)
{
    if (m_file) {
        return editFile([&](DocumentEditor &editor) {
            return editor.setFrameRate(frameRate);
        });
    }
    if (!requireValidDocument()) {
        return m_lastResult;
    }
    if (m_document->timeline.frameRate.numerator == frameRate.numerator
        && m_document->timeline.frameRate.denominator == frameRate.denominator) {
        return unchanged();
    }

    const FrameRate prior = m_document->timeline.frameRate;
    m_document->timeline.frameRate = frameRate;
    if (const std::optional<ValidationIssue> issue = firstValidationIssue(*m_document)) {
        m_document->timeline.frameRate = prior;
        return reject(DocumentEditCode::ValidationRejected, issue->path, issue->message);
    }
    return applied();
}

DocumentEditResult DocumentEditor::setFrameCount(FrameIndex frameCount)
{
    if (m_file) {
        return editFile([&](DocumentEditor &editor) {
            return editor.setFrameCount(frameCount);
        });
    }
    if (!requireValidDocument()) {
        return m_lastResult;
    }
    if (m_document->timeline.frameCount == frameCount) {
        return unchanged();
    }

    const FrameIndex prior = m_document->timeline.frameCount;
    m_document->timeline.frameCount = frameCount;
    if (const std::optional<ValidationIssue> issue = firstValidationIssue(*m_document)) {
        m_document->timeline.frameCount = prior;
        return reject(DocumentEditCode::ValidationRejected, issue->path, issue->message);
    }
    return applied();
}

DocumentEditResult DocumentEditor::setStableDiffusionMetadata(
    StableDiffusionMetadata metadata)
{
    if (m_file) {
        return editFile([&](DocumentEditor &editor) {
            return editor.setStableDiffusionMetadata(std::move(metadata));
        });
    }
    if (!requireValidDocument()) {
        return m_lastResult;
    }
    if (m_document->stableDiffusionMetadata
        && *m_document->stableDiffusionMetadata == metadata) {
        return unchanged();
    }

    const FormatVersion priorVersion = m_document->formatVersion;
    std::optional<StableDiffusionMetadata> prior =
        std::move(m_document->stableDiffusionMetadata);
    m_document->formatVersion.major = CurrentFormatMajor;
    m_document->formatVersion.minor = std::max<std::uint16_t>(
        m_document->formatVersion.minor, 2);
    m_document->stableDiffusionMetadata = std::move(metadata);
    if (const std::optional<ValidationIssue> issue = firstValidationIssue(*m_document)) {
        m_document->formatVersion = priorVersion;
        m_document->stableDiffusionMetadata = std::move(prior);
        return reject(DocumentEditCode::ValidationRejected, issue->path, issue->message);
    }
    return applied();
}

DocumentEditResult DocumentEditor::clearStableDiffusionMetadata()
{
    if (m_file) {
        return editFile([&](DocumentEditor &editor) {
            return editor.clearStableDiffusionMetadata();
        });
    }
    if (!requireValidDocument()) {
        return m_lastResult;
    }
    if (!m_document->stableDiffusionMetadata) {
        return unchanged();
    }

    std::optional<StableDiffusionMetadata> prior =
        std::move(m_document->stableDiffusionMetadata);
    m_document->stableDiffusionMetadata.reset();
    if (const std::optional<ValidationIssue> issue = firstValidationIssue(*m_document)) {
        m_document->stableDiffusionMetadata = std::move(prior);
        return reject(DocumentEditCode::ValidationRejected, issue->path, issue->message);
    }
    return applied();
}

DocumentEditResult DocumentEditor::insertRasterAsset(std::string id,
                                                     RasterLayer pixels,
                                                     std::size_t index)
{
    if (m_file) {
        return editFile([&](DocumentEditor &editor) {
            return editor.insertRasterAsset(std::move(id), std::move(pixels), index);
        });
    }
    if (!requireValidDocument()) {
        return m_lastResult;
    }
    if (id.empty()) {
        return reject(DocumentEditCode::InvalidArgument, "asset.id",
                      "asset id must not be empty");
    }
    if (findAsset(*m_document, id)) {
        return reject(DocumentEditCode::DuplicateAssetId, "asset.id",
                      "asset id already exists");
    }
    const std::size_t position = insertionIndex(index, m_document->assets.size());
    if (position > m_document->assets.size()) {
        return reject(DocumentEditCode::IndexOutOfRange, "assets",
                      "asset insertion index is outside the collection");
    }

    m_document->assets.insert(
        m_document->assets.begin() + static_cast<std::ptrdiff_t>(position),
        RasterAsset{std::move(id), std::move(pixels)});
    if (const std::optional<ValidationIssue> issue = firstValidationIssue(*m_document)) {
        m_document->assets.erase(
            m_document->assets.begin() + static_cast<std::ptrdiff_t>(position));
        return reject(DocumentEditCode::ValidationRejected, issue->path, issue->message);
    }
    return applied();
}

DocumentEditResult DocumentEditor::insertVectorAsset(std::string id,
                                                     CanvasExtent viewport,
                                                     std::vector<VectorPath> paths,
                                                     std::size_t index)
{
    if (m_file) {
        return editFile([&](DocumentEditor &editor) {
            return editor.insertVectorAsset(std::move(id), viewport, std::move(paths), index);
        });
    }
    if (!requireValidDocument()) {
        return m_lastResult;
    }
    if (id.empty()) {
        return reject(DocumentEditCode::InvalidArgument, "asset.id",
                      "asset id must not be empty");
    }
    if (findAsset(*m_document, id)) {
        return reject(DocumentEditCode::DuplicateAssetId, "asset.id",
                      "asset id already exists");
    }
    const std::size_t position = insertionIndex(index, m_document->assets.size());
    if (position > m_document->assets.size()) {
        return reject(DocumentEditCode::IndexOutOfRange, "assets",
                      "asset insertion index is outside the collection");
    }

    m_document->assets.insert(
        m_document->assets.begin() + static_cast<std::ptrdiff_t>(position),
        VectorAsset{std::move(id), viewport, std::move(paths)});
    if (const std::optional<ValidationIssue> issue = firstValidationIssue(*m_document)) {
        m_document->assets.erase(
            m_document->assets.begin() + static_cast<std::ptrdiff_t>(position));
        return reject(DocumentEditCode::ValidationRejected, issue->path, issue->message);
    }
    return applied();
}

DocumentEditResult DocumentEditor::replaceRasterPixels(const std::string &id,
                                                       RasterLayer pixels)
{
    if (m_file) {
        return editFile([&](DocumentEditor &editor) {
            return editor.replaceRasterPixels(id, std::move(pixels));
        });
    }
    if (!requireValidDocument()) {
        return m_lastResult;
    }
    Asset *asset = findAsset(*m_document, id);
    if (!asset) {
        return reject(DocumentEditCode::AssetNotFound, "assets",
                      "asset was not found");
    }
    auto *raster = std::get_if<RasterAsset>(asset);
    if (!raster) {
        return reject(DocumentEditCode::AssetKindMismatch, "assets",
                      "asset is not raster content");
    }
    if (sameRaster(raster->pixels, pixels)) {
        return unchanged();
    }

    RasterLayer prior = std::move(raster->pixels);
    raster->pixels = std::move(pixels);
    if (const std::optional<ValidationIssue> issue = firstValidationIssue(*m_document)) {
        raster->pixels = std::move(prior);
        return reject(DocumentEditCode::ValidationRejected, issue->path, issue->message);
    }
    return applied();
}

DocumentEditResult DocumentEditor::replaceVectorData(const std::string &id,
                                                     CanvasExtent viewport,
                                                     std::vector<VectorPath> paths)
{
    if (m_file) {
        return editFile([&](DocumentEditor &editor) {
            return editor.replaceVectorData(id, viewport, std::move(paths));
        });
    }
    if (!requireValidDocument()) {
        return m_lastResult;
    }
    Asset *asset = findAsset(*m_document, id);
    if (!asset) {
        return reject(DocumentEditCode::AssetNotFound, "assets",
                      "asset was not found");
    }
    auto *vector = std::get_if<VectorAsset>(asset);
    if (!vector) {
        return reject(DocumentEditCode::AssetKindMismatch, "assets",
                      "asset is not vector content");
    }

    const CanvasExtent priorViewport = vector->viewport;
    std::vector<VectorPath> priorPaths = std::move(vector->paths);
    vector->viewport = viewport;
    vector->paths = std::move(paths);
    if (const std::optional<ValidationIssue> issue = firstValidationIssue(*m_document)) {
        vector->viewport = priorViewport;
        vector->paths = std::move(priorPaths);
        return reject(DocumentEditCode::ValidationRejected, issue->path, issue->message);
    }
    return applied();
}

DocumentEditResult DocumentEditor::renameAsset(const std::string &id,
                                               std::string replacementId)
{
    if (m_file) {
        return editFile([&](DocumentEditor &editor) {
            return editor.renameAsset(id, std::move(replacementId));
        });
    }
    if (!requireValidDocument()) {
        return m_lastResult;
    }
    Asset *asset = findAsset(*m_document, id);
    if (!asset) {
        return reject(DocumentEditCode::AssetNotFound, "assets",
                      "asset was not found");
    }
    if (id == replacementId) {
        return unchanged();
    }
    if (replacementId.empty()) {
        return reject(DocumentEditCode::InvalidArgument, "asset.id",
                      "replacement asset id must not be empty");
    }
    if (findAsset(*m_document, replacementId)) {
        return reject(DocumentEditCode::DuplicateAssetId, "asset.id",
                      "replacement asset id already exists");
    }

    setAssetIdentifier(*asset, replacementId);
    replaceAssetReferences(*m_document, id, replacementId);
    if (const std::optional<ValidationIssue> issue = firstValidationIssue(*m_document)) {
        setAssetIdentifier(*asset, id);
        replaceAssetReferences(*m_document, replacementId, id);
        return reject(DocumentEditCode::ValidationRejected, issue->path, issue->message);
    }
    return applied();
}

DocumentEditResult DocumentEditor::moveAsset(const std::string &id,
                                             std::size_t destinationIndex)
{
    if (m_file) {
        return editFile([&](DocumentEditor &editor) {
            return editor.moveAsset(id, destinationIndex);
        });
    }
    if (!requireValidDocument()) {
        return m_lastResult;
    }
    const std::optional<std::size_t> sourceIndex = assetIndex(*m_document, id);
    if (!sourceIndex) {
        return reject(DocumentEditCode::AssetNotFound, "assets",
                      "asset was not found");
    }
    if (destinationIndex >= m_document->assets.size()) {
        return reject(DocumentEditCode::IndexOutOfRange, "assets",
                      "asset destination index is outside the collection");
    }
    if (*sourceIndex == destinationIndex) {
        return unchanged();
    }
    moveElement(m_document->assets, *sourceIndex, destinationIndex);
    return applied();
}

DocumentEditResult DocumentEditor::removeAsset(const std::string &id)
{
    if (m_file) {
        return editFile([&](DocumentEditor &editor) {
            return editor.removeAsset(id);
        });
    }
    if (!requireValidDocument()) {
        return m_lastResult;
    }
    const std::optional<std::size_t> position = assetIndex(*m_document, id);
    if (!position) {
        return reject(DocumentEditCode::AssetNotFound, "assets",
                      "asset was not found");
    }
    if (!assetReferences(*m_document, id).empty()) {
        return reject(DocumentEditCode::AssetReferenced, "assets",
                      "asset is still referenced by one or more layers");
    }

    Asset removed = std::move(m_document->assets[*position]);
    m_document->assets.erase(
        m_document->assets.begin() + static_cast<std::ptrdiff_t>(*position));
    if (const std::optional<ValidationIssue> issue = firstValidationIssue(*m_document)) {
        m_document->assets.insert(
            m_document->assets.begin() + static_cast<std::ptrdiff_t>(*position),
            std::move(removed));
        return reject(DocumentEditCode::ValidationRejected, issue->path, issue->message);
    }
    return applied();
}

DocumentEditResult DocumentEditor::insertAudioAsset(AudioAsset asset, std::size_t index)
{
    if (m_file) {
        return editFile([&](DocumentEditor &editor) {
            return editor.insertAudioAsset(std::move(asset), index);
        });
    }
    if (!requireValidDocument()) {
        return m_lastResult;
    }
    if (asset.id.empty()) {
        return reject(DocumentEditCode::InvalidArgument, "audioAsset.id", "audio asset id must not be empty");
    }
    if (findAsset(*m_document, asset.id) || findAudioAsset(*m_document, asset.id)) {
        return reject(DocumentEditCode::DuplicateAssetId, "audioAsset.id", "asset id already exists");
    }
    const std::size_t position = insertionIndex(index, m_document->audioAssets.size());
    if (position > m_document->audioAssets.size()) {
        return reject(DocumentEditCode::IndexOutOfRange, "audioAssets", "audio asset insertion index is outside the collection");
    }
    const FormatVersion priorVersion = m_document->formatVersion;
    m_document->formatVersion = {CurrentFormatMajor, CurrentFormatMinor};
    m_document->audioAssets.insert(m_document->audioAssets.begin()
        + static_cast<std::ptrdiff_t>(position), std::move(asset));
    if (const auto issue = firstValidationIssue(*m_document)) {
        m_document->audioAssets.erase(m_document->audioAssets.begin()
            + static_cast<std::ptrdiff_t>(position));
        m_document->formatVersion = priorVersion;
        return reject(codeForValidationIssue(*issue), issue->path, issue->message);
    }
    return applied();
}

DocumentEditResult DocumentEditor::replaceAudioAsset(const std::string &id, AudioAsset asset)
{
    if (m_file) {
        return editFile([&](DocumentEditor &editor) {
            return editor.replaceAudioAsset(id, std::move(asset));
        });
    }
    if (!requireValidDocument()) {
        return m_lastResult;
    }
    AudioAsset *target = findAudioAsset(*m_document, id);
    if (!target) {
        return reject(DocumentEditCode::AssetNotFound, "audioAssets", "audio asset was not found");
    }
    if (asset.id != id) {
        return reject(DocumentEditCode::InvalidArgument, "audioAsset.id", "replacement must preserve the audio asset id");
    }
    if (*target == asset) {
        return unchanged();
    }
    AudioAsset prior = std::move(*target);
    *target = std::move(asset);
    if (const auto issue = firstValidationIssue(*m_document)) {
        *target = std::move(prior);
        return reject(codeForValidationIssue(*issue), issue->path, issue->message);
    }
    return applied();
}

DocumentEditResult DocumentEditor::removeAudioAsset(const std::string &id)
{
    if (m_file) {
        return editFile([&](DocumentEditor &editor) { return editor.removeAudioAsset(id); });
    }
    if (!requireValidDocument()) {
        return m_lastResult;
    }
    AudioAsset *target = findAudioAsset(*m_document, id);
    if (!target) {
        return reject(DocumentEditCode::AssetNotFound, "audioAssets", "audio asset was not found");
    }
    for (const AudioTrackLayer &track : m_document->audioTracks) {
        if (std::any_of(track.clips.begin(), track.clips.end(),
                        [&id](const AudioClip &clip) { return clip.assetId == id; })) {
            return reject(DocumentEditCode::AssetReferenced, "audioAssets", "audio asset is referenced by a clip");
        }
    }
    m_document->audioAssets.erase(m_document->audioAssets.begin()
        + (target - m_document->audioAssets.data()));
    return applied();
}

DocumentEditResult DocumentEditor::insertAudioTrack(AudioTrackLayer track, std::size_t index)
{
    if (m_file) {
        return editFile([&](DocumentEditor &editor) {
            return editor.insertAudioTrack(std::move(track), index);
        });
    }
    if (!requireValidDocument()) {
        return m_lastResult;
    }
    if (track.id.empty()) {
        return reject(DocumentEditCode::InvalidArgument, "audioTrack.id", "audio track id must not be empty");
    }
    if (findLayer(*m_document, track.id) || findAudioTrack(*m_document, track.id)) {
        return reject(DocumentEditCode::DuplicateLayerId, "audioTrack.id", "layer id already exists");
    }
    const std::size_t position = insertionIndex(index, m_document->audioTracks.size());
    if (position > m_document->audioTracks.size()) {
        return reject(DocumentEditCode::IndexOutOfRange, "audioTracks", "audio track insertion index is outside the collection");
    }
    const FormatVersion priorVersion = m_document->formatVersion;
    m_document->formatVersion = {CurrentFormatMajor, CurrentFormatMinor};
    m_document->audioTracks.insert(m_document->audioTracks.begin()
        + static_cast<std::ptrdiff_t>(position), std::move(track));
    if (const auto issue = firstValidationIssue(*m_document)) {
        m_document->audioTracks.erase(m_document->audioTracks.begin()
            + static_cast<std::ptrdiff_t>(position));
        m_document->formatVersion = priorVersion;
        return reject(codeForValidationIssue(*issue), issue->path, issue->message);
    }
    return applied();
}

DocumentEditResult DocumentEditor::replaceAudioTrack(const std::string &id, AudioTrackLayer track)
{
    if (m_file) {
        return editFile([&](DocumentEditor &editor) {
            return editor.replaceAudioTrack(id, std::move(track));
        });
    }
    if (!requireValidDocument()) {
        return m_lastResult;
    }
    AudioTrackLayer *target = findAudioTrack(*m_document, id);
    if (!target) {
        return reject(DocumentEditCode::LayerNotFound, "audioTracks", "audio track was not found");
    }
    if (track.id != id) {
        return reject(DocumentEditCode::InvalidArgument, "audioTrack.id", "replacement must preserve the audio track id");
    }
    if (*target == track) {
        return unchanged();
    }
    AudioTrackLayer prior = std::move(*target);
    *target = std::move(track);
    if (const auto issue = firstValidationIssue(*m_document)) {
        *target = std::move(prior);
        return reject(codeForValidationIssue(*issue), issue->path, issue->message);
    }
    return applied();
}

DocumentEditResult DocumentEditor::moveAudioTrack(const std::string &id, std::size_t destinationIndex)
{
    if (m_file) {
        return editFile([&](DocumentEditor &editor) { return editor.moveAudioTrack(id, destinationIndex); });
    }
    if (!requireValidDocument()) {
        return m_lastResult;
    }
    AudioTrackLayer *target = findAudioTrack(*m_document, id);
    if (!target) {
        return reject(DocumentEditCode::LayerNotFound, "audioTracks", "audio track was not found");
    }
    if (destinationIndex >= m_document->audioTracks.size()) {
        return reject(DocumentEditCode::IndexOutOfRange, "audioTracks", "audio track destination is outside the collection");
    }
    const auto sourceIndex = static_cast<std::size_t>(target - m_document->audioTracks.data());
    if (sourceIndex == destinationIndex) {
        return unchanged();
    }
    moveElement(m_document->audioTracks, sourceIndex, destinationIndex);
    return applied();
}

DocumentEditResult DocumentEditor::removeAudioTrack(const std::string &id)
{
    if (m_file) {
        return editFile([&](DocumentEditor &editor) { return editor.removeAudioTrack(id); });
    }
    if (!requireValidDocument()) {
        return m_lastResult;
    }
    AudioTrackLayer *target = findAudioTrack(*m_document, id);
    if (!target) {
        return reject(DocumentEditCode::LayerNotFound, "audioTracks", "audio track was not found");
    }
    m_document->audioTracks.erase(m_document->audioTracks.begin()
        + (target - m_document->audioTracks.data()));
    return applied();
}

DocumentEditResult DocumentEditor::insertAudioClip(const std::string &id, AudioClip clip)
{
    if (m_file) {
        return editFile([&](DocumentEditor &editor) { return editor.insertAudioClip(id, std::move(clip)); });
    }
    if (!requireValidDocument()) {
        return m_lastResult;
    }
    const AudioTrackLayer *target = findAudioTrack(*m_document, id);
    if (!target) {
        return reject(DocumentEditCode::LayerNotFound, "audioTracks", "audio track was not found");
    }
    AudioTrackLayer replacement = *target;
    replacement.clips.push_back(std::move(clip));
    std::stable_sort(replacement.clips.begin(), replacement.clips.end(),
        [](const AudioClip &a, const AudioClip &b) { return a.startFrame < b.startFrame; });
    return replaceAudioTrack(id, std::move(replacement));
}

DocumentEditResult DocumentEditor::replaceAudioClip(const std::string &id,
                                                   const std::string &clipId, AudioClip clip)
{
    if (m_file) {
        return editFile([&](DocumentEditor &editor) { return editor.replaceAudioClip(id, clipId, std::move(clip)); });
    }
    if (!requireValidDocument()) {
        return m_lastResult;
    }
    const AudioTrackLayer *target = findAudioTrack(*m_document, id);
    if (!target) {
        return reject(DocumentEditCode::LayerNotFound, "audioTracks", "audio track was not found");
    }
    if (!findAudioClip(*target, clipId)) {
        return reject(DocumentEditCode::AudioClipNotFound, "audioTrack.clips", "audio clip was not found");
    }
    if (clip.id != clipId) {
        return reject(DocumentEditCode::InvalidArgument, "audioClip.id", "replacement must preserve the audio clip id");
    }
    AudioTrackLayer replacement = *target;
    *findAudioClip(replacement, clipId) = std::move(clip);
    std::stable_sort(replacement.clips.begin(), replacement.clips.end(),
        [](const AudioClip &a, const AudioClip &b) { return a.startFrame < b.startFrame; });
    return replaceAudioTrack(id, std::move(replacement));
}

DocumentEditResult DocumentEditor::removeAudioClip(const std::string &id, const std::string &clipId)
{
    if (m_file) {
        return editFile([&](DocumentEditor &editor) { return editor.removeAudioClip(id, clipId); });
    }
    if (!requireValidDocument()) {
        return m_lastResult;
    }
    const AudioTrackLayer *target = findAudioTrack(*m_document, id);
    if (!target) {
        return reject(DocumentEditCode::LayerNotFound, "audioTracks", "audio track was not found");
    }
    if (!findAudioClip(*target, clipId)) {
        return reject(DocumentEditCode::AudioClipNotFound, "audioTrack.clips", "audio clip was not found");
    }
    AudioTrackLayer replacement = *target;
    std::erase_if(replacement.clips, [&clipId](const AudioClip &clip) { return clip.id == clipId; });
    return replaceAudioTrack(id, std::move(replacement));
}

DocumentEditResult DocumentEditor::insertLayer(Layer layer, std::size_t index)
{
    if (m_file) {
        return editFile([&](DocumentEditor &editor) {
            return editor.insertLayer(std::move(layer), index);
        });
    }
    if (!requireValidDocument()) {
        return m_lastResult;
    }
    const std::string &id = layerProperties(layer).id;
    if (id.empty()) {
        return reject(DocumentEditCode::InvalidArgument, "layer.id",
                      "layer id must not be empty");
    }
    if (findLayer(*m_document, id)) {
        return reject(DocumentEditCode::DuplicateLayerId, "layer.id",
                      "layer id already exists");
    }
    const std::size_t position = insertionIndex(index, m_document->layers.size());
    if (position > m_document->layers.size()) {
        return reject(DocumentEditCode::IndexOutOfRange, "layers",
                      "layer insertion index is outside the collection");
    }

    const FormatVersion priorVersion = m_document->formatVersion;
    if (layerProperties(layer).frameRange) {
        m_document->formatVersion = {CurrentFormatMajor, CurrentFormatMinor};
    }
    m_document->layers.insert(
        m_document->layers.begin() + static_cast<std::ptrdiff_t>(position),
        std::move(layer));
    if (const std::optional<ValidationIssue> issue = firstValidationIssue(*m_document)) {
        m_document->layers.erase(
            m_document->layers.begin() + static_cast<std::ptrdiff_t>(position));
        m_document->formatVersion = priorVersion;
        return reject(codeForValidationIssue(*issue), issue->path, issue->message);
    }
    return applied();
}

DocumentEditResult DocumentEditor::insertKeyframedLayer(
    Layer layer,
    std::vector<KeyframePlacement> keyframes,
    std::size_t index)
{
    if (m_file) {
        return editFile([&](DocumentEditor &editor) {
            return editor.insertKeyframedLayer(std::move(layer), std::move(keyframes), index);
        });
    }
    if (!requireValidDocument()) {
        return m_lastResult;
    }
    if (!std::holds_alternative<KeyframedSource>(layerSource(layer))) {
        return reject(DocumentEditCode::InvalidArgument, "layer.source",
                      "atomic keyframed-layer insertion requires a keyframed source marker");
    }
    const std::string id = layerProperties(layer).id;
    if (id.empty()) {
        return reject(DocumentEditCode::InvalidArgument, "layer.id",
                      "layer id must not be empty");
    }
    if (findLayer(*m_document, id)) {
        return reject(DocumentEditCode::DuplicateLayerId, "layer.id",
                      "layer id already exists");
    }
    const std::size_t position = insertionIndex(index, m_document->layers.size());
    if (position > m_document->layers.size()) {
        return reject(DocumentEditCode::IndexOutOfRange, "layers",
                      "layer insertion index is outside the collection");
    }

    std::sort(keyframes.begin(), keyframes.end(),
              [](const KeyframePlacement &left,
                 const KeyframePlacement &right) {
                  return left.frame < right.frame;
              });
    const FormatVersion priorVersion = m_document->formatVersion;
    if (layerProperties(layer).frameRange) {
        m_document->formatVersion = {CurrentFormatMajor, CurrentFormatMinor};
    }
    std::get<KeyframedSource>(layerSource(layer)).frameIndices.clear();
    std::vector<Frame> priorFrames = m_document->frames;
    m_document->layers.insert(
        m_document->layers.begin() + static_cast<std::ptrdiff_t>(position),
        std::move(layer));
    for (KeyframePlacement &placement : keyframes) {
        insertFrameKeyframe(*m_document,
                            placement.frame,
                            Keyframe{id, std::move(placement.assetId)});
    }
    rebuildLayerFrameIndex(*m_document, id);
    if (const std::optional<ValidationIssue> issue = firstValidationIssue(*m_document)) {
        m_document->layers.erase(
            m_document->layers.begin() + static_cast<std::ptrdiff_t>(position));
        m_document->frames = std::move(priorFrames);
        m_document->formatVersion = priorVersion;
        return reject(codeForValidationIssue(*issue), issue->path, issue->message);
    }
    return applied();
}

DocumentEditResult DocumentEditor::replaceLayer(const std::string &id, Layer layer)
{
    if (m_file) {
        return editFile([&](DocumentEditor &editor) {
            return editor.replaceLayer(id, std::move(layer));
        });
    }
    if (!requireValidDocument()) {
        return m_lastResult;
    }
    const std::optional<std::size_t> position = layerIndex(*m_document, id);
    if (!position) {
        return reject(DocumentEditCode::LayerNotFound, "layers",
                      "layer was not found");
    }
    const std::string replacementId = layerProperties(layer).id;
    if (replacementId.empty()) {
        return reject(DocumentEditCode::InvalidArgument, "layer.id",
                      "replacement layer id must not be empty");
    }
    if (replacementId != id && findLayer(*m_document, replacementId)) {
        return reject(DocumentEditCode::DuplicateLayerId, "layer.id",
                      "replacement layer id already exists");
    }

    const FormatVersion priorVersion = m_document->formatVersion;
    if (layerProperties(layer).frameRange) {
        m_document->formatVersion = {CurrentFormatMajor, CurrentFormatMinor};
    }
    const std::string priorId = layerProperties(m_document->layers[*position]).id;
    const bool replacementIsKeyframed = std::holds_alternative<KeyframedSource>(
        layerSource(layer));
    Layer prior = std::move(m_document->layers[*position]);
    std::vector<Frame> priorFrames = m_document->frames;
    m_document->layers[*position] = std::move(layer);
    if (priorId != replacementId) {
        renameLayerKeyframes(*m_document, priorId, replacementId);
    }
    if (!replacementIsKeyframed) {
        removeLayerKeyframes(*m_document, replacementId);
    } else {
        rebuildLayerFrameIndex(*m_document, replacementId);
    }
    if (const std::optional<ValidationIssue> issue = firstValidationIssue(*m_document)) {
        m_document->layers[*position] = std::move(prior);
        m_document->frames = std::move(priorFrames);
        m_document->formatVersion = priorVersion;
        return reject(codeForValidationIssue(*issue), issue->path, issue->message);
    }
    return applied();
}

DocumentEditResult DocumentEditor::renameLayer(const std::string &id,
                                               std::string replacementId)
{
    if (m_file) {
        return editFile([&](DocumentEditor &editor) {
            return editor.renameLayer(id, std::move(replacementId));
        });
    }
    if (!requireValidDocument()) {
        return m_lastResult;
    }
    Layer *layer = findLayer(*m_document, id);
    if (!layer) {
        return reject(DocumentEditCode::LayerNotFound, "layers",
                      "layer was not found");
    }
    if (id == replacementId) {
        return unchanged();
    }
    if (replacementId.empty()) {
        return reject(DocumentEditCode::InvalidArgument, "layer.id",
                      "replacement layer id must not be empty");
    }
    if (findLayer(*m_document, replacementId)) {
        return reject(DocumentEditCode::DuplicateLayerId, "layer.id",
                      "replacement layer id already exists");
    }
    layerProperties(*layer).id = replacementId;
    renameLayerKeyframes(*m_document, id, replacementId);
    rebuildLayerFrameIndex(*m_document, replacementId);
    return applied();
}

DocumentEditResult DocumentEditor::setLayerName(const std::string &id,
                                                std::string name)
{
    if (m_file) {
        return editFile([&](DocumentEditor &editor) {
            return editor.setLayerName(id, std::move(name));
        });
    }
    if (!requireValidDocument()) {
        return m_lastResult;
    }
    Layer *layer = findLayer(*m_document, id);
    if (!layer) {
        return reject(DocumentEditCode::LayerNotFound, "layers",
                      "layer was not found");
    }
    LayerProperties &properties = layerProperties(*layer);
    if (properties.name == name) {
        return unchanged();
    }
    properties.name = std::move(name);
    return applied();
}

DocumentEditResult DocumentEditor::setLayerVisible(const std::string &id, bool visible)
{
    if (m_file) {
        return editFile([&](DocumentEditor &editor) {
            return editor.setLayerVisible(id, visible);
        });
    }
    if (!requireValidDocument()) {
        return m_lastResult;
    }
    Layer *layer = findLayer(*m_document, id);
    if (!layer) {
        return reject(DocumentEditCode::LayerNotFound, "layers",
                      "layer was not found");
    }
    LayerProperties &properties = layerProperties(*layer);
    if (properties.visible == visible) {
        return unchanged();
    }
    properties.visible = visible;
    return applied();
}

DocumentEditResult DocumentEditor::setLayerOpacity(const std::string &id, double opacity)
{
    if (m_file) {
        return editFile([&](DocumentEditor &editor) {
            return editor.setLayerOpacity(id, opacity);
        });
    }
    if (!requireValidDocument()) {
        return m_lastResult;
    }
    Layer *layer = findLayer(*m_document, id);
    if (!layer) {
        return reject(DocumentEditCode::LayerNotFound, "layers",
                      "layer was not found");
    }
    LayerProperties &properties = layerProperties(*layer);
    if (properties.opacity == opacity) {
        return unchanged();
    }
    const double prior = properties.opacity;
    properties.opacity = opacity;
    if (const std::optional<ValidationIssue> issue = firstValidationIssue(*m_document)) {
        properties.opacity = prior;
        return reject(DocumentEditCode::ValidationRejected, issue->path, issue->message);
    }
    return applied();
}

DocumentEditResult DocumentEditor::setLayerTransform(const std::string &id,
                                                     AffineTransform transform)
{
    if (m_file) {
        return editFile([&](DocumentEditor &editor) {
            return editor.setLayerTransform(id, transform);
        });
    }
    if (!requireValidDocument()) {
        return m_lastResult;
    }
    Layer *layer = findLayer(*m_document, id);
    if (!layer) {
        return reject(DocumentEditCode::LayerNotFound, "layers",
                      "layer was not found");
    }
    LayerProperties &properties = layerProperties(*layer);
    if (sameTransform(properties.transform, transform)) {
        return unchanged();
    }
    const AffineTransform prior = properties.transform;
    properties.transform = transform;
    if (const std::optional<ValidationIssue> issue = firstValidationIssue(*m_document)) {
        properties.transform = prior;
        return reject(DocumentEditCode::ValidationRejected, issue->path, issue->message);
    }
    return applied();
}

DocumentEditResult DocumentEditor::setLayerBlendMode(const std::string &id,
                                                     RasterBlendMode blendMode)
{
    if (m_file) {
        return editFile([&](DocumentEditor &editor) {
            return editor.setLayerBlendMode(id, blendMode);
        });
    }
    if (!requireValidDocument()) {
        return m_lastResult;
    }
    Layer *layer = findLayer(*m_document, id);
    if (!layer) {
        return reject(DocumentEditCode::LayerNotFound, "layers",
                      "layer was not found");
    }
    LayerProperties &properties = layerProperties(*layer);
    if (properties.blendMode == blendMode) {
        return unchanged();
    }
    const RasterBlendMode prior = properties.blendMode;
    properties.blendMode = blendMode;
    if (const std::optional<ValidationIssue> issue = firstValidationIssue(*m_document)) {
        properties.blendMode = prior;
        return reject(DocumentEditCode::ValidationRejected, issue->path, issue->message);
    }
    return applied();
}

DocumentEditResult DocumentEditor::setLayerFrameRange(
    const std::string &id,
    std::optional<LayerFrameRange> frameRange)
{
    if (m_file) {
        return editFile([&](DocumentEditor &editor) {
            return editor.setLayerFrameRange(id, frameRange);
        });
    }
    if (!requireValidDocument()) {
        return m_lastResult;
    }
    Layer *layer = findLayer(*m_document, id);
    if (!layer) {
        return reject(DocumentEditCode::LayerNotFound, "layers",
                      "layer was not found");
    }
    LayerProperties &properties = layerProperties(*layer);
    if (properties.frameRange == frameRange) {
        return unchanged();
    }

    const FormatVersion priorVersion = m_document->formatVersion;
    std::optional<LayerFrameRange> prior = properties.frameRange;
    if (frameRange) {
        m_document->formatVersion = {CurrentFormatMajor, CurrentFormatMinor};
    }
    properties.frameRange = std::move(frameRange);
    if (const std::optional<ValidationIssue> issue = firstValidationIssue(*m_document)) {
        properties.frameRange = std::move(prior);
        m_document->formatVersion = priorVersion;
        return reject(DocumentEditCode::ValidationRejected, issue->path, issue->message);
    }
    return applied();
}

DocumentEditResult DocumentEditor::setStaticSource(const std::string &id,
                                                   std::string assetIdValue)
{
    if (m_file) {
        return editFile([&](DocumentEditor &editor) {
            return editor.setStaticSource(id, std::move(assetIdValue));
        });
    }
    if (!requireValidDocument()) {
        return m_lastResult;
    }
    Layer *layer = findLayer(*m_document, id);
    if (!layer) {
        return reject(DocumentEditCode::LayerNotFound, "layers",
                      "layer was not found");
    }
    LayerSource &sourceValue = layerSource(*layer);
    if (const auto *source = std::get_if<StaticSource>(&sourceValue);
        source && source->assetId == assetIdValue) {
        return unchanged();
    }
    LayerSource prior = std::move(sourceValue);
    std::vector<Frame> priorFrames = m_document->frames;
    sourceValue = StaticSource{std::move(assetIdValue)};
    removeLayerKeyframes(*m_document, id);
    if (const std::optional<ValidationIssue> issue = firstValidationIssue(*m_document)) {
        sourceValue = std::move(prior);
        m_document->frames = std::move(priorFrames);
        return reject(codeForValidationIssue(*issue), issue->path, issue->message);
    }
    return applied();
}

DocumentEditResult DocumentEditor::setKeyframedSource(const std::string &id,
                                                      std::vector<KeyframePlacement> keyframes)
{
    if (m_file) {
        return editFile([&](DocumentEditor &editor) {
            return editor.setKeyframedSource(id, std::move(keyframes));
        });
    }
    if (!requireValidDocument()) {
        return m_lastResult;
    }
    Layer *layer = findLayer(*m_document, id);
    if (!layer) {
        return reject(DocumentEditCode::LayerNotFound, "layers",
                      "layer was not found");
    }
    std::sort(keyframes.begin(), keyframes.end(),
              [](const KeyframePlacement &left,
                 const KeyframePlacement &right) {
                  return left.frame < right.frame;
              });
    LayerSource &sourceValue = layerSource(*layer);
    if (const auto *source = std::get_if<KeyframedSource>(&sourceValue);
        source && sameKeyframes(keyframePlacements(*m_document, id), keyframes)) {
        return unchanged();
    }
    LayerSource prior = std::move(sourceValue);
    std::vector<Frame> priorFrames = m_document->frames;
    sourceValue = KeyframedSource{};
    removeLayerKeyframes(*m_document, id);
    for (KeyframePlacement &placement : keyframes) {
        insertFrameKeyframe(*m_document,
                            placement.frame,
                            Keyframe{id, std::move(placement.assetId)});
    }
    rebuildLayerFrameIndex(*m_document, id);
    if (const std::optional<ValidationIssue> issue = firstValidationIssue(*m_document)) {
        sourceValue = std::move(prior);
        m_document->frames = std::move(priorFrames);
        return reject(codeForValidationIssue(*issue), issue->path, issue->message);
    }
    return applied();
}

DocumentEditResult DocumentEditor::moveLayer(const std::string &id,
                                             std::size_t destinationIndex)
{
    if (m_file) {
        return editFile([&](DocumentEditor &editor) {
            return editor.moveLayer(id, destinationIndex);
        });
    }
    if (!requireValidDocument()) {
        return m_lastResult;
    }
    const std::optional<std::size_t> sourceIndex = layerIndex(*m_document, id);
    if (!sourceIndex) {
        return reject(DocumentEditCode::LayerNotFound, "layers",
                      "layer was not found");
    }
    if (destinationIndex >= m_document->layers.size()) {
        return reject(DocumentEditCode::IndexOutOfRange, "layers",
                      "layer destination index is outside the collection");
    }
    if (*sourceIndex == destinationIndex) {
        return unchanged();
    }
    moveElement(m_document->layers, *sourceIndex, destinationIndex);
    return applied();
}

DocumentEditResult DocumentEditor::removeLayer(const std::string &id)
{
    if (m_file) {
        return editFile([&](DocumentEditor &editor) {
            return editor.removeLayer(id);
        });
    }
    if (!requireValidDocument()) {
        return m_lastResult;
    }
    const std::optional<std::size_t> position = layerIndex(*m_document, id);
    if (!position) {
        return reject(DocumentEditCode::LayerNotFound, "layers",
                      "layer was not found");
    }
    removeLayerKeyframes(*m_document, id);
    m_document->layers.erase(
        m_document->layers.begin() + static_cast<std::ptrdiff_t>(*position));
    return applied();
}

DocumentEditResult DocumentEditor::insertKeyframe(const std::string &id,
                                                  FrameIndex frame,
                                                  std::string assetIdValue)
{
    if (m_file) {
        return editFile([&](DocumentEditor &editor) {
            return editor.insertKeyframe(id, frame, std::move(assetIdValue));
        });
    }
    if (!requireValidDocument()) {
        return m_lastResult;
    }
    Layer *layer = findLayer(*m_document, id);
    if (!layer) {
        return reject(DocumentEditCode::LayerNotFound, "layers",
                      "layer was not found");
    }
    auto *source = std::get_if<KeyframedSource>(&layerSource(*layer));
    if (!source) {
        return reject(DocumentEditCode::SourceNotKeyframed, "layer.source",
                      "layer source is not keyframed");
    }
    if (findKeyframe(*m_document, id, frame)) {
        return reject(DocumentEditCode::DuplicateKeyframe, "frames",
                      "a keyframe already exists at the requested frame");
    }
    const std::vector<FrameIndex> priorFrameIndices = source->frameIndices;
    std::vector<Frame> priorFrames = m_document->frames;
    insertFrameKeyframe(*m_document,
                        frame,
                        Keyframe{id, std::move(assetIdValue)});
    rebuildLayerFrameIndex(*m_document, id);
    if (const std::optional<ValidationIssue> issue = firstValidationIssue(*m_document)) {
        m_document->frames = std::move(priorFrames);
        source->frameIndices = priorFrameIndices;
        return reject(codeForValidationIssue(*issue), issue->path, issue->message);
    }
    return applied();
}

DocumentEditResult DocumentEditor::setKeyframeAsset(const std::string &id,
                                                    FrameIndex frame,
                                                    std::string assetIdValue)
{
    if (m_file) {
        return editFile([&](DocumentEditor &editor) {
            return editor.setKeyframeAsset(id, frame, std::move(assetIdValue));
        });
    }
    if (!requireValidDocument()) {
        return m_lastResult;
    }
    Layer *layer = findLayer(*m_document, id);
    if (!layer) {
        return reject(DocumentEditCode::LayerNotFound, "layers",
                      "layer was not found");
    }
    if (!std::holds_alternative<KeyframedSource>(layerSource(*layer))) {
        return reject(DocumentEditCode::SourceNotKeyframed, "layer.source",
                      "layer source is not keyframed");
    }
    Keyframe *keyframe = findKeyframe(*m_document, id, frame);
    if (!keyframe) {
        return reject(DocumentEditCode::KeyframeNotFound, "frames",
                      "keyframe was not found");
    }
    if (keyframe->assetId == assetIdValue) {
        return unchanged();
    }
    std::string prior = std::move(keyframe->assetId);
    keyframe->assetId = std::move(assetIdValue);
    if (const std::optional<ValidationIssue> issue = firstValidationIssue(*m_document)) {
        keyframe->assetId = std::move(prior);
        return reject(codeForValidationIssue(*issue), issue->path, issue->message);
    }
    return applied();
}

DocumentEditResult DocumentEditor::moveKeyframe(const std::string &id,
                                                FrameIndex frame,
                                                FrameIndex destinationFrame)
{
    if (m_file) {
        return editFile([&](DocumentEditor &editor) {
            return editor.moveKeyframe(id, frame, destinationFrame);
        });
    }
    if (!requireValidDocument()) {
        return m_lastResult;
    }
    Layer *layer = findLayer(*m_document, id);
    if (!layer) {
        return reject(DocumentEditCode::LayerNotFound, "layers",
                      "layer was not found");
    }
    auto *source = std::get_if<KeyframedSource>(&layerSource(*layer));
    if (!source) {
        return reject(DocumentEditCode::SourceNotKeyframed, "layer.source",
                      "layer source is not keyframed");
    }
    const std::vector<FrameIndex> priorFrameIndices = source->frameIndices;
    Keyframe *keyframe = findKeyframe(*m_document, id, frame);
    if (!keyframe) {
        return reject(DocumentEditCode::KeyframeNotFound, "frames",
                      "keyframe was not found");
    }
    if (frame == destinationFrame) {
        return unchanged();
    }
    if (findKeyframe(*m_document, id, destinationFrame)) {
        return reject(DocumentEditCode::DuplicateKeyframe, "frames",
                      "a keyframe already exists at the destination frame");
    }
    std::vector<Frame> priorFrames = m_document->frames;
    std::string assetIdValue = keyframe->assetId;
    eraseFrameKeyframe(*m_document, frame, id);
    insertFrameKeyframe(*m_document,
                        destinationFrame,
                        Keyframe{id, std::move(assetIdValue)});
    rebuildLayerFrameIndex(*m_document, id);
    if (const std::optional<ValidationIssue> issue = firstValidationIssue(*m_document)) {
        m_document->frames = std::move(priorFrames);
        source->frameIndices = priorFrameIndices;
        return reject(codeForValidationIssue(*issue), issue->path, issue->message);
    }
    return applied();
}

DocumentEditResult DocumentEditor::removeKeyframe(const std::string &id,
                                                  FrameIndex frame)
{
    if (m_file) {
        return editFile([&](DocumentEditor &editor) {
            return editor.removeKeyframe(id, frame);
        });
    }
    if (!requireValidDocument()) {
        return m_lastResult;
    }
    Layer *layer = findLayer(*m_document, id);
    if (!layer) {
        return reject(DocumentEditCode::LayerNotFound, "layers",
                      "layer was not found");
    }
    auto *source = std::get_if<KeyframedSource>(&layerSource(*layer));
    if (!source) {
        return reject(DocumentEditCode::SourceNotKeyframed, "layer.source",
                      "layer source is not keyframed");
    }
    const std::vector<FrameIndex> priorFrameIndices = source->frameIndices;
    if (!findKeyframe(*m_document, id, frame)) {
        return reject(DocumentEditCode::KeyframeNotFound, "frames",
                      "keyframe was not found");
    }
    std::vector<Frame> priorFrames = m_document->frames;
    eraseFrameKeyframe(*m_document, frame, id);
    rebuildLayerFrameIndex(*m_document, id);
    if (const std::optional<ValidationIssue> issue = firstValidationIssue(*m_document)) {
        m_document->frames = std::move(priorFrames);
        source->frameIndices = priorFrameIndices;
        return reject(codeForValidationIssue(*issue), issue->path, issue->message);
    }
    return applied();
}

DocumentEditResult DocumentEditor::insertVectorPath(const std::string &id,
                                                    VectorPath path,
                                                    std::size_t index)
{
    if (m_file) {
        return editFile([&](DocumentEditor &editor) {
            return editor.insertVectorPath(id, std::move(path), index);
        });
    }
    if (!requireValidDocument()) {
        return m_lastResult;
    }
    Asset *asset = findAsset(*m_document, id);
    if (!asset) {
        return reject(DocumentEditCode::AssetNotFound, "assets",
                      "asset was not found");
    }
    auto *vector = std::get_if<VectorAsset>(asset);
    if (!vector) {
        return reject(DocumentEditCode::AssetKindMismatch, "assets",
                      "asset is not vector content");
    }
    const std::size_t position = insertionIndex(index, vector->paths.size());
    if (position > vector->paths.size()) {
        return reject(DocumentEditCode::IndexOutOfRange, "asset.paths",
                      "path insertion index is outside the collection");
    }
    vector->paths.insert(
        vector->paths.begin() + static_cast<std::ptrdiff_t>(position),
        std::move(path));
    if (const std::optional<ValidationIssue> issue = firstValidationIssue(*m_document)) {
        vector->paths.erase(
            vector->paths.begin() + static_cast<std::ptrdiff_t>(position));
        return reject(DocumentEditCode::ValidationRejected, issue->path, issue->message);
    }
    return applied();
}

DocumentEditResult DocumentEditor::replaceVectorPath(const std::string &id,
                                                     std::size_t index,
                                                     VectorPath path)
{
    if (m_file) {
        return editFile([&](DocumentEditor &editor) {
            return editor.replaceVectorPath(id, index, std::move(path));
        });
    }
    if (!requireValidDocument()) {
        return m_lastResult;
    }
    Asset *asset = findAsset(*m_document, id);
    if (!asset) {
        return reject(DocumentEditCode::AssetNotFound, "assets",
                      "asset was not found");
    }
    auto *vector = std::get_if<VectorAsset>(asset);
    if (!vector) {
        return reject(DocumentEditCode::AssetKindMismatch, "assets",
                      "asset is not vector content");
    }
    if (index >= vector->paths.size()) {
        return reject(DocumentEditCode::IndexOutOfRange, "asset.paths",
                      "path index is outside the collection");
    }
    VectorPath prior = std::move(vector->paths[index]);
    vector->paths[index] = std::move(path);
    if (const std::optional<ValidationIssue> issue = firstValidationIssue(*m_document)) {
        vector->paths[index] = std::move(prior);
        return reject(DocumentEditCode::ValidationRejected, issue->path, issue->message);
    }
    return applied();
}

DocumentEditResult DocumentEditor::moveVectorPath(const std::string &id,
                                                  std::size_t index,
                                                  std::size_t destinationIndex)
{
    if (m_file) {
        return editFile([&](DocumentEditor &editor) {
            return editor.moveVectorPath(id, index, destinationIndex);
        });
    }
    if (!requireValidDocument()) {
        return m_lastResult;
    }
    Asset *asset = findAsset(*m_document, id);
    if (!asset) {
        return reject(DocumentEditCode::AssetNotFound, "assets",
                      "asset was not found");
    }
    auto *vector = std::get_if<VectorAsset>(asset);
    if (!vector) {
        return reject(DocumentEditCode::AssetKindMismatch, "assets",
                      "asset is not vector content");
    }
    if (index >= vector->paths.size() || destinationIndex >= vector->paths.size()) {
        return reject(DocumentEditCode::IndexOutOfRange, "asset.paths",
                      "path source or destination index is outside the collection");
    }
    if (index == destinationIndex) {
        return unchanged();
    }
    moveElement(vector->paths, index, destinationIndex);
    return applied();
}

DocumentEditResult DocumentEditor::removeVectorPath(const std::string &id,
                                                    std::size_t index)
{
    if (m_file) {
        return editFile([&](DocumentEditor &editor) {
            return editor.removeVectorPath(id, index);
        });
    }
    if (!requireValidDocument()) {
        return m_lastResult;
    }
    Asset *asset = findAsset(*m_document, id);
    if (!asset) {
        return reject(DocumentEditCode::AssetNotFound, "assets",
                      "asset was not found");
    }
    auto *vector = std::get_if<VectorAsset>(asset);
    if (!vector) {
        return reject(DocumentEditCode::AssetKindMismatch, "assets",
                      "asset is not vector content");
    }
    if (index >= vector->paths.size()) {
        return reject(DocumentEditCode::IndexOutOfRange, "asset.paths",
                      "path index is outside the collection");
    }
    vector->paths.erase(vector->paths.begin() + static_cast<std::ptrdiff_t>(index));
    return applied();
}

DocumentEditResult DocumentEditor::reject(DocumentEditCode code,
                                          std::string path,
                                          std::string message)
{
    m_lastResult = {code, false, std::move(path), std::move(message)};
    return m_lastResult;
}

DocumentEditResult DocumentEditor::editFile(
    const std::function<DocumentEditResult(DocumentEditor &)> &edit)
{
    if (!isBound()) {
        return reject(DocumentEditCode::NotBound, "file", "the working-file binding is no longer valid");
    }
    DocumentEditor working = *this;
    working.m_file = nullptr;
    working.m_lastResult = {};
    const auto result = m_file->edit([&](Document &draft) {
        working.m_document = &draft;
        return edit(working).ok();
    });
    if (!working.m_lastResult.ok()) {
        return m_lastResult = std::move(working.m_lastResult);
    }
    if (!result.ok()) {
        return reject(DocumentEditCode::PersistenceFailed, "file", result.message);
    }
    m_revision = working.m_revision;
    m_lastResult = std::move(working.m_lastResult);
    return m_lastResult;
}

DocumentEditResult DocumentEditor::unchanged()
{
    m_lastResult = {};
    return m_lastResult;
}

DocumentEditResult DocumentEditor::applied()
{
    ++m_revision;
    m_lastResult = {DocumentEditCode::None, true, {}, {}};
    return m_lastResult;
}

bool DocumentEditor::requireValidDocument()
{
    if (!isBound()) {
        (void)reject(DocumentEditCode::NotBound, "document",
                     "no document is bound to the editor");
        return false;
    }
    if (const std::optional<ValidationIssue> issue = firstValidationIssue(*m_document)) {
        (void)reject(DocumentEditCode::InvalidDocument, issue->path, issue->message);
        return false;
    }
    return true;
}

} // namespace iiSharedCanvas
