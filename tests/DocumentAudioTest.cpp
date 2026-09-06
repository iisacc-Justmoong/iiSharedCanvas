#include <iiSharedCanvas.h>

#include <algorithm>
#include <iostream>
#include <limits>
#include <string>

namespace {
int failures = 0;
void expect(bool condition, const std::string &message)
{
    if (!condition) {
        std::cerr << message << '\n';
        ++failures;
    }
}
bool hasIssue(const iiSharedCanvas::Document &document, iiSharedCanvas::ValidationCode code)
{
    const auto result = iiSharedCanvas::validate(document);
    return std::any_of(result.issues.begin(), result.issues.end(), [code](const auto &issue) {
        return issue.code == code;
    });
}
iiSharedCanvas::Document audioDocument()
{
    using namespace iiSharedCanvas;
    Document document;
    document.extent = {8, 8};
    document.timeline = {{24, 1}, 96};
    document.audioAssets.push_back({"sound", 48000, 2, std::vector<std::int16_t>(192000, 123)});
    document.audioTracks.push_back({"dialogue", "Dialogue", false, -3.0,
        {{"clip-a", "Opening", "sound", 12, 24, 24000, -6.0, true},
         {"clip-b", "Ending", "sound", 48, 12, 0, 0.0, false}}});
    return document;
}
} // namespace

