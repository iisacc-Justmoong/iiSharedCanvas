// Reuse the existing mixed-stream validation fixture verbatim so this test
// exercises more than the minimum fields in a persisted timeline project.
#define main timelineProjectStandaloneTestMain
#include "TimelineProjectTest.cpp"
#undef main

#include <iiSharedCanvas>
#include <fstream>
#include <memory>

int main(int argc, char **argv)
{
    using namespace iiSharedCanvas;
    auto project = makeProject();
    std::vector<TimelineOwnedMedia> owned;
    auto &representations = project.mediaSources[0].representations;
    for (std::size_t index = 0; index < representations.size(); ++index) {
        auto &representation = representations[index];
        representation.uri.clear();
        representation.ownedMediaId = "media-" + std::to_string(index);
        representation.fileSizeBytes = 5;
        for (auto &attachment : representation.attachments) {
            attachment.uri.clear();
            attachment.data = {0x89, 0x50, 0x4e, 0x47};
        }
        owned.push_back({representation.ownedMediaId, representation.container.mimeType,
            std::make_shared<const std::vector<std::uint8_t>>(std::initializer_list<std::uint8_t>{
                0, static_cast<std::uint8_t>(index), 127, 128, 255})});
    }
    TimelineSequence nested;
    nested.id = "nested";
    nested.timeBase = {1, 60'000};
    nested.durationTicks = 60'000;
    nested.canvasExtent = {1920, 1080};
    project.sequences.push_back(nested);
    TimelineVideoClip child;
    child.properties.id = "child-sequence";
    child.properties.source = TimelineSequenceReference{"nested", TimelineStreamKind::Video};
    child.properties.timelineRange = {0, 60'000};
    child.properties.sourceRange = {0, 60'000};
    TimelineVideoTrack nestedTrack;
    nestedTrack.properties.id = "nested-track";
    nestedTrack.clips.push_back(child);
    project.sequences[0].tracks.emplace_back(nestedTrack);

    expect(validateTimelineProject(project).ok(), "rich owned project remains valid");
    Document document;
    document.extent = {16, 16};
    DocumentHistory history(document);
    auto result = history.setSequenceProject(project, owned);
    if (!result.ok()) std::cerr << "rich history: " << result.message() << '\n';
    expect(result.changed, "rich project publishes through document history");
    auto encoded = encodeIisc(document);
    if (argc==3 && std::string_view(argv[1])=="--fixture") {
        if (!encoded.ok()) return 1;
        std::ofstream output(argv[2],std::ios::binary);
        output.write(reinterpret_cast<const char *>(encoded.bytes.data()),static_cast<std::streamsize>(encoded.bytes.size()));
        return output ? 0 : 1;
    }
    auto decoded = encoded.ok() ? decodeIisc(encoded.bytes) : IiscDecodeResult{};
    expect(encoded.ok() && decoded.ok() && decoded.document.sequenceProject &&
           *decoded.document.sequenceProject == project && decoded.document.sequenceMedia == owned,
           "all nested, timed, typed stream and render settings survive IISC serialization");

    SerializationLimits limited;
    limited.maximumTimelineProjectBytes = 16;
    expect(!encodeIisc(document, limited).ok(), "sequence metadata serialization respects byte limits");
    limited = {};
    limited.maximumTimelineMediaBytes = 4;
    expect(!encodeIisc(document, limited).ok(), "owned media serialization respects aggregate byte limits");
    limited = {};
    limited.maximumStringBytes = 16;
    expect(!encodeIisc(document, limited).ok() && !decodeIisc(encoded.bytes, limited).ok(),
           "timeline text respects the document's per-string budget on encode and decode");

    Document empty;
    empty.extent = {16, 16};
    const auto edit = encodeIiscEdit(empty, document);
    limited = {};
    limited.maximumTimelineMediaBytes = 8;
    const auto rejected = applyIiscEdit(empty, edit.bytes, limited);
    expect(edit.ok() && rejected.code == IiscErrorCode::LimitExceeded && !empty.sequenceProject && empty.sequenceMedia.empty(),
           "a multi-media edit exceeding the aggregate budget is rejected atomically");

    auto mutableBytes = std::make_shared<std::vector<std::uint8_t>>(std::initializer_list<std::uint8_t>{1, 2, 3});
    auto modified = project;
    auto newMedia = owned;
    newMedia[0].bytes = mutableBytes;
    modified.mediaSources[0].representations[0].fileSizeBytes = mutableBytes->size();
    expect(history.setSequenceProject(modified, newMedia).changed, "changed owned bytes publish as one edit");
    (*mutableBytes)[0] = 99;
    expect(document.sequenceMedia[0].bytes && (*document.sequenceMedia[0].bytes)[0] == 1,
           "later caller mutation cannot change document-owned source bytes");
    expect(history.undo().changed && document.sequenceMedia == owned && history.redo().changed &&
           (*document.sequenceMedia[0].bytes)[0] == 1,
           "undo and redo restore detached media values");

    auto largeProject = project;
    auto largeMedia = owned;
    largeMedia[0].bytes = std::make_shared<const std::vector<std::uint8_t>>(256 * 1024, 0x42);
    largeProject.mediaSources[0].representations[0].fileSizeBytes = largeMedia[0].bytes->size();
    Document alreadyOwned;
    alreadyOwned.extent = {16, 16};
    alreadyOwned.sequenceProject = largeProject;
    alreadyOwned.sequenceMedia = largeMedia;
    DocumentHistoryLimits smallBudget;
    smallBudget.maximumBytes = 128 * 1024;
    DocumentHistory metadataHistory(alreadyOwned, smallBudget);
    largeProject.name = "metadata-only change";
    expect(metadataHistory.setSequenceProject(largeProject, largeMedia).changed && metadataHistory.undo().changed,
           "metadata-only history does not charge unchanged owned video bytes to every entry");
    return failures ? 1 : 0;
}
