#include <iiSharedCanvas.h>

#include <QDir>
#include <QGuiApplication>
#include <QImage>
#include <QPainter>
#include <QQmlComponent>
#include <QQmlEngine>

#include <iostream>
#include <memory>
#include <string>

namespace {

int failures = 0;

void expect(bool condition, const std::string &message)
{
    if (!condition) {
        std::cerr << message << '\n';
        ++failures;
    }
}

QImage render(iiSharedCanvas::CanvasItem &item, int width, int height)
{
    item.setWidth(width);
    item.setHeight(height);
    QImage output(width, height, QImage::Format_ARGB32);
    output.fill(Qt::transparent);
    QPainter painter(&output);
    item.paint(&painter);
    return output;
}

iiSharedCanvas::VectorAsset filledRectangle(std::string id,
                                            int width,
                                            int height,
                                            std::uint32_t argb)
{
    using namespace iiSharedCanvas;
    VectorPath path;
    path.commands = {
        MoveTo{{1.0, 1.0}},
        LineTo{{static_cast<double>(width - 1), 1.0}},
        LineTo{{static_cast<double>(width - 1), static_cast<double>(height - 1)}},
        LineTo{{1.0, static_cast<double>(height - 1)}},
        ClosePath{},
    };
    path.fill = SolidPaint{argb};
    return {std::move(id), {width, height}, {std::move(path)}};
}

iiSharedCanvas::Document mixedDocument()
{
    using namespace iiSharedCanvas;
    Document document;
    document.extent = {4, 4};
    document.timeline = {{24, 1}, 2};
    document.assets.emplace_back(RasterAsset{"background-0", makeRasterLayer(4, 4, 0xff102030U)});
    document.assets.emplace_back(RasterAsset{"background-1", makeRasterLayer(4, 4, 0xff304050U)});
    document.assets.emplace_back(filledRectangle("vector", 4, 4, 0xffffcc00U));
    document.layers.push_back({"background", "Background", true, 1.0, {},
                               RasterBlendMode::SourceOver,
                               KeyframedSource{ContentKind::Raster,
                                               {{0, "background-0"}, {1, "background-1"}}}});
    document.layers.push_back({"vector", "Vector", true, 1.0, {},
                               RasterBlendMode::SourceOver, StaticSource{"vector"}});
    return document;
}

} // namespace

