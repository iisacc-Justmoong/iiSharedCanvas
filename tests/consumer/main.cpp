#include <iiSharedCanvas.h>

#include <algorithm>
#include <cstdint>
#include <optional>
#include <string_view>
#include <variant>

#include <QTemporaryDir>
#include <QGuiApplication>
#include <QByteArray>
#include <QFile>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>

namespace {

bool verifyAudioTimeline()
{
    using namespace iiSharedCanvas;
    Document document; document.extent = {4, 4}; document.timeline.frameCount = 24;
    DocumentEditor editor(document);
    AudioAsset pcm{"pcm", 48000, 1, std::vector<std::int16_t>(48001, 1234)};
    const auto wav = encodeAudioWav(pcm);
    auto decoded = decodeAudioWav(wav.bytes, {.assetId = "pcm"});
    if (!wav.ok() || !decoded.ok() || decoded.asset.samples != pcm.samples
        || !editor.insertAudioAsset(std::move(decoded.asset)).changed
        || !editor.insertAudioTrack({"audio", "Voice", false, -3,
            {{"clip", "Trim", "pcm", 0, 24, 1, -6, true}}}).changed) { return false; }
    QTemporaryDir directory(QStringLiteral(IISHAREDCANVAS_CONSUMER_OUTPUT_DIR "/audio-XXXXXX"));
    if (!directory.isValid()) { return false; }
    const auto path = directory.filePath("working.iisc").toStdString();
    DocumentFile file;
    if (!file.create(path, document).ok()) { return false; }
    DocumentFile reopened;
    return reopened.open(path).ok() && reopened.document()->audioAssets == document.audioAssets
        && reopened.document()->audioTracks == document.audioTracks
        && exportTimelineInterchange(*reopened.document(), directory.filePath("package").toStdString()).ok()
        && QFile::exists(directory.filePath("package/media/audio-0001.wav"));
}

bool verifyTimelineInterchange()
{
    using namespace iiSharedCanvas;
    Document document; document.extent = {4, 4}; document.timeline = {{30000, 1001}, 10};
    document.assets.emplace_back(RasterAsset{"red", makeRasterLayer(4, 4, 0xffff0000U)});
    document.assets.emplace_back(RasterAsset{"blue", makeRasterLayer(4, 4, 0xff0000ffU)});
    document.layers.emplace_back(BitmapLayer{{"layer", "Installed timeline"}, KeyframedSource{{0, 5}}});
    document.frames = {{0, {{"layer", "red"}}}, {5, {{"layer", "blue"}}}};
    const auto original = encodeIisc(document);
    QTemporaryDir directory(QStringLiteral(IISHAREDCANVAS_CONSUMER_OUTPUT_DIR "/timeline-XXXXXX"));
    if (!directory.isValid()) { return false; }
    const auto package = directory.filePath("package");
    const auto result = exportTimelineInterchange(document, package.toStdString());
    if (!result.ok() || encodeIisc(document).bytes != original.bytes
        || !QFile::exists(package + "/timeline.xml") || !QFile::exists(package + "/timeline.fcpxml")
        || exportTimelineInterchange(document, package.toStdString()).code != MediaIoCode::AlreadyExists) { return false; }
    QFile file(package + "/manifest.json"); if (!file.open(QIODevice::ReadOnly)) { return false; }
    const auto manifest = QJsonDocument::fromJson(file.readAll()).object();
    const auto tracks = manifest["tracks"].toArray();
    if (tracks.size() != 1 || manifest["frameRate"].toObject()["numerator"].toInt() != 30000
        || manifest["frameRate"].toObject()["denominator"].toInt() != 1001) { return false; }
    const auto clips = tracks[0].toObject()["clips"].toArray();
    if (clips.size() != 2 || clips[1].toObject()["startFrame"].toInt() != 5
        || clips[1].toObject()["durationFrames"].toInt() != 5) { return false; }
    BitmapImportOptions options; options.extendedCodecs = false;
    const auto red = importBitmap((package + '/' + clips[0].toObject()["media"].toString()).toStdString(), options);
    const auto blue = importBitmap((package + '/' + clips[1].toObject()["media"].toString()).toStdString(), options);
    return red.ok() && blue.ok() && red.asset.pixels.pixels[0] == 0xffff0000U && blue.asset.pixels.pixels[0] == 0xff0000ffU;
}

bool verifyPsdExport()
{
    using namespace iiSharedCanvas;
    Document document;
    document.extent = {4, 4};
    document.timeline.frameCount = 2;
    document.assets.emplace_back(RasterAsset{"base", makeRasterLayer(4, 4, 0xffff0000U)});
    VectorAsset first{"first", {4, 4}, {{{MoveTo{{1, 1}}, LineTo{{3, 1}},
        LineTo{{3, 3}}, LineTo{{1, 3}}, ClosePath{}}, SolidPaint{0xff00ff00U}, std::nullopt}}};
    auto later = first;
    later.id = "later";
    later.paths.front().fill = SolidPaint{0xff0000ffU};
    document.assets.emplace_back(first);
    document.assets.emplace_back(later);
    document.layers.emplace_back(BitmapLayer{{"base-layer", "Base"}, StaticSource{"base"}});
    document.layers.emplace_back(VectorLayer{{"vector-layer", "Vector frame zero"}, KeyframedSource{{0, 1}}});
    document.frames = {{0, {{"vector-layer", "first"}}}, {1, {{"vector-layer", "later"}}}};
    const auto original = encodeIisc(document);
    const auto encoded = encodePsd(document);
    const auto contains = [&](std::string_view text) {
        return std::search(encoded.bytes.begin(), encoded.bytes.end(), text.begin(), text.end()) != encoded.bytes.end();
    };
    if (!original.ok() || !encoded.ok() || encoded.result.warnings.empty()
        || !contains("8BPS") || !contains("SoLd") || !contains("lnk2") || !contains("%PDF-")
        || encodeIisc(document).bytes != original.bytes) { return false; }
    const auto path = std::string(IISHAREDCANVAS_CONSUMER_OUTPUT_DIR "/installed-frame-zero.psd");
    PsdExportOptions options;
    options.overwrite = true; // A test-owned output, never a source document.
    if (!exportPsd(document, path, options).ok()
        || exportPsd(document, path).code != MediaIoCode::AlreadyExists) { return false; }
    const auto tinyLimit = [&] {
        PsdExportOptions limited;
        limited.limits.maxOutputBytes = 32;
        return encodePsd(document, limited);
    }();
    if (tinyLimit.ok() || !tinyLimit.bytes.empty()) { return false; }
    // Smart Objects deliberately remain outside the strict pixel-only reader.
    if (decodeLayeredDocument(encoded.bytes).result.code != MediaIoCode::UnsupportedFeature) { return false; }
    document.layers.resize(1);
    document.frames.clear();
    const auto pixelOnly = encodePsd(document);
    const auto restored = decodeLayeredDocument(pixelOnly.bytes);
    return pixelOnly.ok() && restored.ok()
        && renderFrame(restored.document, 0).pixels.pixels == renderFrame(document, 0).pixels.pixels;
}

bool verifyLayeredInterchange()
{
    using namespace iiSharedCanvas;
    // Independently constructed PSD v1, raw RGB. PSD records are bottom-to-top:
    // a 2x2 blue Base followed by a 1x1 green Top at (1, 0).
    const auto bytes = QByteArray::fromHex(
        "38425053000100000000000000030000000200000002000800030000000000000000000000aa000000a200020000000000000000000000020000000200030000000000060001000000060002000000063842494d6e6f726dff00000000000010000000000000000004426173650000000000000000000001000000010000000200030000000000030001000000030002000000033842494d6e6f726dff0000000000000c000000000000000003546f700000000000000000000000000000ffffffff0000000000ff000000000000000000000000000000ff0000ff00ffff");
    LayeredDocumentImportOptions options;
    options.idPrefix = "installed-layers";
    const auto imported = decodeLayeredDocument(
        {reinterpret_cast<const std::uint8_t *>(bytes.constData()), std::size_t(bytes.size())}, options);
    if (!imported.ok() || imported.format != "psd" || layeredDocumentFormats().size() != 2
        || imported.document.layers.size() != 2 || imported.document.assets.size() != 2
        || layerProperties(imported.document.layers[0]).name != "Base"
        || layerProperties(imported.document.layers[1]).name != "Top") { return false; }
    const std::vector<std::uint32_t> expected{0xff0000ffU, 0xff00ff00U, 0xff0000ffU, 0xff0000ffU};
    const auto rendered = renderFrame(imported.document, 0);
    const auto encoded = encodeIisc(imported.document);
    const auto restored = decodeIisc(encoded.bytes);
    if (!rendered.ok() || rendered.pixels.pixels != expected || !encoded.ok() || !restored.ok()
        || encodeIisc(restored.document).bytes != encoded.bytes) { return false; }

    QTemporaryDir directory(QStringLiteral(IISHAREDCANVAS_CONSUMER_OUTPUT_DIR "/layered-XXXXXX"));
    if (!directory.isValid()) { return false; }
    QFile source(directory.filePath("layered input.data"));
    if (!source.open(QIODevice::WriteOnly) || source.write(bytes) != bytes.size()) { return false; }
    source.close();
    const auto fromFile = importLayeredDocument(source.fileName().toStdString(), options);
    if (!fromFile.ok() || encodeIisc(fromFile.document).bytes != encoded.bytes) { return false; }

    Document initial;
    initial.extent = imported.document.extent;
    DocumentFile file;
    const auto path = directory.filePath("installed-layers.iisc").toStdString();
    if (!file.create(path, initial).ok()) { return false; }
    const auto append = [&](Document &draft) {
        draft.assets.insert(draft.assets.end(), imported.document.assets.begin(), imported.document.assets.end());
        draft.layers.insert(draft.layers.end(), imported.document.layers.begin(), imported.document.layers.end());
        return true;
    };
    if (!file.edit(append).ok() || file.revision() != 1) { return false; }
    const auto committed = encodeIisc(*file.document()).bytes;
    if (file.edit(append).ok() || file.revision() != 1
        || encodeIisc(*file.document()).bytes != committed) { return false; }
    DocumentFile reopened;
    if (!reopened.open(path).ok() || renderFrame(*reopened.document(), 0).pixels.pixels != expected) { return false; }
    BitmapEditor editor(file, "installed-layers-asset-0");
    if (!editor.setPixel(0, 0, 0xffff0000U)) { return false; }
    // Imported values stay detached while the selected committed raster is editable.
    return renderFrame(imported.document, 0).pixels.pixels == expected;
}

bool verifyMediaInterchange()
{
    using namespace iiSharedCanvas;
    const auto source = makeRasterLayer(8, 8, 0x80443322U);
    BitmapExportOptions bitmapOptions;
    bitmapOptions.text = {{"parameters", "a tree\nSteps: 12, Sampler: Euler, Seed: 6"}};
    auto bitmapBytes = encodeBitmap(source, bitmapOptions);
    auto bitmap = decodeBitmap(bitmapBytes.bytes);
    if (!bitmapBytes.ok() || !bitmap.ok() || bitmap.asset.pixels.pixels != source.pixels
        || bitmap.text != bitmapOptions.text || bitmapFormats().empty()) { return false; }

    VectorAsset vector{"vector", {8, 8}, {{{MoveTo{{1, 1}}, LineTo{{7, 1}},
                                         QuadraticTo{{7, 7}, {1, 7}}, ClosePath{}},
                                        SolidPaint{0xff2244ffU}, std::nullopt}}};
    VectorExportOptions svgOptions;
    svgOptions.compressed = true;
    auto svg = encodeSvg(vector, svgOptions);
    auto imported = decodeSvg(svg.bytes);
    if (!svg.ok() || !imported.ok() || imported.asset.paths.empty()) { return false; }
    Document document;
    document.extent = {8, 8};
    document.assets.emplace_back(imported.asset);
    document.layers.emplace_back(VectorLayer{{"layer", "Imported SVGZ"}, StaticSource{imported.asset.id}});
    QTemporaryDir directory(QStringLiteral(IISHAREDCANVAS_CONSUMER_OUTPUT_DIR "/media-XXXXXX"));
    if (!directory.isValid()) { return false; }
    if (!exportPdf(document, directory.filePath("vector.pdf").toStdString()).ok()
        || !exportBitmapFrame(document, 0, directory.filePath("frame.png").toStdString()).ok()) { return false; }
    const auto filePath = directory.filePath("import.iisc").toStdString();
    DocumentFile file;
    if (!file.create(filePath, document).ok()) { return false; }
    DocumentFile reopened;
    if (!reopened.open(filePath).ok() || renderFrame(*reopened.document(), 0).pixels.pixels
        != renderFrame(document, 0).pixels.pixels) { return false; }
    const auto capabilities = videoCapabilities();
    if (capabilities.result.code == MediaIoCode::DependencyUnavailable) { return true; }
    const auto videoPath = directory.filePath("animation.mkv").toStdString();
    if (!capabilities.ok() || !exportVideo(document, videoPath).ok()) { return false; }
    auto movie = importVideo(videoPath);
    return movie.ok() && movie.document.timeline.frameCount == 1
        && renderFrame(movie.document, 0).pixels.pixels == renderFrame(document, 0).pixels.pixels;
}

bool verifyWorkingFile(const iiSharedCanvas::Document &document)
{
    using namespace iiSharedCanvas;
    QTemporaryDir directory(QStringLiteral(IISHAREDCANVAS_CONSUMER_OUTPUT_DIR "/file-XXXXXX"));
    if (!directory.isValid()) {
        return false;
    }
    const auto path = directory.filePath(QStringLiteral("installed.iisc")).toStdString();
    DocumentFile file;
    if (!file.create(path, document).ok()) {
        return false;
    }
    DocumentEditor structure(file);
    BitmapEditor pixels(file, "installed-pixels");
    VectorEditor vectors(file, "installed-shape");
    if (!structure.setLayerName("installed-layer", "Write-through installed package").ok()
        || !pixels.setPixel(0, 0, 0xff778899U) || !pixels.undo() || !pixels.redo()
        || !vectors.setViewport({8, 8}).ok()) {
        return false;
    }
    DocumentFile reopened;
    if (!reopened.open(path).ok()) {
        return false;
    }
    return encodeIisc(*file.document()).bytes == encodeIisc(*reopened.document()).bytes
        && findRasterAsset(*reopened.document(), "installed-pixels")->pixels.pixels.front() == 0xff778899U
        && reopened.revision() == file.revision();
}

} // namespace

