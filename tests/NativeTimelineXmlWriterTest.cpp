#include "Native/TimelineXmlWriter_p.hpp"

#include <libxml/parser.h>

#include <filesystem>
#include <iostream>
#include <limits>
#include <memory>
#include <string>
#include <string_view>

namespace {
using namespace iiSharedCanvas;
using namespace iiSharedCanvas::timeline_detail;
int failures = 0;
void expect(bool value, const char *message)
{
    if (!value) { std::cerr << message << '\n'; ++failures; }
}
using XmlDocument = std::unique_ptr<xmlDoc, decltype(&xmlFreeDoc)>;
XmlDocument parse(const std::string &bytes)
{
    return {xmlReadMemory(bytes.data(), int(bytes.size()), "timeline.xml", "UTF-8", XML_PARSE_NONET), xmlFreeDoc};
}
xmlNode *child(xmlNode *parent, std::string_view name)
{
    if (!parent) return nullptr;
    for (auto *node = parent->children; node; node = node->next)
        if (node->type == XML_ELEMENT_NODE && name == reinterpret_cast<const char *>(node->name)) return node;
    return nullptr;
}
std::size_t count(xmlNode *parent, std::string_view name)
{
    std::size_t result = 0;
    if (parent) for (auto *node = parent->children; node; node = node->next)
        if (node->type == XML_ELEMENT_NODE && name == reinterpret_cast<const char *>(node->name)) ++result;
    return result;
}
std::string text(xmlNode *node)
{
    if (!node) return {};
    auto *raw = xmlNodeGetContent(node); if (!raw) return {};
    std::string value(reinterpret_cast<const char *>(raw)); xmlFree(raw); return value;
}
std::string attr(xmlNode *node, const char *name)
{
    if (!node) return {};
    auto *raw = xmlGetProp(node, reinterpret_cast<const xmlChar *>(name)); if (!raw) return {};
    std::string value(reinterpret_cast<const char *>(raw)); xmlFree(raw); return value;
}
InterchangePlan fixture()
{
    InterchangePlan plan;
    plan.name = "Sequence <&> \"한글\"\n\t"; plan.extent = {1920, 1080};
    plan.frameRate = {30000, 1001}; plan.frameCount = 90;
    plan.media = {{"media/한 글 &#+%.png"}, {"media/other.png"}};
    plan.tracks = {{"base", "Base <&>", true, 1.0, RasterBlendMode::SourceOver,
                    {{0, 30, 0, "a"}, {60, 30, 1, "b"}}},
                   {"hidden", "Hidden", false, 0.25, RasterBlendMode::Multiply,
                    {{15, 45, 0, "a"}}}};
    plan.audioMedia = {{"media/음악 & stereo.wav", 48000, 2, 240240}};
    plan.audioTracks = {{"music", "Music <&>", false, -3.0,
                         {{5, 30, 0, "clip", "Intro", "sound", 48048, -3.0, true}}}};
    return plan;
}
std::string outputDirectory()
{
    const auto path = std::filesystem::absolute(std::filesystem::path(IISHAREDCANVAS_TEST_OUTPUT_DIR))
        / std::filesystem::path(u8"timeline 한 글 &#+%");
    const auto value = path.u8string(); return {value.begin(), value.end()};
}
}

