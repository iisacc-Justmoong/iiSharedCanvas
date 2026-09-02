#pragma once

#include "Export.h"
#include "Timeline/TimelineProject.h"

#include <cstddef>
#include <cstdint>
#include <limits>
#include <string>

namespace iiSharedCanvas {

inline constexpr std::size_t AppendTimelineIndex =
    std::numeric_limits<std::size_t>::max();

enum class TimelineEditCode : std::uint8_t {
    None,
    NotBound,
    InvalidProject,
    InvalidArgument,
    DuplicateId,
    NotFound,
    KindMismatch,
    IndexOutOfRange,
    ValidationRejected,
};

struct TimelineEditResult {
    TimelineEditCode code = TimelineEditCode::None;
    bool changed = false;
    std::string path;
    std::string message;

    [[nodiscard]] bool ok() const noexcept
    {
        return code == TimelineEditCode::None;
    }
};

class IISHAREDCANVAS_EXPORT TimelineEditor final {
public:
    TimelineEditor() = default;
    explicit TimelineEditor(TimelineProject &project);
    TimelineEditor(const TimelineEditor &) = delete;
    TimelineEditor &operator=(const TimelineEditor &) = delete;
    TimelineEditor(TimelineEditor &&) = delete;
    TimelineEditor &operator=(TimelineEditor &&) = delete;

    TimelineEditResult bind(TimelineProject &project);
    void unbind() noexcept;
    [[nodiscard]] bool isBound() const noexcept;
    [[nodiscard]] TimelineProject *project() noexcept;
    [[nodiscard]] const TimelineProject *project() const noexcept;
    [[nodiscard]] std::uint64_t revision() const noexcept;
    [[nodiscard]] const TimelineEditResult &lastResult() const noexcept;

    TimelineEditResult setActiveSequence(std::string sequenceId);
    TimelineEditResult setSequenceFrameRate(const std::string &sequenceId,
                                            TimelineFrameRate frameRate);
    TimelineEditResult setRenderContainer(
        const std::string &profileId,
        TimelineContainerDescriptor container);
    TimelineEditResult setRenderVideoCodec(
        const std::string &profileId,
        TimelineCodecDescriptor codec);
    TimelineEditResult setRenderAudioCodec(
        const std::string &profileId,
        TimelineCodecDescriptor codec);

    TimelineEditResult insertMediaSource(
        TimelineMediaSource source,
        std::size_t index = AppendTimelineIndex);
    TimelineEditResult replaceMediaSource(const std::string &sourceId,
                                          TimelineMediaSource source);
    TimelineEditResult moveMediaSource(const std::string &sourceId,
                                       std::size_t destinationIndex);
    TimelineEditResult removeMediaSource(const std::string &sourceId);

    TimelineEditResult insertSequence(
        TimelineSequence sequence,
        std::size_t index = AppendTimelineIndex);
    TimelineEditResult replaceSequence(const std::string &sequenceId,
                                       TimelineSequence sequence);
    TimelineEditResult moveSequence(const std::string &sequenceId,
                                    std::size_t destinationIndex);
    TimelineEditResult removeSequence(const std::string &sequenceId);

    TimelineEditResult insertRenderProfile(
        TimelineRenderProfile profile,
        std::size_t index = AppendTimelineIndex);
    TimelineEditResult replaceRenderProfile(const std::string &profileId,
                                            TimelineRenderProfile profile);
    TimelineEditResult moveRenderProfile(const std::string &profileId,
                                         std::size_t destinationIndex);
    TimelineEditResult removeRenderProfile(const std::string &profileId);

    TimelineEditResult insertTrack(
        const std::string &sequenceId,
        TimelineTrack track,
        std::size_t index = AppendTimelineIndex);
    TimelineEditResult replaceTrack(const std::string &sequenceId,
                                    const std::string &trackId,
                                    TimelineTrack track);
    TimelineEditResult moveTrack(const std::string &sequenceId,
                                 const std::string &trackId,
                                 std::size_t destinationIndex);
    TimelineEditResult removeTrack(const std::string &sequenceId,
                                   const std::string &trackId);

    TimelineEditResult insertClip(
        const std::string &sequenceId,
        const std::string &trackId,
        TimelineClip clip,
        std::size_t index = AppendTimelineIndex);
    TimelineEditResult replaceClip(const std::string &sequenceId,
                                   const std::string &clipId,
                                   TimelineClip clip);
    TimelineEditResult moveClip(const std::string &sequenceId,
                                const std::string &clipId,
                                std::size_t destinationIndex);
    TimelineEditResult removeClip(const std::string &sequenceId,
                                  const std::string &clipId);

private:
    [[nodiscard]] bool requireValidProject();
    [[nodiscard]] TimelineEditResult commit(TimelineProject replacement);
    [[nodiscard]] TimelineEditResult reject(TimelineEditCode code,
                                            std::string path,
                                            std::string message);
    [[nodiscard]] TimelineEditResult unchanged();

    TimelineProject *m_project = nullptr;
    std::uint64_t m_revision = 0;
    TimelineEditResult m_lastResult;
};

} // namespace iiSharedCanvas
