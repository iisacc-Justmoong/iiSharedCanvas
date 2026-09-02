#include "QtAdapter/CanvasItem.h"

#include "Render/FrameRenderer.h"
#include "Validation/Validation.h"

#include <Input/PressureInput.h>

#include <QImage>
#include <QMatrix4x4>
#include <QMouseEvent>
#include <QPainter>
#include <QPointingDevice>
#include <QQuickWindow>
#include <QSGOpacityNode>
#include <QSGRendererInterface>
#include <QSGSimpleTextureNode>
#include <QSGTransformNode>
#include <QTabletEvent>
#include <QTimer>
#include <QWheelEvent>

#include <algorithm>
#include <cmath>
#include <limits>
#include <tuple>
#include <utility>

namespace iiSharedCanvas {
namespace {

constexpr int RenderTilePixelSize = 512;
constexpr std::size_t MaximumResidentTiles = 64;
constexpr std::size_t MaximumResidentLayerTiles = 256;
constexpr std::uint64_t MaximumContiguousCompatibilityPixels = 16U * 1024U * 1024U;

class CanvasSceneNode final : public QSGTransformNode {
public:
    std::uint64_t uploadedTileRevision = std::numeric_limits<std::uint64_t>::max();
};

std::int64_t floorDivision(std::int64_t value, std::int64_t divisor) noexcept
{
    const std::int64_t quotient = value / divisor;
    const std::int64_t remainder = value % divisor;
    return remainder < 0 ? quotient - 1 : quotient;
}

bool sameRegion(CanvasRegion first, CanvasRegion second) noexcept
{
    return first.origin.x == second.origin.x
        && first.origin.y == second.origin.y
        && first.extent.width == second.extent.width
        && first.extent.height == second.extent.height;
}

bool regionsOverlap(CanvasRegion first, CanvasRegion second) noexcept
{
    const std::int64_t firstRight = static_cast<std::int64_t>(first.origin.x)
        + first.extent.width;
    const std::int64_t firstBottom = static_cast<std::int64_t>(first.origin.y)
        + first.extent.height;
    const std::int64_t secondRight = static_cast<std::int64_t>(second.origin.x)
        + second.extent.width;
    const std::int64_t secondBottom = static_cast<std::int64_t>(second.origin.y)
        + second.extent.height;
    return first.origin.x < secondRight && second.origin.x < firstRight
        && first.origin.y < secondBottom && second.origin.y < firstBottom;
}

int lodForZoom(qreal zoom) noexcept
{
    int lod = 1;
    const double desired = 1.0 / std::max(zoom, qreal{0.000001});
    while (lod < desired && lod <= (1 << 20)) {
        lod *= 2;
    }
    return lod;
}

QString normalizedToolMode(const QString &mode)
{
    const QString normalized = mode.trimmed().toLower();
    return normalized == QStringLiteral("eraser")
            || normalized == QStringLiteral("pan")
            || normalized == QStringLiteral("move")
            || normalized == QStringLiteral("zoom")
            || normalized == QStringLiteral("fill")
            || normalized == QStringLiteral("text")
            || normalized == QStringLiteral("shape")
        ? normalized
        : QStringLiteral("brush");
}

bool isTabletEraser(const QPointingDevice *device)
{
    return device && device->pointerType() == QPointingDevice::PointerType::Eraser;
}

} // namespace

CanvasItem::CanvasItem(QQuickItem *parent)
    : QQuickItem(parent)
{
    setFlag(ItemHasContents, true);
    setClip(true);
    setAcceptedMouseButtons(Qt::LeftButton | Qt::MiddleButton);
    setAcceptHoverEvents(false);
    connect(&m_asyncRenderer, &AsyncFrameRenderer::busyChanged,
            this, &CanvasItem::updateRenderingState,
            Qt::QueuedConnection);
    connect(&m_asyncRenderer, &AsyncFrameRenderer::finished,
            this, &CanvasItem::applyAsyncRender);
    connect(this, &QQuickItem::windowChanged,
            this, [this](QQuickWindow *) { emit graphicsBackendChanged(); });
}

bool CanvasItem::bind(Document &value)
{
    const ValidationResult validation = validate(value);
    if (!validation.ok()) {
        setLastError(QString::fromStdString(validation.issues.front().path + ": "
                                             + validation.issues.front().message));
        return false;
    }

    const bool hadSelection = !m_selectedLayerId.empty();
    m_asyncRenderer.cancel();
    m_editor.unbind();
    m_chunkedEditor.unbind();
    m_selectedLayerId.clear();
    resetEditState();
    m_document = &value;
    const DocumentEditResult editorResult = m_documentEditor.bind(value);
    if (!editorResult.ok()) {
        m_document = nullptr;
        setLastError(QString::fromStdString(editorResult.message));
        return false;
    }
    m_frame = 0;
    m_documentValid = true;
    m_framePixels = {};
    m_renderSnapshot.reset();
    clearTileCache();
    resetView();
    scheduleVisibleRender(true);
    setLastError({});
    emit documentChanged();
    emit frameChanged();
    if (hadSelection) {
        emit selectionChanged();
        emit undoRedoChanged();
    }
    return true;
}

void CanvasItem::unbind()
{
    const bool hadDocument = m_document != nullptr;
    const bool hadSelection = !m_selectedLayerId.empty();
    m_asyncRenderer.cancel();
    m_renderSchedulePending = false;
    m_editor.unbind();
    m_chunkedEditor.unbind();
    m_selectedLayerId.clear();
    resetEditState();
    m_documentEditor.unbind();
    m_document = nullptr;
    m_documentValid = false;
    m_renderSnapshot.reset();
    m_framePixels = {};
    clearTileCache();
    m_frame = 0;
    m_panning = false;
    if (hadDocument) {
        ++m_revision;
    }
    resetView();
    setLastError({});
    if (hadDocument) {
        emit documentChanged();
        emit frameChanged();
        emit revisionChanged();
    }
    if (hadSelection) {
        emit selectionChanged();
        emit undoRedoChanged();
    }
    update();
    updateRenderingState();
}

Document *CanvasItem::document() noexcept
{
    return m_document;
}

const Document *CanvasItem::document() const noexcept
{
    return m_document;
}

DocumentEditor *CanvasItem::documentEditor() noexcept
{
    return m_documentEditor.isBound() ? &m_documentEditor : nullptr;
}

const DocumentEditor *CanvasItem::documentEditor() const noexcept
{
    return m_documentEditor.isBound() ? &m_documentEditor : nullptr;
}

DocumentEditResult CanvasItem::editDocument(
    const std::function<DocumentEditResult(DocumentEditor &)> &edit)
{
    if (!m_document || !m_documentEditor.isBound()) {
        const DocumentEditResult result{
            DocumentEditCode::NotBound,
            false,
            "document",
            "no document is bound to the canvas item",
        };
        setLastError(QString::fromStdString(result.message));
        return result;
    }
    if (!edit) {
        const DocumentEditResult result{
            DocumentEditCode::InvalidArgument,
            false,
            "edit",
            "document edit callback must not be empty",
        };
        setLastError(QString::fromStdString(result.message));
        return result;
    }

    const DocumentEditResult result = edit(m_documentEditor);
    if (!result.ok()) {
        setLastError(QString::fromStdString(result.message));
        return result;
    }
    if (!result.changed) {
        setLastError({});
        return result;
    }

    bool currentFrameChanged = false;
    if (m_frame >= m_document->timeline.frameCount) {
        m_frame = m_document->timeline.frameCount - 1;
        currentFrameChanged = true;
    }
    const bool refreshed = refresh();
    if (currentFrameChanged) {
        emit frameChanged();
    }
    if (!refreshed && lastError().isEmpty()) {
        setLastError(QStringLiteral("document edit applied but frame refresh failed"));
    }
    return result;
}

const RasterLayer *CanvasItem::framePixels() const noexcept
{
    return documentReady()
            && m_framePixels.width == m_document->extent.width
            && m_framePixels.height == m_document->extent.height
        ? &m_framePixels
        : nullptr;
}

bool CanvasItem::createDocument(int width, int height, quint32 count)
{
    if (width <= 0 || height <= 0 || count == 0) {
        setLastError(QStringLiteral("canvas dimensions and frame count must be positive"));
        return false;
    }
    const std::size_t pixelWidth = static_cast<std::size_t>(width);
    const std::size_t pixelHeight = static_cast<std::size_t>(height);
    if (pixelWidth > std::numeric_limits<std::size_t>::max() / pixelHeight) {
        setLastError(QStringLiteral("canvas pixel count is too large"));
        return false;
    }

    m_ownedDocument = {};
    m_ownedDocument.extent = {width, height};
    m_ownedDocument.timeline = {{24, 1}, count};
    return bind(m_ownedDocument);
}

bool CanvasItem::createRasterDocument(int width, int height, quint32 count)
{
    if (width <= 0 || height <= 0 || count == 0) {
        setLastError(QStringLiteral("canvas dimensions and frame count must be positive"));
        return false;
    }
    const std::size_t pixelWidth = static_cast<std::size_t>(width);
    const std::size_t pixelHeight = static_cast<std::size_t>(height);
    if (pixelWidth > std::numeric_limits<std::size_t>::max() / pixelHeight) {
        setLastError(QStringLiteral("canvas pixel count is too large"));
        return false;
    }

    m_ownedDocument = {};
    m_ownedDocument.extent = {width, height};
    m_ownedDocument.timeline = {{24, 1}, count};
    m_ownedDocument.assets.emplace_back(
        RasterAsset{"canvas.raster.0", makeRasterLayer(width, height, 0x00000000U)});
    m_ownedDocument.layers.emplace_back(BitmapLayer{
        {"canvas.layer.0", "Raster", true, 1.0, {}, RasterBlendMode::SourceOver},
        StaticSource{"canvas.raster.0"},
    });
    return bind(m_ownedDocument) && selectLayer(QStringLiteral("canvas.layer.0"));
}

bool CanvasItem::createInfiniteRasterDocument(int width,
                                              int height,
                                              int chunkSize,
                                              quint32 count)
{
    if (width <= 0 || height <= 0 || chunkSize <= 0 || count == 0) {
        setLastError(QStringLiteral("canvas dimensions, chunk size, and frame count must be positive"));
        return false;
    }

    m_ownedDocument = {};
    m_ownedDocument.canvasMode = CanvasMode::Infinite;
    m_ownedDocument.infiniteCanvas = {{0, 0}, chunkSize};
    m_ownedDocument.extent = {width, height};
    m_ownedDocument.timeline = {{24, 1}, count};
    m_ownedDocument.assets.emplace_back(ChunkedRasterAsset{"canvas.raster.0", {}});
    m_ownedDocument.layers.emplace_back(BitmapLayer{
        {"canvas.layer.0", "Raster", true, 1.0, {}, RasterBlendMode::SourceOver},
        StaticSource{"canvas.raster.0"},
    });
    return bind(m_ownedDocument) && selectLayer(QStringLiteral("canvas.layer.0"));
}

bool CanvasItem::documentReady() const noexcept
{
    return m_document != nullptr && m_documentValid;
}

int CanvasItem::canvasWidth() const noexcept
{
    return m_document ? m_document->extent.width : 0;
}

int CanvasItem::canvasHeight() const noexcept
{
    return m_document ? m_document->extent.height : 0;
}

bool CanvasItem::infiniteCanvas() const noexcept
{
    return m_document && m_document->canvasMode == CanvasMode::Infinite;
}

int CanvasItem::canvasOriginX() const noexcept
{
    return m_document ? canvasOrigin(*m_document).x : 0;
}

int CanvasItem::canvasOriginY() const noexcept
{
    return m_document ? canvasOrigin(*m_document).y : 0;
}

int CanvasItem::canvasChunkSize() const noexcept
{
    return infiniteCanvas() ? m_document->infiniteCanvas.chunkSize : 0;
}

QVariantMap CanvasItem::ensureInfiniteCanvasRegion(qreal x,
                                                   qreal y,
                                                   qreal width,
                                                   qreal height)
{
    QVariantMap result{{QStringLiteral("changed"), false},
                       {QStringLiteral("left"), 0},
                       {QStringLiteral("top"), 0},
                       {QStringLiteral("right"), 0},
                       {QStringLiteral("bottom"), 0}};
    if (!infiniteCanvas() || !std::isfinite(x) || !std::isfinite(y)
        || !std::isfinite(width) || !std::isfinite(height)
        || width <= 0.0 || height <= 0.0) {
        result.insert(QStringLiteral("error"), QStringLiteral("invalid infinite canvas region"));
        return result;
    }

    const double left = std::floor(x);
    const double top = std::floor(y);
    const double right = std::ceil(x + width);
    const double bottom = std::ceil(y + height);
    if (left < std::numeric_limits<std::int32_t>::min()
        || top < std::numeric_limits<std::int32_t>::min()
        || right > std::numeric_limits<std::int32_t>::max()
        || bottom > std::numeric_limits<std::int32_t>::max()) {
        result.insert(QStringLiteral("error"), QStringLiteral("infinite canvas region exceeds supported coordinates"));
        return result;
    }

    const CanvasOrigin oldOrigin = canvasOrigin(*m_document);
    const CanvasExtent oldExtent = m_document->extent;
    const CanvasRegion requested{
        {static_cast<std::int32_t>(left), static_cast<std::int32_t>(top)},
        {static_cast<std::int32_t>(right - left),
         static_cast<std::int32_t>(bottom - top)},
    };
    const DocumentEditResult edit = editDocument(
        [requested](DocumentEditor &editor) {
            return editor.ensureInfiniteCanvasRegion(requested);
        });
    if (!edit.ok()) {
        result.insert(QStringLiteral("error"), QString::fromStdString(edit.message));
        return result;
    }
    if (!edit.changed) {
        return result;
    }

    const CanvasOrigin newOrigin = canvasOrigin(*m_document);
    result[QStringLiteral("changed")] = true;
    result[QStringLiteral("left")] = oldOrigin.x - newOrigin.x;
    result[QStringLiteral("top")] = oldOrigin.y - newOrigin.y;
    result[QStringLiteral("right")] = m_document->extent.width - oldExtent.width
        - result.value(QStringLiteral("left")).toInt();
    result[QStringLiteral("bottom")] = m_document->extent.height - oldExtent.height
        - result.value(QStringLiteral("top")).toInt();
    return result;
}

quint32 CanvasItem::frame() const noexcept
{
    return m_frame;
}

void CanvasItem::setFrame(quint32 value)
{
    if (!m_document) {
        setLastError(QStringLiteral("no document is bound"));
        return;
    }
    if (value >= m_document->timeline.frameCount) {
        setLastError(QStringLiteral("requested frame is outside the document timeline"));
        return;
    }
    if (value == m_frame) {
        setLastError({});
        return;
    }

    m_frame = value;
    syncSelectedLayer();
    scheduleVisibleRender(true);
    setLastError({});
    emit frameChanged();
}

quint32 CanvasItem::frameCount() const noexcept
{
    return m_document ? m_document->timeline.frameCount : 0;
}

bool CanvasItem::selectLayer(const QString &layerId)
{
    if (!m_document) {
        setLastError(QStringLiteral("no document is bound"));
        return false;
    }
    const std::string requestedId = layerId.toUtf8().toStdString();
    const auto match = std::find_if(m_document->layers.begin(),
                                    m_document->layers.end(),
                                    [&](const Layer &layer) {
                                        return layerProperties(layer).id == requestedId;
                                    });
    if (match == m_document->layers.end()) {
        setLastError(QStringLiteral("document layer was not found"));
        return false;
    }
    const Asset *asset = resolveAssetAt(*m_document, *match, m_frame);
    if (!std::holds_alternative<BitmapLayer>(*match)
        || !asset
        || contentKind(*asset) != ContentKind::Raster) {
        setLastError(QStringLiteral("selected layer does not resolve to raster content"));
        return false;
    }

    const std::string &resolvedAssetId = assetId(*asset);
    const bool changed = m_selectedLayerId != requestedId;
    if ((m_editor.isBound() && m_editor.boundAssetId() == resolvedAssetId)
        || (m_chunkedEditor.isBound()
            && m_chunkedEditor.boundAssetId() == resolvedAssetId)) {
        m_selectedLayerId = requestedId;
        setLastError({});
        if (changed) {
            emit selectionChanged();
        }
        return true;
    }

    const bool priorCanUndo = activeEditorCanUndo();
    const bool priorCanRedo = activeEditorCanRedo();
    bool bound = false;
    if (std::holds_alternative<RasterAsset>(*asset)) {
        m_chunkedEditor.unbind();
        bound = m_editor.bind(*m_document, resolvedAssetId);
    } else {
        m_editor.unbind();
        bound = m_chunkedEditor.bind(*m_document, resolvedAssetId)
            && m_chunkedEditor.setBrush(m_editor.brush());
    }
    if (!bound) {
        setLastError(QString::fromStdString(activeEditorError()));
        return false;
    }
    resetEditState();
    m_selectedLayerId = requestedId;
    setLastError({});
    if (changed) {
        emit selectionChanged();
    }
    notifyUndoRedo(priorCanUndo, priorCanRedo);
    return true;
}

void CanvasItem::clearSelection()
{
    if (m_selectedLayerId.empty() && !m_editor.isBound() && !m_chunkedEditor.isBound()) {
        return;
    }
    const bool priorCanUndo = activeEditorCanUndo();
    const bool priorCanRedo = activeEditorCanRedo();
    m_editor.unbind();
    m_chunkedEditor.unbind();
    resetEditState();
    m_selectedLayerId.clear();
    emit selectionChanged();
    notifyUndoRedo(priorCanUndo, priorCanRedo);
}

QString CanvasItem::selectedLayerId() const
{
    return QString::fromUtf8(m_selectedLayerId);
}

bool CanvasItem::rasterLayerSelected() const noexcept
{
    return (m_editor.isBound() || m_chunkedEditor.isBound())
        && !m_selectedLayerId.empty();
}

const RasterLayer *CanvasItem::selectedRasterPixels() const noexcept
{
    if (m_editor.isBound()) {
        return m_editor.pixels();
    }
    if (!m_document || !m_chunkedEditor.isBound()) {
        return nullptr;
    }
    const ChunkedRasterAsset *chunked = findChunkedRasterAsset(
        *m_document, m_chunkedEditor.boundAssetId());
    if (!chunked) {
        return nullptr;
    }

    const std::uint64_t pixelCount = static_cast<std::uint64_t>(m_document->extent.width)
        * static_cast<std::uint64_t>(m_document->extent.height);
    if (pixelCount > MaximumContiguousCompatibilityPixels) {
        return nullptr;
    }

    m_selectedRasterCache = makeRasterLayer(m_document->extent.width,
                                             m_document->extent.height,
                                             0x00000000U);
    const CanvasOrigin origin = canvasOrigin(*m_document);
    const std::int32_t chunkSize = m_document->infiniteCanvas.chunkSize;
    for (const RasterChunk &chunk : chunked->chunks) {
        const std::int64_t chunkX = static_cast<std::int64_t>(chunk.column) * chunkSize;
        const std::int64_t chunkY = static_cast<std::int64_t>(chunk.row) * chunkSize;
        const std::int32_t sourceLeft = static_cast<std::int32_t>(
            std::max<std::int64_t>(0, origin.x - chunkX));
        const std::int32_t sourceTop = static_cast<std::int32_t>(
            std::max<std::int64_t>(0, origin.y - chunkY));
        const std::int32_t sourceRight = static_cast<std::int32_t>(
            std::min<std::int64_t>(chunkSize,
                                   static_cast<std::int64_t>(origin.x)
                                       + m_document->extent.width - chunkX));
        const std::int32_t sourceBottom = static_cast<std::int32_t>(
            std::min<std::int64_t>(chunkSize,
                                   static_cast<std::int64_t>(origin.y)
                                       + m_document->extent.height - chunkY));
        for (std::int32_t sourceY = sourceTop; sourceY < sourceBottom; ++sourceY) {
            for (std::int32_t sourceX = sourceLeft; sourceX < sourceRight; ++sourceX) {
                const std::int32_t targetX = static_cast<std::int32_t>(chunkX) + sourceX - origin.x;
                const std::int32_t targetY = static_cast<std::int32_t>(chunkY) + sourceY - origin.y;
                m_selectedRasterCache.pixels[
                    static_cast<std::size_t>(targetY) * m_selectedRasterCache.width
                    + static_cast<std::size_t>(targetX)] = rasterLayerPixelAt(
                        chunk.pixels, {sourceX, sourceY});
            }
        }
    }
    return &m_selectedRasterCache;
}

bool CanvasItem::replaceSelectedPixels(const RasterLayer &pixels)
{
    const std::uint64_t priorRevision = activeEditorRevision();
    const int priorStrokeCount = m_strokeCount;
    const bool priorCanUndo = activeEditorCanUndo();
    const bool priorCanRedo = activeEditorCanRedo();
    const bool success = chunkedRasterSelected()
        ? m_chunkedEditor.replaceRegion(canvasOrigin(*m_document), pixels)
        : m_editor.replacePixels(pixels);
    if (success && priorRevision != activeEditorRevision()) {
        recordCountHistory(priorStrokeCount);
    }
    const bool finished = finishEdit(success);
    notifyUndoRedo(priorCanUndo, priorCanRedo);
    return finished;
}

QString CanvasItem::toolMode() const
{
    return m_toolMode;
}

void CanvasItem::setToolMode(const QString &mode)
{
    const QString nextMode = normalizedToolMode(mode);
    if (m_toolMode == nextMode) {
        return;
    }
    m_toolMode = nextMode;
    setEraser(m_toolMode == QStringLiteral("eraser"));
    emit toolModeChanged();
}

QColor CanvasItem::brushColor() const
{
    return QColor::fromRgba(m_editor.brush().argb);
}

void CanvasItem::setBrushColor(const QColor &color)
{
    if (!color.isValid() || color.rgba() == m_editor.brush().argb) {
        return;
    }
    BitmapBrush value = m_editor.brush();
    value.argb = color.rgba();
    applyBrush(value);
}

qreal CanvasItem::brushSize() const noexcept
{
    return m_editor.brush().size;
}

void CanvasItem::setBrushSize(qreal size)
{
    BitmapBrush value = m_editor.brush();
    value.size = size;
    applyBrush(value);
}

qreal CanvasItem::brushOpacity() const noexcept
{
    return m_editor.brush().opacity;
}

void CanvasItem::setBrushOpacity(qreal opacity)
{
    BitmapBrush value = m_editor.brush();
    value.opacity = opacity;
    applyBrush(value);
}

bool CanvasItem::brushOpacityEnabled() const noexcept
{
    return m_editor.brush().opacityEnabled;
}

void CanvasItem::setBrushOpacityEnabled(bool enabled)
{
    if (enabled == m_editor.brush().opacityEnabled) {
        return;
    }
    BitmapBrush value = m_editor.brush();
    value.opacityEnabled = enabled;
    applyBrush(value);
}

qreal CanvasItem::brushFlow() const noexcept
{
    return m_editor.brush().flow;
}

void CanvasItem::setBrushFlow(qreal flow)
{
    BitmapBrush value = m_editor.brush();
    value.flow = flow;
    applyBrush(value);
}

bool CanvasItem::brushFlowEnabled() const noexcept
{
    return m_editor.brush().flowEnabled;
}

void CanvasItem::setBrushFlowEnabled(bool enabled)
{
    if (enabled == m_editor.brush().flowEnabled) {
        return;
    }
    BitmapBrush value = m_editor.brush();
    value.flowEnabled = enabled;
    applyBrush(value);
}

qreal CanvasItem::brushHardness() const noexcept
{
    return m_editor.brush().hardness;
}

void CanvasItem::setBrushHardness(qreal hardness)
{
    BitmapBrush value = m_editor.brush();
    value.hardness = hardness;
    applyBrush(value);
}

bool CanvasItem::brushHardnessEnabled() const noexcept
{
    return m_editor.brush().hardnessEnabled;
}

void CanvasItem::setBrushHardnessEnabled(bool enabled)
{
    if (enabled == m_editor.brush().hardnessEnabled) {
        return;
    }
    BitmapBrush value = m_editor.brush();
    value.hardnessEnabled = enabled;
    applyBrush(value);
}

qreal CanvasItem::brushSpacing() const noexcept
{
    return m_editor.brush().spacing;
}

void CanvasItem::setBrushSpacing(qreal spacing)
{
    BitmapBrush value = m_editor.brush();
    value.spacing = spacing;
    applyBrush(value);
}

qreal CanvasItem::brushSpacingRatio() const noexcept
{
    return m_editor.brush().spacingRatio;
}

void CanvasItem::setBrushSpacingRatio(qreal spacingRatio)
{
    BitmapBrush value = m_editor.brush();
    value.spacingRatio = spacingRatio;
    applyBrush(value);
}

bool CanvasItem::brushSpacingEnabled() const noexcept
{
    return m_editor.brush().spacingEnabled;
}

void CanvasItem::setBrushSpacingEnabled(bool enabled)
{
    if (enabled == m_editor.brush().spacingEnabled) {
        return;
    }
    BitmapBrush value = m_editor.brush();
    value.spacingEnabled = enabled;
    applyBrush(value);
}

qreal CanvasItem::pressureCurveMinimum() const noexcept
{
    return m_pressureCurveMinimum;
}

void CanvasItem::setPressureCurveMinimum(qreal value)
{
    if (!std::isfinite(value)) {
        return;
    }
    const qreal nextMinimum = std::clamp(value, qreal{0.0}, qreal{1.0});
    const qreal nextCenter = std::max(m_pressureCurveCenter, nextMinimum);
    const qreal nextMaximum = std::max(m_pressureCurveMaximum, nextMinimum);
    if (m_pressureCurveMinimum == nextMinimum
        && m_pressureCurveCenter == nextCenter
        && m_pressureCurveMaximum == nextMaximum) {
        return;
    }
    m_pressureCurveMinimum = nextMinimum;
    m_pressureCurveCenter = nextCenter;
    m_pressureCurveMaximum = nextMaximum;
    emit strokeSettingsChanged();
}

qreal CanvasItem::pressureCurveCenter() const noexcept
{
    return m_pressureCurveCenter;
}

void CanvasItem::setPressureCurveCenter(qreal value)
{
    if (!std::isfinite(value)) {
        return;
    }
    const qreal nextCenter = std::clamp(value,
                                        m_pressureCurveMinimum,
                                        m_pressureCurveMaximum);
    if (m_pressureCurveCenter == nextCenter) {
        return;
    }
    m_pressureCurveCenter = nextCenter;
    emit strokeSettingsChanged();
}

qreal CanvasItem::pressureCurveMaximum() const noexcept
{
    return m_pressureCurveMaximum;
}

void CanvasItem::setPressureCurveMaximum(qreal value)
{
    if (!std::isfinite(value)) {
        return;
    }
    const qreal nextMaximum = std::clamp(value, qreal{0.0}, qreal{1.0});
    const qreal nextMinimum = std::min(m_pressureCurveMinimum, nextMaximum);
    const qreal nextCenter = std::clamp(m_pressureCurveCenter,
                                        nextMinimum,
                                        nextMaximum);
    if (m_pressureCurveMinimum == nextMinimum
        && m_pressureCurveCenter == nextCenter
        && m_pressureCurveMaximum == nextMaximum) {
        return;
    }
    m_pressureCurveMinimum = nextMinimum;
    m_pressureCurveCenter = nextCenter;
    m_pressureCurveMaximum = nextMaximum;
    emit strokeSettingsChanged();
}

bool CanvasItem::pressureToOpacityEnabled() const noexcept
{
    return m_editor.brush().pressureToOpacityEnabled;
}

void CanvasItem::setPressureToOpacityEnabled(bool enabled)
{
    if (enabled == m_editor.brush().pressureToOpacityEnabled) {
        return;
    }
    BitmapBrush value = m_editor.brush();
    value.pressureToOpacityEnabled = enabled;
    applyBrush(value);
}

qreal CanvasItem::stabilizerStrength() const noexcept
{
    return m_stabilizerStrength;
}

void CanvasItem::setStabilizerStrength(qreal value)
{
    if (!std::isfinite(value)) {
        return;
    }
    const qreal nextValue = std::clamp(value, qreal{0.0}, qreal{1.0});
    if (m_stabilizerStrength == nextValue) {
        return;
    }
    m_stabilizerStrength = nextValue;
    emit strokeSettingsChanged();
}

int CanvasItem::livePreviewFrameIntervalMs() const noexcept
{
    return m_livePreviewFrameIntervalMs;
}

void CanvasItem::setLivePreviewFrameIntervalMs(int value)
{
    const int nextValue = std::clamp(value, 1, 1000);
    if (m_livePreviewFrameIntervalMs == nextValue) {
        return;
    }
    m_livePreviewFrameIntervalMs = nextValue;
    emit livePreviewFrameIntervalMsChanged();
}

bool CanvasItem::multithreadedEventsEnabled() const noexcept
{
    return m_multithreadedEventsEnabled;
}

void CanvasItem::setMultithreadedEventsEnabled(bool enabled)
{
    if (m_multithreadedEventsEnabled == enabled) {
        return;
    }
    m_multithreadedEventsEnabled = enabled;
    emit multithreadedEventsEnabledChanged();
}

bool CanvasItem::eraser() const noexcept
{
    return m_editor.brush().eraser;
}

void CanvasItem::setEraser(bool enabled)
{
    if (enabled == m_editor.brush().eraser) {
        return;
    }
    BitmapBrush value = m_editor.brush();
    value.eraser = enabled;
    applyBrush(value);
}

bool CanvasItem::clearSelectedLayer()
{
    const std::uint64_t priorRevision = activeEditorRevision();
    const int priorStrokeCount = m_strokeCount;
    const bool priorCanUndo = activeEditorCanUndo();
    const bool priorCanRedo = activeEditorCanRedo();
    const bool success = chunkedRasterSelected()
        ? m_chunkedEditor.clear()
        : m_editor.clear();
    if (success && priorRevision != activeEditorRevision()) {
        recordCountHistory(priorStrokeCount);
        if (m_strokeCount != 0) {
            m_strokeCount = 0;
            emit strokeCountChanged();
        }
    }
    const bool finished = finishEdit(success);
    notifyUndoRedo(priorCanUndo, priorCanRedo);
    return finished;
}

bool CanvasItem::beginStrokeAt(const QPointF &documentPosition, qreal pressure)
{
    const bool wasActive = activeEditorStrokeActive();
    m_stabilizerActive = true;
    m_stabilizedPosition = documentPosition;
    DocumentPoint assetPosition;
    if (!mapDocumentToSelectedAsset(stabilizedDocumentPosition(documentPosition, false),
                                    assetPosition)) {
        m_stabilizerActive = false;
        return false;
    }
    const bool priorCanUndo = activeEditorCanUndo();
    const bool priorCanRedo = activeEditorCanRedo();
    m_strokeStartRevision = activeEditorRevision();
    m_strokeStartCount = m_strokeCount;
    const bool success = chunkedRasterSelected()
        ? m_chunkedEditor.beginStroke(assetPosition, normalizedPressure(pressure, true))
        : m_editor.beginStroke(assetPosition, normalizedPressure(pressure, true));
    if (!success) {
        m_stabilizerActive = false;
    }
    const bool finished = finishEdit(success);
    notifyUndoRedo(priorCanUndo, priorCanRedo);
    if (wasActive != activeEditorStrokeActive()) {
        emit liveStrokeActiveChanged();
    }
    return finished;
}

bool CanvasItem::continueStrokeAt(const QPointF &documentPosition, qreal pressure)
{
    DocumentPoint assetPosition;
    if (!mapDocumentToSelectedAsset(stabilizedDocumentPosition(documentPosition, false),
                                    assetPosition)) {
        return false;
    }
    const bool priorCanUndo = activeEditorCanUndo();
    const bool priorCanRedo = activeEditorCanRedo();
    const bool success = chunkedRasterSelected()
        ? m_chunkedEditor.continueStroke(assetPosition, normalizedPressure(pressure, true))
        : m_editor.continueStroke(assetPosition, normalizedPressure(pressure, true));
    const bool finished = finishEdit(success);
    notifyUndoRedo(priorCanUndo, priorCanRedo);
    return finished;
}

bool CanvasItem::endStrokeAt(const QPointF &documentPosition, qreal pressure)
{
    const bool wasActive = activeEditorStrokeActive();
    DocumentPoint assetPosition;
    if (!mapDocumentToSelectedAsset(stabilizedDocumentPosition(documentPosition, true),
                                    assetPosition)) {
        return false;
    }
    const bool priorCanUndo = activeEditorCanUndo();
    const bool priorCanRedo = activeEditorCanRedo();
    const bool success = chunkedRasterSelected()
        ? m_chunkedEditor.endStroke(assetPosition, normalizedPressure(pressure, true))
        : m_editor.endStroke(assetPosition, normalizedPressure(pressure, true));
    m_stabilizerActive = false;
    if (success && m_strokeStartRevision != activeEditorRevision()) {
        recordCountHistory(m_strokeStartCount);
        ++m_strokeCount;
        emit strokeCountChanged();
    }
    const bool finished = finishEdit(success);
    notifyUndoRedo(priorCanUndo, priorCanRedo);
    if (wasActive != activeEditorStrokeActive()) {
        emit liveStrokeActiveChanged();
    }
    return finished;
}

void CanvasItem::cancelStroke()
{
    const bool wasActive = activeEditorStrokeActive();
    const bool priorCanUndo = activeEditorCanUndo();
    const bool priorCanRedo = activeEditorCanRedo();
    if (chunkedRasterSelected()) {
        m_chunkedEditor.cancelStroke();
    } else {
        m_editor.cancelStroke();
    }
    m_stabilizerActive = false;
    refresh();
    notifyUndoRedo(priorCanUndo, priorCanRedo);
    if (wasActive != activeEditorStrokeActive()) {
        emit liveStrokeActiveChanged();
    }
}

bool CanvasItem::liveStrokeActive() const noexcept
{
    return activeEditorStrokeActive();
}

int CanvasItem::strokeCount() const noexcept
{
    return m_strokeCount;
}

QString CanvasItem::inputDevice() const
{
    return m_inputDevice;
}

qreal CanvasItem::inputPressure() const noexcept
{
    return m_inputPressure;
}

bool CanvasItem::canUndo() const noexcept
{
    return activeEditorCanUndo();
}

bool CanvasItem::canRedo() const noexcept
{
    return activeEditorCanRedo();
}

bool CanvasItem::undo()
{
    const bool priorCanUndo = activeEditorCanUndo();
    const bool priorCanRedo = activeEditorCanRedo();
    const bool success = chunkedRasterSelected()
        ? m_chunkedEditor.undo()
        : m_editor.undo();
    if (success && !m_undoStrokeCounts.empty()) {
        m_redoStrokeCounts.push_back(m_strokeCount);
        const int restoredCount = m_undoStrokeCounts.back();
        m_undoStrokeCounts.pop_back();
        if (restoredCount != m_strokeCount) {
            m_strokeCount = restoredCount;
            emit strokeCountChanged();
        }
    }
    const bool finished = finishEdit(success);
    notifyUndoRedo(priorCanUndo, priorCanRedo);
    return finished;
}

bool CanvasItem::redo()
{
    const bool priorCanUndo = activeEditorCanUndo();
    const bool priorCanRedo = activeEditorCanRedo();
    const bool success = chunkedRasterSelected()
        ? m_chunkedEditor.redo()
        : m_editor.redo();
    if (success && !m_redoStrokeCounts.empty()) {
        m_undoStrokeCounts.push_back(m_strokeCount);
        const int restoredCount = m_redoStrokeCounts.back();
        m_redoStrokeCounts.pop_back();
        if (restoredCount != m_strokeCount) {
            m_strokeCount = restoredCount;
            emit strokeCountChanged();
        }
    }
    const bool finished = finishEdit(success);
    notifyUndoRedo(priorCanUndo, priorCanRedo);
    return finished;
}

qreal CanvasItem::zoom() const noexcept
{
    return m_zoom;
}

void CanvasItem::setZoom(qreal value)
{
    if (!std::isfinite(value)) {
        return;
    }
    const qreal constrained = std::clamp(value, qreal{0.01}, qreal{256.0});
    if (qFuzzyCompare(m_zoom, constrained)) {
        return;
    }
    m_zoom = constrained;
    emit viewportChanged();
    update();
    scheduleVisibleRender(false);
}

qreal CanvasItem::panX() const noexcept
{
    return m_panX;
}

void CanvasItem::setPanX(qreal value)
{
    if (!std::isfinite(value) || qFuzzyCompare(m_panX, value)) {
        return;
    }
    m_panX = value;
    emit viewportChanged();
    update();
    scheduleVisibleRender(false);
}

qreal CanvasItem::panY() const noexcept
{
    return m_panY;
}

void CanvasItem::setPanY(qreal value)
{
    if (!std::isfinite(value) || qFuzzyCompare(m_panY, value)) {
        return;
    }
    m_panY = value;
    emit viewportChanged();
    update();
    scheduleVisibleRender(false);
}

void CanvasItem::resetView()
{
    const bool changed = !qFuzzyCompare(m_zoom, qreal{1.0})
        || !qFuzzyIsNull(m_panX) || !qFuzzyIsNull(m_panY);
    m_zoom = 1.0;
    m_panX = 0.0;
    m_panY = 0.0;
    if (changed) {
        emit viewportChanged();
    }
    update();
    scheduleVisibleRender(false);
}

void CanvasItem::fitToView()
{
    if (!documentReady() || width() <= 0.0 || height() <= 0.0) {
        return;
    }
    m_zoom = std::clamp(std::min(width() / canvasWidth(), height() / canvasHeight()),
                        qreal{0.01},
                        qreal{256.0});
    m_panX = (width() - canvasWidth() * m_zoom) * 0.5;
    m_panY = (height() - canvasHeight() * m_zoom) * 0.5;
    emit viewportChanged();
    update();
    scheduleVisibleRender(false);
}

void CanvasItem::panBy(qreal dx, qreal dy)
{
    if (!std::isfinite(dx) || !std::isfinite(dy)) {
        return;
    }
    m_panX += dx;
    m_panY += dy;
    emit viewportChanged();
    update();
    scheduleVisibleRender(false);
}

void CanvasItem::zoomAt(qreal factor, const QPointF &itemPosition)
{
    if (!std::isfinite(factor) || factor <= 0.0
        || !std::isfinite(itemPosition.x()) || !std::isfinite(itemPosition.y())) {
        return;
    }
    const QPointF canvasPosition{(itemPosition.x() - m_panX) / m_zoom,
                                 (itemPosition.y() - m_panY) / m_zoom};
    const qreal nextZoom = std::clamp(m_zoom * factor, qreal{0.01}, qreal{256.0});
    if (qFuzzyCompare(nextZoom, m_zoom)) {
        return;
    }
    m_zoom = nextZoom;
    m_panX = itemPosition.x() - canvasPosition.x() * m_zoom;
    m_panY = itemPosition.y() - canvasPosition.y() * m_zoom;
    emit viewportChanged();
    update();
    scheduleVisibleRender(false);
}

bool CanvasItem::refresh()
{
    return refreshAsync() != 0;
}

qulonglong CanvasItem::refreshAsync()
{
    if (!m_document) {
        setLastError(QStringLiteral("no document is bound"));
        return 0;
    }

    const ValidationResult validation = validate(*m_document);
    if (!validation.ok() || m_frame >= m_document->timeline.frameCount) {
        m_documentValid = false;
        m_framePixels = {};
        clearTileCache();
        setLastError(validation.ok()
                         ? QStringLiteral("requested frame is outside the document timeline")
                         : QString::fromStdString(validation.issues.front().path + ": "
                                                  + validation.issues.front().message));
        emit documentChanged();
        update();
        return 0;
    }

    m_documentValid = true;
    syncSelectedLayer();
    setLastError({});
    scheduleVisibleRender(true);
    emit documentChanged();
    return static_cast<qulonglong>(m_revision);
}

bool CanvasItem::rendering() const noexcept
{
    return m_renderSchedulePending || m_asyncRenderer.busy();
}

int CanvasItem::renderTileSize() const noexcept
{
    return RenderTilePixelSize;
}

int CanvasItem::residentTileCount() const noexcept
{
    return static_cast<int>(m_tileCache.size());
}

int CanvasItem::residentLayerTileCount() const noexcept
{
    return static_cast<int>(m_layerTileCache.size());
}

bool CanvasItem::gpuAccelerated() const noexcept
{
    if (!window() || !window()->rendererInterface()) {
        return false;
    }
    const QSGRendererInterface::GraphicsApi api =
        window()->rendererInterface()->graphicsApi();
    return api == QSGRendererInterface::OpenGL
        || api == QSGRendererInterface::Direct3D11
        || api == QSGRendererInterface::Direct3D12
        || api == QSGRendererInterface::Vulkan
        || api == QSGRendererInterface::Metal;
}

QString CanvasItem::graphicsBackend() const
{
    if (!window() || !window()->rendererInterface()) {
        return QStringLiteral("unavailable");
    }
    switch (window()->rendererInterface()->graphicsApi()) {
    case QSGRendererInterface::Software:
        return QStringLiteral("software");
    case QSGRendererInterface::OpenVG:
        return QStringLiteral("openvg");
    case QSGRendererInterface::OpenGL:
        return QStringLiteral("opengl");
    case QSGRendererInterface::Direct3D11:
        return QStringLiteral("direct3d11");
    case QSGRendererInterface::Direct3D12:
        return QStringLiteral("direct3d12");
    case QSGRendererInterface::Vulkan:
        return QStringLiteral("vulkan");
    case QSGRendererInterface::Metal:
        return QStringLiteral("metal");
    case QSGRendererInterface::Null:
        return QStringLiteral("null");
    case QSGRendererInterface::Unknown:
        return QStringLiteral("unknown");
    }
    return QStringLiteral("unknown");
}

qulonglong CanvasItem::revision() const noexcept
{
    return static_cast<qulonglong>(m_revision);
}

QString CanvasItem::lastError() const
{
    return m_lastError;
}

void CanvasItem::paint(QPainter *painter)
{
    if (!painter) {
        return;
    }
    painter->save();
    painter->setCompositionMode(QPainter::CompositionMode_Source);
    painter->fillRect(boundingRect(), Qt::transparent);
    painter->setCompositionMode(QPainter::CompositionMode_SourceOver);

    if (documentReady()) {
        painter->setRenderHint(QPainter::SmoothPixmapTransform, false);
        painter->translate(m_panX, m_panY);
        painter->scale(m_zoom, m_zoom);
        const CanvasOrigin origin = canvasOrigin(*m_document);
        for (const CachedTile &tile : m_tileCache) {
            if (tile.pixels.width <= 0 || tile.pixels.height <= 0
                || tile.pixels.width > std::numeric_limits<int>::max() / 4) {
                continue;
            }
            const QImage source(
                reinterpret_cast<const uchar *>(tile.pixels.pixels.data()),
                tile.pixels.width,
                tile.pixels.height,
                tile.pixels.width * 4,
                QImage::Format_ARGB32);
            const QRectF target{
                static_cast<qreal>(tile.region.origin.x - origin.x),
                static_cast<qreal>(tile.region.origin.y - origin.y),
                static_cast<qreal>(tile.region.extent.width),
                static_cast<qreal>(tile.region.extent.height),
            };
            painter->drawImage(target, source);
        }
    }
    painter->restore();
}

bool CanvasItem::event(QEvent *event)
{
    if (!event || !rasterLayerSelected()
        || (m_toolMode != QStringLiteral("brush")
            && m_toolMode != QStringLiteral("eraser"))) {
        return QQuickItem::event(event);
    }

    const QEvent::Type type = event->type();
    if (type != QEvent::TabletPress
        && type != QEvent::TabletMove
        && type != QEvent::TabletRelease) {
        return QQuickItem::event(event);
    }

    auto *tabletEvent = static_cast<QTabletEvent *>(event);
    const qreal rawPressure = std::clamp(tabletEvent->pressure(), qreal{0.0}, qreal{1.0});
    noteInputState(QStringLiteral("tablet"), rawPressure);
    const bool tabletEraser = isTabletEraser(tabletEvent->pointingDevice());
    bool handled = false;

    if (type == QEvent::TabletPress) {
        m_tabletPointerActive = true;
        m_suppressMouseAfterTablet = true;
        if (tabletEraser) {
            m_tabletPreviousEraser = eraser();
            m_tabletEraserOverride = true;
            setEraser(true);
        }
        handled = beginStrokeAt(mapItemToDocument(tabletEvent->position()), rawPressure);
    } else if (type == QEvent::TabletMove) {
        m_tabletPointerActive = m_tabletPointerActive || rawPressure > 0.0 || tabletEraser;
        m_suppressMouseAfterTablet = m_suppressMouseAfterTablet || m_tabletPointerActive;
        handled = activeEditorStrokeActive()
            && continueStrokeAt(mapItemToDocument(tabletEvent->position()), rawPressure);
    } else {
        handled = activeEditorStrokeActive()
            && endStrokeAt(mapItemToDocument(tabletEvent->position()), rawPressure);
        m_tabletPointerActive = false;
        m_suppressMouseAfterTablet = true;
        if (m_tabletEraserOverride) {
            const bool priorEraser = m_tabletPreviousEraser;
            m_tabletEraserOverride = false;
            setEraser(priorEraser);
        }
    }

    if (handled) {
        event->accept();
        return true;
    }
    return QQuickItem::event(event);
}

void CanvasItem::mousePressEvent(QMouseEvent *event)
{
    if (event->button() == Qt::MiddleButton) {
        m_panning = true;
        m_lastPanPosition = event->position();
        event->accept();
        return;
    }
    if (m_tabletPointerActive || m_suppressMouseAfterTablet) {
        event->accept();
        return;
    }
    if (event->button() == Qt::LeftButton && rasterLayerSelected()
        && (m_toolMode == QStringLiteral("brush")
            || m_toolMode == QStringLiteral("eraser"))) {
        noteInputState(QStringLiteral("mouse"), 1.0);
        if (beginStrokeAt(mapItemToDocument(event->position()), 1.0)) {
            event->accept();
            return;
        }
    }
    event->ignore();
}

void CanvasItem::mouseMoveEvent(QMouseEvent *event)
{
    if (m_panning && (event->buttons() & Qt::MiddleButton)) {
        const QPointF delta = event->position() - m_lastPanPosition;
        m_lastPanPosition = event->position();
        panBy(delta.x(), delta.y());
        event->accept();
        return;
    }
    if (m_tabletPointerActive || m_suppressMouseAfterTablet) {
        event->accept();
        return;
    }
    if (activeEditorStrokeActive() && (event->buttons() & Qt::LeftButton)
        && (m_toolMode == QStringLiteral("brush")
            || m_toolMode == QStringLiteral("eraser"))) {
        noteInputState(QStringLiteral("mouse"), 1.0);
        if (continueStrokeAt(mapItemToDocument(event->position()), 1.0)) {
            event->accept();
            return;
        }
    }
    event->ignore();
}

void CanvasItem::mouseReleaseEvent(QMouseEvent *event)
{
    if (event->button() == Qt::MiddleButton && m_panning) {
        m_panning = false;
        event->accept();
        return;
    }
    if (m_tabletPointerActive || m_suppressMouseAfterTablet) {
        if (event->button() == Qt::LeftButton && !event->buttons().testFlag(Qt::LeftButton)) {
            m_suppressMouseAfterTablet = false;
        }
        event->accept();
        return;
    }
    if (event->button() == Qt::LeftButton && activeEditorStrokeActive()
        && (m_toolMode == QStringLiteral("brush")
            || m_toolMode == QStringLiteral("eraser"))) {
        noteInputState(QStringLiteral("mouse"), 1.0);
        if (endStrokeAt(mapItemToDocument(event->position()), 1.0)) {
            event->accept();
            return;
        }
    }
    event->ignore();
}

void CanvasItem::mouseUngrabEvent()
{
    m_panning = false;
    if (activeEditorStrokeActive()) {
        cancelStroke();
    }
}

void CanvasItem::wheelEvent(QWheelEvent *event)
{
    const int delta = event->angleDelta().y();
    if (delta == 0) {
        event->ignore();
        return;
    }
    zoomAt(std::pow(1.0015, delta), event->position());
    event->accept();
}

QSGNode *CanvasItem::updatePaintNode(QSGNode *oldNode,
                                     UpdatePaintNodeData *)
{
    auto *root = static_cast<CanvasSceneNode *>(oldNode);
    if (!root) {
        root = new CanvasSceneNode;
    }

    QMatrix4x4 transform;
    transform.translate(static_cast<float>(m_panX), static_cast<float>(m_panY));
    transform.scale(static_cast<float>(m_zoom), static_cast<float>(m_zoom));
    root->setMatrix(transform);

    if (root->uploadedTileRevision == m_tileCacheRevision) {
        return root;
    }

    while (QSGNode *child = root->firstChild()) {
        root->removeChildNode(child);
        delete child;
    }
    root->uploadedTileRevision = m_tileCacheRevision;

    QQuickWindow *quickWindow = window();
    if (!quickWindow || !m_document) {
        return root;
    }

    const CanvasOrigin origin = canvasOrigin(*m_document);
    const auto appendTextureNode = [&](QSGNode *parent,
                                       const CanvasRegion &region,
                                       const RasterLayer &pixels) {
        if (pixels.width <= 0 || pixels.height <= 0
            || pixels.width > std::numeric_limits<int>::max() / 4) {
            return false;
        }
        const QImage image(
            reinterpret_cast<const uchar *>(pixels.pixels.data()),
            pixels.width,
            pixels.height,
            pixels.width * 4,
            QImage::Format_ARGB32);
        QSGTexture *texture = quickWindow->createTextureFromImage(
            image,
            QQuickWindow::CreateTextureOptions(QQuickWindow::TextureHasAlphaChannel)
                | QQuickWindow::TextureCanUseAtlas);
        if (!texture) {
            return false;
        }

        auto *textureNode = new QSGSimpleTextureNode;
        textureNode->setTexture(texture);
        textureNode->setOwnsTexture(true);
        textureNode->setFiltering(QSGTexture::Nearest);
        textureNode->setRect(
            static_cast<qreal>(region.origin.x - origin.x),
            static_cast<qreal>(region.origin.y - origin.y),
            static_cast<qreal>(region.extent.width),
            static_cast<qreal>(region.extent.height));
        parent->appendChildNode(textureNode);
        return true;
    };

    if (canPresentLayerTiles()) {
        for (std::size_t layerIndex = 0;
             layerIndex < m_document->layers.size();
             ++layerIndex) {
            const auto firstTile = std::find_if(
                m_layerTileCache.begin(), m_layerTileCache.end(),
                [&](const CachedLayerTile &tile) {
                    return tile.contentGeneration == m_contentGeneration
                        && tile.layerIndex == layerIndex;
                });
            if (firstTile == m_layerTileCache.end()) {
                continue;
            }

            auto *opacityNode = new QSGOpacityNode;
            opacityNode->setOpacity(static_cast<float>(firstTile->opacity));
            for (const CachedLayerTile &tile : m_layerTileCache) {
                if (tile.contentGeneration == m_contentGeneration
                    && tile.layerIndex == layerIndex) {
                    appendTextureNode(opacityNode, tile.region, tile.pixels);
                }
            }
            if (opacityNode->firstChild()) {
                root->appendChildNode(opacityNode);
            } else {
                delete opacityNode;
            }
        }
    } else {
        for (const CachedTile &tile : m_tileCache) {
            appendTextureNode(root, tile.region, tile.pixels);
        }
    }
    return root;
}

void CanvasItem::geometryChange(const QRectF &newGeometry,
                                const QRectF &oldGeometry)
{
    QQuickItem::geometryChange(newGeometry, oldGeometry);
    if (newGeometry.size() != oldGeometry.size()) {
        scheduleVisibleRender(false);
    }
}

void CanvasItem::scheduleVisibleRender(bool contentChanged)
{
    if (!m_document || !m_documentValid) {
        return;
    }
    if (contentChanged) {
        ++m_contentGeneration;
        ++m_revision;
        m_renderSnapshot.reset();
        m_framePixels = {};
        emit revisionChanged();
    }
    if (m_renderSchedulePending) {
        return;
    }
    m_renderSchedulePending = true;
    updateRenderingState();
    const int delay = activeEditorStrokeActive()
        ? std::max(0, m_livePreviewFrameIntervalMs)
        : 0;
    QTimer::singleShot(delay, this, &CanvasItem::startScheduledRender);
}

void CanvasItem::startScheduledRender()
{
    if (!m_renderSchedulePending) {
        return;
    }
    if (!m_document || !m_documentValid) {
        m_renderSchedulePending = false;
        updateRenderingState();
        return;
    }

    if (!m_renderSnapshot) {
        m_renderSnapshot = std::make_shared<Document>(*m_document);
    }

    std::vector<FrameRenderTileRequest> requests = visibleTileRequests();
    std::vector<FrameRenderTileRequest> missing;
    missing.reserve(requests.size());
    for (const FrameRenderTileRequest &request : requests) {
        bool cached = false;
        for (CachedTile &tile : m_tileCache) {
            if (tile.contentGeneration == m_contentGeneration
                && sameRegion(tile.region, request.region)
                && tile.pixels.width == request.outputExtent.width
                && tile.pixels.height == request.outputExtent.height) {
                tile.lastUse = ++m_tileUseCounter;
                for (CachedLayerTile &layerTile : m_layerTileCache) {
                    if (layerTile.contentGeneration == m_contentGeneration
                        && sameRegion(layerTile.region, request.region)
                        && layerTile.pixels.width == request.outputExtent.width
                        && layerTile.pixels.height == request.outputExtent.height) {
                        layerTile.lastUse = m_tileUseCounter;
                    }
                }
                cached = true;
                break;
            }
        }
        if (!cached) {
            missing.push_back(request);
        }
    }

    if (!missing.empty()) {
        m_latestRequestContentGeneration = m_contentGeneration;
        m_latestAsyncRequest = m_asyncRenderer.request(
            m_renderSnapshot, m_frame, std::move(missing));
    }
    m_renderSchedulePending = false;
    updateRenderingState();
    update();
}

void CanvasItem::applyAsyncRender(qulonglong requestId)
{
    if (requestId != m_latestAsyncRequest
        || m_latestRequestContentGeneration != m_contentGeneration
        || !m_document || !m_documentValid) {
        return;
    }

    FrameLayerBatchRenderResult layerResult = m_asyncRenderer.takeLayerResult();
    FrameTileRenderResult result = m_asyncRenderer.takeResult();
    if (!layerResult.ok() || !result.ok()) {
        setLastError(QString::fromStdString(
            layerResult.ok() ? result.message : layerResult.message));
        emit renderCompleted(requestId);
        return;
    }

    const int priorCount = residentTileCount();
    const int priorLayerCount = residentLayerTileCount();
    for (FrameRenderTile &rendered : result.tiles) {
        m_tileCache.erase(
            std::remove_if(m_tileCache.begin(), m_tileCache.end(),
                           [&](const CachedTile &tile) {
                               return regionsOverlap(tile.region, rendered.region);
                           }),
            m_tileCache.end());
        m_tileCache.push_back({
            rendered.region,
            std::move(rendered.pixels),
            m_contentGeneration,
            ++m_tileUseCounter,
        });
    }

    for (const FrameRenderTileRequest &request : layerResult.requests) {
        m_layerTileCache.erase(
            std::remove_if(m_layerTileCache.begin(), m_layerTileCache.end(),
                           [&](const CachedLayerTile &tile) {
                               return regionsOverlap(tile.region, request.region);
                           }),
            m_layerTileCache.end());
    }
    for (FrameLayerTileRenderResult &layer : layerResult.layers) {
        if (!layer.visible) {
            continue;
        }
        for (FrameRenderTile &rendered : layer.tiles) {
            m_layerTileCache.push_back({
                layer.layerIndex,
                layer.layerId,
                rendered.region,
                std::move(rendered.pixels),
                layer.opacity,
                layer.blendMode,
                m_contentGeneration,
                ++m_tileUseCounter,
            });
        }
    }
    trimTileCache();

    const CanvasRegion completeRegion = canvasRegion(*m_document);
    const auto complete = std::find_if(
        m_tileCache.begin(), m_tileCache.end(),
        [&](const CachedTile &tile) {
            return tile.contentGeneration == m_contentGeneration
                && sameRegion(tile.region, completeRegion)
                && tile.pixels.width == completeRegion.extent.width
                && tile.pixels.height == completeRegion.extent.height;
        });
    m_framePixels = complete == m_tileCache.end()
        ? RasterLayer{}
        : complete->pixels;

    ++m_tileCacheRevision;
    if (priorCount != residentTileCount()) {
        emit residentTileCountChanged();
    }
    if (priorLayerCount != residentLayerTileCount()) {
        emit residentLayerTileCountChanged();
    }
    setLastError({});
    update();
    emit renderCompleted(requestId);
}

std::vector<FrameRenderTileRequest> CanvasItem::visibleTileRequests() const
{
    std::vector<FrameRenderTileRequest> result;
    if (!m_document || !m_documentValid) {
        return result;
    }

    const CanvasRegion available = canvasRegion(*m_document);
    const int lod = lodForZoom(m_zoom);
    const std::int64_t tileSpan = static_cast<std::int64_t>(RenderTilePixelSize) * lod;

    double visibleLeft = available.origin.x;
    double visibleTop = available.origin.y;
    double visibleRight = visibleLeft + std::min(available.extent.width,
                                                 RenderTilePixelSize * lod);
    double visibleBottom = visibleTop + std::min(available.extent.height,
                                                 RenderTilePixelSize * lod);
    if (width() > 0.0 && height() > 0.0) {
        visibleLeft = available.origin.x - m_panX / m_zoom;
        visibleTop = available.origin.y - m_panY / m_zoom;
        visibleRight = available.origin.x + (width() - m_panX) / m_zoom;
        visibleBottom = available.origin.y + (height() - m_panY) / m_zoom;
    }

    const double availableRight = static_cast<double>(available.origin.x)
        + available.extent.width;
    const double availableBottom = static_cast<double>(available.origin.y)
        + available.extent.height;
    visibleLeft = std::max(visibleLeft, static_cast<double>(available.origin.x));
    visibleTop = std::max(visibleTop, static_cast<double>(available.origin.y));
    visibleRight = std::min(visibleRight, availableRight);
    visibleBottom = std::min(visibleBottom, availableBottom);
    if (visibleLeft >= visibleRight || visibleTop >= visibleBottom) {
        return result;
    }

    const std::int64_t firstColumn = floorDivision(
        static_cast<std::int64_t>(std::floor(visibleLeft)) - available.origin.x,
        tileSpan) - 1;
    const std::int64_t lastColumn = floorDivision(
        static_cast<std::int64_t>(std::ceil(visibleRight)) - 1 - available.origin.x,
        tileSpan) + 1;
    const std::int64_t firstRow = floorDivision(
        static_cast<std::int64_t>(std::floor(visibleTop)) - available.origin.y,
        tileSpan) - 1;
    const std::int64_t lastRow = floorDivision(
        static_cast<std::int64_t>(std::ceil(visibleBottom)) - 1 - available.origin.y,
        tileSpan) + 1;

    struct Candidate {
        FrameRenderTileRequest request;
        double distance = 0.0;
    };
    std::vector<Candidate> candidates;
    const double centerX = (visibleLeft + visibleRight) * 0.5;
    const double centerY = (visibleTop + visibleBottom) * 0.5;
    for (std::int64_t row = firstRow; row <= lastRow; ++row) {
        for (std::int64_t column = firstColumn; column <= lastColumn; ++column) {
            const std::int64_t worldLeft = static_cast<std::int64_t>(available.origin.x)
                + column * tileSpan;
            const std::int64_t worldTop = static_cast<std::int64_t>(available.origin.y)
                + row * tileSpan;
            const std::int64_t worldRight = std::min<std::int64_t>(
                worldLeft + tileSpan,
                static_cast<std::int64_t>(available.origin.x) + available.extent.width);
            const std::int64_t worldBottom = std::min<std::int64_t>(
                worldTop + tileSpan,
                static_cast<std::int64_t>(available.origin.y) + available.extent.height);
            const std::int64_t clippedLeft = std::max<std::int64_t>(worldLeft,
                                                                    available.origin.x);
            const std::int64_t clippedTop = std::max<std::int64_t>(worldTop,
                                                                   available.origin.y);
            if (clippedLeft >= worldRight || clippedTop >= worldBottom) {
                continue;
            }

            const CanvasRegion region{
                {static_cast<std::int32_t>(clippedLeft),
                 static_cast<std::int32_t>(clippedTop)},
                {static_cast<std::int32_t>(worldRight - clippedLeft),
                 static_cast<std::int32_t>(worldBottom - clippedTop)},
            };
            const CanvasExtent output{
                std::max(1, static_cast<int>(std::ceil(
                    static_cast<double>(region.extent.width) / lod))),
                std::max(1, static_cast<int>(std::ceil(
                    static_cast<double>(region.extent.height) / lod))),
            };
            const double tileCenterX = clippedLeft + region.extent.width * 0.5;
            const double tileCenterY = clippedTop + region.extent.height * 0.5;
            const double dx = tileCenterX - centerX;
            const double dy = tileCenterY - centerY;
            candidates.push_back({{region, output}, dx * dx + dy * dy});
        }
    }

    std::sort(candidates.begin(), candidates.end(),
              [](const Candidate &left, const Candidate &right) {
                  if (left.distance != right.distance) {
                      return left.distance < right.distance;
                  }
                  return std::tie(left.request.region.origin.y,
                                  left.request.region.origin.x)
                      < std::tie(right.request.region.origin.y,
                                 right.request.region.origin.x);
              });
    if (candidates.size() > MaximumResidentTiles) {
        candidates.resize(MaximumResidentTiles);
    }
    result.reserve(candidates.size());
    for (const Candidate &candidate : candidates) {
        result.push_back(candidate.request);
    }
    return result;
}

bool CanvasItem::hasCachedTile(const FrameRenderTileRequest &request) const noexcept
{
    return std::any_of(m_tileCache.begin(), m_tileCache.end(),
                       [&](const CachedTile &tile) {
                           return tile.contentGeneration == m_contentGeneration
                               && sameRegion(tile.region, request.region)
                               && tile.pixels.width == request.outputExtent.width
                               && tile.pixels.height == request.outputExtent.height;
                       });
}

void CanvasItem::trimTileCache()
{
    while (m_tileCache.size() > MaximumResidentTiles) {
        const auto oldest = std::min_element(
            m_tileCache.begin(), m_tileCache.end(),
            [](const CachedTile &left, const CachedTile &right) {
                return left.lastUse < right.lastUse;
            });
        m_tileCache.erase(oldest);
    }
    while (m_layerTileCache.size() > MaximumResidentLayerTiles) {
        const auto oldest = std::min_element(
            m_layerTileCache.begin(), m_layerTileCache.end(),
            [](const CachedLayerTile &left, const CachedLayerTile &right) {
                return left.lastUse < right.lastUse;
            });
        m_layerTileCache.erase(oldest);
    }
}

bool CanvasItem::canPresentLayerTiles() const noexcept
{
    if (!m_document || m_tileCache.empty() || m_layerTileCache.empty()) {
        return false;
    }

    bool hasVisibleLayer = false;
    for (const iiSharedCanvas::Layer &layer : m_document->layers) {
        const LayerProperties &properties = layerProperties(layer);
        if (!properties.visible) {
            continue;
        }
        hasVisibleLayer = true;
        if (properties.blendMode != RasterBlendMode::SourceOver) {
            return false;
        }
    }
    if (!hasVisibleLayer) {
        return false;
    }

    bool hasCurrentTile = false;
    for (const CachedTile &composed : m_tileCache) {
        if (composed.contentGeneration != m_contentGeneration) {
            continue;
        }
        hasCurrentTile = true;
        for (std::size_t layerIndex = 0;
             layerIndex < m_document->layers.size();
             ++layerIndex) {
            if (!layerProperties(m_document->layers[layerIndex]).visible) {
                continue;
            }
            const bool found = std::any_of(
                m_layerTileCache.begin(), m_layerTileCache.end(),
                [&](const CachedLayerTile &tile) {
                    return tile.contentGeneration == m_contentGeneration
                        && tile.layerIndex == layerIndex
                        && tile.blendMode == RasterBlendMode::SourceOver
                        && sameRegion(tile.region, composed.region)
                        && tile.pixels.width == composed.pixels.width
                        && tile.pixels.height == composed.pixels.height;
                });
            if (!found) {
                return false;
            }
        }
    }
    return hasCurrentTile;
}

void CanvasItem::clearTileCache()
{
    const bool hadTiles = !m_tileCache.empty();
    const bool hadLayerTiles = !m_layerTileCache.empty();
    m_tileCache.clear();
    m_layerTileCache.clear();
    m_framePixels = {};
    ++m_tileCacheRevision;
    if (hadTiles) {
        emit residentTileCountChanged();
    }
    if (hadLayerTiles) {
        emit residentLayerTileCountChanged();
    }
    update();
}

void CanvasItem::updateRenderingState()
{
    const bool current = rendering();
    if (m_reportedRendering == current) {
        return;
    }
    m_reportedRendering = current;
    emit renderingChanged();
}

Layer *CanvasItem::selectedLayer() noexcept
{
    if (!m_document || m_selectedLayerId.empty()) {
        return nullptr;
    }
    const auto match = std::find_if(m_document->layers.begin(),
                                    m_document->layers.end(),
                                    [&](const Layer &layer) {
                                        return layerProperties(layer).id == m_selectedLayerId;
                                    });
    return match == m_document->layers.end() ? nullptr : &*match;
}

const Layer *CanvasItem::selectedLayer() const noexcept
{
    if (!m_document || m_selectedLayerId.empty()) {
        return nullptr;
    }
    const auto match = std::find_if(m_document->layers.begin(),
                                    m_document->layers.end(),
                                    [&](const Layer &layer) {
                                        return layerProperties(layer).id == m_selectedLayerId;
                                    });
    return match == m_document->layers.end() ? nullptr : &*match;
}

bool CanvasItem::syncSelectedLayer()
{
    if (m_selectedLayerId.empty()) {
        return true;
    }
    Layer *layer = selectedLayer();
    const Asset *asset = layer ? resolveAssetAt(*m_document, *layer, m_frame) : nullptr;
    if (!layer
        || !std::holds_alternative<BitmapLayer>(*layer)
        || !asset
        || contentKind(*asset) != ContentKind::Raster) {
        clearSelection();
        return false;
    }
    const std::string &resolvedId = assetId(*asset);
    if ((m_editor.isBound() && m_editor.boundAssetId() == resolvedId)
        || (m_chunkedEditor.isBound() && m_chunkedEditor.boundAssetId() == resolvedId)) {
        return true;
    }

    const bool priorCanUndo = activeEditorCanUndo();
    const bool priorCanRedo = activeEditorCanRedo();
    const BitmapBrush currentBrush = m_editor.brush();
    const bool bound = std::holds_alternative<RasterAsset>(*asset)
        ? (m_chunkedEditor.unbind(), m_editor.bind(*m_document, resolvedId))
        : (m_editor.unbind(),
           m_chunkedEditor.bind(*m_document, resolvedId)
               && m_chunkedEditor.setBrush(currentBrush));
    if (!bound) {
        setLastError(QString::fromStdString(activeEditorError()));
        clearSelection();
        return false;
    }
    resetEditState();
    notifyUndoRedo(priorCanUndo, priorCanRedo);
    return true;
}

bool CanvasItem::mapDocumentToSelectedAsset(const QPointF &documentPosition,
                                            DocumentPoint &assetPosition)
{
    const Layer *layer = selectedLayer();
    if (!layer || (!m_editor.isBound() && !m_chunkedEditor.isBound())
        || !std::isfinite(documentPosition.x())
        || !std::isfinite(documentPosition.y())) {
        setLastError(QStringLiteral("no valid raster document layer is selected"));
        return false;
    }

    const AffineTransform &transform = layerProperties(*layer).transform;
    const double determinant = transform.m11 * transform.m22
        - transform.m21 * transform.m12;
    if (std::abs(determinant) <= std::numeric_limits<double>::epsilon()) {
        setLastError(QStringLiteral("selected layer transform is singular"));
        return false;
    }

    const double translatedX = documentPosition.x() - transform.translationX;
    const double translatedY = documentPosition.y() - transform.translationY;
    assetPosition = {
        (translatedX * transform.m22 - translatedY * transform.m21) / determinant,
        (-translatedX * transform.m12 + translatedY * transform.m11) / determinant,
    };
    return true;
}

QPointF CanvasItem::mapItemToDocument(const QPointF &itemPosition) const noexcept
{
    const CanvasOrigin origin = m_document ? canvasOrigin(*m_document) : CanvasOrigin{};
    return {(itemPosition.x() - m_panX) / m_zoom + origin.x,
            (itemPosition.y() - m_panY) / m_zoom + origin.y};
}

qreal CanvasItem::normalizedPressure(qreal rawPressure, bool pressureSensitive) const noexcept
{
    if (!pressureSensitive) {
        return 1.0;
    }
    return resolvePressureInput(PressureInput{
        true,
        0.0,
        1.0,
        static_cast<Types::Scalar>(rawPressure),
        true,
        false,
        static_cast<Types::Scalar>(m_pressureCurveMinimum),
        static_cast<Types::Scalar>(m_pressureCurveCenter),
        static_cast<Types::Scalar>(m_pressureCurveMaximum),
    });
}

QPointF CanvasItem::stabilizedDocumentPosition(const QPointF &position, bool finish)
{
    if (!m_stabilizerActive || m_stabilizerStrength <= 0.0 || finish) {
        m_stabilizedPosition = position;
        return position;
    }

    const qreal response = std::max(qreal{0.02}, qreal{1.0} - m_stabilizerStrength * 0.92);
    m_stabilizedPosition += (position - m_stabilizedPosition) * response;
    return m_stabilizedPosition;
}

void CanvasItem::noteInputState(QString device, qreal pressure)
{
    const qreal normalized = std::clamp(pressure, qreal{0.0}, qreal{1.0});
    if (m_inputDevice == device && m_inputPressure == normalized) {
        return;
    }
    m_inputDevice = std::move(device);
    m_inputPressure = normalized;
    emit inputStateChanged();
}

void CanvasItem::resetEditState()
{
    const bool countChanged = m_strokeCount != 0;
    const bool activeChanged = activeEditorStrokeActive();
    m_strokeCount = 0;
    m_strokeStartCount = 0;
    m_strokeStartRevision = 0;
    m_undoStrokeCounts.clear();
    m_redoStrokeCounts.clear();
    m_stabilizerActive = false;
    m_tabletPointerActive = false;
    m_suppressMouseAfterTablet = false;
    m_tabletEraserOverride = false;
    if (countChanged) {
        emit strokeCountChanged();
    }
    if (activeChanged) {
        emit liveStrokeActiveChanged();
    }
}

void CanvasItem::recordCountHistory(int priorStrokeCount)
{
    m_undoStrokeCounts.push_back(priorStrokeCount);
    if (m_undoStrokeCounts.size() > 32) {
        m_undoStrokeCounts.erase(m_undoStrokeCounts.begin());
    }
    m_redoStrokeCounts.clear();
}

void CanvasItem::applyBrush(const BitmapBrush &brush)
{
    const bool regularApplied = m_editor.setBrush(brush);
    const bool chunkedApplied = m_chunkedEditor.setBrush(brush);
    if (regularApplied && chunkedApplied) {
        setLastError({});
        emit brushChanged();
        return;
    }
    setLastError(QString::fromStdString(regularApplied
                                            ? m_chunkedEditor.lastError()
                                            : m_editor.lastError()));
}

bool CanvasItem::finishEdit(bool success)
{
    if (!success) {
        setLastError(QString::fromStdString(activeEditorError()));
        return false;
    }
    return refresh();
}

void CanvasItem::notifyUndoRedo(bool priorCanUndo, bool priorCanRedo)
{
    if (priorCanUndo != activeEditorCanUndo() || priorCanRedo != activeEditorCanRedo()) {
        emit undoRedoChanged();
    }
}

bool CanvasItem::chunkedRasterSelected() const noexcept
{
    return m_chunkedEditor.isBound() && !m_selectedLayerId.empty();
}

std::uint64_t CanvasItem::activeEditorRevision() const noexcept
{
    return chunkedRasterSelected() ? m_chunkedEditor.revision() : m_editor.revision();
}

bool CanvasItem::activeEditorCanUndo() const noexcept
{
    return chunkedRasterSelected() ? m_chunkedEditor.canUndo() : m_editor.canUndo();
}

bool CanvasItem::activeEditorCanRedo() const noexcept
{
    return chunkedRasterSelected() ? m_chunkedEditor.canRedo() : m_editor.canRedo();
}

bool CanvasItem::activeEditorStrokeActive() const noexcept
{
    return chunkedRasterSelected() ? m_chunkedEditor.strokeActive() : m_editor.strokeActive();
}

const std::string &CanvasItem::activeEditorError() const noexcept
{
    return !m_chunkedEditor.lastError().empty()
        ? m_chunkedEditor.lastError()
        : m_editor.lastError();
}

void CanvasItem::setLastError(QString message)
{
    if (m_lastError == message) {
        return;
    }
    m_lastError = std::move(message);
    emit lastErrorChanged();
}

} // namespace iiSharedCanvas
