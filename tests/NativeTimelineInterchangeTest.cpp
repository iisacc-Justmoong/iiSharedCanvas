#include <Timeline/TimelineInterchange.h>
#include <Audio/AudioCodec.h>
#include <Bitmap/BitmapCodec.h>
#include <Serialization/IiscCodec.h>
#include <iiFileProviderNative.h>
#include <iiJson/Json.h>
#include <libxml/parser.h>

#include <algorithm>
#include <cstdint>
#include <filesystem>
#include <iostream>
#include <memory>
#include <string>
#include <string_view>
#include <thread>
#include <variant>
#include <vector>

namespace {
using namespace iiSharedCanvas;
namespace fs = std::filesystem;
namespace provider = iiFileProvider::native;
using Json = ii::json::Value;
int failures = 0;
void expect(bool value, const char *message)
{
    if (!value) { std::cerr << message << '\n'; ++failures; }
}
std::string utf8(const fs::path &path)
{
    const auto value = path.u8string(); return {value.begin(), value.end()};
}
fs::path path(const std::string &value)
{
    return fs::path(std::u8string(value.begin(), value.end()));
}
const Json *member(const Json *value, std::string_view key)
{
    if (!value) return nullptr;
    const auto *object = std::get_if<Json::Object>(&value->storage());
    if (!object) return nullptr;
    const auto found = object->find(key); return found == object->end() ? nullptr : &found->second;
}
const Json::Array *array(const Json *value)
{
    return value ? std::get_if<Json::Array>(&value->storage()) : nullptr;
}
std::string string(const Json *value)
{
    if (value) if (const auto *text = std::get_if<std::string>(&value->storage())) return *text;
    return {};
}
bool boolean(const Json *value, bool fallback = false)
{
    if (value) if (const auto *flag = std::get_if<bool>(&value->storage())) return *flag;
    return fallback;
}
std::uint64_t integer(const Json *value)
{
    if (value) {
        if (const auto *number = std::get_if<std::uint64_t>(&value->storage())) return *number;
        if (const auto *number = std::get_if<std::int64_t>(&value->storage()); number && *number >= 0)
            return std::uint64_t(*number);
    }
    return UINT64_MAX;
}
std::int64_t signedInteger(const Json *value)
{
    if (value) {
        if (const auto *number = std::get_if<std::int64_t>(&value->storage())) return *number;
        if (const auto *number = std::get_if<std::uint64_t>(&value->storage()); number && *number <= INT64_MAX)
            return std::int64_t(*number);
    }
    return INT64_MAX;
}
Document fixture()
{
    Document document; document.extent = {4, 4}; document.timeline = {{30000, 1001}, 12};
    document.assets.emplace_back(RasterAsset{"red", makeRasterLayer(4, 4, 0xffff0000U)});
    document.assets.emplace_back(RasterAsset{"green", makeRasterLayer(2, 2, 0xff00ff00U)});
    StaticBitmapLayer base; base.properties.id = "base"; base.properties.name = "바탕 & <red>";
    base.content = StaticSource{"red"}; document.layers.emplace_back(std::move(base));
    DynamicBitmapLayer keyed; keyed.properties.id = "keyed"; keyed.properties.name = "Keyed";
    keyed.properties.opacity = 0.5; keyed.properties.transform.translationX = 1;
    keyed.properties.frameRange = LayerFrameRange{2, 9}; keyed.content = KeyframedSource{{0, 5, 8}};
    document.layers.emplace_back(std::move(keyed));
    document.frames = {{0, {{"keyed", "green"}}}, {5, {{"keyed", "red"}}}, {8, {{"keyed", "green"}}}};
    AudioAsset audio{"voice", 48000, 1, std::vector<std::int16_t>(24000)};
    for (std::size_t index = 0; index < audio.samples.size(); ++index) audio.samples[index] = std::int16_t(int(index % 200) - 100);
    document.audioAssets.push_back(std::move(audio));
    document.audioTracks.push_back({"audio", "Voice & 대사", false, -3.0,
        {{"clip", "Take", "voice", 0, 6, 8009, -1.0, true}}});
    return document;
}
bool validXml(const fs::path &path, const char *rootName)
{
    const auto bytes = provider::File::read(path);
    using Xml = std::unique_ptr<xmlDoc, decltype(&xmlFreeDoc)>;
    Xml parsed(xmlReadMemory(bytes.data(), int(bytes.size()), "timeline.xml", "UTF-8", XML_PARSE_NONET), xmlFreeDoc);
    const auto *root = parsed ? xmlDocGetRootElement(parsed.get()) : nullptr;
    return root && std::string_view(reinterpret_cast<const char *>(root->name)) == rootName;
}
}

