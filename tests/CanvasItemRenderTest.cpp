#include <iiSharedCanvas.h>

#include <QDir>
#include <QElapsedTimer>
#include <QGuiApplication>
#include <QImage>
#include <QPainter>
#include <QQmlComponent>
#include <QQmlEngine>
#include <QQuickWindow>
#include <QThread>

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
    QElapsedTimer timeout;
    timeout.start();
    while (item.rendering() && timeout.elapsed() < 5000) {
        QCoreApplication::processEvents(QEventLoop::AllEvents, 10);
        QThread::msleep(1);
    }
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
    document.layers.emplace_back(DynamicBitmapLayer{
        {"background", "Background", true, 1.0, {}, RasterBlendMode::SourceOver},
        KeyframedSource{{0, 1}},
    });
    document.layers.emplace_back(StaticVectorLayer{
        {"vector", "Vector", true, 1.0, {}, RasterBlendMode::SourceOver,
         LayerFrameRange{0, 0}},
        StaticSource{"vector"},
    });
    document.frames = {
        {0, {{"background", "background-0"}}},
        {1, {{"background", "background-1"}}},
    };
    return document;
}

} // namespace

int main(int argc, char **argv)
{
    if (qEnvironmentVariableIsEmpty("QT_QPA_PLATFORM")) {
        qputenv("QT_QPA_PLATFORM", "offscreen");
    }
    QGuiApplication application(argc, argv);
    {
        QQuickWindow host;
        auto *retiring = new iiSharedCanvas::CanvasItem(host.contentItem());
        int backendChanges = 0;
        QObject::connect(retiring, &iiSharedCanvas::CanvasItem::graphicsBackendChanged,
                         &host, [&] { ++backendChanges; });
        delete retiring;
        expect(backendChanges == 0,
               "base-item teardown must not call the destroyed canvas window tracker");
    }

    using namespace iiSharedCanvas;

    {
        Document frames;
        frames.extent = {8, 4}; frames.timeline.frameCount = 3;
        DocumentEditor editor(frames);
        expect(editor.insertRasterAsset("frame-bitmap-0", makeRasterLayer(4, 4, 0xff102030U)).ok(), "dynamic adapter bitmap seed");
        auto seed = filledRectangle("frame-vector-0", 4, 4, 0xff00ff00U);
        expect(editor.insertVectorAsset(seed.id, seed.viewport, seed.paths).ok(), "dynamic adapter vector seed");
        expect(editor.insertDynamicLayer({"bitmap"}, LayerRepresentation::Bitmap, {{0,"frame-bitmap-0"}}).ok(), "dynamic adapter bitmap layer");
        LayerProperties p; p.id = "vector"; p.transform.translationX = 4;
        expect(editor.insertDynamicLayer(p, LayerRepresentation::Vector, {{0,"frame-vector-0"}}).ok(), "dynamic adapter vector layer");
        const std::uint32_t bitmapColors[] = {0xff102030U, 0xff304050U, 0xff506070U};
        const std::uint32_t vectorColors[] = {0xff00ff00U, 0xffff0000U, 0xff0000ffU};
        for (FrameIndex frame = 1; frame < 3; ++frame) {
            expect(editor.setDynamicFrameContent("bitmap", frame,
                RasterAsset{"frame-bitmap-" + std::to_string(frame), makeRasterLayer(4,4,bitmapColors[frame])}).changed,
                "author independent adapter bitmap frames");
            auto geometry = filledRectangle("frame-vector-" + std::to_string(frame),4,4,vectorColors[frame]);
            expect(editor.setDynamicFrameContent("vector",frame,std::move(geometry)).changed,
                "author independent adapter vector frames");
        }
        CanvasItem view;
        expect(view.bind(frames), "CanvasItem binds independently authored dynamic content");
        for (const FrameIndex frame : {0U, 2U, 1U, 0U, 2U}) {
            view.setFrame(frame);
            const auto image = render(view,8,4);
            expect(image.pixel(0,0) == bitmapColors[frame] && image.pixel(5,1) == vectorColors[frame],
                "CanvasItem refreshes both bitmap and vector content when scrubbing in either direction");
            expect(view.framePixels() && view.framePixels()->pixels == renderFrame(frames,frame).pixels.pixels,
                "CanvasItem's complete cached frame agrees with native per-frame content");
        }
    }

    expect(registerIiSharedCanvasQmlTypes() >= 0,
           "iiSharedCanvas QML types must register once");
    QQmlEngine engine;
    QQmlComponent component(&engine);
    component.setData("import QtQuick\nimport iiSharedCanvas 1.0\nSharedCanvas {}\n", QUrl{});
    std::unique_ptr<QObject> qmlObject(component.create());
    expect(qmlObject && qobject_cast<CanvasItem *>(qmlObject.get()),
           "SharedCanvas must be constructible by an actual QML engine");

    {
        auto controlled = mixedDocument();
        controlled.assets.emplace_back(RasterAsset{"semantic-mask", makeRasterLayer(4,4,0xff112233U)});
        SemanticSegmentLayer semantic; semantic.properties = {"semantic", "Control only"};
        semantic.source = StaticSource{"semantic-mask"}; semantic.segmentation.taxonomy.id = "test";
        SemanticClass category; category.id = 1; category.key = "object"; category.name = "Object";
        semantic.segmentation.taxonomy.classes = {category};
        SemanticRegion region; region.id = 1; region.classId = 1; region.maskColor = 0xff112233U;
        semantic.segmentation.regions = {region}; controlled.layers.emplace_back(semantic);
        CanvasItem controlledItem;
        expect(controlledItem.bind(controlled), "CanvasItem accepts dedicated ControlNet layers");
        const auto controlledImage = render(controlledItem,4,4);
        expect(controlledImage.pixel(0,0) == 0xff102030U && controlledImage.pixel(1,1) == 0xffffcc00U
            && controlledItem.residentLayerTileCount() == 2,
            "ControlNet masks stay out of composed and independent artwork textures");
    }

    Document document = mixedDocument();
    {
        Document artboards;
        artboards.extent = {2, 2};
        artboards.artboards = {{"left", "Left", {{-2,0},{2,2}},0xffff0000U},
                               {"right", "Right", {{3,0},{3,2}},0xff0000ffU}};
        artboards.assets.emplace_back(RasterAsset{"paint", makeRasterLayer(1,1,0xff00ff00U)});
        LayerProperties p; p.id = "paint-layer"; p.artboardId = "right";
        artboards.layers.emplace_back(StaticBitmapLayer{p, StaticSource{"paint"}});
        CanvasItem view;
        expect(view.bind(artboards) && view.canvasWidth() == 8 && view.canvasOriginX() == -2,
               "CanvasItem must expose the complete artboard workspace");
        auto image = render(view,8,2);
        expect(view.framePixels() && view.framePixels()->width == 8,
               "complete cached frame access must use artboard workspace dimensions");
        expect(image.pixel(0,0) == 0xffff0000U && image.pixel(2,0) == 0
            && image.pixel(5,0) == 0xff00ff00U && image.pixel(6,0) == 0xff0000ffU,
            "CanvasItem must display grouped artboard backgrounds and local layers");
        view.setBrushColor(QColor::fromRgba(0xffff00ffU));
        view.setBrushSize(1.0); view.setBrushHardness(1.0);
        expect(view.selectLayer(QStringLiteral("paint-layer"))
            && view.beginStrokeAt({3,0}) && view.endStrokeAt({3,0}),
            "CanvasItem editing must map workspace positions into artboard-local asset pixels");
        image = render(view,8,2);
        expect(image.pixel(5,0) == 0xffff00ffU,
               "artboard-local pixel edits must refresh composed presentation");
        image.save(QString::fromUtf8(IISHAREDCANVAS_TEST_OUTPUT_DIR) + QStringLiteral("/artboard-adapter.png"));
    }
    CanvasItem item;
    expect(item.bind(document), "CanvasItem must bind a valid caller-owned document");
    expect(item.documentReady() && item.canvasWidth() == 4 && item.canvasHeight() == 4,
           "CanvasItem must expose document readiness and extent");
    expect(item.frame() == 0 && item.frameCount() == 2,
           "CanvasItem must expose the current frame and timeline length");
    expect(item.rendering() && item.renderTileSize() == 512,
           "CanvasItem binding must schedule bounded asynchronous texture-tile rendering");
    expect(!item.gpuAccelerated()
               && item.graphicsBackend() == QStringLiteral("unavailable"),
           "a detached CanvasItem must not falsely claim an active hardware backend");

    QImage output = render(item, 4, 4);
    expect(item.residentTileCount() == 1
               && item.residentLayerTileCount() == 2,
           "CanvasItem must retain one independent texture tile for each visible layer");
    expect(output.pixel(0, 0) == 0xff102030U,
           "CanvasItem must display the resolved raster background");
    expect(output.pixel(1, 1) == 0xffffcc00U,
           "CanvasItem must display native vector content in the same frame");

    item.setFrame(1);
    output = render(item, 4, 4);
    expect(output.pixel(0, 0) == 0xff304050U,
           "CanvasItem frame changes must re-evaluate raster keyframes");
    expect(output.pixel(1, 1) == 0xff304050U
               && item.residentLayerTileCount() == 1,
           "CanvasItem must stop requesting and presenting a layer outside its frame range");

    const DocumentEditResult widenedRange = item.editDocument(
        [](DocumentEditor &editor) {
            return editor.setLayerFrameRange("vector", LayerFrameRange{0, 1});
        });
    output = render(item, 4, 4);
    expect(widenedRange.ok() && widenedRange.changed
               && output.pixel(1, 1) == 0xffffcc00U
               && item.residentLayerTileCount() == 2,
           "widening a range on the current frame must replace composed and per-layer cache generations");
    const DocumentEditResult narrowedRange = item.editDocument(
        [](DocumentEditor &editor) {
            return editor.setLayerFrameRange("vector", LayerFrameRange{0, 0});
        });
    output = render(item, 4, 4);
    expect(narrowedRange.ok() && narrowedRange.changed
               && output.pixel(1, 1) == 0xff304050U
               && item.residentLayerTileCount() == 1,
           "narrowing a range on the current frame must evict stale layer presentation tiles");

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

    expect(item.smoothRendering(), "canvas presentation defaults to smooth sampling");
    const auto revisionBeforeSampling = item.revision();
    item.setSmoothRendering(false);
    expect(item.revision() == revisionBeforeSampling, "sampling policy must not dirty the document");
    item.setZoom(2.0);
    item.setPanX(0.0);
    item.setPanY(0.0);
    output = render(item, 8, 8);
    expect(output.pixel(0, 0) == output.pixel(1, 1),
           "mixed document zoom must preserve nearest-neighbor pixel presentation");

    {
        Document detail;
        detail.extent = {64, 64};
        auto checker = makeRasterLayer(64, 64);
        for (int y = 0; y < 64; ++y) for (int x = 0; x < 64; ++x)
            checker.pixels[y * 64 + x] = (x + y) % 2 ? 0xffffffffU : 0xff000000U;
        detail.assets.emplace_back(RasterAsset{"detail", checker});
        detail.layers.emplace_back(StaticBitmapLayer{{"detail-layer", "Detail"}, StaticSource{"detail"}});
        CanvasItem preview;
        expect(preview.bind(detail), "detailed raster must bind");
        preview.setZoom(0.25);
        const auto reduced = render(preview, 16, 16);
        expect(reduced.pixel(8, 8) == 0xff808080U, "canvas paint must display the area-filtered raster emission");
        preview.setZoom(0.45);
        expect(preview.renderLevelOfDetail() == 2, "LOD must not round down physical texture resolution");
        preview.setZoom(0.8);
        expect(preview.renderLevelOfDetail() == 1, "non-integer fit zoom retains sufficient detail");
        QQuickWindow target;
        preview.setParentItem(target.contentItem());
        expect(preview.renderDevicePixelRatio() == target.effectiveDevicePixelRatio(), "mounted canvas must use the window's physical pixel ratio");
        preview.setZoom(0.45);
        expect(preview.renderLevelOfDetail() * preview.zoom() * preview.renderDevicePixelRatio()
                   <= std::max(qreal{1.0}, preview.zoom() * preview.renderDevicePixelRatio()),
               "display tile samples must meet physical display resolution");
        preview.setParentItem(nullptr);
        expect(std::get<RasterAsset>(detail.assets.front()).pixels.pixels == checker.pixels,
               "display filtering never resamples stored raster pixels");
    }

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
               && layerProperties(editableOwned.document()->layers.front()).opacity == 0.5,
           "CanvasItem must expose its structural editor and apply edits to owned document data");
    render(editableOwned, 8, 6);
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
    Layer transformedLayer = StaticBitmapLayer{
        {"paint-layer", "Paint", true, 1.0, {}, RasterBlendMode::SourceOver},
        StaticSource{"paint"},
    };
    layerProperties(transformedLayer).transform.translationX = 1.0;
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

    Document animatedDocument = transformedDocument;
    animatedDocument.timeline.frameCount = 3;
    findRasterAsset(animatedDocument, "paint")->pixels = makeRasterLayer(1, 1);
    MotionKeyframe motionStart;
    MotionKeyframe motionEnd;
    motionEnd.frame = 2;
    motionEnd.value.position.x = 1;
    layerProperties(animatedDocument.layers.front()).motion = {motionStart, motionEnd};
    animatedDocument.assets.emplace_back(VideoAsset{"movie", {24, 1},
        {makeRasterLayer(1, 1, 0xffff0000), makeRasterLayer(1, 1, 0xff0000ff)}});
    animatedDocument.layers.emplace_back(VideoLayer{{"movie", "Video"}, StaticSource{"movie"},
        {0, {}, VideoEndBehavior::Hold}});
    CanvasItem animatedItem;
    expect(animatedItem.bind(animatedDocument) && animatedItem.selectLayer(QStringLiteral("paint-layer")),
           "a native video and motion canvas must bind to Qt Quick");
    animatedItem.setFrame(2);
    animatedItem.setBrushColor(QColor::fromRgba(0xff22d3eeU));
    animatedItem.setBrushSize(1.0);
    expect(animatedItem.beginStrokeAt({2.0, 0.0}, 1.0) && animatedItem.endStrokeAt({2.0, 0.0}, 1.0),
           "brush input must invert the evaluated motion transform at the current frame");
    output = render(animatedItem, 3, 1);
    expect(output.pixel(0, 0) == 0xff0000ff && output.pixelColor(2, 0).alpha() > 0,
           "async canvas presentation must contain native video and the edited moving raster");

    CanvasItem largeCanvas;
    largeCanvas.setWidth(1024);
    largeCanvas.setHeight(768);
    expect(largeCanvas.createInfiniteRasterDocument(65536, 49152, 256)
               && largeCanvas.documentReady(),
           "a tens-of-thousands-pixel sparse canvas must bind without a monolithic frame allocation");
    render(largeCanvas, 1024, 768);
    expect(!largeCanvas.rendering()
               && largeCanvas.residentTileCount() > 0
               && largeCanvas.residentTileCount() <= 64
               && largeCanvas.residentLayerTileCount() > 0
               && largeCanvas.residentLayerTileCount() <= 64
               && largeCanvas.framePixels() == nullptr,
           "large-canvas rendering must stay within the resident tile budget and avoid full-frame pixels");
    const qulonglong largeRevision = largeCanvas.revision();
    largeCanvas.panBy(-640.0, -384.0);
    expect(largeCanvas.revision() == largeRevision,
           "camera-only manipulation must remain a GPU transform and not recomposite document content");

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

    if (qEnvironmentVariableIntValue("IISHAREDCANVAS_VERIFY_GPU") == 1) {
        QQuickWindow window;
        window.resize(64, 64);
        CanvasItem gpuItem(window.contentItem());
        gpuItem.setWidth(64);
        gpuItem.setHeight(64);
        Document gpuDocument = mixedDocument();
        expect(gpuItem.bind(gpuDocument),
               "the hardware scene-graph probe document must bind");
        gpuItem.setZoom(16.0);
        window.show();

        QElapsedTimer timeout;
        timeout.start();
        while ((gpuItem.rendering() || !gpuItem.gpuAccelerated())
               && timeout.elapsed() < 5000) {
            window.requestUpdate();
            QCoreApplication::processEvents(QEventLoop::AllEvents, 10);
            QThread::msleep(1);
        }
        const QImage gpuFrame = window.grabWindow();
        expect(gpuItem.gpuAccelerated()
                   && gpuItem.graphicsBackend() != QStringLiteral("software"),
               "the explicit hardware probe must use a Qt Quick GPU backend");
        bool sawBackground = false;
        bool sawVector = false;
        for (int y = 0; y < gpuFrame.height(); ++y) {
            for (int x = 0; x < gpuFrame.width(); ++x) {
                const QRgb pixel = gpuFrame.pixel(x, y);
                sawBackground = sawBackground || pixel == 0xff102030U;
                sawVector = sawVector || pixel == 0xffffcc00U;
            }
        }
        const bool gpuPixelsMatch = !gpuFrame.isNull()
            && sawBackground && sawVector;
        if (!gpuPixelsMatch && !gpuFrame.isNull()) {
            std::cerr << "GPU probe backend="
                      << gpuItem.graphicsBackend().toStdString()
                      << " size=" << gpuFrame.width() << 'x' << gpuFrame.height()
                      << " dpr=" << gpuFrame.devicePixelRatio()
                      << " background=0x" << std::hex << gpuFrame.pixel(0, 0)
                      << " sawBackground=" << sawBackground
                      << " sawVector=" << sawVector
                      << std::dec << '\n';
        }
        expect(gpuPixelsMatch,
               "the GPU scene graph must present uploaded mixed-document texture tiles");
        if (gpuPixelsMatch && gpuItem.gpuAccelerated()) {
            std::cout << "GPU_BACKEND="
                      << gpuItem.graphicsBackend().toStdString()
                      << " GPU_FRAME=" << gpuFrame.width() << 'x'
                      << gpuFrame.height() << '\n';
        }
        window.hide();
    }

    return failures == 0 ? 0 : 1;
}
