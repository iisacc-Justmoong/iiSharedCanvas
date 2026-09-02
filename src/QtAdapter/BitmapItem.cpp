#include "QtAdapter/BitmapItem.h"

#include "QtAdapter/CanvasItem.h"

#include <QImage>
#include <QMouseEvent>
#include <QPainter>
#include <QQmlEngine>
#include <QWheelEvent>

#include <algorithm>
#include <cmath>
#include <limits>
#include <mutex>

namespace iiSharedCanvas {

BitmapItem::BitmapItem(QQuickItem *parent)
    : QQuickPaintedItem(parent)
{
    setAcceptedMouseButtons(Qt::LeftButton | Qt::MiddleButton);
    setAcceptHoverEvents(false);
    setAntialiasing(false);
    setMipmap(false);
    setOpaquePainting(false);
}

bool BitmapItem::bind(Document &document, const std::string &assetId)
{
    const bool success = m_editor.bind(document, assetId);
    if (success) {
        resetView();
        emit bitmapChanged();
        emit undoRedoChanged();
        emit revisionChanged();
        update();
    }
    emit lastErrorChanged();
    return success;
}

void BitmapItem::unbind()
{
    m_editor.unbind();
    m_panning = false;
    emit bitmapChanged();
    emit undoRedoChanged();
    emit revisionChanged();
    emit lastErrorChanged();
    update();
}

BitmapEditor &BitmapItem::editor() noexcept
{
    return m_editor;
}

const BitmapEditor &BitmapItem::editor() const noexcept
{
    return m_editor;
}

const RasterLayer *BitmapItem::pixels() const noexcept
{
    return m_editor.pixels();
}

bool BitmapItem::createBitmap(int width, int height)
{
    return createBitmap(width, height, QColor::fromRgba(0x00000000U));
}

bool BitmapItem::createBitmap(int width, int height, const QColor &clearColor)
{
    if (width <= 0 || height <= 0 || !clearColor.isValid()) {
        return false;
    }

    const auto pixelWidth = static_cast<std::size_t>(width);
    const auto pixelHeight = static_cast<std::size_t>(height);
    if (pixelWidth > std::numeric_limits<std::size_t>::max() / pixelHeight) {
        return false;
    }

    m_editor.unbind();
    m_ownedDocument = {};
    m_ownedDocument.extent = {width, height};
    m_ownedDocument.timeline = {{24, 1}, 1};
    m_ownedDocument.assets.emplace_back(
        RasterAsset{"bitmap", makeRasterLayer(width, height, clearColor.rgba())});
    m_ownedDocument.layers.emplace_back(BitmapLayer{
        {"bitmap-layer", "Bitmap", true, 1.0, {}, RasterBlendMode::SourceOver},
        StaticSource{"bitmap"},
    });
    return bind(m_ownedDocument, "bitmap");
}

bool BitmapItem::bitmapReady() const noexcept
{
    return m_editor.isBound();
}

int BitmapItem::bitmapWidth() const noexcept
{
    return m_editor.width();
}

int BitmapItem::bitmapHeight() const noexcept
{
    return m_editor.height();
}

qreal BitmapItem::zoom() const noexcept
{
    return m_zoom;
}

void BitmapItem::setZoom(qreal value)
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

qreal BitmapItem::panX() const noexcept
{
    return m_panX;
}

void BitmapItem::setPanX(qreal value)
{
    if (!std::isfinite(value) || qFuzzyCompare(m_panX, value)) {
        return;
    }
    m_panX = value;
    emit viewportChanged();
    update();
}

qreal BitmapItem::panY() const noexcept
{
    return m_panY;
}

void BitmapItem::setPanY(qreal value)
{
    if (!std::isfinite(value) || qFuzzyCompare(m_panY, value)) {
        return;
    }
    m_panY = value;
    emit viewportChanged();
    update();
}

void BitmapItem::resetView()
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

void BitmapItem::fitToView()
{
    if (!bitmapReady() || width() <= 0.0 || height() <= 0.0) {
        return;
    }
    const qreal fittedZoom = std::min(width() / bitmapWidth(), height() / bitmapHeight());
    m_zoom = std::clamp(fittedZoom, qreal{0.01}, qreal{256.0});
    m_panX = (width() - bitmapWidth() * m_zoom) * 0.5;
    m_panY = (height() - bitmapHeight() * m_zoom) * 0.5;
    emit viewportChanged();
    update();
}

void BitmapItem::panBy(qreal dx, qreal dy)
{
    if (!std::isfinite(dx) || !std::isfinite(dy)) {
        return;
    }
    m_panX += dx;
    m_panY += dy;
    emit viewportChanged();
    update();
}

void BitmapItem::zoomAt(qreal factor, const QPointF &itemPosition)
{
    if (!std::isfinite(factor) || factor <= 0.0
        || !std::isfinite(itemPosition.x()) || !std::isfinite(itemPosition.y())) {
        return;
    }
    const QPointF bitmapPosition = mapItemToBitmap(itemPosition);
    const qreal nextZoom = std::clamp(m_zoom * factor, qreal{0.01}, qreal{256.0});
    if (qFuzzyCompare(nextZoom, m_zoom)) {
        return;
    }
    m_zoom = nextZoom;
    m_panX = itemPosition.x() - bitmapPosition.x() * m_zoom;
    m_panY = itemPosition.y() - bitmapPosition.y() * m_zoom;
    emit viewportChanged();
    update();
}

QColor BitmapItem::brushColor() const
{
    return QColor::fromRgba(m_editor.brush().argb);
}

void BitmapItem::setBrushColor(const QColor &color)
{
    if (!color.isValid() || color.rgba() == m_editor.brush().argb) {
        return;
    }
    BitmapBrush value = m_editor.brush();
    value.argb = color.rgba();
    applyBrush(value);
}

qreal BitmapItem::brushSize() const noexcept
{
    return m_editor.brush().size;
}

void BitmapItem::setBrushSize(qreal size)
{
    BitmapBrush value = m_editor.brush();
    value.size = size;
    applyBrush(value);
}

qreal BitmapItem::brushOpacity() const noexcept
{
    return m_editor.brush().opacity;
}

void BitmapItem::setBrushOpacity(qreal opacity)
{
    BitmapBrush value = m_editor.brush();
    value.opacity = opacity;
    applyBrush(value);
}

qreal BitmapItem::brushFlow() const noexcept
{
    return m_editor.brush().flow;
}

void BitmapItem::setBrushFlow(qreal flow)
{
    BitmapBrush value = m_editor.brush();
    value.flow = flow;
    applyBrush(value);
}

qreal BitmapItem::brushHardness() const noexcept
{
    return m_editor.brush().hardness;
}

void BitmapItem::setBrushHardness(qreal hardness)
{
    BitmapBrush value = m_editor.brush();
    value.hardness = hardness;
    applyBrush(value);
}

bool BitmapItem::eraser() const noexcept
{
    return m_editor.brush().eraser;
}

void BitmapItem::setEraser(bool enabled)
{
    if (enabled == m_editor.brush().eraser) {
        return;
    }
    BitmapBrush value = m_editor.brush();
    value.eraser = enabled;
    applyBrush(value);
}

bool BitmapItem::setPixel(int x, int y, const QColor &color)
{
    if (!color.isValid()) {
        return false;
    }
    const std::uint64_t priorRevision = m_editor.revision();
    const bool priorCanUndo = m_editor.canUndo();
    const bool priorCanRedo = m_editor.canRedo();
    const bool success = m_editor.setPixel(x, y, color.rgba());
    finishEdit(success);
    notifyStateChange(priorRevision, priorCanUndo, priorCanRedo);
    return success;
}

QColor BitmapItem::pixelColor(int x, int y) const
{
    const auto pixel = m_editor.pixelAt(x, y);
    return pixel ? QColor::fromRgba(*pixel) : QColor{};
}

bool BitmapItem::clear()
{
    return clear(QColor::fromRgba(0x00000000U));
}

bool BitmapItem::clear(const QColor &color)
{
    if (!color.isValid()) {
        return false;
    }
    const std::uint64_t priorRevision = m_editor.revision();
    const bool priorCanUndo = m_editor.canUndo();
    const bool priorCanRedo = m_editor.canRedo();
    const bool success = m_editor.clear(color.rgba());
    finishEdit(success);
    notifyStateChange(priorRevision, priorCanUndo, priorCanRedo);
    return success;
}

bool BitmapItem::beginStrokeAt(const QPointF &bitmapPosition, qreal pressure)
{
    const std::uint64_t priorRevision = m_editor.revision();
    const bool priorCanUndo = m_editor.canUndo();
    const bool priorCanRedo = m_editor.canRedo();
    const bool success = m_editor.beginStroke({bitmapPosition.x(), bitmapPosition.y()}, pressure);
    finishEdit(success);
    notifyStateChange(priorRevision, priorCanUndo, priorCanRedo);
    return success;
}

bool BitmapItem::continueStrokeAt(const QPointF &bitmapPosition, qreal pressure)
{
    const std::uint64_t priorRevision = m_editor.revision();
    const bool priorCanUndo = m_editor.canUndo();
    const bool priorCanRedo = m_editor.canRedo();
    const bool success = m_editor.continueStroke({bitmapPosition.x(), bitmapPosition.y()}, pressure);
    finishEdit(success);
    notifyStateChange(priorRevision, priorCanUndo, priorCanRedo);
    return success;
}

bool BitmapItem::endStrokeAt(const QPointF &bitmapPosition, qreal pressure)
{
    const std::uint64_t priorRevision = m_editor.revision();
    const bool priorCanUndo = m_editor.canUndo();
    const bool priorCanRedo = m_editor.canRedo();
    const bool success = m_editor.endStroke({bitmapPosition.x(), bitmapPosition.y()}, pressure);
    finishEdit(success);
    notifyStateChange(priorRevision, priorCanUndo, priorCanRedo);
    return success;
}

void BitmapItem::cancelStroke()
{
    const std::uint64_t priorRevision = m_editor.revision();
    const bool priorCanUndo = m_editor.canUndo();
    const bool priorCanRedo = m_editor.canRedo();
    m_editor.cancelStroke();
    finishEdit(true);
    notifyStateChange(priorRevision, priorCanUndo, priorCanRedo);
}

bool BitmapItem::canUndo() const noexcept
{
    return m_editor.canUndo();
}

bool BitmapItem::canRedo() const noexcept
{
    return m_editor.canRedo();
}

bool BitmapItem::undo()
{
    const std::uint64_t priorRevision = m_editor.revision();
    const bool priorCanUndo = m_editor.canUndo();
    const bool priorCanRedo = m_editor.canRedo();
    const bool success = m_editor.undo();
    finishEdit(success);
    notifyStateChange(priorRevision, priorCanUndo, priorCanRedo);
    return success;
}

bool BitmapItem::redo()
{
    const std::uint64_t priorRevision = m_editor.revision();
    const bool priorCanUndo = m_editor.canUndo();
    const bool priorCanRedo = m_editor.canRedo();
    const bool success = m_editor.redo();
    finishEdit(success);
    notifyStateChange(priorRevision, priorCanUndo, priorCanRedo);
    return success;
}

void BitmapItem::refresh()
{
    emit bitmapChanged();
    emit undoRedoChanged();
    emit revisionChanged();
    emit lastErrorChanged();
    update();
}

qulonglong BitmapItem::revision() const noexcept
{
    return static_cast<qulonglong>(m_editor.revision());
}

QString BitmapItem::lastError() const
{
    return QString::fromStdString(m_editor.lastError());
}

void BitmapItem::paint(QPainter *painter)
{
    painter->save();
    painter->setCompositionMode(QPainter::CompositionMode_Source);
    painter->fillRect(boundingRect(), Qt::transparent);
    painter->setCompositionMode(QPainter::CompositionMode_SourceOver);

    const RasterLayer *layer = m_editor.pixels();
    if (layer && layer->width <= std::numeric_limits<int>::max() / 4) {
        const QImage source(reinterpret_cast<const uchar *>(layer->pixels.data()),
                            layer->width,
                            layer->height,
                            layer->width * 4,
                            QImage::Format_ARGB32);
        painter->setRenderHint(QPainter::SmoothPixmapTransform, false);
        painter->translate(m_panX, m_panY);
        painter->scale(m_zoom, m_zoom);
        painter->drawImage(QPointF{0.0, 0.0}, source);
    }
    painter->restore();
}

void BitmapItem::mousePressEvent(QMouseEvent *event)
{
    if (event->button() == Qt::MiddleButton) {
        m_panning = true;
        m_lastPanPosition = event->position();
        event->accept();
        return;
    }
    if (event->button() == Qt::LeftButton && bitmapReady()) {
        beginStrokeAt(mapItemToBitmap(event->position()), 1.0);
        event->accept();
        return;
    }
    event->ignore();
}

void BitmapItem::mouseMoveEvent(QMouseEvent *event)
{
    if (m_panning && (event->buttons() & Qt::MiddleButton)) {
        const QPointF delta = event->position() - m_lastPanPosition;
        m_lastPanPosition = event->position();
        panBy(delta.x(), delta.y());
        event->accept();
        return;
    }
    if (m_editor.strokeActive() && (event->buttons() & Qt::LeftButton)) {
        continueStrokeAt(mapItemToBitmap(event->position()), 1.0);
        event->accept();
        return;
    }
    event->ignore();
}

void BitmapItem::mouseReleaseEvent(QMouseEvent *event)
{
    if (event->button() == Qt::MiddleButton && m_panning) {
        m_panning = false;
        event->accept();
        return;
    }
    if (event->button() == Qt::LeftButton && m_editor.strokeActive()) {
        endStrokeAt(mapItemToBitmap(event->position()), 1.0);
        event->accept();
        return;
    }
    event->ignore();
}

void BitmapItem::mouseUngrabEvent()
{
    m_panning = false;
    cancelStroke();
}

void BitmapItem::wheelEvent(QWheelEvent *event)
{
    const int delta = event->angleDelta().y();
    if (delta == 0) {
        event->ignore();
        return;
    }
    zoomAt(std::pow(1.0015, delta), event->position());
    event->accept();
}

QPointF BitmapItem::mapItemToBitmap(const QPointF &position) const
{
    return {(position.x() - m_panX) / m_zoom,
            (position.y() - m_panY) / m_zoom};
}

void BitmapItem::applyBrush(const BitmapBrush &brush)
{
    if (m_editor.setBrush(brush)) {
        emit brushChanged();
    }
    emit lastErrorChanged();
}

bool BitmapItem::finishEdit(bool success)
{
    if (success) {
        update();
    }
    emit lastErrorChanged();
    return success;
}

void BitmapItem::notifyStateChange(std::uint64_t priorRevision,
                                   bool priorCanUndo,
                                   bool priorCanRedo)
{
    if (priorRevision != m_editor.revision()) {
        emit revisionChanged();
    }
    if (priorCanUndo != m_editor.canUndo() || priorCanRedo != m_editor.canRedo()) {
        emit undoRedoChanged();
    }
}

int registerIiSharedCanvasQmlTypes()
{
    static std::once_flag once;
    static int typeId = 0;
    std::call_once(once, [] {
        typeId = qmlRegisterType<BitmapItem>("iiSharedCanvas", 1, 0, "Bitmap");
        qmlRegisterType<CanvasItem>("iiSharedCanvas", 1, 0, "SharedCanvas");
    });
    return typeId;
}

} // namespace iiSharedCanvas