int run(const std::vector<std::string> &arguments)
{
    if (arguments.size() == 2 && arguments[0] == "--fixture") {
        const auto result = exportTimelineInterchange(fixture(), arguments[1]);
        if (!result.ok()) std::cerr << result.message << '\n';
        return result.ok() ? 0 : 1;
    }
    fs::create_directories(IISHAREDCANVAS_TEST_OUTPUT_DIR);
    provider::TemporaryDirectory temporary{fs::path(IISHAREDCANVAS_TEST_OUTPUT_DIR)};
    const auto document = fixture(); const auto original = encodeIisc(document);
    expect(original.ok(), "timeline fixture encodes");
    const auto destination = temporary.path() / u8"한글 & timeline";
    const auto exported = exportTimelineInterchange(document, utf8(destination));
    expect(exported.ok(), "native timeline package exports");
    if (!exported.ok()) { std::cerr << exported.message << '\n'; return 1; }
    expect(!exported.warnings.empty(), "timeline export exposes conversion limitations");
    const auto jsonBytes = provider::File::read(destination / "manifest.json");
    const auto parsed = ii::json::parse(jsonBytes);
    expect(bool(parsed), "independent iiJson parser accepts the package manifest");
    const auto *root = parsed ? &*parsed : nullptr;
    const auto *tracks = array(member(root, "tracks"));
    const auto *audioTracks = array(member(root, "audioTracks"));
    expect(string(member(root, "format")) == "iiSharedCanvas.timeline-interchange"
           && integer(member(root, "version")) == 2 && integer(member(root, "frameCount")) == 12
           && tracks && tracks->size() == 2 && audioTracks && audioTracks->size() == 1,
           "manifest preserves version, duration and independent tracks");
    if (tracks && tracks->size() == 2) {
        expect(string(member(&(*tracks)[0], "name")) == "바탕 & <red>"
               && boolean(member(&(*tracks)[0], "visible")), "manifest preserves Unicode names and visibility");
        const auto *clips = array(member(&(*tracks)[1], "clips"));
        expect(clips && clips->size() == 3 && integer(member(&(*clips)[0], "startFrame")) == 2
               && integer(member(&(*clips)[0], "durationFrames")) == 3
               && string(member(&(*clips)[0], "media")) == string(member(&(*clips)[2], "media")),
               "keyframe holds and repeated media references survive");
        if (clips && !clips->empty()) {
            BitmapImportOptions options; options.assetId = "media";
            const auto imported = importBitmap(utf8(destination / path(string(member(&(*clips)[0], "media")))), options);
            expect(imported.ok() && imported.asset.pixels.width == 4 && imported.asset.pixels.height == 4,
                   "timeline PNG media has full-canvas pixels");
            if (imported.ok()) {
                const auto &pixels = imported.asset.pixels.pixels;
                bool exact = pixels.size() == 16;
                for (std::size_t y = 0; exact && y < 4; ++y)
                    for (std::size_t x = 0; x < 4; ++x)
                        exact &= pixels[y * 4 + x] == (y < 2 && x >= 1 && x <= 2 ? 0xff00ff00U : 0U);
                expect(exact, "PNG media preserves every translated source pixel and transparent outside area");
            }
            if (clips->size() > 1) {
                const auto red = importBitmap(utf8(destination / path(string(member(&(*clips)[1], "media")))), options);
                bool exact = red.ok() && red.asset.pixels.pixels.size() == 16;
                for (std::size_t y = 0; exact && y < 4; ++y)
                    for (std::size_t x = 0; x < 4; ++x)
                        exact &= red.asset.pixels.pixels[y * 4 + x] == (x ? 0xffff0000U : 0U);
                expect(exact, "the second keyframe uses its own translated media pixels");
            }
        }
    }
    if (audioTracks && !audioTracks->empty()) {
        const auto *clips = array(member(&(*audioTracks)[0], "clips"));
        expect(clips && clips->size() == 1 && string(member(&(*clips)[0], "sourceOffsetSamples")) == "8009"
               && string(member(&(*clips)[0], "mediaOffsetSamples")) == "8008"
               && string(member(&(*clips)[0], "mediaTrimSamples")) == "1", "manifest preserves exact subframe audio trim");
        if (clips && !clips->empty()) {
            const auto wav = importAudioWav(utf8(destination / path(string(member(&(*clips)[0], "media")))));
            expect(wav.ok() && wav.asset.samples.size() + 1 == document.audioAssets[0].samples.size()
                   && std::equal(wav.asset.samples.begin(), wav.asset.samples.end(), document.audioAssets[0].samples.begin() + 1),
                   "WAV media preserves exact shifted PCM samples");
        }
    }
    expect(validXml(destination / "timeline.xml", "xmeml") && validXml(destination / "timeline.fcpxml", "fcpxml"),
           "both interchange XML files independently parse");
    const auto snapshot = provider::File::read(destination / "source.iisc");
    expect(std::string(original.bytes.begin(), original.bytes.end()) == snapshot
           && encodeIisc(document).bytes == original.bytes, "source snapshot and source document remain unchanged");
    expect(exportTimelineInterchange(document, utf8(destination)).code == MediaIoCode::AlreadyExists
           && provider::File::read(destination / "manifest.json") == jsonBytes, "existing package is never replaced");
    const auto trailing = temporary.path() / "trailing-separator";
    expect(exportTimelineInterchange(document, utf8(trailing) + "/").ok()
           && fs::exists(trailing / "manifest.json"), "a trailing directory separator is accepted");
    auto infinite = document;
    infinite.canvasMode = CanvasMode::Infinite; infinite.infiniteCanvas.origin = {-2, -3};
    const auto infinitePath = temporary.path() / "infinite";
    const auto infiniteResult = exportTimelineInterchange(infinite, utf8(infinitePath));
    expect(infiniteResult.ok(), "infinite canvas exports its fixed viewport");
    if (infiniteResult.ok()) {
        expect(std::any_of(infiniteResult.warnings.begin(), infiniteResult.warnings.end(), [](const auto &warning) {
            return warning.find("Infinite canvas") != std::string::npos;
        }), "infinite canvas clipping is reported");
        const auto infiniteJson = ii::json::parse(provider::File::read(infinitePath / "manifest.json"));
        const auto *canvas = infiniteJson ? member(&*infiniteJson, "canvas") : nullptr;
        expect(signedInteger(member(canvas, "originX")) == -2 && signedInteger(member(canvas, "originY")) == -3,
               "manifest retains the signed infinite-canvas origin");
    }
    const auto link = temporary.path() / "destination-link";
    std::error_code linkError;
    fs::create_directory_symlink(temporary.path() / "missing", link, linkError);
    if (!linkError) expect(exportTimelineInterchange(document, utf8(link)).code == MediaIoCode::AlreadyExists
                           && fs::is_symlink(link), "dangling destination symlink is never replaced");
    auto invalidText = document;
    layerProperties(invalidText.layers[0]).name = std::string("bad\x01name", 8);
    const auto badName = temporary.path() / "bad-name";
    expect(exportTimelineInterchange(invalidText, utf8(badName)).code == MediaIoCode::InvalidArgument
           && !fs::exists(badName), "invalid XML text is rejected before publication");
    auto invalidAssetId = document;
    std::get<RasterAsset>(invalidAssetId.assets[0]).id = std::string("bad\xff", 4);
    setLayerSource(invalidAssetId.layers[0], StaticSource{std::string("bad\xff", 4)});
    invalidAssetId.frames[1].keyframes[0].assetId = std::string("bad\xff", 4);
    const auto badId = temporary.path() / "bad-id";
    expect(exportTimelineInterchange(invalidAssetId, utf8(badId)).code == MediaIoCode::InvalidArgument
           && !fs::exists(badId), "invalid UTF-8 asset identifiers cannot corrupt the manifest");
    TimelineInterchangeOptions limited; limited.limits.maxOutputBytes = 200;
    const auto small = temporary.path() / "small";
    expect(exportTimelineInterchange(document, utf8(small), limited).code == MediaIoCode::LimitExceeded
           && !fs::exists(small), "output budget failure publishes nothing");
    limited = {}; limited.maxClips = 1;
    const auto clipsPath = temporary.path() / "clips";
    expect(exportTimelineInterchange(document, utf8(clipsPath), limited).code == MediaIoCode::LimitExceeded
           && !fs::exists(clipsPath), "clip budget failure publishes nothing");
    auto unsupported = document; layerProperties(unsupported.layers[0]).appearance.fillOpacity = 0.5;
    const auto appearance = temporary.path() / "appearance";
    expect(exportTimelineInterchange(unsupported, utf8(appearance)).code == MediaIoCode::UnsupportedFeature
           && !fs::exists(appearance), "unsupported appearance never silently loses data");
    const auto contested = temporary.path() / "contested";
    MediaIoResult first, second;
    std::thread a([&] { first = exportTimelineInterchange(document, utf8(contested)); });
    std::thread b([&] { second = exportTimelineInterchange(document, utf8(contested)); });
    a.join(); b.join();
    expect((first.ok() && second.code == MediaIoCode::AlreadyExists)
           || (second.ok() && first.code == MediaIoCode::AlreadyExists), "concurrent publication has exactly one winner");
    expect(fs::exists(contested / "source.iisc"), "winning package is complete");
    return failures ? 1 : 0;
}
#ifdef _WIN32
int wmain(int argc, wchar_t **argv)
{
    std::vector<std::string> arguments;
    for (int i = 1; i < argc; ++i) {
        const auto value = fs::path(argv[i]).u8string();
        arguments.emplace_back(value.begin(), value.end());
    }
    return run(arguments);
}
#else
int main(int argc, char **argv) { return run({argv + 1, argv + argc}); }
#endif
