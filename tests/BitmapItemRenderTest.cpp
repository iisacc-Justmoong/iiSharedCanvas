#include <iiSharedCanvas.h>

#include <QColor>
#include <QDir>
#include <QGuiApplication>
#include <QImage>
#include <QPainter>
#include <QPointF>

#include <cstdint>
#include <iostream>
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

QImage render(iiSharedCanvas::BitmapItem &item, int width, int height)
{
    item.setWidth(width);
    item.setHeight(height);
    QImage output(width, height, QImage::Format_ARGB32);
    output.fill(Qt::transparent);
    QPainter painter(&output);
    item.paint(&painter);
    return output;
}

} // namespace

int main(int argc, char **argv)
{
    qputenv("QT_QPA_PLATFORM", "offscreen");
    QGuiApplication application(argc, argv);

    using namespace iiSharedCanvas;

    expect(registerIiSharedCanvasQmlTypes() >= 0,
           "BitmapItem must be registerable as an iiSharedCanvas QML type");

    BitmapItem item;
    expect(item.createBitmap(4, 4, QColor::fromRgba(0xff102030U)),
           "BitmapItem must create an owned raster asset for QML authoring");
    expect(item.bitmapReady() && item.bitmapWidth() == 4 && item.bitmapHeight() == 4,
           "BitmapItem must expose the bound bitmap dimensions");
    expect(item.setPixel(1, 2, QColor::fromRgba(0xffff0000U)),
           "BitmapItem must expose direct pixel manipulation");

    QImage output = render(item, 4, 4);
    expect(output.pixel(0, 0) == 0xff102030U,
           "BitmapItem paint() must display the bound RasterAsset background");
    expect(output.pixel(1, 2) == 0xffff0000U,
           "BitmapItem paint() must display a manipulated pixel exactly");

    expect(item.undo(), "BitmapItem must expose bitmap undo");
    output = render(item, 4, 4);
    expect(output.pixel(1, 2) == 0xff102030U,
           "BitmapItem must repaint restored pixels after undo");
    expect(item.redo(), "BitmapItem must expose bitmap redo");

    item.setBrushColor(QColor::fromRgba(0xff00ff00U));
    item.setBrushSize(1.0);
    item.setBrushHardness(1.0);
    item.setBrushOpacity(1.0);
    item.setBrushFlow(1.0);
    expect(item.beginStrokeAt({0.0, 0.0}, 1.0)
               && item.continueStrokeAt({3.0, 3.0}, 1.0)
               && item.endStrokeAt({3.0, 3.0}, 1.0),
           "BitmapItem must route streaming brush input into iiPaintEngine rasterization");
    output = render(item, 4, 4);
    expect(output.pixelColor(0, 0).green() > output.pixelColor(0, 0).red(),
           "the rendered bitmap must include the committed brush result");

    item.setZoom(2.0);
    item.setPanX(0.0);
    item.setPanY(0.0);
    output = render(item, 8, 8);
    expect(output.pixel(0, 0) == output.pixel(1, 1),
           "pixel-art zoom must use nearest-neighbor display without smoothing");

    Document externalDocument;
    externalDocument.extent = {3, 2};
    externalDocument.timeline = {{24, 1}, 1};
    externalDocument.assets.emplace_back(
        RasterAsset{"external", makeRasterLayer(3, 2, 0xff334455U)});
    BitmapItem externalItem;
    expect(externalItem.bind(externalDocument, "external"),
           "BitmapItem must bind a raster asset from a caller-owned document");
    expect(externalItem.setPixel(2, 1, QColor::fromRgba(0xffe879f9U)),
           "a bound BitmapItem must manipulate the caller-owned RasterAsset");
    const auto &externalRaster = std::get<RasterAsset>(externalDocument.assets.front());
    expect(rasterLayerPixelAt(externalRaster.pixels, {2, 1}) == 0xffe879f9U,
           "BitmapItem edits must be document edits rather than a detached display copy");
    output = render(externalItem, 3, 2);
    expect(output.pixel(2, 1) == 0xffe879f9U,
           "the external RasterAsset edit must be visible in the same item render");

    BitmapItem artifact;
    artifact.setWidth(192);
    artifact.setHeight(128);
    expect(artifact.createBitmap(48, 32, QColor::fromRgba(0xff111827U)),
           "render artifact bitmap must be created");
    artifact.setZoom(4.0);
    artifact.setBrushColor(QColor::fromRgba(0xff22d3eeU));
    artifact.setBrushSize(4.0);
    expect(artifact.beginStrokeAt({4.0, 6.0}, 1.0)
               && artifact.continueStrokeAt({42.0, 25.0}, 1.0)
               && artifact.endStrokeAt({42.0, 25.0}, 1.0),
           "artifact foreground stroke must render");
    artifact.setBrushColor(QColor::fromRgba(0xfffbbf24U));
    artifact.setBrushSize(3.0);
    expect(artifact.beginStrokeAt({4.0, 25.0}, 1.0)
               && artifact.continueStrokeAt({42.0, 6.0}, 1.0)
               && artifact.endStrokeAt({42.0, 6.0}, 1.0),
           "artifact second stroke must render");
    const QImage artifactImage = render(artifact, 192, 128);
    const QString outputDirectory = QStringLiteral(IISHAREDCANVAS_TEST_OUTPUT_DIR);
    expect(QDir().mkpath(outputDirectory),
           "test output directory must be creatable inside build/");
    expect(artifactImage.save(outputDirectory + QStringLiteral("/bitmap-item.png")),
           "actual BitmapItem output must be written as a PNG verification artifact");

    return failures == 0 ? 0 : 1;
}