int main(int argc, char **argv)
{
    qputenv("QT_QPA_PLATFORM", "offscreen");
    QGuiApplication application(argc, argv);

    using namespace iiSharedCanvas;

    expect(registerIiSharedCanvasQmlTypes() >= 0,
           "iiSharedCanvas QML types must register once");
    QQmlEngine engine;
    QQmlComponent component(&engine);
    component.setData("import QtQuick\nimport iiSharedCanvas 1.0\nSharedCanvas {}\n", QUrl{});
    std::unique_ptr<QObject> qmlObject(component.create());
    expect(qmlObject && qobject_cast<CanvasItem *>(qmlObject.get()),
           "SharedCanvas must be constructible by an actual QML engine");

    Document document = mixedDocument();
    CanvasItem item;
    expect(item.bind(document), "CanvasItem must bind a valid caller-owned document");
    expect(item.documentReady() && item.canvasWidth() == 4 && item.canvasHeight() == 4,
           "CanvasItem must expose document readiness and extent");
    expect(item.frame() == 0 && item.frameCount() == 2,
           "CanvasItem must expose the current frame and timeline length");

    QImage output = render(item, 4, 4);
    expect(output.pixel(0, 0) == 0xff102030U,
           "CanvasItem must display the resolved raster background");
    expect(output.pixel(1, 1) == 0xffffcc00U,
           "CanvasItem must display native vector content in the same frame");

    item.setFrame(1);
    output = render(item, 4, 4);
    expect(output.pixel(0, 0) == 0xff304050U,
           "CanvasItem frame changes must re-evaluate raster keyframes");
    expect(output.pixel(1, 1) == 0xffffcc00U,
           "static vector content must remain visible across frames");

    expect(item.selectLayer(QStringLiteral("background")) && item.rasterLayerSelected(),
           "CanvasItem must select the current raster asset through its document layer");
    item.setBrushColor(QColor::fromRgba(0xffff00ffU));
    item.setBrushSize(1.0);
    item.setBrushHardness(1.0);
    expect(item.beginStrokeAt({0.0, 0.0}, 1.0)
               && item.endStrokeAt({0.0, 0.0}, 1.0),
           "CanvasItem must author the selected raster layer in document coordinates");
    output = render(item, 4, 4);
    expect(output.pixelColor(0, 0).red() > output.pixelColor(0, 0).green()
               && output.pixelColor(0, 0).blue() > output.pixelColor(0, 0).green(),
           "selected-layer brush changes must be visible in the composed frame immediately");
    expect(item.selectLayer(QStringLiteral("background")) && item.canUndo() && item.undo(),
           "reselecting the same resolved raster must preserve its edit history");
    output = render(item, 4, 4);
    expect(output.pixel(0, 0) == 0xff304050U,
           "selected-raster undo must restore the authoritative frame asset pixels");

    std::get<RasterAsset>(document.assets[1]).pixels.pixels[0] = 0xff00ff00U;
    expect(item.refresh(), "CanvasItem must refresh caller-owned document mutations");
    output = render(item, 4, 4);
    expect(output.pixel(0, 0) == 0xff00ff00U,
           "refresh must repaint the current mixed frame from authoritative document pixels");

    item.setFrame(2);
    expect(item.frame() == 1 && !item.lastError().isEmpty(),
           "an out-of-range QML frame assignment must fail closed without changing frame");

    item.setZoom(2.0);
    item.setPanX(0.0);
    item.setPanY(0.0);
    output = render(item, 8, 8);
    expect(output.pixel(0, 0) == output.pixel(1, 1),
           "mixed document zoom must preserve nearest-neighbor pixel presentation");

    CanvasItem owned;
    expect(owned.createDocument(3, 2, 3),
           "QML hosts must be able to create an empty owned document");
    expect(owned.documentReady() && owned.frameCount() == 3,
           "an owned empty document must be a renderable transparent timeline");

    CanvasItem editableOwned;
    expect(editableOwned.createRasterDocument(8, 6, 2)
               && editableOwned.rasterLayerSelected(),
           "product hosts must be able to create an immediately editable raster document");
    expect(editableOwned.toolMode() == QStringLiteral("brush")
               && editableOwned.strokeCount() == 0
               && editableOwned.inputDevice() == QStringLiteral("mouse"),
           "an editable document must expose deterministic initial tool and input state");
    editableOwned.setBrushSpacing(3.0);
    editableOwned.setBrushSpacingRatio(0.0);
    editableOwned.setBrushSpacingEnabled(false);
    editableOwned.setBrushFlowEnabled(false);
    editableOwned.setBrushOpacityEnabled(false);
    editableOwned.setBrushHardnessEnabled(false);
    editableOwned.setPressureCurveMinimum(0.2);
    editableOwned.setPressureCurveMaximum(0.8);
    editableOwned.setPressureCurveCenter(0.6);
    editableOwned.setPressureToOpacityEnabled(false);
    editableOwned.setStabilizerStrength(0.44);
    editableOwned.setLivePreviewFrameIntervalMs(12);
    editableOwned.setMultithreadedEventsEnabled(false);
    expect(editableOwned.brushSpacing() == 3.0
               && editableOwned.brushSpacingRatio() == 0.0
               && !editableOwned.brushSpacingEnabled()
               && !editableOwned.brushFlowEnabled()
               && !editableOwned.brushOpacityEnabled()
               && !editableOwned.brushHardnessEnabled()
               && editableOwned.pressureCurveMinimum() == 0.2
               && editableOwned.pressureCurveCenter() == 0.6
               && editableOwned.pressureCurveMaximum() == 0.8
               && !editableOwned.pressureToOpacityEnabled()
               && editableOwned.stabilizerStrength() == 0.44
               && editableOwned.livePreviewFrameIntervalMs() == 12
               && !editableOwned.multithreadedEventsEnabled(),
           "SharedCanvas must expose its product-neutral brush and input contract");
    editableOwned.setBrushFlowEnabled(true);
    editableOwned.setBrushOpacityEnabled(true);
    editableOwned.setBrushHardnessEnabled(true);
    editableOwned.setBrushSpacingEnabled(true);
    editableOwned.setToolMode(QStringLiteral("eraser"));
    expect(editableOwned.eraser() && editableOwned.toolMode() == QStringLiteral("eraser"),
           "eraser tool mode must select destructive bitmap compositing");
    editableOwned.setToolMode(QStringLiteral("brush"));
    editableOwned.setBrushColor(QColor::fromRgba(0xff26c6daU));
    editableOwned.setBrushSize(2.0);
    expect(editableOwned.beginStrokeAt({1.0, 1.0}, 0.5)
               && editableOwned.endStrokeAt({4.0, 1.0}, 0.5)
               && editableOwned.strokeCount() == 1,
           "one committed product-host stroke must update the exposed stroke count once");
    expect(editableOwned.undo() && editableOwned.strokeCount() == 0
               && editableOwned.redo() && editableOwned.strokeCount() == 1,
           "stroke count must follow selected-raster undo and redo history");
    RasterLayer replacement = makeRasterLayer(8, 6, 0xffabcdefU);
    expect(editableOwned.replaceSelectedPixels(replacement)
               && editableOwned.selectedRasterPixels()
               && editableOwned.selectedRasterPixels()->pixels == replacement.pixels,
           "a generic image importer must replace selected raster pixels without a temporary file");

    const DocumentEditResult opacityEdit = editableOwned.editDocument(
        [](DocumentEditor &editor) {
            return editor.setLayerOpacity("canvas.layer.0", 0.5);
        });
    expect(opacityEdit.ok() && opacityEdit.changed
               && editableOwned.documentEditor()
               && editableOwned.documentEditor()->revision() == 1
               && editableOwned.document()
               && editableOwned.document()->layers.front().opacity == 0.5,
           "CanvasItem must expose its structural editor and apply edits to owned document data");
    const RasterLayer *editedFrame = editableOwned.framePixels();
    expect(editedFrame
               && rasterLayerPixelAt(*editedFrame, {0, 0}) == 0x80abcdefU,
           "CanvasItem structural edits must rerender the current mixed frame automatically");
    editableOwned.setFrame(1);
    const DocumentEditResult timelineEdit = editableOwned.editDocument(
        [](DocumentEditor &editor) {
            return editor.setFrameCount(1);
        });
    expect(timelineEdit.ok() && timelineEdit.changed && editableOwned.frame() == 0,
           "CanvasItem must clamp its current frame after a valid timeline shrink");

    Document transformedDocument;
    transformedDocument.extent = {3, 1};
    transformedDocument.timeline = {{24, 1}, 1};
    transformedDocument.assets.emplace_back(
        RasterAsset{"paint", makeRasterLayer(1, 1, 0x00000000U)});
    Layer transformedLayer{"paint-layer", "Paint", true, 1.0, {},
                           RasterBlendMode::SourceOver, StaticSource{"paint"}};
    transformedLayer.transform.translationX = 1.0;
    transformedDocument.layers.push_back(transformedLayer);
    CanvasItem transformedItem;
    expect(transformedItem.bind(transformedDocument)
               && transformedItem.selectLayer(QStringLiteral("paint-layer")),
           "a transformed raster document layer must be selectable");
    transformedItem.setBrushColor(QColor::fromRgba(0xff22d3eeU));
    transformedItem.setBrushSize(1.0);
    expect(transformedItem.beginStrokeAt({1.0, 0.0}, 1.0)
               && transformedItem.endStrokeAt({1.0, 0.0}, 1.0),
           "brush input must invert the selected layer transform");
    output = render(transformedItem, 3, 1);
    expect(output.pixelColor(1, 0).alpha() > 0
               && output.pixel(0, 0) == 0x00000000U,
           "transformed raster editing must land in the selected layer footprint only");

    CanvasItem artifact;
    Document artifactDocument = mixedDocument();
    expect(artifact.bind(artifactDocument), "mixed render artifact document must bind");
    artifact.setZoom(24.0);
    const QImage artifactImage = render(artifact, 96, 96);
    const QString outputDirectory = QStringLiteral(IISHAREDCANVAS_TEST_OUTPUT_DIR);
    expect(QDir().mkpath(outputDirectory),
           "test output directory must be creatable inside build/");
    expect(artifactImage.save(outputDirectory + QStringLiteral("/shared-canvas-item.png")),
           "the mixed CanvasItem verification artifact must be written as PNG");

    return failures == 0 ? 0 : 1;
}
