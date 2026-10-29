#include "VisualCodecVideoDecoder.h"

#include <iiVisualCodec/VideoCodec.h>
#include <Render/FrameRenderer.h>

#include <algorithm>
#include <array>
#include <cmath>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <iterator>
#include <optional>
#include <string_view>
#include <vector>

namespace {
int failures = 0;
void expect(bool value, std::string_view message)
{
    if (!value) { std::cerr << message << '\n'; ++failures; }
}
std::vector<std::uint8_t> fixture(const char *name)
{
    std::ifstream file(std::filesystem::path(IISHAREDCANVAS_VISUAL_FIXTURE_DIR) / name, std::ios::binary);
    if (!file) { ++failures; std::cerr << "cannot open video fixture\n"; return {}; }
    return {std::istreambuf_iterator<char>(file), std::istreambuf_iterator<char>()};
}
std::optional<std::size_t> trackMatrix(const std::vector<std::uint8_t> &data)
{
    constexpr std::array<std::uint8_t, 4> name{'t', 'k', 'h', 'd'};
    auto position = data.begin();
    while ((position = std::search(position, data.end(), name.begin(), name.end())) != data.end()) {
        const auto index = std::size_t(position - data.begin());
        if (index >= 4 && data.size() - index >= 8) {
            const auto start = index - 4;
            const auto size = (std::uint32_t(data[start]) << 24) | (std::uint32_t(data[start + 1]) << 16)
                | (std::uint32_t(data[start + 2]) << 8) | data[start + 3];
            const auto version = data[index + 4];
            if (version <= 1 && size <= data.size() - start) {
                const auto matrix_offset = std::size_t(version == 0 ? 48 : 60);
                if (size >= matrix_offset + 36) return start + matrix_offset;
            }
        }
        ++position;
    }
    return {};
}
}
int main()
{
    using namespace iiSharedCanvas;
    namespace visual = ii::visual;
    std::array<std::uint8_t, 16 * 8 * 4> red{}, green{}, blue{};
    for (std::size_t i = 0; i < red.size(); i += 4) {
        red[i] = green[i + 1] = blue[i + 2] = 255;
        red[i + 3] = green[i + 3] = blue[i + 3] = 255;
    }
    auto view = [](const auto &pixels) {
        return visual::image::ImageView{16, 8, visual::image::PixelFormat::rgba8, 0, pixels};
    };
    const std::array frames{
        visual::video::FrameView{view(red), 0, 1}, visual::video::FrameView{view(green), 1, 1},
        visual::video::FrameView{view(blue), 2, 1}};
    visual::video::EncodeOptions options; options.time_base = {1, 25};
    const auto encoded = visual::video::encode(frames, options);
    expect(bool(encoded), "the iiVisualCodec FFV1 fixture is encoded in memory");
    if (!encoded) return 1;
    example::VisualCodecVideoDecoder decoder;
    VideoDecodeRequest request; request.firstFrame = 1; request.frameCount = 1;
    request.frameRate = FrameRate{25, 1};
    const auto document = decodeVideoDocument(*encoded, decoder, "frame", "layer", request);
    expect(document.ok() && document.document.timeline.frameCount == 1,
           "the application adapter imports the selected constant-rate frame");
    if (document.ok()) {
        const auto rendered = renderFrame(document.document, 0);
        expect(rendered.ok() && rendered.pixels.pixels[0] == 0xff00ff00U,
               "the selected decoded frame renders with its original color");
    }
    request.frameRate = FrameRate{24, 1};
    expect(decodeVideoAsset(*encoded, decoder, "video", request).result.code == MediaIoCode::UnsupportedFeature,
           "the example does not silently resample a requested frame rate");
    request = {};
    auto irregular_frames = frames; irregular_frames[2].pts = 3;
    const auto irregular = visual::video::encode(irregular_frames, options);
    expect(bool(irregular), "a variable-timing fixture is encoded in memory");
    if (irregular)
        expect(decodeVideoAsset(*irregular, decoder, "video", request).result.code == MediaIoCode::UnsupportedFeature,
               "the example rejects uneven source frame timestamps");

    std::array<std::uint8_t, 16 * 8 * 4> pattern{};
    for (std::size_t y = 0; y < 8; ++y) for (std::size_t x = 0; x < 16; ++x) {
        const auto offset = (y * 16 + x) * 4;
        pattern[offset] = x < 8 ? 240 : 10;
        pattern[offset + 1] = y < 4 ? 230 : 20;
        pattern[offset + 2] = x < 8 ? 20 : 210;
        pattern[offset + 3] = 255;
    }
    const std::array rotation_frames{visual::video::FrameView{view(pattern), 0, 1}};
    auto rotation_options = options;
    rotation_options.container = visual::video::Container::mp4;
    rotation_options.codec = visual::video::Codec::mpeg4;
    auto rotated_video = visual::video::encode(rotation_frames, rotation_options);
    const auto reference = fixture("display-matrix-90.mp4");
    const auto reference_matrix = trackMatrix(reference);
    const auto target_matrix = rotated_video ? trackMatrix(*rotated_video) : std::nullopt;
    expect(rotated_video && reference_matrix && target_matrix, "in-memory MP4 exposes its display matrix");
    if (rotated_video && reference_matrix && target_matrix) {
        std::copy_n(reference.begin() + std::ptrdiff_t(*reference_matrix), 36,
                    rotated_video->begin() + std::ptrdiff_t(*target_matrix));
        const auto display = visual::video::inspect_display(*rotated_video);
        const auto coded = visual::video::decode(*rotated_video);
        expect(display && display->has_display_matrix && std::abs(display->rotation_degrees - 90.0) < 0.01
               && coded && coded->frames.size() == 1 && coded->frames[0].image.width == 16
               && coded->frames[0].image.height == 8 && coded->frames[0].image.rgba.size() == 16 * 8 * 4,
               "the rotated fixture retains coded pixels and the display matrix");
        const auto displayed = decodeVideoAsset(*rotated_video, decoder, "rotated");
        expect(displayed.ok() && displayed.asset.frames.size() == 1
               && displayed.asset.frames[0].width == 8 && displayed.asset.frames[0].height == 16
               && !displayed.result.warnings.empty(), "a right-angle display matrix becomes a normalized canvas frame");
        if (displayed.ok() && coded && coded->frames.size() == 1 && coded->frames[0].image.rgba.size() == 16 * 8 * 4) {
            const auto &rgba = coded->frames[0].image.rgba;
            const auto &argb = displayed.asset.frames[0].pixels;
            bool matches = true;
            for (std::size_t y = 0; y < 8; ++y) for (std::size_t x = 0; x < 16; ++x) {
                const auto source = (y * 16 + x) * 4;
                const auto expected = (std::uint32_t(rgba[source + 3]) << 24)
                    | (std::uint32_t(rgba[source]) << 16) | (std::uint32_t(rgba[source + 1]) << 8) | rgba[source + 2];
                matches &= argb[(15 - x) * 8 + y] == expected;
            }
            expect(matches, "counterclockwise rotation preserves every decoded pixel and its alpha");
        }
        const auto setMatrix = [&](const std::array<std::int32_t, 9> &matrix) {
            for (std::size_t i = 0; i < matrix.size(); ++i) {
                const auto value = std::uint32_t(matrix[i]);
                for (unsigned byte = 0; byte < 4; ++byte)
                    (*rotated_video)[*target_matrix + 4 * i + byte] = std::uint8_t(value >> (24 - 8 * byte));
            }
        };
        const std::array rotations{
            std::array<std::int32_t, 9>{-65536, 0, 0, 0, -65536, 0, 0, 0, 1073741824},
            std::array<std::int32_t, 9>{0, 65536, 0, -65536, 0, 0, 0, 0, 1073741824}};
        for (std::size_t i = 0; i < rotations.size(); ++i) {
            setMatrix(rotations[i]);
            const auto result = decodeVideoAsset(*rotated_video, decoder, "rotated");
            expect(result.ok() && result.asset.frames.size() == 1
                   && result.asset.frames[0].width == (i == 0 ? 16 : 8)
                   && result.asset.frames[0].height == (i == 0 ? 8 : 16),
                   "180 and 270-degree matrices normalize the frame dimensions");
            if (!result.ok() || result.asset.frames.size() != 1
                || result.asset.frames[0].width != (i == 0 ? 16 : 8)
                || result.asset.frames[0].height != (i == 0 ? 8 : 16)
                || !coded || coded->frames.size() != 1
                || coded->frames[0].image.rgba.size() != 16 * 8 * 4) continue;
            const auto &rgba = coded->frames[0].image.rgba;
            const auto &frame = result.asset.frames[0];
            bool matches = true;
            for (std::size_t y = 0; y < 8; ++y) for (std::size_t x = 0; x < 16; ++x) {
                const auto source = (y * 16 + x) * 4;
                const auto expected = (std::uint32_t(rgba[source + 3]) << 24)
                    | (std::uint32_t(rgba[source]) << 16) | (std::uint32_t(rgba[source + 1]) << 8) | rgba[source + 2];
                const auto dx = i == 0 ? 15 - x : 7 - y;
                const auto dy = i == 0 ? 7 - y : x;
                matches &= frame.pixels[dy * std::size_t(frame.width) + dx] == expected;
            }
            expect(matches, "180 and 270-degree rotation preserve every decoded pixel and its alpha");
        }
        // A reflected matrix must not be silently approximated as a rotation.
        setMatrix({-65536, 0, 0, 0, 65536, 0, 0, 0, 1073741824});
        expect(decodeVideoAsset(*rotated_video, decoder, "reflection").result.code == MediaIoCode::UnsupportedFeature,
               "reflected display matrices are rejected instead of changing the picture");
    }
    request.frameCount = 1; request.limits.maxFrames = 2;
    expect(decodeVideoAsset(*encoded, decoder, "video", request).result.code == MediaIoCode::LimitExceeded,
           "the whole source must fit the decoder limit even when selecting a smaller range");
    request = {};
    expect(decodeVideoAsset(fixture("display-sar-2x.mp4"), decoder, "video", request).result.code == MediaIoCode::UnsupportedFeature,
           "the example rejects non-square pixels until it can normalize them");
    expect(decodeVideoAsset(fixture("display-tagged.mkv"), decoder, "video", request).result.code == MediaIoCode::UnsupportedFeature,
           "the example rejects tagged video that also has non-square pixels");
    expect(decodeVideoAsset(fixture("display-frame-sei-90.mkv"), decoder, "video", request).result.code == MediaIoCode::UnsupportedFeature,
           "a frame-level display matrix differing from the stream is not silently ignored");
    request.cancelled = [] { return true; };
    expect(decodeVideoAsset(*encoded, decoder, "video", request).result.code == MediaIoCode::Cancelled,
           "cancellation is passed through the canvas boundary");
    int checks = 0;
    request.cancelled = [&] { return ++checks >= 6; };
    expect(decodeVideoAsset(*encoded, decoder, "video", request).result.code == MediaIoCode::Cancelled && checks >= 6,
           "cancellation also reaches the application decoder after probing");
    return failures ? 1 : 0;
}