int main()
{
    using namespace iiSharedCanvas;
    const Document source = audioDocument();
    expect(validate(source).ok(), "a PCM audio track with gaps and source trim must validate");
    expect(findAudioAsset(source, "sound") == &source.audioAssets[0]
               && findAudioTrack(source, "dialogue") == &source.audioTracks[0]
               && findAudioClip(source.audioTracks[0], "clip-a") == &source.audioTracks[0].clips[0],
           "typed audio lookups must expose the authoritative aggregate");
    expect(audioSampleFrameCount(1, {30000, 1001}, 48000) == 1602
               && audioSampleFrameCount(30, {30000, 1001}, 48000) == 48048
               && !audioSampleFrameCount(1, {0, 1}, 48000)
               && !audioSampleFrameCount(std::numeric_limits<FrameIndex>::max(),
                    {1, std::numeric_limits<std::uint32_t>::max()}, 192000),
           "sample counts must round rational frame durations upward without overflow");

    auto invalid = source;
    invalid.formatVersion.minor = 3;
    expect(!validate(invalid).ok(), "audio fields require document format 1.4");
    invalid = source;
    invalid.audioAssets[0].channelCount = 3;
    expect(hasIssue(invalid, ValidationCode::InvalidAudioAsset), "unsupported channel layouts must fail");
    invalid = source;
    invalid.audioAssets[0].samples.pop_back();
    expect(hasIssue(invalid, ValidationCode::InvalidAudioAsset), "interleaved samples must form complete sample frames");
    invalid = source;
    invalid.audioTracks[0].clips[0].sourceOffsetSamples = 48001;
    expect(hasIssue(invalid, ValidationCode::InvalidAudioClip), "source trims must fit the full clip duration");
    invalid = source;
    invalid.audioTracks[0].clips[0].sourceOffsetSamples = std::numeric_limits<std::uint64_t>::max();
    expect(hasIssue(invalid, ValidationCode::InvalidAudioClip), "source offsets must not overflow range checks");
    invalid = source;
    invalid.audioTracks[0].clips[1].startFrame = 35;
    expect(hasIssue(invalid, ValidationCode::InvalidAudioClip), "clips in the same track may not overlap");
    invalid = source;
    invalid.audioTracks[0].clips[1].startFrame = 95;
    expect(hasIssue(invalid, ValidationCode::InvalidAudioClip), "clips must remain inside the document timeline");
    invalid = source;
    invalid.audioTracks[0].gainDb = std::numeric_limits<double>::quiet_NaN();
    expect(hasIssue(invalid, ValidationCode::InvalidAudioTrack), "track gain must be finite");
    invalid = source;
    invalid.audioTracks[0].clips[0].gainDb = 25;
    expect(hasIssue(invalid, ValidationCode::InvalidAudioClip), "clip gain must remain within its declared range");
    invalid = source;
    invalid.audioTracks.push_back({"music", "Music", false, 0.0,
        {{"clip-c", "Music", "sound", 12, 24, 0, 0.0, true}}});
    expect(validate(invalid).ok(), "different audio tracks may overlap");
    invalid.audioTracks.back().clips.front().id = "clip-a";
    expect(hasIssue(invalid, ValidationCode::DuplicateAudioClipId), "audio clip ids must be globally unique");
    invalid = source;
    invalid.assets.emplace_back(RasterAsset{"sound", makeRasterLayer(1, 1)});
    expect(hasIssue(invalid, ValidationCode::DuplicateAssetId), "visual and audio assets share a unique id namespace");
    invalid = source;
    invalid.assets.emplace_back(RasterAsset{"image", makeRasterLayer(1, 1)});
    invalid.layers.emplace_back(BitmapLayer{{"dialogue", "Image"}, StaticSource{"image"}});
    expect(hasIssue(invalid, ValidationCode::DuplicateLayerId), "visual and audio tracks share a unique id namespace");

    Document document;
    document.extent = {8, 8};
    document.timeline = source.timeline;
    document.formatVersion.minor = 3;
    DocumentEditor editor(document);
    expect(editor.insertAudioAsset(source.audioAssets.front()).changed
               && document.formatVersion.minor == CurrentFormatMinor,
           "inserting PCM audio must migrate the document format atomically");
    expect(editor.insertAudioTrack(source.audioTracks.front()).changed,
           "audio track insertion must be a validated structural edit");
    const auto revision = editor.revision();
    auto bad = source.audioTracks.front();
    bad.clips.front().durationFrames = 96;
    expect(!editor.replaceAudioTrack("dialogue", bad).ok()
               && editor.revision() == revision && document.audioTracks == source.audioTracks,
           "rejected audio edits must preserve the document and editor revision");
    expect(editor.replaceAudioTrack("dialogue", source.audioTracks.front()).ok()
               && !editor.lastResult().changed && editor.revision() == revision,
           "equal audio replacements must preserve revision");
    expect(editor.removeAudioAsset("sound").code == DocumentEditCode::AssetReferenced,
           "audio assets referenced by clips may not be deleted");
    expect(!editor.setFrameCount(59).ok() && document.timeline.frameCount == 96,
           "timeline shrink must respect audio clips");
    expect(!editor.setFrameRate({1, 1}).ok() && document.timeline.frameRate.numerator == 24,
           "frame rate edits must preserve available source samples");
    auto shortened = source.audioAssets.front();
    shortened.samples.resize(8);
    expect(!editor.replaceAudioAsset("sound", shortened).ok()
               && document.audioAssets == source.audioAssets,
           "replacing referenced PCM with insufficient samples must roll back");
    expect(editor.insertAudioTrack({"music", "Music"}).changed
               && editor.moveAudioTrack("music", 0).changed
               && document.audioTracks.front().id == "music",
           "audio track order must be editable");
    expect(editor.insertAudioClip("music", {"music-a", "Music", "sound", 0, 12}).changed,
           "audio clips must be insertable by track id");
    AudioClip changed = document.audioTracks.front().clips.front();
    changed.startFrame = 6;
    changed.sourceOffsetSamples = 100;
    changed.gainDb = -12;
    expect(editor.replaceAudioClip("music", "music-a", changed).changed
               && findAudioClip(*findAudioTrack(document, "music"), "music-a")->sourceOffsetSamples == 100,
           "clip replacement must support timing, source trim and gain");
    AudioTrackLayer muted = *findAudioTrack(document, "music");
    muted.muted = true;
    expect(editor.replaceAudioTrack("music", muted).changed
               && findAudioTrack(document, "music")->muted,
           "track replacement must support mute");
    expect(editor.removeAudioClip("music", "music-a").changed
               && editor.removeAudioTrack("music").changed
               && editor.removeAudioTrack("dialogue").changed
               && editor.removeAudioAsset("sound").changed && validate(document).ok(),
           "audio editing must support complete cleanup without invalid references");
    return failures == 0 ? 0 : 1;
}
