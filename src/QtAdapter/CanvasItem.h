#pragma once

#include "Bitmap/BitmapEditor.h"
#include "Bitmap/ChunkedBitmapEditor.h"
#include "Document/DocumentEditor.h"
#include "Export.h"
#include "File/DocumentFile.h"
#include "QtAdapter/AsyncFrameRenderer.h"

#include <QColor>
#include <QPointF>
#include <QQuickItem>
#include <QString>
#include <QVariantMap>

#include <cstddef>
#include <cstdint>
#include <functional>
#include <memory>
#include <string>
#include <vector>

class QEvent;
class QMouseEvent;
class QPainter;
class QSGNode;
class QTabletEvent;
class QWheelEvent;

namespace iiSharedCanvas {

class IISHAREDCANVAS_EXPORT CanvasItem : public QQuickItem {
    Q_OBJECT
    Q_PROPERTY(bool documentReady READ documentReady NOTIFY documentChanged)
    Q_PROPERTY(int canvasWidth READ canvasWidth NOTIFY documentChanged)
    Q_PROPERTY(int canvasHeight READ canvasHeight NOTIFY documentChanged)
    Q_PROPERTY(bool infiniteCanvas READ infiniteCanvas NOTIFY documentChanged)
    Q_PROPERTY(int canvasOriginX READ canvasOriginX NOTIFY documentChanged)
    Q_PROPERTY(int canvasOriginY READ canvasOriginY NOTIFY documentChanged)
    Q_PROPERTY(int canvasChunkSize READ canvasChunkSize NOTIFY documentChanged)
    Q_PROPERTY(quint32 frame READ frame WRITE setFrame NOTIFY frameChanged)
    Q_PROPERTY(quint32 frameCount READ frameCount NOTIFY documentChanged)
    Q_PROPERTY(QString selectedLayerId READ selectedLayerId NOTIFY selectionChanged)
    Q_PROPERTY(bool rasterLayerSelected READ rasterLayerSelected NOTIFY selectionChanged)
    Q_PROPERTY(QString toolMode READ toolMode WRITE setToolMode NOTIFY toolModeChanged)
    Q_PROPERTY(QColor brushColor READ brushColor WRITE setBrushColor NOTIFY brushChanged)
    Q_PROPERTY(qreal brushSize READ brushSize WRITE setBrushSize NOTIFY brushChanged)
    Q_PROPERTY(qreal brushOpacity READ brushOpacity WRITE setBrushOpacity NOTIFY brushChanged)
    Q_PROPERTY(bool brushOpacityEnabled READ brushOpacityEnabled WRITE setBrushOpacityEnabled NOTIFY brushChanged)
    Q_PROPERTY(qreal brushFlow READ brushFlow WRITE setBrushFlow NOTIFY brushChanged)
    Q_PROPERTY(bool brushFlowEnabled READ brushFlowEnabled WRITE setBrushFlowEnabled NOTIFY brushChanged)
    Q_PROPERTY(qreal brushHardness READ brushHardness WRITE setBrushHardness NOTIFY brushChanged)
    Q_PROPERTY(bool brushHardnessEnabled READ brushHardnessEnabled WRITE setBrushHardnessEnabled NOTIFY brushChanged)
    Q_PROPERTY(qreal brushSpacing READ brushSpacing WRITE setBrushSpacing NOTIFY brushChanged)
    Q_PROPERTY(qreal brushSpacingRatio READ brushSpacingRatio WRITE setBrushSpacingRatio NOTIFY brushChanged)
    Q_PROPERTY(bool brushSpacingEnabled READ brushSpacingEnabled WRITE setBrushSpacingEnabled NOTIFY brushChanged)
    Q_PROPERTY(qreal pressureCurveMinimum READ pressureCurveMinimum WRITE setPressureCurveMinimum NOTIFY strokeSettingsChanged)
    Q_PROPERTY(qreal pressureCurveCenter READ pressureCurveCenter WRITE setPressureCurveCenter NOTIFY strokeSettingsChanged)
    Q_PROPERTY(qreal pressureCurveMaximum READ pressureCurveMaximum WRITE setPressureCurveMaximum NOTIFY strokeSettingsChanged)
    Q_PROPERTY(bool pressureToOpacityEnabled READ pressureToOpacityEnabled WRITE setPressureToOpacityEnabled NOTIFY brushChanged)
    Q_PROPERTY(qreal stabilizerStrength READ stabilizerStrength WRITE setStabilizerStrength NOTIFY strokeSettingsChanged)
    Q_PROPERTY(int livePreviewFrameIntervalMs READ livePreviewFrameIntervalMs WRITE setLivePreviewFrameIntervalMs NOTIFY livePreviewFrameIntervalMsChanged)
    Q_PROPERTY(bool multithreadedEventsEnabled READ multithreadedEventsEnabled WRITE setMultithreadedEventsEnabled NOTIFY multithreadedEventsEnabledChanged)
    Q_PROPERTY(bool eraser READ eraser WRITE setEraser NOTIFY brushChanged)
    Q_PROPERTY(bool liveStrokeActive READ liveStrokeActive NOTIFY liveStrokeActiveChanged)
    Q_PROPERTY(int strokeCount READ strokeCount NOTIFY strokeCountChanged)
    Q_PROPERTY(QString inputDevice READ inputDevice NOTIFY inputStateChanged)
    Q_PROPERTY(qreal inputPressure READ inputPressure NOTIFY inputStateChanged)
    Q_PROPERTY(bool canUndo READ canUndo NOTIFY undoRedoChanged)
    Q_PROPERTY(bool canRedo READ canRedo NOTIFY undoRedoChanged)
    Q_PROPERTY(qreal zoom READ zoom WRITE setZoom NOTIFY viewportChanged)
    Q_PROPERTY(qreal panX READ panX WRITE setPanX NOTIFY viewportChanged)
    Q_PROPERTY(qreal panY READ panY WRITE setPanY NOTIFY viewportChanged)
    Q_PROPERTY(bool rendering READ rendering NOTIFY renderingChanged)
    Q_PROPERTY(int renderTileSize READ renderTileSize CONSTANT)
    Q_PROPERTY(int residentTileCount READ residentTileCount NOTIFY residentTileCountChanged)
    Q_PROPERTY(int residentLayerTileCount READ residentLayerTileCount NOTIFY residentLayerTileCountChanged)
    Q_PROPERTY(bool gpuAccelerated READ gpuAccelerated NOTIFY graphicsBackendChanged)
    Q_PROPERTY(QString graphicsBackend READ graphicsBackend NOTIFY graphicsBackendChanged)
    Q_PROPERTY(qulonglong revision READ revision NOTIFY revisionChanged)
    Q_PROPERTY(QString lastError READ lastError NOTIFY lastErrorChanged)
    Q_PROPERTY(QString filePath READ filePath NOTIFY documentChanged)

public:
    explicit CanvasItem(QQuickItem *parent = nullptr);

