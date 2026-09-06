# Persisted audio timeline

Document format 1.4 adds `Document::audioAssets` and `Document::audioTracks`.
These are authoritative public aggregate data, independent of the separate
`TimelineProject` model. Visual `Layer` remains a bitmap/vector variant;
`AudioTrackLayer` owns audio clips in document timeline coordinates and is not
passed to the image renderer.

## Source and timing contract

`AudioAsset` owns signed 16-bit interleaved PCM, one or two channels, at
8000–192000 Hz. It must contain at least one complete sample frame. A stereo
sample frame contains the left and right samples. Asset ids are unique across
both visual and audio asset collections.

`AudioClip` references one audio asset. `startFrame` and `durationFrames` are
integer document frames. The half-open interval
`[startFrame, startFrame + durationFrames)` must have positive duration and fit
inside `Document::timeline.frameCount`. `sourceOffsetSamples` counts per-channel
sample frames, not interleaved scalar samples. Its source range must contain
at least `ceil(durationFrames * frameRate.denominator * sampleRate /
frameRate.numerator)` sample frames. `audioSampleFrameCount()` implements that
exact rational calculation with checked integer arithmetic; invalid rates or
overflow return `nullopt`.

Tracks are ordered as stored. Clips within one track must be sorted by start
frame and cannot overlap; separate tracks may overlap freely. A track may be
empty and may contain gaps. Track ids are unique across both visual and audio
layers. Clip ids are unique across every audio track.

Track and clip `gainDb` are finite values from -96 through +24 dB. Gains add in
decibels; track `muted` or clip `enabled == false` makes that clip inaudible.
Muted and disabled data still obey the same source, reference and timing
validation so re-enabling it is safe. This persisted contract does not supply
device playback, resampling, mixing, effects, fades, or waveform rendering.

## Structural editing

Use `DocumentEditor::insertAudioAsset`, `replaceAudioAsset`, and
`removeAudioAsset` to manage owned sources. Deleting a source used by any clip,
including a disabled or muted clip, returns `AssetReferenced`. Replacement
preserves its id and must leave all clips' source ranges valid.

Use `insertAudioTrack`, `replaceAudioTrack`, `moveAudioTrack`, and
`removeAudioTrack` to manage layers. Track replacement edits its name, gain,
mute flag, and complete clip collection while preserving its id. Removing a
track removes its clips and retains its reusable audio assets.

`insertAudioClip`, `replaceAudioClip`, and `removeAudioClip` operate by track id.
Clip replacement preserves its id and edits its name, source, timeline interval,
trim, gain, or enabled state. Insertion and replacement sort clips by start
frame; overlap still rejects the entire operation. Direct aggregate track
insertion or replacement requires already canonical clip order.

Every edit validates references, source bounds, and the whole document. Failed
edits preserve the document, format version, and editor revision. Equal
replacements are no-ops. Audio insertion upgrades older documents to format
1.4. Existing frame count and frame rate edits also validate audio ranges.
File-bound edits use the same synchronous `DocumentFile` transaction as visual
edits. Use stable ids and the const/mutable `findAudioAsset`, `findAudioTrack`,
and `findAudioClip` helpers; collection mutation can invalidate retained pointers.

For example, this creates a one-second audio timeline from a supported WAV with
at least one second of source audio. The edit rejects shorter inputs instead of
silently padding the clip:

```cpp
#include <iiSharedCanvas.h>
#include <stdexcept>
#include <utility>

iiSharedCanvas::Document makeAudioTimeline(const std::string &wavPath)
{
    using namespace iiSharedCanvas;
    AudioImportOptions options;
    options.assetId = "dialogue-source";
    auto imported = importAudioWav(wavPath, options);
    if (!imported.ok()) {
        throw std::runtime_error(imported.result.message);
    }

    Document document;
    document.extent = {1920, 1080};
    document.timeline = {{24, 1}, 24};
    DocumentEditor editor(document);
    auto result = editor.insertAudioAsset(std::move(imported.asset));
    if (!result.ok()) {
        throw std::runtime_error(result.message);
    }
    AudioTrackLayer track;
    track.id = "dialogue-track";
    track.name = "Dialogue";
    track.clips.push_back({"dialogue-clip", "Opening", "dialogue-source",
                           0, 24, 0, -3.0, true});
    result = editor.insertAudioTrack(std::move(track));
    if (!result.ok()) {
        throw std::runtime_error(result.message);
    }
    return document;
}
```

## Interchange boundary

The timeline interchange adapter translates the persisted audio data alongside
visual tracks. Its package and application compatibility contract is documented
in `TIMELINE_INTERCHANGE.md`. `source.iisc` retains the authoritative audio
aggregates; editor XML and generated WAV media are derived interchange outputs.
Unsupported projections must be reported instead of silently discarding audio.

The audio model and integer timing validation introduce no external dependency.
`DocumentAudioTest` covers model constraints, rational rounding and overflow,
cross-track overlap, source trim bounds, rollback, deletion safety, track order,
clip edits, and format migration.
