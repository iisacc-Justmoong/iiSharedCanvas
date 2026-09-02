#include "Timeline/TimelineEditor.h"

#include <algorithm>
#include <optional>
#include <utility>

namespace iiSharedCanvas {
namespace {

std::size_t insertionIndex(std::size_t requested, std::size_t size) noexcept
{
    return requested == AppendTimelineIndex ? size : requested;
}

template<typename Value>
void moveElement(std::vector<Value> &values,
                 std::size_t sourceIndex,
                 std::size_t destinationIndex)
{
    if (sourceIndex < destinationIndex) {
        std::rotate(values.begin() + static_cast<std::ptrdiff_t>(sourceIndex),
                    values.begin() + static_cast<std::ptrdiff_t>(sourceIndex + 1),
                    values.begin() + static_cast<std::ptrdiff_t>(destinationIndex + 1));
    } else if (sourceIndex > destinationIndex) {
        std::rotate(values.begin() + static_cast<std::ptrdiff_t>(destinationIndex),
                    values.begin() + static_cast<std::ptrdiff_t>(sourceIndex),
                    values.begin() + static_cast<std::ptrdiff_t>(sourceIndex + 1));
    }
}

template<typename Value, typename Identifier>
std::optional<std::size_t> indexById(const std::vector<Value> &values,
                                     const std::string &id,
                                     Identifier identifier)
{
    const auto found = std::find_if(values.begin(), values.end(),
                                    [&id, identifier](const Value &value) {
                                        return identifier(value) == id;
                                    });
    return found == values.end()
        ? std::nullopt
        : std::optional<std::size_t>{static_cast<std::size_t>(
              std::distance(values.begin(), found))};
}

std::optional<std::size_t> mediaSourceIndex(const TimelineProject &project,
                                            const std::string &id)
{
    return indexById(project.mediaSources, id,
                     [](const TimelineMediaSource &value) -> const std::string & {
                         return value.id;
                     });
}

std::optional<std::size_t> sequenceIndex(const TimelineProject &project,
                                         const std::string &id)
{
    return indexById(project.sequences, id,
                     [](const TimelineSequence &value) -> const std::string & {
                         return value.id;
                     });
}

std::optional<std::size_t> profileIndex(const TimelineProject &project,
                                        const std::string &id)
{
    return indexById(project.renderProfiles, id,
                     [](const TimelineRenderProfile &value) -> const std::string & {
                         return value.id;
                     });
}

std::optional<std::size_t> trackIndex(const TimelineSequence &sequence,
                                      const std::string &id)
{
    return indexById(sequence.tracks, id,
                     [](const TimelineTrack &value) -> const std::string & {
                         return timelineTrackProperties(value).id;
                     });
}

template<typename Track, typename Clip>
bool insertTypedClip(Track &track,
                     TimelineClip clip,
                     std::size_t requested,
                     bool &indexValid)
{
    Clip *typed = std::get_if<Clip>(&clip);
    if (!typed) {
        return false;
    }
    const std::size_t position = insertionIndex(requested, track.clips.size());
    indexValid = position <= track.clips.size();
    if (!indexValid) {
        return true;
    }
    track.clips.insert(
        track.clips.begin() + static_cast<std::ptrdiff_t>(position),
        std::move(*typed));
    return true;
}

bool insertClipIntoTrack(TimelineTrack &track,
                         TimelineClip clip,
                         std::size_t requested,
                         bool &indexValid)
{
    switch (track.index()) {
    case 0:
        return insertTypedClip<TimelineVideoTrack, TimelineVideoClip>(
            std::get<TimelineVideoTrack>(track), std::move(clip), requested, indexValid);
    case 1:
        return insertTypedClip<TimelineAudioTrack, TimelineAudioClip>(
            std::get<TimelineAudioTrack>(track), std::move(clip), requested, indexValid);
    case 2:
        return insertTypedClip<TimelineSubtitleTrack, TimelineSubtitleClip>(
            std::get<TimelineSubtitleTrack>(track), std::move(clip), requested, indexValid);
    case 3:
        return insertTypedClip<TimelineDataTrack, TimelineDataClip>(
            std::get<TimelineDataTrack>(track), std::move(clip), requested, indexValid);
    default:
        indexValid = false;
        return false;
    }
}

struct ClipLocation {
    std::size_t trackIndex = 0;
    std::size_t clipIndex = 0;
};

template<typename Track>
std::optional<std::size_t> clipIndexInTrack(const Track &track,
                                            const std::string &clipId)
{
    return indexById(track.clips, clipId,
                     [](const auto &clip) -> const std::string & {
                         return clip.properties.id;
                     });
}

std::optional<ClipLocation> locateClip(const TimelineSequence &sequence,
                                       const std::string &clipId)
{
    for (std::size_t trackPosition = 0;
         trackPosition < sequence.tracks.size();
         ++trackPosition) {
        const std::optional<std::size_t> clipPosition = std::visit(
            [&clipId](const auto &track) {
                return clipIndexInTrack(track, clipId);
            }, sequence.tracks[trackPosition]);
        if (clipPosition) {
            return ClipLocation{trackPosition, *clipPosition};
        }
    }
    return std::nullopt;
}

template<typename Track, typename Clip>
bool replaceTypedClip(Track &track,
                      std::size_t index,
                      TimelineClip replacement)
{
    Clip *typed = std::get_if<Clip>(&replacement);
    if (!typed) {
        return false;
    }
    track.clips[index] = std::move(*typed);
    return true;
}

bool replaceClipInTrack(TimelineTrack &track,
                        std::size_t index,
                        TimelineClip replacement)
{
    switch (track.index()) {
    case 0:
        return replaceTypedClip<TimelineVideoTrack, TimelineVideoClip>(
            std::get<TimelineVideoTrack>(track), index, std::move(replacement));
    case 1:
        return replaceTypedClip<TimelineAudioTrack, TimelineAudioClip>(
            std::get<TimelineAudioTrack>(track), index, std::move(replacement));
    case 2:
        return replaceTypedClip<TimelineSubtitleTrack, TimelineSubtitleClip>(
            std::get<TimelineSubtitleTrack>(track), index, std::move(replacement));
    case 3:
        return replaceTypedClip<TimelineDataTrack, TimelineDataClip>(
            std::get<TimelineDataTrack>(track), index, std::move(replacement));
    default:
        return false;
    }
}

TimelineClip clipAt(const TimelineTrack &track, std::size_t index)
{
    return std::visit([index](const auto &typedTrack) -> TimelineClip {
        return typedTrack.clips[index];
    }, track);
}

void removeClipFromTrack(TimelineTrack &track, std::size_t index)
{
    std::visit([index](auto &typedTrack) {
        typedTrack.clips.erase(
            typedTrack.clips.begin() + static_cast<std::ptrdiff_t>(index));
    }, track);
}

bool moveClipInTrack(TimelineTrack &track,
                     std::size_t index,
                     std::size_t destinationIndex)
{
    return std::visit([index, destinationIndex](auto &typedTrack) {
        if (destinationIndex >= typedTrack.clips.size()) {
            return false;
        }
        moveElement(typedTrack.clips, index, destinationIndex);
        return true;
    }, track);
}

} // namespace

TimelineEditor::TimelineEditor(TimelineProject &projectValue)
{
    bind(projectValue);
}

TimelineEditResult TimelineEditor::bind(TimelineProject &projectValue)
{
    const TimelineValidationResult validation = validateTimelineProject(projectValue);
    if (!validation.ok()) {
        const TimelineValidationIssue &issue = validation.issues.front();
        return reject(TimelineEditCode::InvalidProject, issue.path, issue.message);
    }
    m_project = &projectValue;
    m_revision = 0;
    return unchanged();
}

void TimelineEditor::unbind() noexcept
{
    m_project = nullptr;
    m_revision = 0;
    m_lastResult = {};
}

bool TimelineEditor::isBound() const noexcept
{
    return m_project != nullptr;
}

TimelineProject *TimelineEditor::project() noexcept
{
    return m_project;
}

const TimelineProject *TimelineEditor::project() const noexcept
{
    return m_project;
}

std::uint64_t TimelineEditor::revision() const noexcept
{
    return m_revision;
}

const TimelineEditResult &TimelineEditor::lastResult() const noexcept
{
    return m_lastResult;
}

TimelineEditResult TimelineEditor::setActiveSequence(std::string sequenceId)
{
    if (!requireValidProject()) {
        return m_lastResult;
    }
    if (m_project->activeSequenceId == sequenceId) {
        return unchanged();
    }
    TimelineProject replacement = *m_project;
    replacement.activeSequenceId = std::move(sequenceId);
    return commit(std::move(replacement));
}

TimelineEditResult TimelineEditor::setSequenceFrameRate(
    const std::string &sequenceIdValue,
    TimelineFrameRate frameRate)
{
    if (!requireValidProject()) {
        return m_lastResult;
    }
    const TimelineSequence *sequence = findTimelineSequence(
        *m_project, sequenceIdValue);
    if (!sequence) {
        return reject(TimelineEditCode::NotFound,
                      "sequences",
                      "sequence was not found");
    }
    if (sequence->editingFrameRate == frameRate) {
        return unchanged();
    }
    TimelineProject replacement = *m_project;
    findTimelineSequence(replacement, sequenceIdValue)->editingFrameRate = frameRate;
    return commit(std::move(replacement));
}

TimelineEditResult TimelineEditor::setRenderContainer(
    const std::string &profileId,
    TimelineContainerDescriptor container)
{
    if (!requireValidProject()) {
        return m_lastResult;
    }
    const TimelineRenderProfile *profile = findTimelineRenderProfile(
        *m_project, profileId);
    if (!profile) {
        return reject(TimelineEditCode::NotFound,
                      "renderProfiles",
                      "render profile was not found");
    }
    if (profile->container == container) {
        return unchanged();
    }
    TimelineProject replacement = *m_project;
    findTimelineRenderProfile(replacement, profileId)->container = std::move(container);
    return commit(std::move(replacement));
}

TimelineEditResult TimelineEditor::setRenderVideoCodec(
    const std::string &profileId,
    TimelineCodecDescriptor codec)
{
    if (!requireValidProject()) {
        return m_lastResult;
    }
    const TimelineRenderProfile *profile = findTimelineRenderProfile(
        *m_project, profileId);
    if (!profile) {
        return reject(TimelineEditCode::NotFound,
                      "renderProfiles",
                      "render profile was not found");
    }
    if (!profile->video) {
        return reject(TimelineEditCode::KindMismatch,
                      "renderProfiles.video",
                      "render profile has no video output");
    }
    if (profile->video->codec == codec) {
        return unchanged();
    }
    TimelineProject replacement = *m_project;
    findTimelineRenderProfile(replacement, profileId)->video->codec = std::move(codec);
    return commit(std::move(replacement));
}

TimelineEditResult TimelineEditor::setRenderAudioCodec(
    const std::string &profileId,
    TimelineCodecDescriptor codec)
{
    if (!requireValidProject()) {
        return m_lastResult;
    }
    const TimelineRenderProfile *profile = findTimelineRenderProfile(
        *m_project, profileId);
    if (!profile) {
        return reject(TimelineEditCode::NotFound,
                      "renderProfiles",
                      "render profile was not found");
    }
    if (!profile->audio) {
        return reject(TimelineEditCode::KindMismatch,
                      "renderProfiles.audio",
                      "render profile has no audio output");
    }
    if (profile->audio->codec == codec) {
        return unchanged();
    }
    TimelineProject replacement = *m_project;
    findTimelineRenderProfile(replacement, profileId)->audio->codec = std::move(codec);
    return commit(std::move(replacement));
}

TimelineEditResult TimelineEditor::insertMediaSource(TimelineMediaSource source,
                                                     std::size_t index)
{
    if (!requireValidProject()) {
        return m_lastResult;
    }
    if (findTimelineMediaSource(*m_project, source.id)) {
        return reject(TimelineEditCode::DuplicateId,
                      "mediaSources",
                      "media source id already exists");
    }
    const std::size_t position = insertionIndex(index, m_project->mediaSources.size());
    if (position > m_project->mediaSources.size()) {
        return reject(TimelineEditCode::IndexOutOfRange,
                      "mediaSources",
                      "media source insertion index is outside the collection");
    }
    TimelineProject replacement = *m_project;
    replacement.mediaSources.insert(
        replacement.mediaSources.begin() + static_cast<std::ptrdiff_t>(position),
        std::move(source));
    return commit(std::move(replacement));
}

TimelineEditResult TimelineEditor::replaceMediaSource(
    const std::string &sourceId,
    TimelineMediaSource source)
{
    if (!requireValidProject()) {
        return m_lastResult;
    }
    const std::optional<std::size_t> index = mediaSourceIndex(*m_project, sourceId);
    if (!index) {
        return reject(TimelineEditCode::NotFound,
                      "mediaSources",
                      "media source was not found");
    }
    if (source.id != sourceId) {
        return reject(TimelineEditCode::InvalidArgument,
                      "mediaSources.id",
                      "replacement must preserve the media source id");
    }
    if (m_project->mediaSources[*index] == source) {
        return unchanged();
    }
    TimelineProject replacement = *m_project;
    replacement.mediaSources[*index] = std::move(source);
    return commit(std::move(replacement));
}

TimelineEditResult TimelineEditor::moveMediaSource(const std::string &sourceId,
                                                   std::size_t destinationIndex)
{
    if (!requireValidProject()) {
        return m_lastResult;
    }
    const std::optional<std::size_t> index = mediaSourceIndex(*m_project, sourceId);
    if (!index) {
        return reject(TimelineEditCode::NotFound,
                      "mediaSources",
                      "media source was not found");
    }
    if (destinationIndex >= m_project->mediaSources.size()) {
        return reject(TimelineEditCode::IndexOutOfRange,
                      "mediaSources",
                      "media source destination index is outside the collection");
    }
    if (*index == destinationIndex) {
        return unchanged();
    }
    TimelineProject replacement = *m_project;
    moveElement(replacement.mediaSources, *index, destinationIndex);
    return commit(std::move(replacement));
}

TimelineEditResult TimelineEditor::removeMediaSource(const std::string &sourceId)
{
    if (!requireValidProject()) {
        return m_lastResult;
    }
    const std::optional<std::size_t> index = mediaSourceIndex(*m_project, sourceId);
    if (!index) {
        return reject(TimelineEditCode::NotFound,
                      "mediaSources",
                      "media source was not found");
    }
    TimelineProject replacement = *m_project;
    replacement.mediaSources.erase(
        replacement.mediaSources.begin() + static_cast<std::ptrdiff_t>(*index));
    return commit(std::move(replacement));
}

TimelineEditResult TimelineEditor::insertSequence(TimelineSequence sequence,
                                                  std::size_t index)
{
    if (!requireValidProject()) {
        return m_lastResult;
    }
    if (findTimelineSequence(*m_project, sequence.id)) {
        return reject(TimelineEditCode::DuplicateId,
                      "sequences",
                      "sequence id already exists");
    }
    const std::size_t position = insertionIndex(index, m_project->sequences.size());
    if (position > m_project->sequences.size()) {
        return reject(TimelineEditCode::IndexOutOfRange,
                      "sequences",
                      "sequence insertion index is outside the collection");
    }
    TimelineProject replacement = *m_project;
    replacement.sequences.insert(
        replacement.sequences.begin() + static_cast<std::ptrdiff_t>(position),
        std::move(sequence));
    return commit(std::move(replacement));
}

TimelineEditResult TimelineEditor::replaceSequence(
    const std::string &sequenceIdValue,
    TimelineSequence sequence)
{
    if (!requireValidProject()) {
        return m_lastResult;
    }
    const std::optional<std::size_t> index = sequenceIndex(*m_project, sequenceIdValue);
    if (!index) {
        return reject(TimelineEditCode::NotFound,
                      "sequences",
                      "sequence was not found");
    }
    if (sequence.id != sequenceIdValue) {
        return reject(TimelineEditCode::InvalidArgument,
                      "sequences.id",
                      "replacement must preserve the sequence id");
    }
    if (m_project->sequences[*index] == sequence) {
        return unchanged();
    }
    TimelineProject replacement = *m_project;
    replacement.sequences[*index] = std::move(sequence);
    return commit(std::move(replacement));
}

TimelineEditResult TimelineEditor::moveSequence(const std::string &sequenceIdValue,
                                                std::size_t destinationIndex)
{
    if (!requireValidProject()) {
        return m_lastResult;
    }
    const std::optional<std::size_t> index = sequenceIndex(*m_project, sequenceIdValue);
    if (!index) {
        return reject(TimelineEditCode::NotFound,
                      "sequences",
                      "sequence was not found");
    }
    if (destinationIndex >= m_project->sequences.size()) {
        return reject(TimelineEditCode::IndexOutOfRange,
                      "sequences",
                      "sequence destination index is outside the collection");
    }
    if (*index == destinationIndex) {
        return unchanged();
    }
    TimelineProject replacement = *m_project;
    moveElement(replacement.sequences, *index, destinationIndex);
    return commit(std::move(replacement));
}

TimelineEditResult TimelineEditor::removeSequence(const std::string &sequenceIdValue)
{
    if (!requireValidProject()) {
        return m_lastResult;
    }
    const std::optional<std::size_t> index = sequenceIndex(*m_project, sequenceIdValue);
    if (!index) {
        return reject(TimelineEditCode::NotFound,
                      "sequences",
                      "sequence was not found");
    }
    TimelineProject replacement = *m_project;
    replacement.sequences.erase(
        replacement.sequences.begin() + static_cast<std::ptrdiff_t>(*index));
    return commit(std::move(replacement));
}

TimelineEditResult TimelineEditor::insertRenderProfile(
    TimelineRenderProfile profile,
    std::size_t index)
{
    if (!requireValidProject()) {
        return m_lastResult;
    }
    if (findTimelineRenderProfile(*m_project, profile.id)) {
        return reject(TimelineEditCode::DuplicateId,
                      "renderProfiles",
                      "render profile id already exists");
    }
    const std::size_t position = insertionIndex(index, m_project->renderProfiles.size());
    if (position > m_project->renderProfiles.size()) {
        return reject(TimelineEditCode::IndexOutOfRange,
                      "renderProfiles",
                      "render profile insertion index is outside the collection");
    }
    TimelineProject replacement = *m_project;
    replacement.renderProfiles.insert(
        replacement.renderProfiles.begin() + static_cast<std::ptrdiff_t>(position),
        std::move(profile));
    return commit(std::move(replacement));
}

TimelineEditResult TimelineEditor::replaceRenderProfile(
    const std::string &profileId,
    TimelineRenderProfile profile)
{
    if (!requireValidProject()) {
        return m_lastResult;
    }
    const std::optional<std::size_t> index = profileIndex(*m_project, profileId);
    if (!index) {
        return reject(TimelineEditCode::NotFound,
                      "renderProfiles",
                      "render profile was not found");
    }
    if (profile.id != profileId) {
        return reject(TimelineEditCode::InvalidArgument,
                      "renderProfiles.id",
                      "replacement must preserve the render profile id");
    }
    if (m_project->renderProfiles[*index] == profile) {
        return unchanged();
    }
    TimelineProject replacement = *m_project;
    replacement.renderProfiles[*index] = std::move(profile);
    return commit(std::move(replacement));
}

TimelineEditResult TimelineEditor::moveRenderProfile(const std::string &profileId,
                                                     std::size_t destinationIndex)
{
    if (!requireValidProject()) {
        return m_lastResult;
    }
    const std::optional<std::size_t> index = profileIndex(*m_project, profileId);
    if (!index) {
        return reject(TimelineEditCode::NotFound,
                      "renderProfiles",
                      "render profile was not found");
    }
    if (destinationIndex >= m_project->renderProfiles.size()) {
        return reject(TimelineEditCode::IndexOutOfRange,
                      "renderProfiles",
                      "render profile destination index is outside the collection");
    }
    if (*index == destinationIndex) {
        return unchanged();
    }
    TimelineProject replacement = *m_project;
    moveElement(replacement.renderProfiles, *index, destinationIndex);
    return commit(std::move(replacement));
}

TimelineEditResult TimelineEditor::removeRenderProfile(const std::string &profileId)
{
    if (!requireValidProject()) {
        return m_lastResult;
    }
    const std::optional<std::size_t> index = profileIndex(*m_project, profileId);
    if (!index) {
        return reject(TimelineEditCode::NotFound,
                      "renderProfiles",
                      "render profile was not found");
    }
    TimelineProject replacement = *m_project;
    replacement.renderProfiles.erase(
        replacement.renderProfiles.begin() + static_cast<std::ptrdiff_t>(*index));
    return commit(std::move(replacement));
}

TimelineEditResult TimelineEditor::insertTrack(const std::string &sequenceIdValue,
                                               TimelineTrack track,
                                               std::size_t index)
{
    if (!requireValidProject()) {
        return m_lastResult;
    }
    const TimelineSequence *sequence = findTimelineSequence(*m_project, sequenceIdValue);
    if (!sequence) {
        return reject(TimelineEditCode::NotFound,
                      "sequences",
                      "sequence was not found");
    }
    const std::string trackId = timelineTrackProperties(track).id;
    if (findTimelineTrack(*sequence, trackId)) {
        return reject(TimelineEditCode::DuplicateId,
                      "sequence.tracks",
                      "track id already exists");
    }
    const std::size_t position = insertionIndex(index, sequence->tracks.size());
    if (position > sequence->tracks.size()) {
        return reject(TimelineEditCode::IndexOutOfRange,
                      "sequence.tracks",
                      "track insertion index is outside the collection");
    }
    TimelineProject replacement = *m_project;
    TimelineSequence *target = findTimelineSequence(replacement, sequenceIdValue);
    target->tracks.insert(
        target->tracks.begin() + static_cast<std::ptrdiff_t>(position),
        std::move(track));
    return commit(std::move(replacement));
}

TimelineEditResult TimelineEditor::replaceTrack(const std::string &sequenceIdValue,
                                                const std::string &trackId,
                                                TimelineTrack track)
{
    if (!requireValidProject()) {
        return m_lastResult;
    }
    const TimelineSequence *sequence = findTimelineSequence(*m_project, sequenceIdValue);
    if (!sequence) {
        return reject(TimelineEditCode::NotFound,
                      "sequences",
                      "sequence was not found");
    }
    const std::optional<std::size_t> index = trackIndex(*sequence, trackId);
    if (!index) {
        return reject(TimelineEditCode::NotFound,
                      "sequence.tracks",
                      "track was not found");
    }
    if (timelineTrackProperties(track).id != trackId) {
        return reject(TimelineEditCode::InvalidArgument,
                      "sequence.tracks.id",
                      "replacement must preserve the track id");
    }
    if (sequence->tracks[*index] == track) {
        return unchanged();
    }
    TimelineProject replacement = *m_project;
    findTimelineSequence(replacement, sequenceIdValue)->tracks[*index] = std::move(track);
    return commit(std::move(replacement));
}

TimelineEditResult TimelineEditor::moveTrack(const std::string &sequenceIdValue,
                                             const std::string &trackId,
                                             std::size_t destinationIndex)
{
    if (!requireValidProject()) {
        return m_lastResult;
    }
    const TimelineSequence *sequence = findTimelineSequence(*m_project, sequenceIdValue);
    if (!sequence) {
        return reject(TimelineEditCode::NotFound,
                      "sequences",
                      "sequence was not found");
    }
    const std::optional<std::size_t> index = trackIndex(*sequence, trackId);
    if (!index) {
        return reject(TimelineEditCode::NotFound,
                      "sequence.tracks",
                      "track was not found");
    }
    if (destinationIndex >= sequence->tracks.size()) {
        return reject(TimelineEditCode::IndexOutOfRange,
                      "sequence.tracks",
                      "track destination index is outside the collection");
    }
    if (*index == destinationIndex) {
        return unchanged();
    }
    TimelineProject replacement = *m_project;
    moveElement(findTimelineSequence(replacement, sequenceIdValue)->tracks,
                *index, destinationIndex);
    return commit(std::move(replacement));
}

TimelineEditResult TimelineEditor::removeTrack(const std::string &sequenceIdValue,
                                               const std::string &trackId)
{
    if (!requireValidProject()) {
        return m_lastResult;
    }
    const TimelineSequence *sequence = findTimelineSequence(*m_project, sequenceIdValue);
    if (!sequence) {
        return reject(TimelineEditCode::NotFound,
                      "sequences",
                      "sequence was not found");
    }
    const std::optional<std::size_t> index = trackIndex(*sequence, trackId);
    if (!index) {
        return reject(TimelineEditCode::NotFound,
                      "sequence.tracks",
                      "track was not found");
    }
    TimelineProject replacement = *m_project;
    TimelineSequence *target = findTimelineSequence(replacement, sequenceIdValue);
    target->tracks.erase(
        target->tracks.begin() + static_cast<std::ptrdiff_t>(*index));
    return commit(std::move(replacement));
}

TimelineEditResult TimelineEditor::insertClip(const std::string &sequenceIdValue,
                                              const std::string &trackId,
                                              TimelineClip clip,
                                              std::size_t index)
{
    if (!requireValidProject()) {
        return m_lastResult;
    }
    const TimelineSequence *sequence = findTimelineSequence(*m_project, sequenceIdValue);
    if (!sequence) {
        return reject(TimelineEditCode::NotFound,
                      "sequences",
                      "sequence was not found");
    }
    if (findTimelineClip(*sequence, timelineClipProperties(clip).id)) {
        return reject(TimelineEditCode::DuplicateId,
                      "sequence.tracks.clips",
                      "clip id already exists in the sequence");
    }
    const std::optional<std::size_t> targetIndex = trackIndex(*sequence, trackId);
    if (!targetIndex) {
        return reject(TimelineEditCode::NotFound,
                      "sequence.tracks",
                      "track was not found");
    }
    if (timelineTrackKind(sequence->tracks[*targetIndex]) != timelineClipKind(clip)) {
        return reject(TimelineEditCode::KindMismatch,
                      "sequence.tracks.clips",
                      "clip kind must match the owning track kind");
    }
    TimelineProject replacement = *m_project;
    TimelineTrack &target = findTimelineSequence(
        replacement, sequenceIdValue)->tracks[*targetIndex];
    bool indexValid = false;
    if (!insertClipIntoTrack(target, std::move(clip), index, indexValid)) {
        return reject(TimelineEditCode::KindMismatch,
                      "sequence.tracks.clips",
                      "clip kind must match the owning track kind");
    }
    if (!indexValid) {
        return reject(TimelineEditCode::IndexOutOfRange,
                      "sequence.tracks.clips",
                      "clip insertion index is outside the collection");
    }
    return commit(std::move(replacement));
}

TimelineEditResult TimelineEditor::replaceClip(const std::string &sequenceIdValue,
                                               const std::string &clipId,
                                               TimelineClip clip)
{
    if (!requireValidProject()) {
        return m_lastResult;
    }
    const TimelineSequence *sequence = findTimelineSequence(*m_project, sequenceIdValue);
    if (!sequence) {
        return reject(TimelineEditCode::NotFound,
                      "sequences",
                      "sequence was not found");
    }
    const std::optional<ClipLocation> location = locateClip(*sequence, clipId);
    if (!location) {
        return reject(TimelineEditCode::NotFound,
                      "sequence.tracks.clips",
                      "clip was not found");
    }
    if (timelineClipProperties(clip).id != clipId) {
        return reject(TimelineEditCode::InvalidArgument,
                      "sequence.tracks.clips.id",
                      "replacement must preserve the clip id");
    }
    if (timelineTrackKind(sequence->tracks[location->trackIndex])
        != timelineClipKind(clip)) {
        return reject(TimelineEditCode::KindMismatch,
                      "sequence.tracks.clips",
                      "clip kind must match the owning track kind");
    }
    if (clipAt(sequence->tracks[location->trackIndex], location->clipIndex) == clip) {
        return unchanged();
    }
    TimelineProject replacement = *m_project;
    TimelineTrack &target = findTimelineSequence(
        replacement, sequenceIdValue)->tracks[location->trackIndex];
    if (!replaceClipInTrack(target, location->clipIndex, std::move(clip))) {
        return reject(TimelineEditCode::KindMismatch,
                      "sequence.tracks.clips",
                      "clip kind must match the owning track kind");
    }
    return commit(std::move(replacement));
}

TimelineEditResult TimelineEditor::moveClip(const std::string &sequenceIdValue,
                                            const std::string &clipId,
                                            std::size_t destinationIndex)
{
    if (!requireValidProject()) {
        return m_lastResult;
    }
    const TimelineSequence *sequence = findTimelineSequence(*m_project, sequenceIdValue);
    if (!sequence) {
        return reject(TimelineEditCode::NotFound,
                      "sequences",
                      "sequence was not found");
    }
    const std::optional<ClipLocation> location = locateClip(*sequence, clipId);
    if (!location) {
        return reject(TimelineEditCode::NotFound,
                      "sequence.tracks.clips",
                      "clip was not found");
    }
    if (location->clipIndex == destinationIndex) {
        return unchanged();
    }
    TimelineProject replacement = *m_project;
    TimelineTrack &target = findTimelineSequence(
        replacement, sequenceIdValue)->tracks[location->trackIndex];
    if (!moveClipInTrack(target, location->clipIndex, destinationIndex)) {
        return reject(TimelineEditCode::IndexOutOfRange,
                      "sequence.tracks.clips",
                      "clip destination index is outside the owning track");
    }
    return commit(std::move(replacement));
}

TimelineEditResult TimelineEditor::removeClip(const std::string &sequenceIdValue,
                                              const std::string &clipId)
{
    if (!requireValidProject()) {
        return m_lastResult;
    }
    const TimelineSequence *sequence = findTimelineSequence(*m_project, sequenceIdValue);
    if (!sequence) {
        return reject(TimelineEditCode::NotFound,
                      "sequences",
                      "sequence was not found");
    }
    const std::optional<ClipLocation> location = locateClip(*sequence, clipId);
    if (!location) {
        return reject(TimelineEditCode::NotFound,
                      "sequence.tracks.clips",
                      "clip was not found");
    }
    TimelineProject replacement = *m_project;
    TimelineTrack &target = findTimelineSequence(
        replacement, sequenceIdValue)->tracks[location->trackIndex];
    removeClipFromTrack(target, location->clipIndex);
    return commit(std::move(replacement));
}

bool TimelineEditor::requireValidProject()
{
    if (!m_project) {
        (void)reject(TimelineEditCode::NotBound,
                     "project",
                     "timeline editor is not bound");
        return false;
    }
    const TimelineValidationResult validation = validateTimelineProject(*m_project);
    if (!validation.ok()) {
        const TimelineValidationIssue &issue = validation.issues.front();
        (void)reject(TimelineEditCode::InvalidProject, issue.path, issue.message);
        return false;
    }
    return true;
}

TimelineEditResult TimelineEditor::commit(TimelineProject replacement)
{
    const TimelineValidationResult validation = validateTimelineProject(replacement);
    if (!validation.ok()) {
        const TimelineValidationIssue &issue = validation.issues.front();
        return reject(TimelineEditCode::ValidationRejected,
                      issue.path,
                      issue.message);
    }
    if (*m_project == replacement) {
        return unchanged();
    }
    *m_project = std::move(replacement);
    ++m_revision;
    m_lastResult = {TimelineEditCode::None, true, {}, {}};
    return m_lastResult;
}

TimelineEditResult TimelineEditor::reject(TimelineEditCode code,
                                          std::string path,
                                          std::string message)
{
    m_lastResult = {code, false, std::move(path), std::move(message)};
    return m_lastResult;
}

TimelineEditResult TimelineEditor::unchanged()
{
    m_lastResult = {};
    return m_lastResult;
}

} // namespace iiSharedCanvas
