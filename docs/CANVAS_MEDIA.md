# One canvas for images, vectors, video and motion graphics

Package 0.11.0 makes `Document` the shared, renderable and persisted owner of
all four kinds of visual work. Images remain `RasterAsset` (or sparse
`ChunkedRasterAsset`); vectors remain editable `VectorAsset` paths. Native
`VideoAsset` stores an ordered sequence of equally sized ARGB display frames
and its own positive rational `FrameRate`. It is self-contained: rendering,
sampling, binary snapshots and working-file reopen need no media path or decoder.

`Layer` is `BitmapLayer | VectorLayer | VideoLayer`. Each layer keeps one content
kind. A bitmap cannot reference video; a video layer must have one `StaticSource`
referencing a video asset. The existing frame-owned bitmap/vector keys retain
hold sampling. Video is not disguised as a collection of image layers.

## Video placement and timing

`VideoPlayback::sourceInFrame` is inclusive and `sourceOutFrame` is exclusive;
an absent out point means the number of owned frames. Empty or out-of-bounds
trims are rejected. The clip begins at `LayerProperties::frameRange.firstFrame`,
or document frame zero when no range is set. The inclusive layer existence range
always limits playback. The source frame at document frame `f` is:

```
sourceInFrame + floor((f - layerStart) * sourceRate / documentRate)
```

The library evaluates this exactly using integer products and comparisons, even
for full-width uint32 rational rates; it never converts persisted timing to
floating seconds. After the trimmed source ends, `VideoEndBehavior::Transparent`
reveals lower layers; `Hold` holds the final trimmed frame until the layer ends.
Playback is forward at the asset rate. There is no implicit looping, optical
flow, reverse playback or speed-ramp interpretation.

`videoFrameIndexAt` returns an optional source-frame index and
`resolveVideoFrameAt` returns a read-only owned frame. Visibility does not affect
source timing. Neither function modifies the document.

## Motion graphics

Every visual layer has `LayerProperties::motion`, an ordered vector of
`MotionKeyframe`. Each key owns an absolute document `frame`, a `MotionValue`,
and the interpolation of its outgoing segment. Keys must be strictly increasing
and inside the document timeline; the first key need not be at zero. Sampling
before the first key or after the last holds the respective endpoint.

`MotionValue` exposes position, nonuniform scale, anchor, rotation in degrees
and opacity. Values must be finite; opacity must be in `[0, 1]`. Zero and
negative scale are permitted. Rotation uses unwrapped degrees, so 0 to 720
means two complete revolutions. Hold, Linear and SmoothStep (`t*t*(3-2*t)`)
are supported. Each key controls all motion channels together.

The sampled transform is:

```
baseTransform * T(position + anchor) * R(rotation) * S(scale) * T(-anchor)
```

Sampled opacity is base layer opacity multiplied by motion opacity. An empty
motion vector preserves the static transform and opacity. Asset-switch keys
and property animation are independent and can be used on the same image or
vector layer. `sampleLayerAt` returns evaluated visibility, transform and opacity.
Overflow to a nonfinite composed transform is rejected by the renderer.

This supports moving/rotating/scaling/fading image, vector and video graphics
without converting editable vectors to stored raster frames. Text layout,
path morphing, masks, effects, nested compositions and expressions are not part
of this model extension.

## Editing example

```cpp
#include <iiSharedCanvas.h>
using namespace iiSharedCanvas;

Document document;
document.extent = {1920, 1080};
document.timeline = {{24, 1}, 120};
DocumentEditor editor(document);
auto imported = importVideoAsset("clip.mov", "footage");
if (!imported.ok()) { /* report imported.result */ }
else {
    auto inserted = editor.insertVideoAsset(std::move(imported.asset));
    if (inserted.ok()) {
        VideoLayer layer{{"video", "Footage"}, StaticSource{"footage"}};
        layer.properties.frameRange = LayerFrameRange{0, 119};
        MotionKeyframe first;
        MotionKeyframe last;
        last.frame = 119;
        last.value.position = {120, 0};
        layer.properties.motion = {first, last};
        auto result = editor.insertLayer(std::move(layer));
        // Inspect result; file-bound editors commit before returning.
    }
}
auto frame = renderFrame(document, 60);
auto snapshot = encodeIisc(document);
```

`insertVideoAsset`, `replaceVideoAsset`, `setVideoPlayback` and `setLayerMotion`
validate before committing. Replacement preserves the asset id; rename/move/
remove use the existing generic asset and layer methods. Referenced media cannot
be removed. Invalid changes preserve model state, format version and revision;
identical media/playback/motion replacements do not advance the revision.
`setLayerMotion(id, {})` removes animation. Existing documents upgrade on edits
that add new fields. Direct aggregate writers must call `validate` themselves.

## Persistence, limits and interoperability

`.iisc` 1.6 preserves video frames, rate, trim/end behavior and all editable motion
keys. Readers retain 1.0–1.5 support; older readers reject 1.6. Working-file schema
1 is unchanged: new fields live in versioned asset/layer records. Unchanged
video payloads are skipped during property edits. Source and installed consumers
exercise pixel rendering, binary round trips, immediate file edits and reopen.

`SerializationLimits::maximumTotalVideoFrames` defaults to 262144 and
`maximumTotalMotionKeyframes` to 1048576. Video pixels also count toward
`maximumTotalRasterPixels`. Both encode and decode enforce these budgets; decode
checks collection counts against remaining bytes before allocating.

`importVideoAsset` reuses the reviewed bounded FFmpeg import path and returns
one detached native asset. `importVideo` retains its previous frame-owned bitmap
document API. Video imports are 8-bit display RGB at a constant sampled rate;
original compressed packets, HDR precision, variable frame timestamps and source
audio are not retained. The existing warnings disclose these conversions.
Audio remains in independent `AudioAsset` / `AudioTrackLayer` fields.

`exportVideo` renders the complete mixed canvas, including video and motion.
Native `.iisc` retains editability. PSD and timeline XML/FCPXML adapters currently
return `UnsupportedFeature` for native video/motion instead of silently dropping
their semantics. Their existing bitmap/vector hold-key behavior is unchanged.

No new runtime or link dependency was added. This is a C++23 package/ABI revision
with SOVERSION 0.11 and exact-version discovery; consumers must rebuild against
0.11.0. Existing bitmap/vector source initializers retain their field order, but
exhaustive visitors must handle the appended video alternatives.

Qt Quick `CanvasItem` uses the sampled transform for inverse brush coordinates,
so editing a moving raster changes the pixel under the current frame's displayed
position. Its async renderer shares the same video/motion evaluator. PDF export
samples the requested document frames, retains opaque vectors as paths, and
embeds the selected video frames as images; it does not preserve editable motion.
