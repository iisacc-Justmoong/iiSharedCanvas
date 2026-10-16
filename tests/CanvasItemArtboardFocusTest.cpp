#include <iiSharedCanvas.h>
#include <QElapsedTimer>
#include <QGuiApplication>
#include <QImage>
#include <QMetaProperty>
#include <QPainter>
#include <QThread>
#include <iostream>

namespace {
int failures = 0;
void expect(bool condition, const char *message) {
    if (!condition) { std::cerr << message << '\n'; ++failures; }
}
QImage rendered(iiSharedCanvas::CanvasItem &item) {
    QElapsedTimer timer; timer.start();
    while (item.rendering() && timer.elapsed() < 5000) {
        QCoreApplication::processEvents(QEventLoop::AllEvents, 10);
        QThread::msleep(1);
    }
    expect(!item.rendering(), "focused presentation finishes within the render budget");
    QImage image(6, 4, QImage::Format_ARGB32); image.fill(Qt::transparent);
    QPainter painter(&image); item.paint(&painter);
    return image;
}
iiSharedCanvas::Document overlappingDocument() {
    using namespace iiSharedCanvas;
    Document document; document.extent = {4, 4}; document.timeline.frameCount = 2;
    document.artboards = {{"first", "First", {{-2, 0}, {4, 4}}, 0xff222222u},
                          {"second", "Second", {{-2, 0}, {4, 4}}, 0xff0000ffu}};
    document.assets.emplace_back(RasterAsset{"first.red", makeRasterLayer(2, 2, 0xffff0000u)});
    document.assets.emplace_back(RasterAsset{"first.green", makeRasterLayer(2, 2, 0xff00ff00u)});
    document.assets.emplace_back(RasterAsset{"loose.white", makeRasterLayer(6, 4, 0xffffffffu)});
    LayerProperties first; first.id = "first.layer"; first.artboardId = "first";
    document.layers.emplace_back(DynamicBitmapLayer{first, KeyframedSource{{0, 1}}});
    LayerProperties loose; loose.id = "loose.layer"; loose.transform.translationX = -2;
    document.layers.emplace_back(StaticBitmapLayer{loose, StaticSource{"loose.white"}});
    document.frames = {{0, {{"first.layer", "first.red"}}}, {1, {{"first.layer", "first.green"}}}};
    return document;
}
void focus(iiSharedCanvas::CanvasItem &view, const QString &id) {
    view.setRenderArtboardId(id);
    expect(view.renderArtboardId() == id, "the public C++ focus setter and getter retain the stable ID");
    expect(view.setProperty("renderArtboardId", id), "renderArtboardId is a writable native QML property");
    expect(view.property("renderArtboardId").toString() == id, "renderArtboardId retains the exact stable ID");
}
}
int main(int argc, char **argv) {
    if (qEnvironmentVariableIsEmpty("QT_QPA_PLATFORM")) qputenv("QT_QPA_PLATFORM", "offscreen");
    QGuiApplication app(argc, argv);
    using namespace iiSharedCanvas;
    auto document = overlappingDocument();
    const auto encoded = encodeIisc(document);
    expect(encoded.ok(), "overlapping artboard fixture is a valid native document");
    CanvasItem view; view.setSize({6, 4});
    expect(view.bind(document), "the renderer binds the complete editable document");
    expect(view.canvasWidth() == 6 && view.canvasOriginX() == -2, "native workspace dimensions retain the original union");
    const int propertyIndex = view.metaObject()->indexOfProperty("renderArtboardId");
    expect(propertyIndex >= 0 && view.metaObject()->property(propertyIndex).hasNotifySignal(), "render focus exposes a native notify signal");
    expect(rendered(view).pixel(0, 0) == 0xffffffffu, "empty focus displays the complete original composition including loose artwork");
    const auto revision = view.revision();
    const auto origin = QPointF(view.canvasOriginX(), view.canvasOriginY());
    const auto pan = QPointF(view.panX(), view.panY());
    int focusSignals = 0;
    QObject::connect(&view, &CanvasItem::renderArtboardIdChanged, &view, [&] { ++focusSignals; });
    focus(view, "first");
    expect(focusSignals == 1, "a changed focus emits one notify signal while assigning the same value is a no-op");
    auto image = rendered(view);
    expect(image.pixel(0, 0) == 0xffff0000u && image.pixel(3, 3) == 0xff222222u && image.pixel(5, 1) == 0,
           "focus composes only the requested background and owned artwork, excluding overlapping and loose content");
    expect(view.framePixels() && view.framePixels()->width == 6 && view.framePixels()->pixels[0] == 0xffff0000u,
           "focused framePixels preserves full workspace coordinates and transparent outside pixels");
    expect(view.revision() == revision && encodeIisc(document).bytes == encoded.bytes && view.document() == &document,
           "render focus neither edits nor replaces the native document");
    expect(QPointF(view.canvasOriginX(), view.canvasOriginY()) == origin && QPointF(view.panX(), view.panY()) == pan,
           "render focus preserves viewport and document origin");
    focus(view, "second");
    expect(rendered(view).pixel(0, 0) == 0xff0000ffu, "another overlapping artboard displays its own background");
    focus(view, "first"); focus(view, "second"); focus(view, "first");
    expect(rendered(view).pixel(0, 0) == 0xffff0000u, "superseded asynchronous focus requests never restore an older artboard");
    focus(view, "missing");
    image = rendered(view);
    bool transparent = true;
    for (int y = 0; y < image.height(); ++y) for (int x = 0; x < image.width(); ++x) transparent &= image.pixel(x, y) == 0;
    expect(transparent && view.documentReady() && view.lastError().isEmpty(), "a missing ID presents transparency without rejecting or dirtying the document");
    focus(view, "");
    expect(rendered(view).pixel(0, 0) == 0xffffffffu && view.revision() == revision,
           "clearing focus restores the complete composition without a revision change");
    expect(view.selectLayer("first.layer"), "focus remains independent from editable selection");
    focus(view, "first");
    view.setBrushColor(Qt::yellow); view.setBrushSize(1); view.setBrushHardness(1);
    expect(view.beginStrokeAt({-2, 0}) && view.endStrokeAt({-2, 0}), "native brush input retains document coordinates under render focus");
    expect(rendered(view).pixel(0, 0) == 0xffffff00u && view.canUndo(), "focused presentation refreshes committed brush pixels");
    const auto selected = view.selectedLayerId(); const auto paintedRevision = view.revision();
    focus(view, "second"); rendered(view);
    expect(view.selectedLayerId() == selected && view.canUndo() && view.revision() == paintedRevision,
           "changing presentation focus preserves selection and brush undo history");
    expect(view.undo(), "brush undo remains available after focus changes");
    focus(view, "first");
    expect(rendered(view).pixel(0, 0) == 0xffff0000u, "undo invalidates the focused snapshot and restores the source pixels");
    view.setFrame(1);
    expect(rendered(view).pixel(0, 0) == 0xff00ff00u, "focused snapshots reevaluate native frame content");
    view.setFrame(0);
    expect(rendered(view).pixel(0, 0) == 0xffff0000u, "focused snapshots reevaluate frame content when scrubbing backwards");
    focus(view, "first");
    auto replacement = overlappingDocument(); replacement.artboards[0].id = "replacement";
    layerProperties(replacement.layers[0]).artboardId = "replacement";
    expect(view.bind(replacement), "another document can retain the caller's render focus setting");
    expect(rendered(view).pixel(0, 0) == 0, "an ID absent from a newly bound document cannot show stale focused pixels");
    focus(view, "replacement");
    expect(rendered(view).pixel(0, 0) == 0xffff0000u, "render focus resolves IDs against the newly bound document");
    return failures ? 1 : 0;
}