    bool bind(Document &document);
    bool bind(DocumentFile &file);
    Q_INVOKABLE bool createFile(const QString &path, int width, int height, quint32 frameCount = 1);
    Q_INVOKABLE bool openFile(const QString &path);
    [[nodiscard]] QString filePath() const;
    void unbind();
    [[nodiscard]] Document *document() noexcept;
    [[nodiscard]] const Document *document() const noexcept;
    [[nodiscard]] DocumentEditor *documentEditor() noexcept;
    [[nodiscard]] const DocumentEditor *documentEditor() const noexcept;
    DocumentEditResult editDocument(
        const std::function<DocumentEditResult(DocumentEditor &)> &edit);
    [[nodiscard]] const RasterLayer *framePixels() const noexcept;

    Q_INVOKABLE bool createDocument(int width, int height, quint32 frameCount = 1);
    Q_INVOKABLE bool createRasterDocument(int width, int height, quint32 frameCount = 1);
    Q_INVOKABLE bool createInfiniteRasterDocument(int width,
                                                  int height,
                                                  int chunkSize = 256,
                                                  quint32 frameCount = 1);
    [[nodiscard]] bool documentReady() const noexcept;
    [[nodiscard]] int canvasWidth() const noexcept;
    [[nodiscard]] int canvasHeight() const noexcept;
    [[nodiscard]] bool infiniteCanvas() const noexcept;
    [[nodiscard]] int canvasOriginX() const noexcept;
    [[nodiscard]] int canvasOriginY() const noexcept;
    [[nodiscard]] int canvasChunkSize() const noexcept;
    Q_INVOKABLE QVariantMap ensureInfiniteCanvasRegion(qreal x,
                                                       qreal y,
                                                       qreal width,
                                                       qreal height);
    [[nodiscard]] quint32 frame() const noexcept;
    void setFrame(quint32 frame);
    [[nodiscard]] quint32 frameCount() const noexcept;

