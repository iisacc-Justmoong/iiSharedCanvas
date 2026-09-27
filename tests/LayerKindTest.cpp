#include <iiSharedCanvas.h>
#include <QTemporaryDir>
#include <iostream>

namespace {
int failures = 0;
void expect(bool value, const char *message) {
    if (!value) { std::cerr << message << '\n'; ++failures; }
}
void verify(const iiSharedCanvas::Document &document) {
    using namespace iiSharedCanvas;
    const LayerKind kinds[] = {LayerKind::StaticBitmap, LayerKind::StaticVector,
        LayerKind::DynamicBitmap, LayerKind::DynamicVector, LayerKind::DynamicBitmap};
    if (document.layers.size() != 5) { expect(false, "five layer fixtures required"); return; }
    for (std::size_t i = 0; i < 5; ++i) {
        expect(layerKind(document.layers[i]) == kinds[i], "four-way kind must survive persistence");
        expect(layerTiming(document.layers[i]) == (i < 2 ? LayerTiming::Static : LayerTiming::Dynamic),
               "content timing must distinguish static references from video playback");
        expect(layerRepresentation(document.layers[i]) == (i == 1 || i == 3
            ? LayerRepresentation::Vector : LayerRepresentation::Bitmap), "representation must classify video as bitmap");
        auto isolated = document;
        for (std::size_t j = 0; j < 5; ++j) { layerProperties(isolated.layers[j]).visible = i == j; }
        for (FrameIndex frame = 0; frame < 3; ++frame) {
            auto rendered = renderFrame(isolated, frame);
            const auto expected = i < 2 || frame == 0 ? 0xffff0000U : 0xff0000ffU;
            expect(rendered.ok() && !rendered.pixels.pixels.empty()
                && rendered.pixels.pixels[0] == expected, "frame changes must select dynamic content and preserve static content");
        }
    }
}
}
int main() {
    using namespace iiSharedCanvas;
    Document document; document.extent = {2, 2}; document.timeline.frameCount = 3;
    DocumentEditor editor(document);
    for (const auto color : {0xffff0000U, 0xff0000ffU}) {
        const auto suffix = color == 0xffff0000U ? "red" : "blue";
        expect(editor.insertRasterAsset(std::string("b-") + suffix, makeRasterLayer(2, 2, color)).ok(), "insert bitmap");
        VectorPath path; path.commands = {MoveTo{{0,0}}, LineTo{{2,0}}, LineTo{{2,2}}, LineTo{{0,2}}, ClosePath{}};
        path.fill = SolidPaint{color};
        expect(editor.insertVectorAsset(std::string("v-") + suffix, {2,2}, {path}).ok(), "insert vector");
    }
    expect(editor.insertStaticLayer({"sb", "Static bitmap"}, LayerRepresentation::Bitmap, "b-red").changed, "create static bitmap");
    expect(editor.insertStaticLayer({"sv", "Static vector"}, LayerRepresentation::Vector, "v-red").changed, "create static vector");
    expect(editor.insertDynamicLayer({"db", "Dynamic bitmap"}, LayerRepresentation::Bitmap, {{0,"b-red"},{1,"b-blue"}}).changed, "create dynamic bitmap");
    expect(editor.insertDynamicLayer({"dv", "Dynamic vector"}, LayerRepresentation::Vector, {{0,"v-red"},{1,"v-blue"}}).changed, "create dynamic vector");
    expect(editor.insertVideoAsset({"video", {24,1}, {makeRasterLayer(2,2,0xffff0000U), makeRasterLayer(2,2,0xff0000ffU)}}).ok(), "insert video");
    expect(editor.insertLayer(VideoLayer{{"video-layer", "Video"}, StaticSource{"video"}, {0, {}, VideoEndBehavior::Hold}}).ok(), "insert video layer");
    Layer movingDrawing = document.layers.front();
    layerProperties(movingDrawing).motion = {{0, {}}, {2, {}}};
    expect(layerKind(movingDrawing) == LayerKind::StaticBitmap, "property motion does not change content identity");
    verify(document);
    auto snapshot = encodeIisc(document); const auto revision = editor.revision();
    expect(!editor.insertDynamicLayer({"bad"}, LayerRepresentation::Bitmap, {{0,"b-red"},{1,"v-blue"}}).ok(), "reject mixed representations");
    expect(!editor.insertDynamicLayer({"bad"}, LayerRepresentation::Vector, {}).ok(), "reject empty dynamic content");
    expect(!editor.insertDynamicLayer({"bad"}, LayerRepresentation::Vector, {{1,"v-red"}}).ok(), "dynamic content starts at zero");
    expect(!editor.insertStaticLayer({"bad"}, static_cast<LayerRepresentation>(99), "b-red").ok(), "reject unknown representation");
    expect(!editor.insertStaticLayer({"bad"}, LayerRepresentation::Vector, "b-red").ok(), "reject static mismatch");
    expect(editor.revision() == revision && encodeIisc(document).bytes == snapshot.bytes, "rejected edits preserve document and revision");
    expect(editor.setStaticSource("dv", "v-blue").changed && layerKind(*findLayer(document,"dv")) == LayerKind::StaticVector, "conversion updates kind without stale metadata");
    expect(editor.setKeyframedSource("dv", {{0,"v-red"},{1,"v-blue"}}).changed, "restore dynamic vector");
    expect(editor.setKeyframedSource("sb", {{0,"b-red"}}).changed && layerKind(*findLayer(document,"sb")) == LayerKind::DynamicBitmap, "one-key dynamic source remains dynamic");
    expect(editor.setStaticSource("sb", "b-red").changed, "restore static bitmap");
    const auto decoded = decodeIisc(encodeIisc(document).bytes);
    expect(decoded.ok(), "snapshot round trip"); if (decoded.ok()) { verify(decoded.document); }
    QTemporaryDir directory(QStringLiteral(IISHAREDCANVAS_TEST_OUTPUT_DIR "/layer-kind-XXXXXX"));
    expect(directory.isValid(), "working file directory");
    if (directory.isValid()) {
        const auto path = directory.filePath("layers.iisc").toStdString();
        DocumentFile file; expect(file.create(path, document).ok(), "create working file");
        DocumentEditor bound(file);
        expect(bound.insertStaticLayer({"temporary-static"}, LayerRepresentation::Bitmap, "b-red").changed,
               "file-bound static creation");
        expect(bound.insertDynamicLayer({"temporary-dynamic"}, LayerRepresentation::Vector,
            {{0,"v-red"},{1,"v-blue"}}).changed, "file-bound dynamic creation");
        expect(bound.removeLayer("temporary-static").changed && bound.removeLayer("temporary-dynamic").changed,
               "remove temporary layers and their content keys");
        expect(bound.setStaticSource("dv", "v-blue").changed, "persist static conversion");
        expect(bound.setKeyframedSource("dv", {{0,"v-red"},{1,"v-blue"}}).changed, "persist dynamic conversion");
        DocumentFile reopened; expect(reopened.open(path).ok(), "reopen working file");
        if (reopened.document()) { verify(*reopened.document()); }
    }
    return failures ? 1 : 0;
}
