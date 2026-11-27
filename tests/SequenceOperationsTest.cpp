#include <iiSharedCanvas>
#include <iiFileProviderNative.h>

#include <cstdint>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <memory>
#include <vector>

using namespace iiSharedCanvas;

namespace {
int failures = 0;
void expect(bool condition, const char *message)
{
    if (!condition) { std::cerr << "FAIL: " << message << '\n'; ++failures; }
}

TimelineProject project()
{
    TimelineVideoStream stream;
    stream.id = "video";
    stream.timeBase = {1, 24};
    stream.durationTicks = 24;
    stream.codec.identifier = "test-video";
    stream.codedExtent = {4, 4};
    stream.displayExtent = {4, 4};
    stream.pixelFormat = "rgba";
    stream.bitDepth = 8;
    stream.timing.nominal = TimelineFrameRate{24, 1};

    TimelineMediaRepresentation representation;
    representation.id = "original";
    representation.role = TimelineMediaRepresentationRole::Original;
    representation.ownedMediaId = "owned-video";
    representation.container.identifier = "test-container";
    representation.timeBase = {1, 24};
    representation.durationTicks = 24;
    representation.streams.emplace_back(stream);

    TimelineMediaSource source;
    source.id = "source";
    source.originalRepresentationId = "original";
    source.activeRepresentationId = "original";
    source.representations.push_back(representation);

    TimelineVideoClip clip;
    clip.properties.id = "clip";
    clip.properties.source = TimelineMediaReference{"source", "video"};
    clip.properties.timelineRange = {0, 24};
    clip.properties.sourceRange = {0, 24};
    TimelineVideoTrack track;
    track.properties.id = "track";
    track.clips.push_back(clip);

    TimelineSequence sequence;
    sequence.id = "sequence";
    sequence.timeBase = {1, 24};
    sequence.editingFrameRate = {24, 1};
    sequence.durationTicks = 24;
    sequence.canvasExtent = {4, 4};
    sequence.tracks.emplace_back(track);

    TimelineProject result;
    result.id = "project";
    result.activeSequenceId = "sequence";
    result.mediaSources.push_back(source);
    result.sequences.push_back(sequence);
    return result;
}

TimelineOwnedMedia ownedMedia()
{
    return {"owned-video", "video/test", std::make_shared<const std::vector<std::uint8_t>>(
        std::initializer_list<std::uint8_t>{0, 1, 2, 253, 254, 255})};
}

Document document()
{
    Document value;
    value.extent = {4, 4};
    return value;
}

void documentOwnershipAndHistory()
{
    auto canvas = document();
    DocumentHistory history(canvas);
    const auto original = project();
    const auto validation = validateTimelineProject(original);
    for (const auto &issue : validation.issues) std::cerr << issue.path << ": " << issue.message << '\n';
    const auto first = history.setSequenceProject(original, {ownedMedia()});
    if (!first.ok()) std::cerr << "sequence history: " << first.message() << '\n';
    expect(first.changed,
           "sequence project and its owned media publish as one edit");
    expect(canvas.sequenceProject && *canvas.sequenceProject == original &&
           canvas.sequenceMedia.size() == 1 && *canvas.sequenceMedia[0].bytes ==
               std::vector<std::uint8_t>({0, 1, 2, 253, 254, 255}),
           "the document owns the sequence and exact original bytes");
    const auto encoded = encodeIisc(canvas);
    const auto decoded = encoded.ok() ? decodeIisc(encoded.bytes) : IiscDecodeResult{};
    expect(encoded.ok() && decoded.ok() && decoded.document.sequenceProject &&
           *decoded.document.sequenceProject == original && decoded.document.sequenceMedia.size() == 1 &&
           *decoded.document.sequenceMedia[0].bytes == *canvas.sequenceMedia[0].bytes,
           "portable IISC round-trip preserves the complete sequence and original bytes");
    expect(history.undo().changed && !canvas.sequenceProject && canvas.sequenceMedia.empty() &&
           history.redo().changed && canvas.sequenceProject && *canvas.sequenceProject == original,
           "one undo and redo removes and restores the sequence and owned media");

    auto missing = original;
    missing.mediaSources[0].representations[0].ownedMediaId = "missing";
    const auto before = canvas.authorship.revision();
    expect(!history.setSequenceProject(missing, {ownedMedia()}).ok() &&
           canvas.authorship.revision() == before && canvas.sequenceProject && *canvas.sequenceProject == original,
           "missing owned media are rejected atomically");
    auto external = original;
    external.mediaSources[0].representations[0].uri = "file:///outside.mov";
    expect(!history.setSequenceProject(external, {ownedMedia()}).ok() &&
           canvas.sequenceProject && *canvas.sequenceProject == original,
           "a document-owned sequence cannot retain an external source path");
    auto wrongSize = original;
    wrongSize.mediaSources[0].representations[0].fileSizeBytes = 7;
    expect(!history.setSequenceProject(wrongSize, {ownedMedia()}).ok() &&
           canvas.sequenceProject && *canvas.sequenceProject == original,
           "declared source size must match the bytes owned by the document");
}

void workingFileRoundTrip()
{
    std::filesystem::create_directories(IISHAREDCANVAS_TEST_OUTPUT_DIR);
    iiFileProvider::native::TemporaryDirectory temporary{std::filesystem::path(IISHAREDCANVAS_TEST_OUTPUT_DIR)};
    expect(temporary.isValid(), "working-file sequence test directory");
    if (!temporary.isValid()) return;
    const auto path = (temporary.path() / "sequence.iisc").string();
    DocumentFile file;
    if (!file.create(path, document()).ok()) { expect(false, "create sequence working file"); return; }
    {
        DocumentHistory history(file);
        expect(history.setSequenceProject(project(), {ownedMedia()}).changed &&
               history.undo().changed && history.redo().changed,
               "working-file sequence edits and undo/redo are committed atomically");
    }
    file.close();
    Document reopened;
    expect(DocumentFile::readSnapshot(path, reopened).ok() && reopened.sequenceProject &&
           *reopened.sequenceProject == project() && reopened.sequenceMedia.size() == 1 &&
           *reopened.sequenceMedia[0].bytes == *ownedMedia().bytes,
           "working-file reopen preserves the project and its original media bytes");
}

void windowPersistence()
{
    auto original=project();
    auto &clip=std::get<TimelineVideoTrack>(original.sequences[0].tracks[0]).clips[0];
    clip.properties.loopMode=TimelineLoopMode::Repeat;
    clip.properties.sourceRange.duration=8;
    TimelineEffect effect; effect.id="animated"; effect.pluginId="test";
    effect.activeRange=TimelineTickRange{2,18}; clip.properties.effects.push_back(effect);
    auto canvas=document(); canvas.formatVersion.minor=28; canvas.sequenceProject=original; canvas.sequenceMedia={ownedMedia()};
    const auto legacy=encodeIisc(canvas);
    expect(legacy.ok() && decodeIisc(legacy.bytes).ok(),"a 1.28 project without windows remains readable");
    auto cut=TimelineSplitPlan{}(original,"sequence",{{"clip","right"}},{},5);
    expect(cut.ok(),"looping clip can be split at an arbitrary content position");
    if(!cut.ok())return;
    DocumentHistory history(canvas);
    expect(history.setSequenceProject(*cut.project,{ownedMedia()}).changed && canvas.formatVersion.minor==29,
           "publishing a windowed edit upgrades the document and creates one history entry");
    const auto encoded=encodeIisc(canvas);
    const auto decoded=encoded.ok()?decodeIisc(encoded.bytes):IiscDecodeResult{};
    expect(encoded.ok() && decoded.ok() && decoded.document.sequenceProject && *decoded.document.sequenceProject==*cut.project,
           "the content window and original automation survive the document round trip");
    auto downgraded=canvas;downgraded.formatVersion.minor=28;
    expect(!validate(downgraded).ok(),"a windowed project cannot be silently written as format 1.28");
    expect(history.undo().changed && canvas.sequenceProject && *canvas.sequenceProject==original &&
           history.redo().changed && canvas.sequenceProject && *canvas.sequenceProject==*cut.project,
           "windowed edits undo and redo as one sequence history entry");
    auto trackWindow=document();trackWindow.formatVersion.minor=28;trackWindow.sequenceProject=project();
    trackWindow.sequenceMedia={ownedMedia()};
    auto &properties=std::get<TimelineVideoTrack>(trackWindow.sequenceProject->sequences[0].tracks[0]).properties;
    properties.effectTimeMap=TimelineTrackEffectTimeMap{24,{{{0,12},0},{{12,8},16}}};
    expect(!validate(trackWindow).ok(),"a track-effect time map cannot be silently stored in version 1.28");
    trackWindow.formatVersion.minor=29;
    const auto mappedBytes=encodeIisc(trackWindow);
    const auto mapped=mappedBytes.ok()?decodeIisc(mappedBytes.bytes):IiscDecodeResult{};
    expect(mappedBytes.ok() && mapped.ok() && mapped.document.sequenceProject==trackWindow.sequenceProject,
           "track effect time mappings survive IISC 1.29 persistence");
}
} // namespace

int main(int argc, char **argv)
{
    if (argc == 3 && std::string_view(argv[1]) == "--fixture") {
        auto value = document();
        value.sequenceProject = project();
        value.sequenceMedia = {ownedMedia()};
        auto bytes = encodeIisc(value);
        if (!bytes.ok()) return 1;
        std::ofstream output(argv[2], std::ios::binary);
        output.write(reinterpret_cast<const char *>(bytes.bytes.data()), static_cast<std::streamsize>(bytes.bytes.size()));
        return output ? 0 : 1;
    }
    documentOwnershipAndHistory();
    workingFileRoundTrip();
    windowPersistence();
    return failures ? 1 : 0;
}
