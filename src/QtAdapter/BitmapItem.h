#pragma once

#include "Bitmap/BitmapEditor.h"
#include "iiSharedCanvas/Export.h"

#include <QColor>
#include <QPointF>
#include <QQuickPaintedItem>
#include <QString>

#include <cstdint>
#include <string>

class QMouseEvent;
class QPainter;
class QWheelEvent;

namespace iiSharedCanvas {

class IISHAREDCANVAS_EXPORT BitmapItem : public QQuickPaintedItem {
    Q_OBJECT
    Q_PROPERTY(bool bitmapReady READ bitmapReady NOTIFY bitmapChanged)
    Q_PROPERTY(int bitmapWidth READ bitmapWidth NOTIFY bitmapChanged)
    Q_PROPERTY(int bitmapHeight READ bitmapHeight NOTIFY bitmapChanged)
    Q_PROPERTY(qreal zoom READ zoom WRITE setZoom NOTIFY viewportChanged)
    Q_PROPERTY(qreal panX READ panX WRITE setPanX NOTIFY viewportChanged)
    Q_PROPERTY(qreal panY READ panY WRITE setPanY NOTIFY viewportChanged)
    Q_PROPERTY(QColor brushColor READ brushColor WRITE setBrushColor NOTIFY brushChanged)
    Q_PROPERTY(qreal brushSize READ brushSize WRITE setBrushSize NOTIFY brushChanged)
    Q_PROPERTY(qreal brushOpacity READ brushOpacity WRITE setBrushOpacity NOTIFY brushChanged)
    Q_PROPERTY(qreal brushFlow READ brushFlow WRITE setBrushFlow NOTIFY brushChanged)
    Q_PROPERTY(qreal brushHardness READ brushHardness WRITE setBrushHardness NOTIFY brushChanged)
    Q_PROPERTY(bool eraser READ eraser WRITE setEraser NOTIFY brushChanged)
    Q_PROPERTY(bool canUndo READ canUndo NOTIFY undoRedoChanged)
    Q_PROPERTY(bool canRedo READ canRedo NOTIFY undoRedoChanged)
    Q_PROPERTY(qulonglong revision READ revision NOTIFY revisionChanged)
    Q_PROPERTY(QString lastError READ lastError NOTIFY lastErrorChanged)

public:
    explicit BitmapItem(QQuickItem *parent = nullptr);

    bool bind(Document &document, const std::string &assetId);
    bool bind(DocumentFile &file, const std::string &assetId);
    void unbind();
    [[nodiscard]] BitmapEditor &editor() noexcept;
    [[nodiscard]] const BitmapEditor &editor() const noexcept;
    [[nodiscard]] const RasterLayer *pixels() const noexcept;

    Q_INVOKABLE bool createBitmap(int width, int height);
    Q_INVOKABLE bool createBitmap(int width, int height, const QColor &clearColor);
    [[nodiscard]] bool bitmapReady() const noexcept;
    [[nodiscard]] int bitmapWidth() const noexcept;
    [[nodiscard]] int bitmapHeight() const noexcept;

    [[nodiscard]] qreal zoom() const noexcept;
    void setZoom(qreal zoom);
    [[nodiscard]] qreal panX() const noexcept;
    void setPanX(qreal panX);
    [[nodiscard]] qreal panY() const noexcept;
    void setPanY(qreal panY);
    Q_INVOKABLE void resetView();
    Q_INVOKABLE void fitToView();
    Q_INVOKABLE void panBy(qreal dx, qreal dy);
    Q_INVOKABLE void zoomAt(qreal factor, const QPointF &itemPosition);

    [[nodiscard]] QColor brushColor() const;
    void setBrushColor(const QColor &color);
    [[nodiscard]] qreal brushSize() const noexcept;
    void setBrushSize(qreal size);
    [[nodiscard]] qreal brushOpacity() const noexcept;
    void setBrushOpacity(qreal opacity);
    [[nodiscard]] qreal brushFlow() const noexcept;
    void setBrushFlow(qreal flow);
    [[nodiscard]] qreal brushHardness() const noexcept;
    void setBrushHardness(qreal hardness);
    [[nodiscard]] bool eraser() const noexcept;
    void setEraser(bool enabled);

    Q_INVOKABLE bool setPixel(int x, int y, const QColor &color);
    Q_INVOKABLE QColor pixelColor(int x, int y) const;
    Q_INVOKABLE bool clear();
    Q_INVOKABLE bool clear(const QColor &color);
    Q_INVOKABLE bool beginStrokeAt(const QPointF &bitmapPosition, qreal pressure = 1.0);
    Q_INVOKABLE bool continueStrokeAt(const QPointF &bitmapPosition, qreal pressure = 1.0);
    Q_INVOKABLE bool endStrokeAt(const QPointF &bitmapPosition, qreal pressure = 1.0);
    Q_INVOKABLE void cancelStroke();
    [[nodiscard]] bool canUndo() const noexcept;
    [[nodiscard]] bool canRedo() const noexcept;
    Q_INVOKABLE bool undo();
    Q_INVOKABLE bool redo();
    Q_INVOKABLE void refresh();

    [[nodiscard]] qulonglong revision() const noexcept;
    [[nodiscard]] QString lastError() const;

    void paint(QPainter *painter) override;

signals:
    void bitmapChanged();
    void viewportChanged();
    void brushChanged();
    void undoRedoChanged();
    void revisionChanged();
    void lastErrorChanged();

protected:
    void mousePressEvent(QMouseEvent *event) override;
    void mouseMoveEvent(QMouseEvent *event) override;
    void mouseReleaseEvent(QMouseEvent *event) override;
    void mouseUngrabEvent() override;
    void wheelEvent(QWheelEvent *event) override;

private:
    [[nodiscard]] QPointF mapItemToBitmap(const QPointF &position) const;
    void applyBrush(const BitmapBrush &brush);
    bool finishEdit(bool success);
    void notifyStateChange(std::uint64_t priorRevision,
                           bool priorCanUndo,
                           bool priorCanRedo);

    Document m_ownedDocument;
    BitmapEditor m_editor;
    qreal m_zoom = 1.0;
    qreal m_panX = 0.0;
    qreal m_panY = 0.0;
    QPointF m_lastPanPosition;
    bool m_panning = false;
};

IISHAREDCANVAS_EXPORT int registerIiSharedCanvasQmlTypes();

} // namespace iiSharedCanvas