    Q_INVOKABLE bool selectLayer(const QString &layerId);
    Q_INVOKABLE void clearSelection();
    [[nodiscard]] QString selectedLayerId() const;
    [[nodiscard]] bool rasterLayerSelected() const noexcept;
    [[nodiscard]] const RasterLayer *selectedRasterPixels() const noexcept;
    bool replaceSelectedPixels(const RasterLayer &pixels);

    [[nodiscard]] QString toolMode() const;
    void setToolMode(const QString &mode);

    [[nodiscard]] QColor brushColor() const;
    void setBrushColor(const QColor &color);
    [[nodiscard]] qreal brushSize() const noexcept;
    void setBrushSize(qreal size);
    [[nodiscard]] qreal brushOpacity() const noexcept;
    void setBrushOpacity(qreal opacity);
    [[nodiscard]] bool brushOpacityEnabled() const noexcept;
    void setBrushOpacityEnabled(bool enabled);
    [[nodiscard]] qreal brushFlow() const noexcept;
    void setBrushFlow(qreal flow);
    [[nodiscard]] bool brushFlowEnabled() const noexcept;
    void setBrushFlowEnabled(bool enabled);
    [[nodiscard]] qreal brushHardness() const noexcept;
    void setBrushHardness(qreal hardness);
    [[nodiscard]] bool brushHardnessEnabled() const noexcept;
    void setBrushHardnessEnabled(bool enabled);
    [[nodiscard]] qreal brushSpacing() const noexcept;
    void setBrushSpacing(qreal spacing);
    [[nodiscard]] qreal brushSpacingRatio() const noexcept;
    void setBrushSpacingRatio(qreal spacingRatio);
    [[nodiscard]] bool brushSpacingEnabled() const noexcept;
    void setBrushSpacingEnabled(bool enabled);
    [[nodiscard]] qreal pressureCurveMinimum() const noexcept;
    void setPressureCurveMinimum(qreal value);
    [[nodiscard]] qreal pressureCurveCenter() const noexcept;
    void setPressureCurveCenter(qreal value);
    [[nodiscard]] qreal pressureCurveMaximum() const noexcept;
    void setPressureCurveMaximum(qreal value);
    [[nodiscard]] bool pressureToOpacityEnabled() const noexcept;
    void setPressureToOpacityEnabled(bool enabled);
    [[nodiscard]] qreal stabilizerStrength() const noexcept;
    void setStabilizerStrength(qreal value);
    [[nodiscard]] int livePreviewFrameIntervalMs() const noexcept;
    void setLivePreviewFrameIntervalMs(int value);
    [[nodiscard]] bool multithreadedEventsEnabled() const noexcept;
    void setMultithreadedEventsEnabled(bool enabled);
    [[nodiscard]] bool eraser() const noexcept;
    void setEraser(bool enabled);