int main(int argc, char **argv)
{
    if (qEnvironmentVariableIsEmpty("QT_QPA_PLATFORM")) { qputenv("QT_QPA_PLATFORM", "offscreen"); }
    QGuiApplication application(argc, argv);
    using namespace iiSharedCanvas;

    Document document;
    document.extent = {4, 4};
    document.timeline = {{24, 1}, 3};
    document.assets.emplace_back(
        RasterAsset{"installed-raster", makeRasterLayer(4, 4, 0xff112233U)});
    document.layers.emplace_back(BitmapLayer{
        {"installed-layer", "Installed package", true, 1.0, {},
         RasterBlendMode::SourceOver},
        StaticSource{"installed-raster"},
    });

    VectorPath shape;
    shape.commands = {
        MoveTo{{0.0, 0.0}},
        LineTo{{3.0, 0.0}},
        LineTo{{3.0, 3.0}},
        ClosePath{},
    };
    shape.fill = SolidPaint{0xff00cc88U};
    shape.stroke = StrokeStyle{SolidPaint{0xff102030U}, 0.75};
    document.assets.emplace_back(VectorAsset{"installed-shape", {4, 4}, {shape}});

    AffineTransform shapeTransform;
    shapeTransform.translationX = 0.5;
    shapeTransform.translationY = 1.0;
    document.layers.emplace_back(VectorLayer{
        {"installed-shape-layer", "Detailed shape", false, 0.625, shapeTransform,
         RasterBlendMode::Multiply},
        KeyframedSource{{0}},
    });
    document.frames.push_back({
        0,
        {{"installed-shape-layer", "installed-shape"}},
    });

    DocumentEditor structure(document);
    const DocumentEditResult renamed = structure.renameAsset(
        "installed-raster", "installed-pixels");
    const DocumentEditResult layerName = structure.setLayerName(
        "installed-layer", "Edited installed package");
    const DocumentEditResult layerRange = structure.setLayerFrameRange(
        "installed-layer", LayerFrameRange{1, 2});
    const StableDiffusionGenerationParametersParseResult generationParameters =
        parseStableDiffusionGenerationParameters(
            "installed package prompt\n"
            "Steps: 20, Sampler: Euler, CFG scale: 7, Seed: 77, "
            "Size: 4x4, Model hash: 0123456789, Model: installed-model");
    StableDiffusionMetadata generation;
    generation.positivePrompt = "installed package test image";
    generation.negativePrompt = "watermark";
    generation.outputExtent = StableDiffusionImageExtent{4, 4};
    generation.samplingPasses.push_back({
        "3", 77, 20, 7.0, "euler", "normal", 1.0,
        std::nullopt, std::nullopt,
    });
    generation.software = "ComfyUI";
    generation.comfyUi.promptJson =
        R"json({"3":{"class_type":"KSampler","inputs":{"seed":77,"steps":20,"cfg":7.0}}})json";
    generation.comfyUi.workflowJson =
        R"json({"version":1,"state":{},"nodes":[]})json";
    const DocumentEditResult generationEdit =
        structure.setStableDiffusionMetadata(generation);
    VectorEditor vectorEditor(document, "installed-shape");
    const DocumentEditResult openedShape = vectorEditor.openPath(0);
    const DocumentEditResult quadraticShape = vectorEditor.appendQuadraticBezierTo(
        0, {2.5, 3.5}, {1.5, 3.0});
    const DocumentEditResult cubicShape = vectorEditor.appendCubicBezierTo(
        0, {1.0, 2.5}, {0.5, 1.5}, {0.0, 0.0});
    const DocumentEditResult closedShape = vectorEditor.closePath(0);
    const Asset *asset = resolveAssetAt(document, document.layers.front(), 1);
    BitmapEditor editor(document, "installed-pixels");
    const bool edited = editor.setPixel(2, 1, 0xffaabbccU);
    const FrameRenderResult rendered = renderFrame(document, 1);
    const FrameRenderTileRequest installedTileRequest{
        {{0, 0}, {4, 4}}, {2, 2}};
    const FrameTileRenderResult renderedTile = renderFrameTiles(
        document, 1, {installedTileRequest});
    const FrameLayerBatchRenderResult renderedLayers = renderFrameLayers(
        document, 1, {installedTileRequest});
    const FrameTileRenderResult recomposedLayers = composeFrameLayers(renderedLayers);
    AsyncFrameRenderer asyncRenderer;
    const IiscEncodeResult encoded = encodeIisc(document);
    const IiscDecodeResult decoded = encoded.ok()
        ? decodeIisc(encoded.bytes)
        : IiscDecodeResult{};
    const RasterAsset *decodedImage = decoded.ok()
        ? findRasterAsset(decoded.document, "installed-pixels")
        : nullptr;
    const VectorAsset *decodedShape = decoded.ok()
        ? findVectorAsset(decoded.document, "installed-shape")
        : nullptr;
    const Layer *decodedShapeLayer = decoded.ok()
        ? findLayer(decoded.document, "installed-shape-layer")
        : nullptr;
    const Layer *decodedInstalledLayer = decoded.ok()
        ? findLayer(decoded.document, "installed-layer")
        : nullptr;
    const KeyframedSource *decodedShapeSource = decodedShapeLayer
        ? std::get_if<KeyframedSource>(&layerSource(*decodedShapeLayer))
        : nullptr;
    const Frame *decodedFrame = decoded.ok()
        ? findFrame(decoded.document, 0)
        : nullptr;
    const Keyframe *decodedShapeKeyframe = decodedFrame
        ? findKeyframe(*decodedFrame, "installed-shape-layer")
        : nullptr;
    const LayerProperties *decodedShapeProperties = decodedShapeLayer
        ? &layerProperties(*decodedShapeLayer)
        : nullptr;

    CameraRawData cameraRaw;
    cameraRaw.image.kind = CameraRawImageKind::Monochrome;
    cameraRaw.image.extent = {2, 1};
    cameraRaw.image.bitsPerSample = 12;
    cameraRaw.image.samplesPerPixel = 1;
    cameraRaw.image.samples = {64, 2048};
    cameraRaw.image.colorChannels = {
        {CameraRawChannelRole::Luminance, "luminance"},
    };

    TimelineVideoStream timelineStream;
    timelineStream.id = "installed-video-stream";
    timelineStream.timeBase = {1, 24'000};
    timelineStream.durationTicks = 240'000;
    timelineStream.codec.identifier = "h264";
    timelineStream.codedExtent = {1'920, 1'080};
    timelineStream.displayExtent = timelineStream.codedExtent;
    timelineStream.pixelAspectRatio = {1, 1};
    timelineStream.timing.mode = TimelineFrameRateMode::Constant;
    timelineStream.timing.nominal = TimelineFrameRate{24, 1};
    timelineStream.pixelFormat = "yuv420p";
    timelineStream.bitDepth = 8;

    TimelineMediaRepresentation timelineRepresentation;
    timelineRepresentation.id = "installed-original";
    timelineRepresentation.uri = "file:///media/installed-source.mp4";
    timelineRepresentation.container.identifier = "mp4";
    timelineRepresentation.durationTicks = 240'000;
    timelineRepresentation.timeBase = {1, 24'000};
    timelineRepresentation.streams.emplace_back(timelineStream);

    TimelineMediaSource timelineSource;
    timelineSource.id = "installed-source";
    timelineSource.name = "Installed source";
    timelineSource.originalRepresentationId = timelineRepresentation.id;
    timelineSource.activeRepresentationId = timelineRepresentation.id;
    timelineSource.representations.push_back(timelineRepresentation);

    TimelineVideoClip timelineClip;
    timelineClip.properties.id = "installed-clip";
    timelineClip.properties.name = "Installed clip";
    timelineClip.properties.source = TimelineMediaReference{
        timelineSource.id, timelineStream.id};
    timelineClip.properties.timelineRange = {0, 240'000};
    timelineClip.properties.sourceRange = {0, 240'000};
    timelineClip.properties.playbackRate = {1, 1};

    TimelineVideoTrack timelineTrack;
    timelineTrack.properties.id = "installed-video-track";
    timelineTrack.properties.name = "Video";
    timelineTrack.clips.push_back(timelineClip);

    TimelineSequence timelineSequence;
    timelineSequence.id = "installed-sequence";
    timelineSequence.name = "Installed sequence";
    timelineSequence.timeBase = {1, 24'000};
    timelineSequence.editingFrameRate = {24, 1};
    timelineSequence.durationTicks = 240'000;
    timelineSequence.canvasExtent = {1'920, 1'080};
    timelineSequence.pixelAspectRatio = {1, 1};
    timelineSequence.tracks.emplace_back(timelineTrack);

    TimelineVideoOutput timelineOutput;
    timelineOutput.codec.identifier = "h264";
    timelineOutput.extent = timelineSequence.canvasExtent;
    timelineOutput.frameRate = {24, 1};
    timelineOutput.pixelAspectRatio = {1, 1};
    timelineOutput.pixelFormat = "yuv420p";
    timelineOutput.bitDepth = 8;

    TimelineRenderProfile timelineProfile;
    timelineProfile.id = "installed-delivery";
    timelineProfile.name = "Installed delivery";
    timelineProfile.sequenceId = timelineSequence.id;
    timelineProfile.container.identifier = "mp4";
    timelineProfile.video = timelineOutput;

    TimelineProject timelineProject;
    timelineProject.id = "installed-timeline";
    timelineProject.name = "Installed timeline";
    timelineProject.activeSequenceId = timelineSequence.id;
    timelineProject.mediaSources.push_back(timelineSource);
    timelineProject.sequences.push_back(timelineSequence);
    timelineProject.renderProfiles.push_back(timelineProfile);
    const bool initialTimelineValid = validateTimelineProject(timelineProject).ok();

    TimelineEditor timelineEditor(timelineProject);
    const TimelineEditResult timelineRate = timelineEditor.setSequenceFrameRate(
        timelineSequence.id, {30'000, 1'001});
    TimelineContainerDescriptor matroska;
    matroska.identifier = "matroska";
    matroska.fileExtension = "mkv";
    const TimelineEditResult timelineContainer = timelineEditor.setRenderContainer(
        timelineProfile.id, matroska);
    TimelineCodecDescriptor av1;
    av1.identifier = "av1";
    av1.profile = "main";
    const TimelineEditResult timelineCodec = timelineEditor.setRenderVideoCodec(
        timelineProfile.id, av1);
    return verifyWorkingFile(document) && verifyMediaInterchange() && verifyLayeredInterchange() && verifyPsdExport() && verifyTimelineInterchange()
        && verifyAudioTimeline()
        && validate(document).ok()
        && validateCameraRaw(cameraRaw).ok()
        && cameraRawSampleAt(cameraRaw.image, 1, 0)
            == std::optional<std::uint32_t>{2048}
        && asset
        && renamed.ok()
        && renamed.changed
        && layerName.ok()
        && layerRange.ok()
        && layerRange.changed
        && !layerExistsAt(document, document.layers.front(), 0)
        && layerExistsAt(document, document.layers.front(), 1)
        && layerExistsAt(document, document.layers.front(), 2)
        && generationParameters.ok()
        && generationParameters.metadata.outputExtent
            == std::optional<StableDiffusionImageExtent>{{4, 4}}
        && generationParameters.metadata.models.size() == 1
        && generationParameters.metadata.models.front().hashType
            == "sha256-prefix-10"
        && generationEdit.ok()
        && generationEdit.changed
        && vectorEditor.isBound()
        && openedShape.changed
        && quadraticShape.changed
        && cubicShape.changed
        && closedShape.changed
        && layerProperties(document.layers.front()).name == "Edited installed package"
        && assetId(*asset) == "installed-pixels"
        && edited
        && editor.pixelAt(2, 1) == std::optional<std::uint32_t>{0xffaabbccU}
        && rendered.ok()
        && rasterLayerPixelAt(rendered.pixels, {2, 1}) == 0xffaabbccU
        && renderedTile.ok()
        && renderedTile.tiles.size() == 1
        && renderedTile.tiles.front().pixels.width == 2
        && renderedLayers.ok()
        && renderedLayers.layers.size() == 2
        && renderedLayers.layers.front().layerId == "installed-layer"
        && renderedLayers.layers.back().layerId == "installed-shape-layer"
        && !renderedLayers.layers.back().visible
        && renderedLayers.layers.back().tiles.empty()
        && recomposedLayers.ok()
        && recomposedLayers.tiles.front().pixels.pixels
            == renderedTile.tiles.front().pixels.pixels
        && !asyncRenderer.busy()
        && decoded.ok()
        && decoded.document.formatVersion.major == 1
        && decoded.document.formatVersion.minor == CurrentFormatMinor
        && decoded.document.stableDiffusionMetadata == generation
        && decodedImage
        && decodedImage->pixels.width == 4
        && decodedImage->pixels.height == 4
        && rasterLayerPixelAt(decodedImage->pixels, {2, 1}) == 0xffaabbccU
        && decodedShape
        && decodedShape->viewport.width == 4
        && decodedShape->paths.size() == 1
        && decodedShape->paths.front().commands.size() == 6
        && std::holds_alternative<QuadraticTo>(
            decodedShape->paths.front().commands[3])
        && std::holds_alternative<CubicTo>(
            decodedShape->paths.front().commands[4])
        && decodedShape->paths.front().fill
        && decodedShape->paths.front().fill->argb == 0xff00cc88U
        && decodedShape->paths.front().stroke
        && decodedShape->paths.front().stroke->width == 0.75
        && decodedShapeLayer
        && std::holds_alternative<VectorLayer>(*decodedShapeLayer)
        && decodedShapeProperties
        && decodedShapeProperties->name == "Detailed shape"
        && !decodedShapeProperties->visible
        && decodedShapeProperties->opacity == 0.625
        && decodedShapeProperties->transform.translationX == 0.5
        && decodedShapeProperties->transform.translationY == 1.0
        && decodedShapeProperties->blendMode == RasterBlendMode::Multiply
        && decodedInstalledLayer
        && layerProperties(*decodedInstalledLayer).frameRange
        && layerProperties(*decodedInstalledLayer).frameRange->firstFrame == 1
        && layerProperties(*decodedInstalledLayer).frameRange->lastFrame == 2
        && !layerExistsAt(decoded.document, *decodedInstalledLayer, 0)
        && layerExistsAt(decoded.document, *decodedInstalledLayer, 1)
        && layerExistsAt(decoded.document, *decodedInstalledLayer, 2)
        && decodedShapeSource
        && decodedShapeSource->frameIndices == std::vector<FrameIndex>{0}
        && decodedFrame
        && decodedShapeKeyframe
        && decodedShapeKeyframe->assetId == "installed-shape"
        && encodeIisc(decoded.document).bytes == encoded.bytes
        && initialTimelineValid
        && timelineEditor.isBound()
        && timelineRate.changed
        && timelineContainer.changed
        && timelineCodec.changed
        && validateTimelineProject(timelineProject).ok()
        && findTimelineSequence(timelineProject, timelineSequence.id)
            ->editingFrameRate == TimelineFrameRate{30'000, 1'001}
        && findTimelineRenderProfile(timelineProject, timelineProfile.id)
            ->container.identifier == "matroska"
        && findTimelineRenderProfile(timelineProject, timelineProfile.id)
            ->video->codec.identifier == "av1"
        ? 0
        : 1;
}
