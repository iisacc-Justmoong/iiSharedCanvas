#include <iiSharedCanvas.h>

#include <QFile>
#include <QGuiApplication>
#include <QTemporaryDir>

#include <filesystem>
#include <iostream>

namespace {
using namespace iiSharedCanvas;
int failures = 0;
void expect(bool condition, const std::string &message)
{
    if (!condition) { std::cerr << message << '\n'; ++failures; }
}
Document artwork()
{
    Document document;
    document.extent = {40, 20};
    document.assets.emplace_back(RasterAsset{"red", makeRasterLayer(40, 20, 0x80ff0000U)});
    document.assets.emplace_back(RasterAsset{"blue", makeRasterLayer(40, 20, 0xff0000ffU)});
    document.layers.emplace_back(StaticBitmapLayer{{"paint", "Paint"}, StaticSource{"red"}});
    return document;
}
QByteArray bytes(const QString &path)
{
    QFile file(path);
    return file.open(QIODevice::ReadOnly) ? file.readAll() : QByteArray{};
}
void writeSnapshot(const QString &path, const Document &document)
{
    const auto encoded = encodeIisc(document);
    expect(encoded.ok(), "snapshot fixture encodes");
    QFile file(path);
    expect(file.open(QIODevice::WriteOnly | QIODevice::NewOnly), "snapshot fixture opens");
    expect(file.write(reinterpret_cast<const char *>(encoded.bytes.data()), qint64(encoded.bytes.size()))
        == qint64(encoded.bytes.size()), "snapshot fixture writes");
}
void expectPixels(const DocumentPreviewResult &preview, const Document &document, FrameIndex frame = 0)
{
    expect(preview.ok(), "preview succeeds: " + preview.result.message);
    if (!preview.ok()) return;
    const auto expected = renderFrameRegion(document, frame, documentViewRegion(document),
        {preview.pixels.width, preview.pixels.height}, RasterSampling::Smooth);
    expect(expected.ok() && expected.pixels.pixels == preview.pixels.pixels,
        "preview is the composed document, including visibility, alpha and transforms");
    expect(preview.region == documentViewRegion(document), "preview reports its original document region");
}
}