    Q_INVOKABLE bool clearSelectedLayer();
    Q_INVOKABLE bool beginStrokeAt(const QPointF &documentPosition, qreal pressure = 1.0);
    Q_INVOKABLE bool continueStrokeAt(const QPointF &documentPosition, qreal pressure = 1.0);
    Q_INVOKABLE bool endStrokeAt(const QPointF &documentPosition, qreal pressure = 1.0);
    Q_INVOKABLE void cancelStroke();
    [[nodiscard]] bool liveStrokeActive() const noexcept;
    [[nodiscard]] int strokeCount() const noexcept;
    [[nodiscard]] QString inputDevice() const;
    [[nodiscard]] qreal inputPressure() const noexcept;
    [[nodiscard]] bool canUndo() const noexcept;
    [[nodiscard]] bool canRedo() const noexcept;
    Q_INVOKABLE bool undo();
    Q_INVOKABLE bool redo();

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

    Q_INVOKABLE bool refresh();
    Q_INVOKABLE qulonglong refreshAsync();
    [[nodiscard]] bool rendering() const noexcept;
    [[nodiscard]] int renderTileSize() const noexcept;
    [[nodiscard]] int residentTileCount() const noexcept;
    [[nodiscard]] int residentLayerTileCount() const noexcept;
    [[nodiscard]] bool gpuAccelerated() const noexcept;
    [[nodiscard]] QString graphicsBackend() const;
    [[nodiscard]] qulonglong revision() const noexcept;
    [[nodiscard]] QString lastError() const;

    void paint(QPainter *painter);

signals:
    void documentChanged();
    void frameChanged();
    void selectionChanged();
    void toolModeChanged();
    void brushChanged();
    void strokeSettingsChanged();
    void livePreviewFrameIntervalMsChanged();
    void multithreadedEventsEnabledChanged();
    void liveStrokeActiveChanged();
    void strokeCountChanged();
    void inputStateChanged();
    void undoRedoChanged();
    void viewportChanged();
    void renderingChanged();
    void renderCompleted(qulonglong requestId);
    void residentTileCountChanged();
    void residentLayerTileCountChanged();
    void graphicsBackendChanged();
    void revisionChanged();
    void lastErrorChanged();

protected:
    bool event(QEvent *event) override;
    void mousePressEvent(QMouseEvent *event) override;
    void mouseMoveEvent(QMouseEvent *event) override;
    void mouseReleaseEvent(QMouseEvent *event) override;
    void mouseUngrabEvent() override;
    void wheelEvent(QWheelEvent *event) override;
    QSGNode *updatePaintNode(QSGNode *oldNode,
                             UpdatePaintNodeData *updatePaintNodeData) override;
    void geometryChange(const QRectF &newGeometry,
                        const QRectF &oldGeometry) override;

private:
    bool bindDocument(Document &document, DocumentFile *file);
    [[nodiscard]] bool fileBindingValid() const noexcept;
    [[nodiscard]] bool chunkedRasterSelected() const noexcept;
    [[nodiscard]] std::uint64_t activeEditorRevision() const noexcept;
    [[nodiscard]] bool activeEditorCanUndo() const noexcept;
    [[nodiscard]] bool activeEditorCanRedo() const noexcept;
    [[nodiscard]] bool activeEditorStrokeActive() const noexcept;
    [[nodiscard]] const std::string &activeEditorError() const noexcept;
    [[nodiscard]] Layer *selectedLayer() noexcept;
    [[nodiscard]] const Layer *selectedLayer() const noexcept;
    bool syncSelectedLayer();
    [[nodiscard]] bool mapDocumentToSelectedAsset(const QPointF &documentPosition,
                                                  DocumentPoint &assetPosition);
    [[nodiscard]] QPointF mapItemToDocument(const QPointF &itemPosition) const noexcept;
    [[nodiscard]] qreal normalizedPressure(qreal rawPressure, bool pressureSensitive) const noexcept;
    [[nodiscard]] QPointF stabilizedDocumentPosition(const QPointF &position, bool finish);
    void noteInputState(QString device, qreal pressure);
    void resetEditState();
    void recordCountHistory(int priorStrokeCount);
    void applyBrush(const BitmapBrush &brush);
    bool finishEdit(bool success);
    void notifyUndoRedo(bool priorCanUndo, bool priorCanRedo);
    struct CachedTile {
        CanvasRegion region;
        RasterLayer pixels;
        std::uint64_t contentGeneration = 0;
        std::uint64_t lastUse = 0;
    };
    struct CachedLayerTile {
        std::size_t layerIndex = 0;
        std::string layerId;
        CanvasRegion region;
        RasterLayer pixels;
        double opacity = 1.0;
        RasterBlendMode blendMode = RasterBlendMode::SourceOver;
        std::uint64_t contentGeneration = 0;
        std::uint64_t lastUse = 0;
    };

