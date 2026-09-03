#include <iiSharedCanvas.h>
#include <Timeline/TimelineInterchange.h>

#include <QDir>
#include <QFile>
#include <QGuiApplication>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QTemporaryDir>

#include <algorithm>
#include <array>
#include <iostream>

namespace {
using namespace iiSharedCanvas;
int failures = 0;
void expect(bool value, const std::string &message)
{
    if (!value) { std::cerr << message << '\n'; ++failures; }
}
QByteArray read(const QString &path)
{
    QFile file(path); expect(file.open(QIODevice::ReadOnly), "open generated package file");
    return file.readAll();
}
Document fixture()
{
    Document document; document.extent = {4, 4}; document.timeline.frameCount = 12;
    document.assets.emplace_back(RasterAsset{"base", makeRasterLayer(4, 4, 0xffff0000U)});
    document.assets.emplace_back(RasterAsset{"green", makeRasterLayer(2, 2, 0xff00ff00U)});
    document.assets.emplace_back(RasterAsset{"blue", makeRasterLayer(2, 2, 0xff0000ffU)});
    BitmapLayer base; base.properties.id = "base"; base.properties.name = "바탕 & <red>";
    base.source = StaticSource{"base"}; document.layers.push_back(base);
    BitmapLayer animated; animated.properties.id = "animated"; animated.properties.name = "Keyed layer";
    animated.properties.transform.translationX = 1; animated.properties.transform.translationY = 1;
    animated.properties.opacity = 0.5; animated.properties.frameRange = LayerFrameRange{2, 9};
    animated.source = KeyframedSource{{0, 5, 8}}; document.layers.push_back(animated);
    BitmapLayer hidden; hidden.properties.id = "hidden"; hidden.properties.name = "Hidden";
    hidden.properties.visible = false; hidden.source = StaticSource{"blue"}; document.layers.push_back(hidden);
    document.frames = {{0, {{"animated", "green"}}}, {5, {{"animated", "blue"}}}, {8, {{"animated", "green"}}}};
    return document;
}
void verifyPackage(const QString &directory, const Document &document)
{
    const auto manifestBytes = read(directory + "/manifest.json");
    QJsonParseError error; const auto manifest = QJsonDocument::fromJson(manifestBytes, &error).object();
    expect(error.error == QJsonParseError::NoError, "valid package manifest");
    expect(manifest["version"].toInt() == 1 && manifest["frameCount"].toInt() == 12, "manifest version/duration");
    const auto tracks = manifest["tracks"].toArray();
    expect(tracks.size() == 3, "one independent track per native layer, including hidden layers");
    if (tracks.size() != 3) { return; }
    expect(tracks[0].toObject()["name"].toString() == QString::fromUtf8("바탕 & <red>"), "Unicode layer names survive");
    expect(!tracks[2].toObject()["visible"].toBool(), "hidden track is retained disabled");
    const auto clips = tracks[1].toObject()["clips"].toArray();
    expect(clips.size() == 3, "each native key interval remains a clip, even repeated asset references");
    if (clips.size() == 3) {
        for (int index = 0; index < 3; ++index) {
            expect(clips[index].toObject()["startFrame"].toInt() == std::array{2, 5, 8}[index]
                   && clips[index].toObject()["durationFrames"].toInt() == std::array{3, 3, 2}[index],
                   "hold keys intersect inclusive layer lifetime exactly");
        }
        expect(clips[0].toObject()["media"].toString() == clips[2].toObject()["media"].toString(), "repeated states reuse media without losing cuts");
    }
    expect(QFile::exists(directory + "/timeline.xml") && QFile::exists(directory + "/timeline.fcpxml"), "both editor exchange formats exist");
    const auto source = read(directory + "/source.iisc");
    const auto decoded = decodeIisc({reinterpret_cast<const std::uint8_t *>(source.constData()), std::size_t(source.size())});
    expect(decoded.ok() && encodeIisc(decoded.document).bytes == encodeIisc(document).bytes, "native snapshot preserves every original edit field");
    for (FrameIndex frame = 0; frame < document.timeline.frameCount; ++frame) {
        Document restored; restored.extent = document.extent;
        for (int trackIndex = 0; trackIndex < tracks.size(); ++trackIndex) {
            const auto track = tracks[trackIndex].toObject();
            for (const auto clipValue : track["clips"].toArray()) {
                const auto clip = clipValue.toObject();
                const auto first = clip["startFrame"].toInt(); const auto duration = clip["durationFrames"].toInt();
                if (int(frame) < first || int(frame) >= first + duration) { continue; }
                BitmapImportOptions options; options.assetId = std::to_string(trackIndex); options.extendedCodecs = false;
                auto imported = importBitmap((directory + '/' + clip["media"].toString()).toStdString(), options);
                expect(imported.ok(), "each timeline media reference resolves to a valid PNG");
                restored.assets.emplace_back(std::move(imported.asset));
                BitmapLayer layer; layer.properties.id = std::to_string(trackIndex);
                layer.properties.visible = track["visible"].toBool(); layer.properties.opacity = track["opacity"].toDouble();
                layer.source = StaticSource{options.assetId}; restored.layers.emplace_back(layer);
            }
        }
        const auto expected = renderFrame(document, frame), actual = renderFrame(restored, 0);
        expect(actual.ok() && expected.ok() && actual.pixels.pixels == expected.pixels.pixels,
               "every reconstructed frame matches native per-layer composition");
    }
}
}

