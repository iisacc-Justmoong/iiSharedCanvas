#include "QtAdapter/CanvasItem.h"

#include "Render/FrameRenderer.h"

#include <Input/PressureInput.h>

#include <QImage>
#include <QMouseEvent>
#include <QPainter>
#include <QPointingDevice>
#include <QTabletEvent>
#include <QWheelEvent>

#include <algorithm>
#include <cmath>
#include <limits>
#include <utility>

namespace iiSharedCanvas {
namespace {

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
    : QQuickPaintedItem(parent)
{
    setAcceptedMouseButtons(Qt::LeftButton | Qt::MiddleButton);
    setAcceptHoverEvents(false);
    setAntialiasing(false);
    setMipmap(false);
    setOpaquePainting(false);
}

bool CanvasItem::bind(Document &value)
{
    FrameRenderResult rendered = renderFrame(value, 0);
    if (!rendered.ok()) {
        setLastError(QString::fromStdString(rendered.message));
        return false;
    }

    const bool hadSelection = !m_selectedLayerId.empty();
    m_editor.unbind();
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
    applyRenderedFrame(std::move(rendered.pixels));
    resetView();
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
    m_editor.unbind();
    m_selectedLayerId.clear();
    resetEditState();
    m_documentEditor.unbind();
    m_document = nullptr;
    m_framePixels = {};
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
    return documentReady() ? &m_framePixels : nullptr;
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
    m_ownedDocument.layers.push_back({
        "canvas.layer.0",
        "Raster",
        true,
        1.0,
        {},
        RasterBlendMode::SourceOver,
        StaticSource{"canvas.raster.0"},
    });
    return bind(m_ownedDocument) && selectLayer(QStringLiteral("canvas.layer.0"));
}

bool CanvasItem::documentReady() const noexcept
{
    return m_document != nullptr
        && m_framePixels.width == m_document->extent.width
        && m_framePixels.height == m_document->extent.height;
}

int CanvasItem::canvasWidth() const noexcept
{
    return m_document ? m_document->extent.width : 0;
}

int CanvasItem::canvasHeight() const noexcept
{
    return m_document ? m_document->extent.height : 0;
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

    FrameRenderResult rendered = renderFrame(*m_document, value);
    if (!rendered.ok()) {
        setLastError(QString::fromStdString(rendered.message));
        return;
    }
    m_frame = value;
    applyRenderedFrame(std::move(rendered.pixels));
    syncSelectedLayer();
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
                                    [&](const Layer &layer) { return layer.id == requestedId; });
    if (match == m_document->layers.end()) {
        setLastError(QStringLiteral("document layer was not found"));
        return false;
    }
    const Asset *asset = resolveAssetAt(*m_document, *match, m_frame);
    if (!asset || !std::holds_alternative<RasterAsset>(*asset)) {
        setLastError(QStringLiteral("selected layer does not resolve to raster content"));
        return false;
    }

    const std::string &resolvedAssetId = assetId(*asset);
    const bool changed = m_selectedLayerId != requestedId;
    if (m_editor.isBound() && m_editor.boundAssetId() == resolvedAssetId) {
        m_selectedLayerId = requestedId;
        setLastError({});
        if (changed) {
            emit selectionChanged();
        }
        return true;
    }

