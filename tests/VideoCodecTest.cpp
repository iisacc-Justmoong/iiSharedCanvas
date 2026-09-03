#include <iiSharedCanvas.h>

#include <QDir>
#include <QFile>
#include <QGuiApplication>
#include <QProcess>
#include <QTemporaryDir>
#include <QThread>

#include <algorithm>
#include <iostream>

namespace {
int failures = 0;
void expect(bool value, const std::string &message)
{
    if (!value) { std::cerr << message << '\n'; ++failures; }
}
}

int main(int argc, char **argv)
{
    // Bounded child-process fixture; tests deadlines without depending on codec speed.
    if (argc == 3 && std::string(argv[1]) == "-hide_banner" && std::string(argv[2]) == "-version") {
        QThread::msleep(250);
        return 0;
    }
    QGuiApplication app(argc, argv);
    using namespace iiSharedCanvas;
    QDir().mkpath(IISHAREDCANVAS_TEST_OUTPUT_DIR);
    QTemporaryDir directory(QStringLiteral(IISHAREDCANVAS_TEST_OUTPUT_DIR "/video-codec-XXXXXX"));

    MediaBackendOptions delayed;
    delayed.ffmpegPath = QCoreApplication::applicationFilePath().toStdString();
    delayed.timeoutMs = 30;
    expect(videoCapabilities(delayed).result.code == MediaIoCode::TimedOut,
           "a stalled backend reports its deadline, not corrupt media or missing dependency");
    delayed.timeoutMs = 1000;
    int cancellationChecks = 0;
    delayed.cancelled = [&] { return ++cancellationChecks >= 3; };
    expect(videoCapabilities(delayed).result.code == MediaIoCode::Cancelled,
           "cancellation also interrupts an already-running backend");

    MediaBackendOptions unavailable;
    unavailable.ffmpegPath = directory.filePath("missing-ffmpeg").toStdString();
    expect(videoCapabilities(unavailable).result.code == MediaIoCode::DependencyUnavailable,
           "missing runtime is explicit, not claimed as supported");
    const auto capabilities = videoCapabilities();
    if (capabilities.result.code == MediaIoCode::DependencyUnavailable) {
        std::cout << "SKIP: FFmpeg runtime unavailable; missing-backend contract tested\n";
        return failures == 0 ? 77 : 1;
    }
    expect(capabilities.ok(), "discover FFmpeg runtime: " + capabilities.result.message);
    if (!capabilities.ok()) { return 1; }
    expect(!capabilities.demuxers.empty() && !capabilities.muxers.empty()
           && !capabilities.videoDecoders.empty() && !capabilities.videoEncoders.empty(),
           "capability parsing must actually discover formats and codecs");
    const auto available = [&](const std::string &muxer, const std::string &encoder) {
        return std::find(capabilities.muxers.begin(), capabilities.muxers.end(), muxer) != capabilities.muxers.end()
            && std::find(capabilities.videoEncoders.begin(), capabilities.videoEncoders.end(), encoder) != capabilities.videoEncoders.end();
    };
    Document document;
    document.extent = {32, 24};
    document.timeline = {{30000, 1001}, 4};
    document.assets.emplace_back(RasterAsset{"red", makeRasterLayer(32, 24, 0xffff0000U)});
    document.assets.emplace_back(RasterAsset{"blue", makeRasterLayer(32, 24, 0xff0000ffU)});
    document.layers.emplace_back(BitmapLayer{{"video", "Video"}, KeyframedSource{{0, 2}}});
    document.frames = {{0, {{"video", "red"}}}, {2, {{"video", "blue"}}}};
    const auto losslessPath = directory.filePath("lossless with spaces 한글.mkv").toStdString();
    expect(exportVideo(document, losslessPath).ok(), "lossless video export");
    auto probe = probeVideo(losslessPath);
    expect(probe.ok() && probe.info.extent.width == 32 && probe.info.extent.height == 24,
           "probe dimensions from real video bytes");
    auto decoded = importVideo(losslessPath);
    expect(decoded.ok(), "video decoding: " + decoded.result.message);
    if (decoded.ok()) {
        expect(validate(decoded.document).ok() && decoded.document.timeline.frameCount == 4,
               "video imports into valid frame-owned bitmap keys");
        expect(decoded.document.timeline.frameRate.numerator == 30000
               && decoded.document.timeline.frameRate.denominator == 1001,
               "preserve rational frame rate");
        for (FrameIndex frame = 0; frame < 4; ++frame) {
            expect(renderFrame(decoded.document, frame).pixels.pixels == renderFrame(document, frame).pixels.pixels,
                   "lossless video frame ordering, hold sampling and pixels");
        }
    }
    const auto audioPath = directory.filePath("with-audio.mkv");
    QProcess mux;
    mux.start("ffmpeg", {"-v", "error", "-nostdin", "-i", QString::fromStdString(losslessPath),
        "-f", "lavfi", "-i", "sine=frequency=440:duration=0.3", "-map", "0:v:0", "-map", "1:a:0",
        "-c:v", "copy", "-c:a", "pcm_s16le", "-shortest", audioPath});
    const bool muxFinished = mux.waitForFinished(30000);
    if (!muxFinished) { mux.kill(); mux.waitForFinished(1000); }
    expect(muxFinished && mux.exitStatus() == QProcess::NormalExit && mux.exitCode() == 0,
           "create independent video with an audio track");
    auto withAudio = importVideo(audioPath.toStdString());
    expect(withAudio.ok() && probeVideo(audioPath.toStdString()).info.audioStreamCount == 1
           && std::any_of(withAudio.result.warnings.begin(), withAudio.result.warnings.end(),
                          [](const auto &message) { return message.find("audio tracks") != std::string::npos; }),
           "audio loss must be detected from source streams and disclosed");
    const auto rangedPath = directory.filePath("ranged.mkv").toStdString();
    auto ranged = document;
    layerProperties(ranged.layers[0]).frameRange = LayerFrameRange{1, 2};
    expect(exportVideo(ranged, rangedPath).ok(), "export honors layer existence range");
    auto rangedImport = importVideo(rangedPath);
    expect(rangedImport.ok(), "read ranged video");
    if (rangedImport.ok()) {
        expect(renderFrame(rangedImport.document, 0).pixels.pixels[0] == 0
               && renderFrame(rangedImport.document, 1).pixels.pixels[0] == 0xffff0000U
               && renderFrame(rangedImport.document, 3).pixels.pixels[0] == 0,
               "ranged export preserves transparent excluded frames and visible held keys");
    }
    struct Profile { const char *extension; const char *muxer; const char *encoder; const char *pixelFormat; };
    for (const auto &profile : {Profile{"mp4", "mp4", "libx264", "yuv420p"},
                               Profile{"hevc.mp4", "mp4", "libx265", "yuv420p"},
                               Profile{"webm", "webm", "libvpx-vp9", "yuv420p"},
                               Profile{"mov", "mov", "qtrle", "argb"},
                               Profile{"prores.mov", "mov", "prores_ks", "yuv422p10le"},
                               Profile{"avi", "avi", "ffv1", "bgra"},
                               Profile{"gif", "gif", "gif", "rgb8"},
                               Profile{"apng", "apng", "apng", "rgba"}}) {
        if (!available(profile.muxer, profile.encoder)) { continue; }
        auto rateDocument = document;
        // GIF time is quantized to centiseconds; use an exactly representable rate.
        rateDocument.timeline.frameRate = {10, 1};
        const auto path = directory.filePath(QString("profile.%1").arg(profile.extension)).toStdString();
        VideoExportOptions options;
        options.container = profile.muxer;
        options.codec = profile.encoder;
        options.pixelFormat = profile.pixelFormat;
        auto result = exportVideo(rateDocument, path, options);
        expect(result.ok(), std::string("encode ") + profile.extension + ": " + result.message);
        if (!result.ok()) { continue; }
        auto roundTrip = importVideo(path);
        expect(roundTrip.ok() && roundTrip.document.timeline.frameCount == 4,
               std::string("decode ") + profile.extension + ": " + roundTrip.result.message);
        std::cout << "video round-trip: " << profile.extension << '/' << profile.encoder << '\n';
    }
    VideoImportOptions selected;
    selected.firstFrame = 1;
    selected.frameCount = 2;
    auto selection = importVideo(losslessPath, selected);
    expect(selection.ok() && selection.document.timeline.frameCount == 2,
           "selected video frame range is rebased to frame zero");
    if (selection.ok()) {
        expect(renderFrame(selection.document, 0).pixels.pixels[0] == 0xffff0000U
               && renderFrame(selection.document, 1).pixels.pixels[0] == 0xff0000ffU,
               "selected frame range uses source order");
    }
    selected = {};
    selected.limits.maxFrames = 2;
    auto tooMany = importVideo(losslessPath, selected);
    expect(tooMany.result.code == MediaIoCode::LimitExceeded && tooMany.document.assets.empty(),
           "limits fail without returning truncated video as success");
    selected = {};
    selected.limits.maxDecodedBytes = 32 * 24 * 4;
    expect(importVideo(losslessPath, selected).result.code == MediaIoCode::LimitExceeded,
           "bound total decoded video memory");
    selected = {};
    selected.backend.cancelled = [] { return true; };
    expect(importVideo(losslessPath, selected).result.code == MediaIoCode::Cancelled,
           "cancellation is distinct from corrupt media");
    expect(!probeVideo("https://example.invalid/movie.mp4").ok(), "only local media inputs are accepted");
    const auto playlistPath = directory.filePath("external.m3u8");
    QFile playlist(playlistPath);
    expect(playlist.open(QIODevice::WriteOnly), "create rejected playlist fixture");
    playlist.write("#EXTM3U\n#EXTINF:1,\nhttps://example.invalid/secret.ts\n#EXT-X-ENDLIST\n");
    playlist.close();
    expect(!probeVideo(playlistPath.toStdString()).ok(), "playlist demuxers cannot open external resources");
    expect(exportVideo(document, losslessPath).code == MediaIoCode::AlreadyExists,
           "video output collision preserves original file");
    VideoExportOptions bad;
    bad.codec = "unavailable_codec";
    const auto badPath = directory.filePath("bad.mkv").toStdString();
    expect(!exportVideo(document, badPath, bad).ok() && !QFile::exists(QString::fromStdString(badPath)),
           "failed encoding must leave no partial destination");
    bad = {};
    bad.limits.maxOutputBytes = 32;
    expect(exportVideo(document, badPath, bad).code == MediaIoCode::LimitExceeded
           && !QFile::exists(QString::fromStdString(badPath)), "bound video output bytes");
    return failures == 0 ? 0 : 1;
}