int main()
{
    auto plan = fixture();
    const auto directory = outputDirectory();
    const auto result = encodeTimelineXml(plan, directory, 1024 * 1024);
    expect(result.result.ok() && !result.legacyXml.empty() && !result.fcpxml.empty(),
           "native timeline XML generates both exchange formats");
    if (!result.result.ok()) return 1;
    auto legacy = parse(result.legacyXml); auto fcpxml = parse(result.fcpxml);
    expect(bool(legacy) && bool(fcpxml), "independent XML parser accepts both generated documents");
    if (!legacy || !fcpxml) return 1;
    auto *root = xmlDocGetRootElement(legacy.get()); auto *sequence = child(root, "sequence");
    expect(root && std::string_view(reinterpret_cast<const char *>(root->name)) == "xmeml"
           && attr(root, "version") == "5" && text(child(sequence, "name")) == plan.name,
           "legacy sequence and escaped Unicode name survive");
    auto *rate = child(sequence, "rate");
    expect(text(child(rate, "timebase")) == "30" && text(child(rate, "ntsc")) == "TRUE",
           "legacy NTSC timebase remains exact");
    auto *media = child(sequence, "media"); auto *video = child(media, "video");
    expect(count(video, "track") == 2, "visual layer count survives legacy XML");
    auto *firstClip = child(child(video, "track"), "clipitem");
    auto *firstFile = child(firstClip, "file"); const auto url = text(child(firstFile, "pathurl"));
    expect(text(child(firstClip, "start")) == "0" && text(child(firstClip, "out")) == "30"
           && url.starts_with("file:") && url.find("%20") != url.npos
           && url.find("%23") != url.npos && url.find("%25") != url.npos,
           "legacy hold timings and escaped final media URL survive");
    auto *audio = child(media, "audio"); auto *audioTrack = child(audio, "track");
    auto *audioClip = child(audioTrack, "clipitem");
    expect(count(audio, "track") == 2 && text(child(audioClip, "start")) == "5"
           && text(child(audioClip, "in")) == "30" && text(child(audioClip, "out")) == "60",
           "stereo channels and sample-aligned trim survive legacy XML");
    root = xmlDocGetRootElement(fcpxml.get()); auto *resources = child(root, "resources");
    auto *format = child(resources, "format"); auto *asset = child(resources, "asset");
    auto *project = child(root, "project"); sequence = child(project, "sequence");
    auto *gap = child(child(sequence, "spine"), "gap"); auto *clip = child(gap, "clip");
    expect(root && std::string_view(reinterpret_cast<const char *>(root->name)) == "fcpxml"
           && attr(root, "version") == "1.9" && attr(project, "name") == plan.name
           && attr(format, "frameDuration") == "1001/30000s"
           && attr(sequence, "duration") == "3003/1000s",
           "FCPXML project and rational timing survive");
    expect(count(resources, "asset") == 3 && count(gap, "clip") == 3
           && count(gap, "asset-clip") == 1 && attr(clip, "duration") == "1001/1000s"
           && attr(child(asset, "media-rep"), "src").starts_with("file:"),
           "FCPXML shares media resources and preserves visual and audio clips");

    const auto size = result.legacyXml.size() + result.fcpxml.size();
    const auto exact = encodeTimelineXml(plan, directory, size);
    const auto small = encodeTimelineXml(plan, directory, size - 1);
    expect(exact.result.ok() && small.result.code == MediaIoCode::LimitExceeded
           && small.legacyXml.empty() && small.fcpxml.empty(), "combined XML output budget is exact and atomic");
    auto invalid = plan; invalid.media[0].relativePath = "../escape.png";
    expect(encodeTimelineXml(invalid, directory, size * 2).result.code == MediaIoCode::InvalidArgument,
           "unsafe media paths are rejected");
    invalid = plan; invalid.name = std::string("bad\x01", 4);
    expect(encodeTimelineXml(invalid, directory, size * 2).result.code == MediaIoCode::InvalidArgument,
           "illegal XML characters are rejected");
    invalid = plan; invalid.name = std::string("\xc0\xaf", 2);
    expect(encodeTimelineXml(invalid, directory, size * 2).result.code == MediaIoCode::InvalidArgument,
           "noncanonical UTF-8 is rejected");
    invalid = plan; invalid.tracks[0].clips[0].mediaIndex = 20;
    expect(encodeTimelineXml(invalid, directory, size * 2).result.code == MediaIoCode::InvalidArgument,
           "invalid media references are rejected before encoding");
    invalid = plan; invalid.media[0].relativePath = "media//image.png";
    expect(encodeTimelineXml(invalid, directory, size * 2).result.code == MediaIoCode::InvalidArgument,
           "empty path components are rejected");
    invalid = plan; invalid.audioTracks[0].clips[0].sourceOffsetSamples = 1;
    expect(encodeTimelineXml(invalid, directory, size * 2).result.code == MediaIoCode::UnsupportedFeature,
           "non-frame-aligned legacy audio trim is rejected");
    invalid = plan; invalid.frameRate = {24, 7};
    expect(encodeTimelineXml(invalid, directory, size * 2).result.code == MediaIoCode::UnsupportedFeature,
           "unsupported legacy frame rates are rejected");
    invalid = plan; invalid.tracks[0].opacity = std::numeric_limits<double>::quiet_NaN();
    expect(encodeTimelineXml(invalid, directory, size * 2).result.code == MediaIoCode::InvalidArgument,
           "non-finite opacity is rejected");
    auto reduced = plan; reduced.frameRate = {7000, 1001};
    reduced.audioTracks.clear(); reduced.audioMedia.clear();
    const auto reducedResult = encodeTimelineXml(reduced, directory, size * 2);
    auto reducedXml = parse(reducedResult.legacyXml);
    auto *reducedSequence = reducedXml ? child(xmlDocGetRootElement(reducedXml.get()), "sequence") : nullptr;
    auto *reducedRate = child(reducedSequence, "rate");
    expect(reducedResult.result.ok() && text(child(reducedRate, "timebase")) == "7"
           && text(child(reducedRate, "ntsc")) == "TRUE", "reduced NTSC rates retain their correct legacy timebase");
    return failures ? 1 : 0;
}
