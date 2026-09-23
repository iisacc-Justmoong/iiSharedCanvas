#include <iiSharedCanvas.h>

#include <QDir>
#include <QTemporaryDir>

#include <iostream>
#include <span>

namespace {
int failures = 0;
void expect(bool value, const char *message)
{
    if (!value) { std::cerr << message << '\n'; ++failures; }
}

std::uint64_t unsignedAt(const std::vector<std::uint8_t> &bytes, std::size_t offset, std::size_t width)
{
    std::uint64_t value = 0;
    for (std::size_t index = 0; index < width; ++index) { value |= std::uint64_t(bytes.at(offset + index)) << (8 * index); }
    return value;
}
void overwrite(std::vector<std::uint8_t> &bytes, std::size_t offset, std::uint64_t value, std::size_t width)
{
    for (std::size_t index = 0; index < width; ++index) { bytes.at(offset + index) = static_cast<std::uint8_t>(value >> (8 * index)); }
    std::uint32_t crc = 0xffffffffU;
    for (const auto byte : std::span(bytes).subspan(iiSharedCanvas::IiscHeaderSize)) {
        crc ^= byte;
        for (int bit = 0; bit < 8; ++bit) { crc = (crc >> 1U) ^ (0xedb88320U & (0U - (crc & 1U))); }
    }
    crc ^= 0xffffffffU;
    for (std::size_t index = 0; index < 4; ++index) { bytes[24 + index] = static_cast<std::uint8_t>(crc >> (8 * index)); }
}
}

