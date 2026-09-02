#include "Document/DocumentEditor.h"

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
            continue;
        }
        auto &source = std::get<KeyframedSource>(sourceValue);
        for (Keyframe &keyframe : source.keyframes) {
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

bool sameKeyframes(const std::vector<Keyframe> &first,
                   const std::vector<Keyframe> &second)
{
    return first.size() == second.size()
        && std::equal(first.begin(), first.end(), second.begin(),
                      [](const Keyframe &left, const Keyframe &right) {
                          return left.frame == right.frame
                              && left.assetId == right.assetId;
                      });
}

DocumentEditCode codeForValidationIssue(const ValidationIssue &issue) noexcept
{
    return issue.code == ValidationCode::ContentKindMismatch
        ? DocumentEditCode::AssetKindMismatch
        : DocumentEditCode::ValidationRejected;
}

} // namespace

DocumentEditor::DocumentEditor(Document &value)
{
    bind(value);
}

DocumentEditResult DocumentEditor::bind(Document &value)
{
    m_document = nullptr;
    m_revision = 0;
    if (const std::optional<ValidationIssue> issue = firstValidationIssue(value)) {
        return reject(DocumentEditCode::InvalidDocument,
                      issue->path,
                      issue->message);
    }
    m_document = &value;
    return unchanged();
}

void DocumentEditor::unbind() noexcept
{
    m_document = nullptr;
    m_revision = 0;
    m_lastResult = {};
}

bool DocumentEditor::isBound() const noexcept
{
    return m_document != nullptr;
}

Document *DocumentEditor::document() noexcept
{
    return m_document;
}