int main(int argc, char **argv)
{
    QGuiApplication application(argc, argv);
    QDir().mkpath(QStringLiteral(IISHAREDCANVAS_TEST_OUTPUT_DIR));
    QTemporaryDir temp(QStringLiteral(IISHAREDCANVAS_TEST_OUTPUT_DIR) + "/timeline-package-XXXXXX");
    const auto document = fixture(); const auto original = encodeIisc(document).bytes;
    const auto directory = temp.filePath("한글 & timeline");
    const auto result = exportTimelineInterchange(document, directory.toStdString());
    expect(result.ok(), "export multi-layer timeline: " + result.message);
    if (result.ok()) { verifyPackage(directory, document); }
    expect(encodeIisc(document).bytes == original, "source is immutable");
    const auto oldManifest = read(directory + "/manifest.json");
    expect(exportTimelineInterchange(document, directory.toStdString()).code == MediaIoCode::AlreadyExists, "existing packages are never replaced");
    expect(read(directory + "/manifest.json") == oldManifest, "collision preserves old package");
    const auto rejects = [&](Document input, TimelineInterchangeOptions options, MediaIoCode code, const char *name) {
        const auto destination = temp.filePath(QString::fromLatin1(name));
        const auto error = exportTimelineInterchange(input, destination.toStdString(), options);
        expect(error.code == code && !QFile::exists(destination), std::string(name) + ": " + error.message);
        expect(QDir(temp.path()).entryList({".iisc-timeline-*"}, QDir::Dirs | QDir::Hidden | QDir::NoDotAndDotDot).empty(), "failed exports clean only their private staging directories");
    };
    TimelineInterchangeOptions options; options.maxClips = 4;
    rejects(document, options, MediaIoCode::LimitExceeded, "clip-limit");
    options = {}; options.maxLayers = 2; rejects(document, options, MediaIoCode::LimitExceeded, "layer-limit");
    options = {}; options.limits.maxOutputBytes = 64; rejects(document, options, MediaIoCode::LimitExceeded, "output-limit");
    options = {}; options.limits.maxDecodedBytes = 64; rejects(document, options, MediaIoCode::LimitExceeded, "memory-limit");
    options = {}; options.limits.maxFrames = 11; rejects(document, options, MediaIoCode::LimitExceeded, "frame-limit");
    options = {}; options.sequenceName = std::string("bad\0name", 8); rejects(document, options, MediaIoCode::InvalidArgument, "invalid-name");
    options = {}; options.limits.maxDecodedBytes = 1024 * 1024; options.sequenceName.assign(300000, 'n');
    rejects(document, options, MediaIoCode::LimitExceeded, "oversized-sequence-name");
    options = {}; options.limits.maxDecodedBytes = 1024;
    expect(exportTimelineInterchange(document, std::string(1024, 'p'), options).code == MediaIoCode::LimitExceeded,
           "oversized path rejected before filesystem text conversion");
    auto invalid = document; layerProperties(invalid.layers[0]).name = std::string("bad\x01", 4);
    rejects(invalid, {}, MediaIoCode::InvalidArgument, "invalid-xml-character");
    invalid = document; invalid.timeline.frameRate = {123, 7}; rejects(invalid, {}, MediaIoCode::UnsupportedFeature, "inexact-rate");
    invalid = document; layerSource(invalid.layers[0]) = StaticSource{"absent"}; rejects(invalid, {}, MediaIoCode::InvalidArgument, "invalid-reference");
    // source.iisc contains even assets and metadata that no rendered clip uses.
    // Reject them before the native encoder materializes its complete payload.
    const auto snapshotBudgetRejects = [&](Document input, const char *name) {
        TimelineInterchangeOptions limited; limited.limits.maxDecodedBytes = 1024 * 1024;
        const auto before = encodeIisc(input).bytes;
        const auto destination = temp.filePath(QString::fromLatin1(name));
        const auto rejected = exportTimelineInterchange(input, destination.toStdString(), limited);
        expect(rejected.code == MediaIoCode::LimitExceeded
               && rejected.message.find("source snapshot") != std::string::npos,
               std::string(name) + " must fail the pre-serialization snapshot budget: " + rejected.message);
        expect(!QFile::exists(destination), "oversized snapshot is never published");
        expect(QDir(temp.path()).entryList({".iisc-timeline-*"}, QDir::Dirs | QDir::Hidden | QDir::NoDotAndDotDot).empty(),
               "snapshot budget failures leave no private staging directories");
        expect(encodeIisc(input).bytes == before, "snapshot budget failure preserves every source field");
    };
    auto unusedVector = document;
    VectorAsset largeVector; largeVector.id = "unused-vector"; largeVector.viewport = {4, 4};
    VectorPath largePath; largePath.fill = SolidPaint{0xffffffffU};
    largePath.commands.emplace_back(MoveTo{{0, 0}});
    largePath.commands.insert(largePath.commands.end(), 20000, LineTo{{1, 1}});
    largeVector.paths.push_back(std::move(largePath)); unusedVector.assets.emplace_back(std::move(largeVector));
    snapshotBudgetRejects(std::move(unusedVector), "unused-vector-snapshot-budget");
    auto largeMetadata = document; largeMetadata.stableDiffusionMetadata.emplace();
    largeMetadata.stableDiffusionMetadata->positivePrompt.assign(300000, 'p');
    snapshotBudgetRejects(std::move(largeMetadata), "prompt-snapshot-budget");
    largeMetadata = document; largeMetadata.stableDiffusionMetadata.emplace();
    largeMetadata.stableDiffusionMetadata->comfyUi.extraPngInfo.push_back({"retained", '"' + std::string(300000, 'x') + '"'});
    snapshotBudgetRejects(std::move(largeMetadata), "nested-metadata-snapshot-budget");
    auto unusedRaster = document;
    unusedRaster.assets.emplace_back(RasterAsset{"unused-raster", makeRasterLayer(512, 256, 0xff112233U)});
    snapshotBudgetRejects(std::move(unusedRaster), "unused-raster-snapshot-budget");
    expect(exportTimelineInterchange(document, temp.filePath("missing/package").toStdString()).code == MediaIoCode::IoError, "missing parent rejected");
    const auto alias = temp.filePath("alias"); expect(QFile::link(directory, alias), "create package symlink fixture");
    expect(exportTimelineInterchange(document, alias.toStdString()).code == MediaIoCode::AlreadyExists, "symlink destinations refused");
    Document empty; empty.extent = {4, 4}; empty.timeline.frameCount = 48;
    expect(exportTimelineInterchange(empty, temp.filePath("empty").toStdString()).ok(), "empty canvas retains a timed empty sequence");
    expect(TimelineInterchangeOptions{}.limits.maxFrames == 1000000, "timeline export defaults allow long hold timelines");
    Document longHold; longHold.extent = {4, 4}; longHold.timeline.frameCount = 86400;
    longHold.assets.emplace_back(RasterAsset{"still", makeRasterLayer(4, 4, 0xffff0000U)});
    BitmapLayer still; still.properties.id = "still"; still.source = StaticSource{"still"};
    longHold.layers.emplace_back(std::move(still));
    const auto longDirectory = temp.filePath("one-hour-hold");
    const auto longResult = exportTimelineInterchange(longHold, longDirectory.toStdString());
    expect(longResult.ok(), "one-hour 24fps hold exports without frame-by-frame allocation: " + longResult.message);
    if (longResult.ok()) {
        const auto longManifest = QJsonDocument::fromJson(read(longDirectory + "/manifest.json")).object();
        const auto longClips = longManifest["tracks"].toArray()[0].toObject()["clips"].toArray();
        expect(longManifest["frameCount"].toInteger() == 86400 && longClips.size() == 1
               && longClips[0].toObject()["durationFrames"].toInteger() == 86400,
               "one-hour hold stays one exact-duration clip");
        expect(QDir(longDirectory + "/media").entryList({"*.png"}, QDir::Files).size() == 1,
               "one-hour hold creates exactly one PNG state");
    }
    auto vector = document; VectorAsset asset; asset.id = "vector"; asset.viewport = {4, 4};
    VectorPath path; path.commands = {MoveTo{{0, 0}}, LineTo{{2, 0}}, LineTo{{2, 2}}, ClosePath{}}; path.fill = SolidPaint{0xffffff00U};
    asset.paths.push_back(path); vector.assets.emplace_back(asset); VectorLayer layer; layer.properties.id = "vector-layer"; layer.source = StaticSource{"vector"}; vector.layers.emplace_back(layer);
    const auto vectorResult = exportTimelineInterchange(vector, temp.filePath("vector").toStdString());
    expect(vectorResult.ok() && std::any_of(vectorResult.warnings.begin(), vectorResult.warnings.end(), [](const auto &w) { return w.find("vector") != std::string::npos; }), "vector tracks are separate rendered media with an explicit editability warning");
    // Persistent, disposable fixture for actual editor import and independent schema validation.
    QTemporaryDir applicationFixture(QStringLiteral(IISHAREDCANVAS_TEST_OUTPUT_DIR) + "/nle-application-XXXXXX");
    applicationFixture.setAutoRemove(false);
    auto appDocument = document; appDocument.timeline.frameCount = 120;
    appDocument.extent = {1280, 720};
    std::get<RasterAsset>(appDocument.assets[0]).pixels = makeRasterLayer(1280, 720, 0xffff0000U);
    std::get<RasterAsset>(appDocument.assets[1]).pixels = makeRasterLayer(320, 180, 0xff00ff00U);
    std::get<RasterAsset>(appDocument.assets[2]).pixels = makeRasterLayer(320, 180, 0xff0000ffU);
    layerProperties(appDocument.layers[1]).transform.translationX = 480;
    layerProperties(appDocument.layers[1]).transform.translationY = 270;
    layerProperties(appDocument.layers[1]).frameRange = LayerFrameRange{24, 95};
    layerSource(appDocument.layers[1]) = KeyframedSource{{0, 48, 72}};
    appDocument.frames = {{0, {{"animated", "green"}}}, {48, {{"animated", "blue"}}}, {72, {{"animated", "green"}}}};
    const auto appPackage = applicationFixture.filePath("package");
    expect(exportTimelineInterchange(appDocument, appPackage.toStdString()).ok(), "actual editor fixture");
    std::cout << "NLE application fixture: " << appPackage.toStdString() << '\n';
    return failures ? 1 : 0;
}