    const bool priorCanUndo = m_editor.canUndo();
    const bool priorCanRedo = m_editor.canRedo();
    if (!m_editor.bind(*m_document, resolvedAssetId)) {
        setLastError(QString::fromStdString(m_editor.lastError()));
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
    if (m_selectedLayerId.empty() && !m_editor.isBound()) {
        return;
    }
    const bool priorCanUndo = m_editor.canUndo();
    const bool priorCanRedo = m_editor.canRedo();
    m_editor.unbind();
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
    return m_editor.isBound() && !m_selectedLayerId.empty();
}

const RasterLayer *CanvasItem::selectedRasterPixels() const noexcept
{
    return m_editor.pixels();
}

bool CanvasItem::replaceSelectedPixels(const RasterLayer &pixels)
{
    const std::uint64_t priorRevision = m_editor.revision();
    const int priorStrokeCount = m_strokeCount;
    const bool priorCanUndo = m_editor.canUndo();
    const bool priorCanRedo = m_editor.canRedo();
    const bool success = m_editor.replacePixels(pixels);
    if (success && priorRevision != m_editor.revision()) {
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
    const std::uint64_t priorRevision = m_editor.revision();
    const int priorStrokeCount = m_strokeCount;
    const bool priorCanUndo = m_editor.canUndo();
    const bool priorCanRedo = m_editor.canRedo();
    const bool success = m_editor.clear();
    if (success && priorRevision != m_editor.revision()) {
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
    const bool wasActive = m_editor.strokeActive();
    m_stabilizerActive = true;
    m_stabilizedPosition = documentPosition;
    DocumentPoint assetPosition;
    if (!mapDocumentToSelectedAsset(stabilizedDocumentPosition(documentPosition, false),
                                    assetPosition)) {
        m_stabilizerActive = false;
        return false;
    }
    const bool priorCanUndo = m_editor.canUndo();
    const bool priorCanRedo = m_editor.canRedo();
    m_strokeStartRevision = m_editor.revision();
    m_strokeStartCount = m_strokeCount;
    const bool success = m_editor.beginStroke(assetPosition,
                                              normalizedPressure(pressure, true));
    if (!success) {
        m_stabilizerActive = false;
    }
    const bool finished = finishEdit(success);
    notifyUndoRedo(priorCanUndo, priorCanRedo);
    if (wasActive != m_editor.strokeActive()) {
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
    const bool priorCanUndo = m_editor.canUndo();
    const bool priorCanRedo = m_editor.canRedo();
    const bool success = m_editor.continueStroke(assetPosition,
                                                 normalizedPressure(pressure, true));
    const bool finished = finishEdit(success);
    notifyUndoRedo(priorCanUndo, priorCanRedo);
    return finished;
}

bool CanvasItem::endStrokeAt(const QPointF &documentPosition, qreal pressure)
{
    const bool wasActive = m_editor.strokeActive();
    DocumentPoint assetPosition;
    if (!mapDocumentToSelectedAsset(stabilizedDocumentPosition(documentPosition, true),
                                    assetPosition)) {
        return false;
    }
    const bool priorCanUndo = m_editor.canUndo();
    const bool priorCanRedo = m_editor.canRedo();
    const bool success = m_editor.endStroke(assetPosition,
                                            normalizedPressure(pressure, true));
    m_stabilizerActive = false;
    if (success && m_strokeStartRevision != m_editor.revision()) {
        recordCountHistory(m_strokeStartCount);
        ++m_strokeCount;
        emit strokeCountChanged();
    }
    const bool finished = finishEdit(success);
    notifyUndoRedo(priorCanUndo, priorCanRedo);
    if (wasActive != m_editor.strokeActive()) {
        emit liveStrokeActiveChanged();
    }
    return finished;
}

void CanvasItem::cancelStroke()
{
    const bool wasActive = m_editor.strokeActive();
    const bool priorCanUndo = m_editor.canUndo();
    const bool priorCanRedo = m_editor.canRedo();
    m_editor.cancelStroke();
    m_stabilizerActive = false;
    refresh();
    notifyUndoRedo(priorCanUndo, priorCanRedo);
    if (wasActive != m_editor.strokeActive()) {
        emit liveStrokeActiveChanged();
    }
}

bool CanvasItem::liveStrokeActive() const noexcept
{
    return m_editor.strokeActive();
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
    return m_editor.canUndo();
}

bool CanvasItem::canRedo() const noexcept
{
    return m_editor.canRedo();
}

bool CanvasItem::undo()
{
    const bool priorCanUndo = m_editor.canUndo();
    const bool priorCanRedo = m_editor.canRedo();
    const bool success = m_editor.undo();
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
    const bool priorCanUndo = m_editor.canUndo();
    const bool priorCanRedo = m_editor.canRedo();
    const bool success = m_editor.redo();
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
}

bool CanvasItem::refresh()
{
    if (!m_document) {
        setLastError(QStringLiteral("no document is bound"));
        return false;
    }
    FrameRenderResult rendered = renderFrame(*m_document, m_frame);
    if (!rendered.ok()) {
        m_framePixels = {};
        setLastError(QString::fromStdString(rendered.message));
        emit documentChanged();
        update();
        return false;
    }
    syncSelectedLayer();
    setLastError({});
    const bool applied = applyRenderedFrame(std::move(rendered.pixels));
    emit documentChanged();
    return applied;
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
    painter->save();
    painter->setCompositionMode(QPainter::CompositionMode_Source);
    painter->fillRect(boundingRect(), Qt::transparent);
    painter->setCompositionMode(QPainter::CompositionMode_SourceOver);

    if (documentReady() && m_framePixels.width <= std::numeric_limits<int>::max() / 4) {
        const QImage source(reinterpret_cast<const uchar *>(m_framePixels.pixels.data()),
                            m_framePixels.width,
                            m_framePixels.height,
                            m_framePixels.width * 4,
                            QImage::Format_ARGB32);
        painter->setRenderHint(QPainter::SmoothPixmapTransform, false);
        painter->translate(m_panX, m_panY);
        painter->scale(m_zoom, m_zoom);
        painter->drawImage(QPointF{0.0, 0.0}, source);
    }
    painter->restore();
}

bool CanvasItem::event(QEvent *event)
{
    if (!event || !rasterLayerSelected()
        || (m_toolMode != QStringLiteral("brush")
            && m_toolMode != QStringLiteral("eraser"))) {
        return QQuickPaintedItem::event(event);
    }

    const QEvent::Type type = event->type();
    if (type != QEvent::TabletPress
        && type != QEvent::TabletMove
        && type != QEvent::TabletRelease) {
        return QQuickPaintedItem::event(event);
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
        handled = m_editor.strokeActive()
            && continueStrokeAt(mapItemToDocument(tabletEvent->position()), rawPressure);
    } else {
        handled = m_editor.strokeActive()
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
    return QQuickPaintedItem::event(event);
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
    if (m_editor.strokeActive() && (event->buttons() & Qt::LeftButton)
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
    if (event->button() == Qt::LeftButton && m_editor.strokeActive()
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
    if (m_editor.strokeActive()) {
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

bool CanvasItem::applyRenderedFrame(RasterLayer pixels)
{
    m_framePixels = std::move(pixels);
    ++m_revision;
    emit revisionChanged();
    update();
    return true;
}

Layer *CanvasItem::selectedLayer() noexcept
{
    if (!m_document || m_selectedLayerId.empty()) {
        return nullptr;
    }
    const auto match = std::find_if(m_document->layers.begin(),
                                    m_document->layers.end(),
                                    [&](const Layer &layer) {
                                        return layer.id == m_selectedLayerId;
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
                                        return layer.id == m_selectedLayerId;
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
    if (!asset || !std::holds_alternative<RasterAsset>(*asset)) {
        clearSelection();
        return false;
    }
    const std::string &resolvedId = assetId(*asset);
    if (m_editor.isBound() && m_editor.boundAssetId() == resolvedId) {
        return true;
    }

    const bool priorCanUndo = m_editor.canUndo();
    const bool priorCanRedo = m_editor.canRedo();
    if (!m_editor.bind(*m_document, resolvedId)) {
        setLastError(QString::fromStdString(m_editor.lastError()));
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
    if (!layer || !m_editor.isBound()
        || !std::isfinite(documentPosition.x())
        || !std::isfinite(documentPosition.y())) {
        setLastError(QStringLiteral("no valid raster document layer is selected"));
        return false;
    }

    const AffineTransform &transform = layer->transform;
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
    return {(itemPosition.x() - m_panX) / m_zoom,
            (itemPosition.y() - m_panY) / m_zoom};
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
    const bool activeChanged = m_editor.strokeActive();
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
    if (m_editor.setBrush(brush)) {
        setLastError({});
        emit brushChanged();
        return;
    }
    setLastError(QString::fromStdString(m_editor.lastError()));
}

bool CanvasItem::finishEdit(bool success)
{
    if (!success) {
        setLastError(QString::fromStdString(m_editor.lastError()));
        return false;
    }
    return refresh();
}

void CanvasItem::notifyUndoRedo(bool priorCanUndo, bool priorCanRedo)
{
    if (priorCanUndo != m_editor.canUndo() || priorCanRedo != m_editor.canRedo()) {
        emit undoRedoChanged();
    }
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