const Document *DocumentEditor::document() const noexcept
{
    return m_document;
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
    m_document->formatVersion = {CurrentFormatMajor, CurrentFormatMinor};
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

DocumentEditResult DocumentEditor::insertLayer(Layer layer, std::size_t index)
{
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

    m_document->layers.insert(
        m_document->layers.begin() + static_cast<std::ptrdiff_t>(position),
        std::move(layer));
    if (const std::optional<ValidationIssue> issue = firstValidationIssue(*m_document)) {
        m_document->layers.erase(
            m_document->layers.begin() + static_cast<std::ptrdiff_t>(position));
        return reject(codeForValidationIssue(*issue), issue->path, issue->message);
    }
    return applied();
}

DocumentEditResult DocumentEditor::replaceLayer(const std::string &id, Layer layer)
{
    if (!requireValidDocument()) {
        return m_lastResult;
    }
    const std::optional<std::size_t> position = layerIndex(*m_document, id);
    if (!position) {
        return reject(DocumentEditCode::LayerNotFound, "layers",
                      "layer was not found");
    }
    const std::string &replacementId = layerProperties(layer).id;
    if (replacementId.empty()) {
        return reject(DocumentEditCode::InvalidArgument, "layer.id",
                      "replacement layer id must not be empty");
    }
    if (replacementId != id && findLayer(*m_document, replacementId)) {
        return reject(DocumentEditCode::DuplicateLayerId, "layer.id",
                      "replacement layer id already exists");
    }

    Layer prior = std::move(m_document->layers[*position]);
    m_document->layers[*position] = std::move(layer);
    if (const std::optional<ValidationIssue> issue = firstValidationIssue(*m_document)) {
        m_document->layers[*position] = std::move(prior);
        return reject(codeForValidationIssue(*issue), issue->path, issue->message);
    }
    return applied();
}

DocumentEditResult DocumentEditor::renameLayer(const std::string &id,
                                               std::string replacementId)
{
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
    layerProperties(*layer).id = std::move(replacementId);
    return applied();
}

DocumentEditResult DocumentEditor::setLayerName(const std::string &id,
                                                std::string name)
{
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

DocumentEditResult DocumentEditor::setStaticSource(const std::string &id,
                                                   std::string assetIdValue)
{
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
    sourceValue = StaticSource{std::move(assetIdValue)};
    if (const std::optional<ValidationIssue> issue = firstValidationIssue(*m_document)) {
        sourceValue = std::move(prior);
        return reject(codeForValidationIssue(*issue), issue->path, issue->message);
    }
    return applied();
}

DocumentEditResult DocumentEditor::setKeyframedSource(const std::string &id,
                                                      std::vector<Keyframe> keyframes)
{
    if (!requireValidDocument()) {
        return m_lastResult;
    }
    Layer *layer = findLayer(*m_document, id);
    if (!layer) {
        return reject(DocumentEditCode::LayerNotFound, "layers",
                      "layer was not found");
    }
    LayerSource &sourceValue = layerSource(*layer);
    if (const auto *source = std::get_if<KeyframedSource>(&sourceValue);
        source && sameKeyframes(source->keyframes, keyframes)) {
        return unchanged();
    }
    LayerSource prior = std::move(sourceValue);
    sourceValue = KeyframedSource{std::move(keyframes)};
    if (const std::optional<ValidationIssue> issue = firstValidationIssue(*m_document)) {
        sourceValue = std::move(prior);
        return reject(codeForValidationIssue(*issue), issue->path, issue->message);
    }
    return applied();
}

DocumentEditResult DocumentEditor::moveLayer(const std::string &id,
                                             std::size_t destinationIndex)
{
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
    if (!requireValidDocument()) {
        return m_lastResult;
    }
    const std::optional<std::size_t> position = layerIndex(*m_document, id);
    if (!position) {
        return reject(DocumentEditCode::LayerNotFound, "layers",
                      "layer was not found");
    }
    m_document->layers.erase(
        m_document->layers.begin() + static_cast<std::ptrdiff_t>(*position));
    return applied();
}

DocumentEditResult DocumentEditor::insertKeyframe(const std::string &id,
                                                  Keyframe keyframe)
{
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
    const auto insertion = std::lower_bound(
        source->keyframes.begin(), source->keyframes.end(), keyframe.frame,
        [](const Keyframe &value, FrameIndex frame) { return value.frame < frame; });
    if (insertion != source->keyframes.end() && insertion->frame == keyframe.frame) {
        return reject(DocumentEditCode::DuplicateKeyframe, "layer.source.keyframes",
                      "a keyframe already exists at the requested frame");
    }
    const std::size_t position = static_cast<std::size_t>(
        std::distance(source->keyframes.begin(), insertion));
    source->keyframes.insert(insertion, std::move(keyframe));
    if (const std::optional<ValidationIssue> issue = firstValidationIssue(*m_document)) {
        source->keyframes.erase(
            source->keyframes.begin() + static_cast<std::ptrdiff_t>(position));
        return reject(codeForValidationIssue(*issue), issue->path, issue->message);
    }
    return applied();
}

DocumentEditResult DocumentEditor::setKeyframeAsset(const std::string &id,
                                                    FrameIndex frame,
                                                    std::string assetIdValue)
{
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
    Keyframe *keyframe = findKeyframe(*source, frame);
    if (!keyframe) {
        return reject(DocumentEditCode::KeyframeNotFound, "layer.source.keyframes",
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
    Keyframe *keyframe = findKeyframe(*source, frame);
    if (!keyframe) {
        return reject(DocumentEditCode::KeyframeNotFound, "layer.source.keyframes",
                      "keyframe was not found");
    }
    if (frame == destinationFrame) {
        return unchanged();
    }
    if (findKeyframe(*source, destinationFrame)) {
        return reject(DocumentEditCode::DuplicateKeyframe, "layer.source.keyframes",
                      "a keyframe already exists at the destination frame");
    }
    std::vector<Keyframe> prior = source->keyframes;
    keyframe->frame = destinationFrame;
    std::sort(source->keyframes.begin(), source->keyframes.end(),
              [](const Keyframe &left, const Keyframe &right) {
                  return left.frame < right.frame;
              });
    if (const std::optional<ValidationIssue> issue = firstValidationIssue(*m_document)) {
        source->keyframes = std::move(prior);
        return reject(codeForValidationIssue(*issue), issue->path, issue->message);
    }
    return applied();
}

DocumentEditResult DocumentEditor::removeKeyframe(const std::string &id,
                                                  FrameIndex frame)
{
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
    const std::optional<std::size_t> position = keyframeIndex(*source, frame);
    if (!position) {
        return reject(DocumentEditCode::KeyframeNotFound, "layer.source.keyframes",
                      "keyframe was not found");
    }
    Keyframe removed = std::move(source->keyframes[*position]);
    source->keyframes.erase(
        source->keyframes.begin() + static_cast<std::ptrdiff_t>(*position));
    if (const std::optional<ValidationIssue> issue = firstValidationIssue(*m_document)) {
        source->keyframes.insert(
            source->keyframes.begin() + static_cast<std::ptrdiff_t>(*position),
            std::move(removed));
        return reject(codeForValidationIssue(*issue), issue->path, issue->message);
    }
    return applied();
}

DocumentEditResult DocumentEditor::insertVectorPath(const std::string &id,
                                                    VectorPath path,
                                                    std::size_t index)
{
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
    if (!m_document) {
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
