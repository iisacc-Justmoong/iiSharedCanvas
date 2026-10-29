#include <iiSharedCanvasNative.h>
#include <iiFileProviderNative.h>

#include <filesystem>
#include <fstream>
#include <iostream>
#include <iterator>
#include <vector>

using namespace iiSharedCanvas;
namespace fs = std::filesystem;

namespace {
int failures = 0;
void check(bool condition, const char *message)
{
    if (!condition) { std::cerr << message << '\n'; ++failures; }
}
std::string pathText(const fs::path &path)
{
    const auto value = path.u8string();
    return {value.begin(), value.end()};
}
std::vector<std::uint8_t> readBytes(const fs::path &path)
{
    std::ifstream stream(path, std::ios::binary);
    return {std::istreambuf_iterator<char>(stream), std::istreambuf_iterator<char>()};
}
}

int main()
{
    fs::create_directories(IISHAREDCANVAS_TEST_OUTPUT_DIR);
    iiFileProvider::native::TemporaryDirectory temporary{fs::path(IISHAREDCANVAS_TEST_OUTPUT_DIR)};

    // Independently specified 8 kHz mono PCM16 fixture, including both extrema.
    const std::vector<std::uint8_t> wav = {
        'R','I','F','F', 40,0,0,0, 'W','A','V','E',
        'f','m','t',' ', 16,0,0,0, 1,0, 1,0, 0x40,0x1f,0,0,
        0x80,0x3e,0,0, 2,0, 16,0, 'd','a','t','a', 4,0,0,0,
        0,0x80, 0xff,0x7f
    };
    const auto decoded = decodeAudioWav(wav, {.assetId = "voice"});
    check(decoded.ok() && decoded.asset.sampleRate == 8000 && decoded.asset.channelCount == 1
              && decoded.asset.samples == std::vector<std::int16_t>{-32768, 32767},
          "independent PCM16 WAV is decoded without Qt");
    const auto encoded = encodeAudioWav(decoded.asset);
    check(encoded.ok() && encoded.bytes == wav, "PCM16 WAV is encoded with correct RIFF fields");
    auto damaged = wav; damaged.pop_back();
    check(!decodeAudioWav(damaged).ok(), "truncated WAV is rejected");
    AudioImportOptions limited; limited.limits.maxDecodedBytes = 3;
    check(decodeAudioWav(wav, limited).result.code == MediaIoCode::LimitExceeded,
          "decoded WAV obeys byte limits");
    const auto audioPath = pathText(temporary.path() / "voice 한글.wav");
    check(exportAudioWav(decoded.asset, audioPath).ok(), "WAV is published to a Unicode path");
    check(exportAudioWav(decoded.asset, audioPath).code == MediaIoCode::AlreadyExists,
          "WAV publication does not overwrite an existing file");
    check(importAudioWav(audioPath).asset.samples == decoded.asset.samples, "WAV file round trip");

    Document document;
    document.extent = {4, 2};
    document.assets.emplace_back(RasterAsset{"pixels", makeRasterLayer(4, 2, 0x80ff0000U)});
    document.layers.emplace_back(StaticBitmapLayer{{"layer", "Layer"}, StaticSource{"pixels"}});
    const auto preview = renderDocumentPreview(document, {{2, 2}});
    check(preview.ok() && preview.pixels.width == 2 && preview.pixels.height == 1
              && preview.pixels.pixels.front() == 0x80ff0000U,
          "preview fits artwork and preserves alpha");
    check(renderDocumentPreview(document, {{0, 2}}).result.code == MediaIoCode::InvalidArgument,
          "invalid preview bounds are rejected");
    check(renderDocumentPreview(document, {{4097, 2}}).result.code == MediaIoCode::LimitExceeded,
          "preview bounds enforce the allocation limit");

    const auto snapshot = temporary.path() / "snapshot 한글.iisc";
    const auto serialized = encodeIisc(document);
    check(serialized.ok(), "preview snapshot fixture encodes");
    {
        std::ofstream stream(snapshot, std::ios::binary);
        stream.write(reinterpret_cast<const char *>(serialized.bytes.data()),
                     static_cast<std::streamsize>(serialized.bytes.size()));
        check(stream.good(), "preview snapshot fixture writes");
    }
    const auto before = readBytes(snapshot);
    const auto lastWrite = fs::last_write_time(snapshot);
    const auto readPreview = readIiscPreview(pathText(snapshot), {{2, 2}});
    check(readPreview.ok() && readPreview.pixels.pixels == preview.pixels.pixels,
          "preview reads a native snapshot without a GUI");
    check(readBytes(snapshot) == before && fs::last_write_time(snapshot) == lastWrite,
          "preview does not modify snapshot bytes or timestamp");
    SerializationLimits limits; limits.maximumContainerBytes = 8;
    check(readIiscPreview(pathText(snapshot), {}, limits).result.code == MediaIoCode::LimitExceeded,
          "preview enforces input limits");

    const auto working = pathText(temporary.path() / "working.iisc");
    DocumentFile file;
    check(file.create(working, document).ok(), "working document fixture is created");
    check(file.edit([](Document &draft) {
        std::get<RasterAsset>(draft.assets.front()).pixels.pixels.assign(8, 0xff123456U);
        return true;
    }).ok(), "working document edit commits");
    const auto revision = file.revision();
    const auto workingPreview = readIiscPreview(working);
    check(workingPreview.ok() && workingPreview.pixels.pixels.front() == 0xff123456U
              && file.revision() == revision,
          "preview includes committed working-file edits without changing revision");
    file.close();
    check(!readIiscPreview("https://example.invalid/canvas.iisc").ok(), "remote paths are rejected");
    return failures ? 1 : 0;
}