int main(int argc, char **argv)
{
    QGuiApplication application(argc, argv);
    QTemporaryDir directory(QStringLiteral(IISHAREDCANVAS_TEST_OUTPUT_DIR "/preview-XXXXXX"));
    expect(directory.isValid(), "private fixtures directory exists");
    auto document = artwork();
    const auto small = renderDocumentPreview(document, {{10, 10}});
    expect(small.pixels.width == 10 && small.pixels.height == 5, "thumbnail fits its box without distorting artwork");
    expectPixels(small, document);
    const auto native = renderDocumentPreview(document);
    expect(native.pixels.width == 40 && native.pixels.height == 20, "small artwork is not enlarged");
    expect(native.pixels.pixels.front() == 0x80ff0000U, "transparent artwork keeps its alpha");
    Document masked; masked.extent = {2, 1};
    masked.assets.emplace_back(RasterAsset{"source", {2, 1, {0xffff0000U, 0xff0000ffU}}});
    masked.assets.emplace_back(RasterAsset{"mask", {2, 1, {0xffffffffU, 0xff000000U}}});
    masked.layers.emplace_back(StaticBitmapLayer{{"masked", "Masked"}, StaticSource{"source"}});
    layerProperties(masked.layers.front()).appearance.maskStack = {{"painted", "Painted", LayerMask{.assetId="mask", .outsideCoverage=0}}};
    const auto maskPreview = renderDocumentPreview(masked);
    expect(maskPreview.ok() && maskPreview.pixels.pixels[0] == 0xffff0000U
        && maskPreview.pixels.pixels[1] >> 24 == 0, "native-size presentation preserves painted mask coverage without resampling its edges");
    auto composite = document;
    composite.layers.emplace_back(StaticBitmapLayer{{"top", "Translated blue"}, StaticSource{"blue"}});
    layerProperties(composite.layers.back()).transform.translationX = 20;
    const auto composed = renderDocumentPreview(composite);
    expect(composed.ok() && composed.pixels.pixels.front() == 0x80ff0000U
        && composed.pixels.pixels[30] == 0xff0000ffU,
        "thumbnail composites multiple transformed layers instead of picking one source asset");
    layerProperties(composite.layers.back()).visible = false;
    const auto hidden = renderDocumentPreview(composite);
    expect(hidden.ok() && hidden.pixels.pixels[30] == 0x80ff0000U, "hidden layers do not leak into previews");
    for (auto maximum : {CanvasExtent{0, 20}, CanvasExtent{-1, 10}, CanvasExtent{20, 0}})
        expect(!renderDocumentPreview(document, {maximum}).ok(), "nonpositive bounds are rejected");
    expect(renderDocumentPreview(document, {{100000, 100000}}).result.code == MediaIoCode::LimitExceeded,
        "unbounded preview allocations are rejected");
    expect(!renderDocumentPreview(Document{}).ok(), "invalid documents return no preview");
    expect(!renderDocumentPreview(document, {{20, 20}, 1}).ok(), "out-of-range frame is rejected");

    document.timeline.frameCount = 2;
    document.layers.front() = DynamicBitmapLayer{{"paint", "Animated"}, KeyframedSource{{0, 1}}};
    document.frames = {{0, {{"paint", "red"}}}, {1, {{"paint", "blue"}}}};
    const auto later = renderDocumentPreview(document, {{20, 20}, 1});
    expect(later.ok() && later.pixels.pixels.front() == 0xff0000ffU, "requested frame is rendered");
    expectPixels(later, document, 1);
    document = artwork();
    document.artboards = {{"board", "Offset board", {{-40, -20}, {40, 20}}, 0xff00ff00U}};
    layerProperties(document.layers.front()).artboardId = "board";
    expectPixels(renderDocumentPreview(document, {{24, 24}}), document);
    document.canvasMode = CanvasMode::Infinite;
    document.infiniteCanvas.origin = {-64, -32};
    expectPixels(renderDocumentPreview(document, {{24, 24}}), document);

    for (const auto minor : {std::uint16_t{19}, CurrentFormatMinor}) {
        document = artwork(); document.formatVersion.minor = minor;
        const auto path = directory.filePath(QString("snapshot-%1 %% # 한글.iisc").arg(minor));
        writeSnapshot(path, document);
        const auto original = bytes(path);
        const auto modified = std::filesystem::last_write_time(path.toStdString());
        expectPixels(readIiscPreview(path.toStdString(), {{10, 10}}), document);
        expect(bytes(path) == original && std::filesystem::last_write_time(path.toStdString()) == modified,
            "preview leaves snapshot bytes and modification time unchanged");
        expect(!QFile::exists(path + "-wal") && !QFile::exists(path + "-shm"), "snapshot gets no database sidecars");
        SerializationLimits limits; limits.maximumContainerBytes = 8;
        expect(readIiscPreview(path.toStdString(), {}, limits).result.code == MediaIoCode::LimitExceeded,
            "input limits apply before decoding");
        auto damaged = original; damaged[damaged.size() - 1] ^= 1;
        QFile corrupt(directory.filePath("corrupt.iisc")); expect(corrupt.open(QIODevice::WriteOnly), "corrupt fixture opens");
        corrupt.write(damaged); corrupt.close();
        expect(!readIiscPreview(corrupt.fileName().toStdString()).ok(), "bad checksum is not shown as artwork");
    }

    document = artwork();
    const auto working = directory.filePath("live-working.iisc");
    DocumentFile owner;
    expect(owner.create(working.toStdString(), document).ok(), "working fixture opens");
    expect(owner.edit([](Document &draft) {
        std::get<RasterAsset>(draft.assets.front()).pixels.pixels.assign(800, 0xff123456U);
        return true;
    }).ok(), "live edit commits");
    const auto before = bytes(working), wal = bytes(working + "-wal");
    const auto revision = owner.revision();
    const auto livePreview = readIiscPreview(working.toStdString());
    expect(livePreview.ok() && livePreview.pixels.pixels.front() == 0xff123456U,
        "working-file preview includes the latest committed WAL contents");
    expectPixels(livePreview, *owner.document());
    expect(before == bytes(working) && wal == bytes(working + "-wal") && revision == owner.revision(),
        "reading preview preserves the live file, WAL and owner revision");
    owner.close();
    QFile::setPermissions(working, QFileDevice::ReadOwner);
    expect(readIiscPreview(working.toStdString()).ok(), "read-only working files can be previewed");
    QFile::setPermissions(working, QFileDevice::ReadOwner | QFileDevice::WriteOwner);
    expect(!readIiscPreview(directory.filePath("missing.iisc").toStdString()).ok(), "missing files fail cleanly");
    expect(!readIiscPreview("https://example.invalid/file.iisc").ok(), "remote URLs are not native file paths");
    expect(!readIiscPreview(std::string("bad\0path", 8)).ok(), "embedded null paths fail cleanly");
    std::cout << "Document preview assertions: " << (failures ? "FAILED" : "passed") << '\n';
    return failures ? 1 : 0;
}