    void scheduleVisibleRender(bool contentChanged);
    void startScheduledRender();
    void applyAsyncRender(qulonglong requestId);
    [[nodiscard]] std::vector<FrameRenderTileRequest> visibleTileRequests() const;
    [[nodiscard]] bool hasCachedTile(const FrameRenderTileRequest &request) const noexcept;
    void trimTileCache();
    [[nodiscard]] bool canPresentLayerTiles() const noexcept;
    void clearTileCache();
    void updateRenderingState();
    void setLastError(QString message);

    Document m_ownedDocument;
    std::unique_ptr<DocumentFile> m_ownedFile;
    Document *m_document = nullptr;
    DocumentFile *m_file = nullptr;
    std::uint64_t m_fileGeneration = 0;
    DocumentEditor m_documentEditor;
    RasterLayer m_framePixels;
    AsyncFrameRenderer m_asyncRenderer;
    std::shared_ptr<const Document> m_renderSnapshot;
    std::vector<CachedTile> m_tileCache;
    std::vector<CachedLayerTile> m_layerTileCache;
    BitmapEditor m_editor;
    ChunkedBitmapEditor m_chunkedEditor;
    mutable RasterLayer m_selectedRasterCache;
    std::string m_selectedLayerId;
    FrameIndex m_frame = 0;
    std::uint64_t m_revision = 0;
    std::uint64_t m_contentGeneration = 0;
    std::uint64_t m_latestRequestContentGeneration = 0;
    std::uint64_t m_tileCacheRevision = 0;
    std::uint64_t m_tileUseCounter = 0;
    std::uint64_t m_strokeStartRevision = 0;
    qulonglong m_latestAsyncRequest = 0;
    qreal m_zoom = 1.0;
    qreal m_panX = 0.0;
    qreal m_panY = 0.0;
    QPointF m_lastPanPosition;
    QPointF m_stabilizedPosition;
    QString m_lastError;
    QString m_toolMode = QStringLiteral("brush");
    QString m_inputDevice = QStringLiteral("mouse");
    qreal m_inputPressure = 1.0;
    qreal m_pressureCurveMinimum = 0.0;
    qreal m_pressureCurveCenter = 0.5;
    qreal m_pressureCurveMaximum = 1.0;
    qreal m_stabilizerStrength = 0.0;
    int m_livePreviewFrameIntervalMs = 8;
    int m_strokeCount = 0;
    int m_strokeStartCount = 0;
    std::vector<int> m_undoStrokeCounts;
    std::vector<int> m_redoStrokeCounts;
    bool m_panning = false;
    bool m_documentValid = false;
    bool m_renderSchedulePending = false;
    bool m_reportedRendering = false;
    bool m_stabilizerActive = false;
    bool m_multithreadedEventsEnabled = true;
    bool m_tabletPointerActive = false;
    bool m_suppressMouseAfterTablet = false;
    bool m_tabletEraserOverride = false;
    bool m_tabletPreviousEraser = false;
};

} // namespace iiSharedCanvas