int main()
{
    using namespace iiSharedCanvas;
    Document source;
    source.extent = {4, 4};
    source.timeline = {{24, 1}, 12};
    source.assets.emplace_back(VideoAsset{"movie", {12, 1},
        {makeRasterLayer(2, 2, 0xffff0000), makeRasterLayer(2, 2, 0xff0000ff)}});
    VideoLayer layer{{"video", "Animated video"}, StaticSource{"movie"},
                     {1, 2, VideoEndBehavior::Hold}};
    layer.properties.frameRange = LayerFrameRange{2, 10};
    MotionKeyframe start;
    start.frame = 2;
    start.interpolation = MotionInterpolation::SmoothStep;
    MotionKeyframe end;
    end.frame = 10;
    end.value.position = {2, 2};
    end.value.opacity = 0.5;
    layer.properties.motion = {start, end};
    source.layers.emplace_back(layer);
    const auto encoded = encodeIisc(source);
    expect(encoded.ok(), "native video and motion must encode");
    if (!encoded.ok()) { return 1; }
    const auto decoded = decodeIisc(encoded.bytes);
    expect(decoded.ok(), "native video and motion must decode");
    if (!decoded.ok()) { return 1; }
    const auto *video = findVideoAsset(decoded.document, "movie");
    const auto *restored = findVideoLayer(decoded.document, "video");
    expect(video && video->frames.size() == 2 && video->frames[1].pixels[0] == 0xff0000ff
        && restored && restored->playback == layer.playback && restored->properties.motion == layer.properties.motion,
        "round trip must preserve owned frames, trim, end behavior and editable motion values");
    expect(encodeIisc(decoded.document).bytes == encoded.bytes, "new fields must have a canonical binary representation");
    // Walk the published 1.6 wire grammar independently to corrupt collection/tag fields.
    std::size_t cursor = IiscHeaderSize + 8 + 1 + 12 + 4;
    expect(encoded.bytes.at(cursor++) == 3, "native video must use asset tag 3");
    const auto skipString = [&] { cursor += 4 + unsignedAt(encoded.bytes, cursor, 4); };
    skipString();
    cursor += 8; // Rational source rate.
    const auto videoCountOffset = cursor;
    cursor += 4;
    for (int frame = 0; frame < 2; ++frame) { cursor += 25 + unsignedAt(encoded.bytes, cursor + 17, 8); }
    cursor += 4; // Layer count.
    skipString(); skipString();
    cursor += 1 + 8 + 48 + 1 + 1; // Visible, opacity, affine transform, blend, static source kind.
    skipString();
    cursor += 1 + 4 + 4; // Explicit frame range.
    const auto motionCountOffset = cursor;
    const auto interpolationOffset = cursor + 4 + 4 + 64;
    const auto endBehaviorOffset = cursor + 4 + 2 * 69 + 4 + 1 + 4;
    auto corrupt = encoded.bytes;
    overwrite(corrupt, videoCountOffset, 0xffffffffU, 4);
    expect(decodeIisc(corrupt).error.code == IiscErrorCode::LimitExceeded,
           "hostile video counts must be rejected before allocating frame collections");
    corrupt = encoded.bytes;
    overwrite(corrupt, motionCountOffset, 0xffffffffU, 4);
    expect(decodeIisc(corrupt).error.code == IiscErrorCode::LimitExceeded,
           "hostile motion counts must be rejected before allocating key collections");
    corrupt = encoded.bytes;
    overwrite(corrupt, interpolationOffset, 255, 1);
    expect(decodeIisc(corrupt).error.code == IiscErrorCode::InvalidData, "unknown interpolation tags must fail closed");
    corrupt = encoded.bytes;
    overwrite(corrupt, endBehaviorOffset, 255, 1);
    expect(decodeIisc(corrupt).error.code == IiscErrorCode::InvalidData, "unknown video end tags must fail closed");
    for (FrameIndex frame = 0; frame < source.timeline.frameCount; ++frame) {
        const auto original = renderFrame(source, frame);
        const auto roundTrip = renderFrame(decoded.document, frame);
        expect(original.ok() && roundTrip.ok() && original.pixels.pixels == roundTrip.pixels.pixels,
               "every rendered frame must survive the binary round trip");
    }
    SerializationLimits limits;
    limits.maximumTotalVideoFrames = 1;
    expect(encodeIisc(source, limits).error.code == IiscErrorCode::LimitExceeded
        && decodeIisc(encoded.bytes, limits).error.code == IiscErrorCode::LimitExceeded,
        "video frame budgets must apply to encoding and decoding");
    limits = {};
    limits.maximumTotalMotionKeyframes = 1;
    expect(encodeIisc(source, limits).error.code == IiscErrorCode::LimitExceeded
        && decodeIisc(encoded.bytes, limits).error.code == IiscErrorCode::LimitExceeded,
        "motion budgets must apply to encoding and decoding");
    limits = {};
    limits.maximumTotalRasterPixels = 7;
    expect(encodeIisc(source, limits).error.code == IiscErrorCode::LimitExceeded
        && decodeIisc(encoded.bytes, limits).error.code == IiscErrorCode::LimitExceeded,
        "video frames must share the total raster pixel budget");

    QDir().mkpath(IISHAREDCANVAS_TEST_OUTPUT_DIR);
    QTemporaryDir directory(QStringLiteral(IISHAREDCANVAS_TEST_OUTPUT_DIR "/canvas-media-XXXXXX"));
    expect(directory.isValid(), "working-file fixture directory must exist");
    expect(encodePsd(source).result.code == MediaIoCode::UnsupportedFeature,
           "PSD must explicitly reject native motion/video rather than discard the animation");
    const auto interchange = directory.filePath("interchange").toStdString();
    expect(exportTimelineInterchange(source, interchange).code == MediaIoCode::UnsupportedFeature
        && !QDir(QString::fromStdString(interchange)).exists(),
        "timeline XML must reject unsupported native animation without publishing a partial package");
    const auto path = directory.filePath("mixed.iisc").toStdString();
    DocumentFile file;
    expect(file.create(path, source).ok(), "working file must persist the new native fields");
    DocumentEditor editor(file);
    auto changed = layer.properties.motion;
    changed.back().value.position.x = 1;
    expect(editor.setLayerMotion("video", changed).changed, "file-bound motion edits must commit synchronously");
    expect(file.lastWriteStatistics().recordsWritten <= 2, "motion-only edits must retain unchanged media records");
    const auto revision = editor.revision();
    expect(!editor.setVideoPlayback("video", {2, {}, VideoEndBehavior::Hold}).ok()
        && editor.revision() == revision, "rejected file edits must preserve revision");
    const VideoPlayback changedPlayback{0, 1, VideoEndBehavior::Hold};
    expect(editor.setVideoPlayback("video", changedPlayback).changed, "valid video trims must commit synchronously");
    auto replacement = *findVideoAsset(*file.document(), "movie");
    replacement.frames[0].pixels[0] = 0xff112233;
    expect(editor.replaceVideoAsset("movie", replacement).changed, "file-bound video frame replacement must commit");
    file.close();
    DocumentFile reopened;
    expect(reopened.open(path).ok(), "mixed working file must reopen");
    if (reopened.document()) {
        const auto *reopenedLayer = findVideoLayer(*reopened.document(), "video");
        expect(reopenedLayer && reopenedLayer->properties.motion == changed
            && reopenedLayer->playback == changedPlayback
            && findVideoAsset(*reopened.document(), "movie")->frames[0].pixels[0] == 0xff112233,
            "committed motion, video trim and frame replacement must survive reopen");
    }

    for (std::uint16_t minor = 0; minor <= 5; ++minor) {
        Document legacy;
        legacy.extent = {1, 1};
        legacy.formatVersion.minor = minor;
        legacy.assets.emplace_back(RasterAsset{"image", makeRasterLayer(1, 1, 0xffabcdef)});
        legacy.layers.emplace_back(BitmapLayer{{"image", "Legacy"}, StaticSource{"image"}});
        const auto bytes = encodeIisc(legacy);
        const auto restoredLegacy = decodeIisc(bytes.bytes);
        expect(bytes.ok() && restoredLegacy.ok() && restoredLegacy.document.formatVersion.minor == minor
            && renderFrame(restoredLegacy.document, 0).pixels.pixels[0] == 0xffabcdef,
            "every legacy minor version must retain its original representation and render");
    }
    return failures ? 1 : 0;
}
