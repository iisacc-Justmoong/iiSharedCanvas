#include "Timeline/TimelineXmlWriter_p.hpp"

#include <QCoreApplication>
#include <QDir>
#include <QFile>
#include <QUrl>
#include <QXmlStreamReader>

#include <cmath>
#include <iostream>
#include <limits>
#include <string>
#include <vector>

namespace {
using namespace iiSharedCanvas;
using namespace iiSharedCanvas::timeline_detail;
int failures = 0;
void expect(bool value, const std::string &message)
{
    if (!value) { std::cerr << message << '\n'; ++failures; }
}

// This parser is independent of the writer: assertions inspect parsed names,
// attributes, nesting and text, never the writer's private serialization helpers.
struct Node {
    QString name;
    QString text;
    QXmlStreamAttributes attributes;
    std::vector<Node> children;
    const Node &one(const QString &wanted) const
    {
        for (const auto &child : children) { if (child.name == wanted) { return child; } }
        static const Node missing;
        return missing;
    }
    std::vector<const Node *> all(const QString &wanted) const
    {
        std::vector<const Node *> result;
        for (const auto &child : children) { if (child.name == wanted) { result.push_back(&child); } }
        return result;
    }
    QString attr(const QString &key) const { return attributes.value(key).toString(); }
};
Node element(QXmlStreamReader &reader)
{
    Node result{reader.name().toString(), {}, reader.attributes(), {}};
    while (!reader.atEnd()) {
        reader.readNext();
        if (reader.isStartElement()) { result.children.push_back(element(reader)); }
        else if (reader.isCharacters()) { result.text += reader.text(); }
        else if (reader.isEndElement()) { break; }
    }
    return result;
}
Node parse(const QByteArray &bytes)
{
    QXmlStreamReader reader(bytes);
    expect(reader.readNextStartElement(), "XML has a root element");
    auto result = element(reader);
    while (!reader.atEnd()) { reader.readNext(); }
    expect(!reader.hasError(), "XML independently parses: " + reader.errorString().toStdString());
    return result;
}
InterchangePlan fixture()
{
    InterchangePlan plan;
    plan.name = "Sequence <&> \"한글\"";
    plan.frameRate = {30000, 1001};
    plan.extent = {1920, 1080};
    plan.frameCount = 90;
    plan.media = {{QStringLiteral("media/한 글 &#+%.png")}, {QStringLiteral("media/second.png")},
                  {QStringLiteral("media/hidden.png")}, {QStringLiteral("media/top.png")}};
    plan.tracks = {
        {"base", "Base <&> \"한글\"", true, 1.0, RasterBlendMode::SourceOver,
         {{0, 30, 0, "a"}, {60, 30, 1, "b"}}},
        {"hidden", "Hidden", false, 0.25, RasterBlendMode::Multiply, {{15, 45, 2, "c"}}},
        {"screen", "Screen", true, 0.7, RasterBlendMode::Screen, {{0, 90, 3, "d"}}},
        {"overlay", "Overlay", true, 0.5, RasterBlendMode::Overlay, {{30, 30, 2, "c"}}}};
    return plan;
}
QString outputDirectory()
{
    return QDir(QString::fromUtf8(IISHAREDCANVAS_TEST_OUTPUT_DIR)).absoluteFilePath(
        QStringLiteral("Timeline XML 한 글 &#+%"));
}
void verifyLegacy(const TimelineXmlResult &encoded, const InterchangePlan &plan)
{
    const auto root = parse(encoded.legacyXml);
    expect(root.name == "xmeml" && root.attr("version") == "5", "legacy FCP7 xmeml version 5");
    const auto &sequence = root.one("sequence");
    expect(sequence.one("name").text == QString::fromUtf8(plan.name), "sequence name survives XML escaping");
    expect(sequence.one("duration").text == "90", "legacy duration is exactly 90 frames");
    expect(sequence.one("rate").one("timebase").text == "30"
           && sequence.one("rate").one("ntsc").text == "TRUE", "legacy rational NTSC timing is exact");
    expect(sequence.one("timecode").one("frame").text == "0"
           && sequence.one("timecode").one("displayformat").text == "NDF", "zero non-drop display origin");
    const auto &video = sequence.one("media").one("video");
    const auto &sample = video.one("format").one("samplecharacteristics");
    expect(sample.one("width").text == "1920" && sample.one("height").text == "1080"
           && sample.one("pixelaspectratio").text == "square"
           && sample.one("fielddominance").text == "none", "legacy full-canvas square progressive format");
    const auto tracks = video.all("track");
    expect(tracks.size() == plan.tracks.size(), "one legacy video track per native layer");
    if (tracks.size() != plan.tracks.size()) { return; }
    const std::vector<QString> blends{"normal", "multiply", "screen", "overlay"};
    for (std::size_t index = 0; index < tracks.size(); ++index) {
        const auto clips = tracks[index]->all("clipitem");
        expect(clips.size() == plan.tracks[index].clips.size(), "hold clips remain separate, without flattening");
        expect(tracks[index]->one("enabled").text == (plan.tracks[index].visible ? "TRUE" : "FALSE"),
               "legacy hidden track is retained and disabled");
        if (clips.empty()) { continue; }
        expect(clips[0]->one("name").text == QString::fromUtf8(plan.tracks[index].name),
               "bottom-to-top legacy order and layer name");
        expect(clips[0]->one("compositemode").text == blends[index], "legacy composite mode token");
        expect(clips[0]->one("alphatype").text == "straight"
               && clips[0]->one("stillframe").text == "TRUE", "PNG clips declare straight alpha and still frames");
        const auto &effect = clips[0]->one("filter").one("effect");
        expect(effect.one("effectid").text == "opacity"
               && effect.one("parameter").one("parameterid").text == "opacity"
               && std::abs(effect.one("parameter").one("value").text.toDouble()
                           - plan.tracks[index].opacity * 100.0) < 1e-10, "legacy opacity is an independent effect");
        for (std::size_t clipIndex = 0; clipIndex < clips.size(); ++clipIndex) {
            const auto &expected = plan.tracks[index].clips[clipIndex];
            const auto &clip = *clips[clipIndex];
            expect(clip.one("start").text.toUInt() == expected.start
                   && clip.one("end").text.toUInt() == expected.start + expected.duration
                   && clip.one("in").text == "0" && clip.one("out").text.toUInt() == expected.duration,
                   "legacy source range is a hold and timeline gaps remain exact");
            expect(clip.one("enabled").text == (plan.tracks[index].visible ? "TRUE" : "FALSE"),
                   "legacy hidden clips also explicitly disabled");
            const auto &file = clip.one("file");
            if (!file.one("pathurl").text.isEmpty()) {
                const auto expectedPath = QDir(outputDirectory()).filePath(plan.media[expected.mediaIndex].relativePath);
                expect(QUrl(file.one("pathurl").text).toLocalFile() == expectedPath,
                       "legacy media URL references final directory, not a staging directory");
                expect(file.one("media").one("video").one("alphatype").text == "straight",
                       "legacy source media also declares straight alpha");
            }
        }
    }
    const auto firstUrl = tracks[0]->one("clipitem").one("file").one("pathurl").text;
    expect(firstUrl.startsWith("file:///") && firstUrl.contains("%20") && firstUrl.contains("%23")
           && firstUrl.contains("%25"), "legacy file URL encodes spaces, hash, and percent");
}
void verifyFcpxml(const TimelineXmlResult &encoded, const InterchangePlan &plan)
{
    const auto root = parse(encoded.fcpxml);
    expect(root.name == "fcpxml" && root.attr("version") == "1.9", "FCPXML version 1.9");
    const auto &resources = root.one("resources");
    const auto formats = resources.all("format");
    expect(formats.size() == 2, "timeless still format is distinct from timeline format");
    if (formats.size() == 2) {
        expect(formats[0]->attr("frameDuration") == "1001/30000s", "FCPXML exact rational frame duration");
        expect(formats[0]->attr("width") == "1920" && formats[0]->attr("height") == "1080",
               "FCPXML full canvas dimensions");
        expect(formats[1]->attr("frameDuration").isEmpty(), "PNG source format has no artificial video frame rate");
    }
    const auto assets = resources.all("asset");
    expect(assets.size() == plan.media.size(), "FCPXML shared media resources are not duplicated per clip");
    for (std::size_t index = 0; index < assets.size() && index < plan.media.size(); ++index) {
        expect(assets[index]->attr("duration") == "0s" && assets[index]->attr("hasVideo") == "1"
               && assets[index]->attr("hasAudio") == "0", "PNG asset is timeless and video-only");
        expect(assets[index]->attr("colorSpaceOverride") == "sRGB IEC61966-2.1", "PNG source color space is explicit");
        expect(assets[index]->attr("src").isEmpty(), "FCPXML1.9 media-rep owns source URL");
        const auto &media = assets[index]->one("media-rep");
        expect(media.attr("kind") == "original-media"
               && QUrl(media.attr("src")).toLocalFile() == QDir(outputDirectory()).filePath(plan.media[index].relativePath),
               "FCPXML final-directory URL roundtrips non-ASCII and reserved characters");
    }
    const auto &project = root.one("project");
    expect(project.attr("name") == QString::fromUtf8(plan.name), "FCPXML escaped project name");
    const auto &sequence = project.one("sequence");
    expect(sequence.attr("duration") == "3003/1000s" && sequence.attr("tcStart") == "0s",
           "FCPXML exact timeline duration and zero origin");
    const auto &gap = sequence.one("spine").one("gap");
    expect(gap.attr("start") == "0s" && gap.attr("duration") == sequence.attr("duration"),
           "primary gap supplies exact timeline duration even across empty intervals");
    const auto clips = gap.all("clip");
    expect(clips.size() == 5 && gap.all("asset-clip").empty(),
           "stills use connected clip/video wrappers, not direct asset-clips");
    const std::vector<QString> lanes{"1", "1", "2", "3", "4"};
    const std::vector<QString> offsets{"0s", "1001/500s", "1001/2000s", "0s", "1001/1000s"};
    const std::vector<QString> durations{"1001/1000s", "1001/1000s", "3003/2000s", "3003/1000s", "1001/1000s"};
    const std::vector<QString> modes{"0", "0", "4", "10", "14"};
    for (std::size_t index = 0; index < clips.size() && index < lanes.size(); ++index) {
        const auto &clip = *clips[index];
        expect(clip.attr("lane") == lanes[index] && clip.attr("offset") == offsets[index]
               && clip.attr("duration") == durations[index], "FCPXML bottom-to-top lanes and hold timing are exact");
        expect(clip.attr("enabled") == (index == 2 ? "0" : "1"), "FCPXML hidden clip is retained but disabled");
        expect(clip.one("adjust-blend").attr("mode") == modes[index], "FCPXML uses documented numeric blend modes");
        const auto &video = clip.one("video");
        expect(video.attr("start") == "0s" && video.attr("duration") == durations[index], "timeless PNG video source is held");
        bool found = false;
        for (const auto *asset : assets) { found = found || asset->attr("id") == video.attr("ref"); }
        expect(found, "every FCPXML video references an existing asset resource");
    }
    if (clips.size() > 2) {
        expect(clips[2]->one("adjust-blend").attr("amount") == "0.25", "FCPXML opacity is not baked into PNG");
    }
}
void rejected(const InterchangePlan &plan, MediaIoCode code, const std::string &message,
              std::uint64_t limit = 1024 * 1024, const QString &directory = outputDirectory())
{
    const auto result = encodeTimelineXml(plan, directory, limit);
    expect(result.result.code == code && result.legacyXml.isEmpty() && result.fcpxml.isEmpty(), message);
}
void validationCases()
{
    auto plan = fixture();
    for (const auto [rate, nominal] : {
             std::pair{FrameRate{24, 1}, 24}, std::pair{FrameRate{48, 2}, 24},
             std::pair{FrameRate{60000, 2002}, 30}, std::pair{FrameRate{24000, 1001}, 24},
             std::pair{FrameRate{7000, 1001}, 7}, std::pair{FrameRate{1000, 143}, 7}}) {
        plan.frameRate = rate;
        const auto result = encodeTimelineXml(plan, outputDirectory(), 1024 * 1024);
        expect(result.result.ok(), "integer/NTSC-equivalent rates are reduced before export");
        const auto root = parse(result.legacyXml);
        expect(root.one("sequence").one("rate").one("timebase").text == QString::number(nominal),
               "reduced rational rate maps to exact legacy timebase");
    }
    plan = fixture(); plan.frameRate = {25, 2};
    rejected(plan, MediaIoCode::UnsupportedFeature, "noninteger/non-NTSC rate fails closed, not rounded");
    plan.frameRate = {2997, 100};
    rejected(plan, MediaIoCode::UnsupportedFeature, "29.97 decimal approximation is not silently treated as 30000/1001");
    plan.frameRate = {0, 1}; rejected(plan, MediaIoCode::InvalidArgument, "zero numerator fails");
    plan.frameRate = {24, 0}; rejected(plan, MediaIoCode::InvalidArgument, "zero denominator fails");
    plan = fixture(); plan.frameCount = 0; rejected(plan, MediaIoCode::InvalidArgument, "zero duration fails");
    plan = fixture(); plan.tracks[0].clips[0].duration = 0;
    rejected(plan, MediaIoCode::InvalidArgument, "zero hold duration fails");
    plan = fixture(); plan.tracks[0].clips[1].start = 29;
    rejected(plan, MediaIoCode::InvalidArgument, "overlapping holds in one track fail");
    plan = fixture(); plan.tracks[0].clips[1].start = 80;
    rejected(plan, MediaIoCode::InvalidArgument, "out-of-range hold fails");
    plan = fixture(); plan.tracks[0].clips[0].mediaIndex = 99;
    rejected(plan, MediaIoCode::InvalidArgument, "missing media reference fails");
    plan = fixture(); plan.tracks[0].opacity = std::numeric_limits<double>::quiet_NaN();
    rejected(plan, MediaIoCode::InvalidArgument, "nonfinite opacity fails");
    plan = fixture(); plan.tracks[0].blendMode = RasterBlendMode::DestinationOut;
    rejected(plan, MediaIoCode::UnsupportedFeature, "unsupported blend fails");
    plan = fixture(); plan.name = std::string("bad\0name", 8);
    rejected(plan, MediaIoCode::InvalidArgument, "XML-illegal NUL name fails");
    plan = fixture(); plan.tracks[0].name = std::string("\xc0\xaf", 2);
    rejected(plan, MediaIoCode::InvalidArgument, "noncanonical UTF8 name fails");
    plan = fixture(); plan.name = "bad\x01";
    rejected(plan, MediaIoCode::InvalidArgument, "XML-illegal control character fails");
    plan = fixture(); plan.media[0].relativePath = "../escape.png";
    rejected(plan, MediaIoCode::InvalidArgument, "media cannot escape final output directory");
    plan = fixture(); plan.media[0].relativePath = "/absolute.png";
    rejected(plan, MediaIoCode::InvalidArgument, "media path must be relative");
    rejected(fixture(), MediaIoCode::InvalidArgument, "output directory must be absolute", 1024 * 1024, "relative");
    rejected(fixture(), MediaIoCode::LimitExceeded, "bounded XML output fails with no partial results", 64);
    const auto full = encodeTimelineXml(fixture(), outputDirectory(), 1024 * 1024);
    const auto exactSize = std::uint64_t(full.legacyXml.size() + full.fcpxml.size());
    expect(encodeTimelineXml(fixture(), outputDirectory(), exactSize).result.ok(), "exact combined XML budget succeeds");
    rejected(fixture(), MediaIoCode::LimitExceeded, "both XML byte arrays share one aggregate budget", exactSize - 1);
    plan = fixture(); plan.frameRate = {24, 1}; plan.frameCount = 2700;
    const auto longStill = parse(encodeTimelineXml(plan, outputDirectory(), 1024 * 1024).legacyXml);
    const auto &source = longStill.one("sequence").one("media").one("video").one("track").one("clipitem").one("file");
    expect(source.one("duration").text == "2" && source.one("media").one("video").one("duration").text == "2",
           "legacy still source duration uses whole minutes covering the timeline");
    plan = fixture(); plan.tracks.clear(); plan.media.clear();
    const auto empty = encodeTimelineXml(plan, outputDirectory(), 1024 * 1024);
    expect(empty.result.ok(), "empty native timeline remains a duration-preserving gap");
    expect(parse(empty.fcpxml).one("project").one("sequence").one("spine").one("gap").attr("duration") == "3003/1000s",
           "empty FCPXML still carries full duration");
}
} // namespace

int main(int argc, char **argv)
{
    QCoreApplication application(argc, argv);
    const auto plan = fixture();
    const auto encoded = encodeTimelineXml(plan, outputDirectory(), 1024 * 1024);
    expect(encoded.result.ok(), "timeline XML encoding succeeds: " + encoded.result.message);
    if (encoded.result.ok()) { verifyLegacy(encoded, plan); verifyFcpxml(encoded, plan); }
    validationCases();
    if (failures == 0) {
        QDir().mkpath(QString::fromUtf8(IISHAREDCANVAS_TEST_OUTPUT_DIR));
        for (const auto &entry : {std::pair{"timeline-writer.xml", encoded.legacyXml},
                                  std::pair{"timeline-writer.fcpxml", encoded.fcpxml}}) {
            QFile file(QDir(QString::fromUtf8(IISHAREDCANVAS_TEST_OUTPUT_DIR)).filePath(QString::fromUtf8(entry.first)));
            expect(file.open(QIODevice::WriteOnly) && file.write(entry.second) == entry.second.size(), "write independent XML validation fixture");
        }
    }
    return failures == 0 ? 0 : 1;
}
