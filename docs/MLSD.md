# MLSD line-segment ControlNet (0.18.0 / format 1.12)

`MlsdSegment`, `MlsdAsset` and `MlsdLayer` preserve editable straight-line
geometry. MLSD belongs to the line-control family (`isLineControlNetLayer`) and
has its own `ControlNetKind::Mlsd` and `ContentKind::Mlsd`. Binary Canny/Scribble
masks cannot substitute for its geometry. Static and keyframed MLSD layers are
`StaticVector` and `DynamicVector` respectively.

## Geometry

Each segment has a nonempty unique id within its asset, normalized x1/y1/x2/y2
endpoints, confidence in [0,1], and enabled flag. All numbers are finite. X runs
right, Y runs down. Endpoints must differ, but intersections, coincident lines
with distinct ids, and disconnected segments are legal. Confidence controls
selection, not grayscale brightness. Disabled segments remain persisted.

An asset owns its id, positive native viewport and ordered segment vector.
An empty vector is a valid blank scene. At most 100000 segments are allowed per
asset; serialization limits default to 1000000 segments across the document.
No detector model, source photo, arbitrary curve, or input event replay is
required to reconstruct it. Endpoints are binary64 and survive exact round trips.

## Rendering and edits

`renderMlsdControlMap` outputs opaque black with one-pixel opaque white straight
segments, plus the selected segment objects in source order. Raster endpoints
are rounded from `coordinate * (extent - 1)` so normalized 0/1 address the first
and last pixel centers. Bresenham stepping is deterministic, with no antialiasing,
thresholding or per-frame normalization. Subpixel geometry may collapse to one
pixel at small resolution but is retained in the asset. Line direction may
change tie-breaking for oblique raster lines; no OpenCV pixel-equivalence is
claimed. The vector display preview draws black background and white strokes
through the existing renderer; its antialiasing may differ from the control map.

`MlsdRenderOptions` includes minimumConfidence (default 0), maximumPixels
(default 16 Mi pixels) and maximumRasterSteps (default 64 Mi pixel visits).
Only enabled segments meeting confidence are emitted. Both output size and the
sum of visits, including overlap, are checked before raster allocation/drawing.
This bounds work even when many long segments share the same small canvas.
`mlsdRasterPreview` throws invalid_argument/length_error; the control-map API
returns a message on failure. `mlsdVectorPreview` validates before creating paths.

```cpp
Document doc;
doc.extent = {512, 512};
DocumentEditor editor(doc);
auto a = editor.insertMlsdAsset({"room.lines", {512, 512}, {
    {"ceiling", 0.1, 0.2, 0.9, 0.2, 1.0, true},
    {"wall", 0.1, 0.2, 0.1, 0.9, 0.95, true}
}});
MlsdLayer layer;
layer.properties = {"mlsd", "MLSD"};
layer.source = StaticSource{"room.lines"};
auto b = editor.insertMlsdLayer(layer);
auto c = editor.setMlsdSegment("room.lines", "wall",
    {"wall", 0.15, 0.2, 0.15, 0.9, 0.95, true});
auto output = renderMlsdControlMap(doc, "mlsd", 0);
// Check a/b/c and output.ok().
```

`replaceMlsdAsset` supports bulk addition/removal/reordering. `setMlsdSegment`
updates an existing segment while preserving its id. All edits validate and
roll back atomically, including editor revision and file transactions. Shared
asset edits affect every reference; create a separate asset for independent
content. `KeyframedSource{}` with `insertMlsdLayer(layer, {{0,"a"},{1,"b"}})`
or `setKeyframedSource` enables existing hold-only frame selection. Each source
can retain a distinct viewport. There is no implicit endpoint interpolation.

Control output honors control.enabled, frame range and timeline. It ignores
display visibility, opacity, transform and blend mode. Ordinary artwork excludes
ControlNet layers; per-layer previews remain available. Shared ControlNet model
identity/strength/guidance settings are persisted with the layer.

## Native storage and boundaries

Package 0.18.0 has ten asset and layer alternatives, requiring a consumer rebuild.
Native format 1.12 adds asset tag 9 and content/role tag 7. An asset stores id,
i32 width/height, u32 segment count, then each segment's string id, five f64
coordinates/confidence values, and enabled u8 boolean. Counts, remaining bytes
and maximumMlsdSegments are checked before allocation. Malformed boolean values,
unknown tags, invalid geometry and unsupported older versions fail closed.
Earlier 1.0–1.11 documents remain readable. SQLite working-file schema remains 1;
unchanged geometry records are reused, and segment edits persist synchronously.
PSD/timeline interchange reject loss of MLSD semantics, including orphan MLSD
assets in timeline packages.

This is an authored geometry/control-map implementation, not automatic MLSD
model inference. The white-segment-on-black convention follows the official
ControlNet MLSD annotator, which draws each detected endpoint pair as a line:
https://github.com/lllyasviel/ControlNet/blob/main/annotator/mlsd/__init__.py
No model download, detector thresholds or generated-image quality is implied.

Detailed layer and object fields can also be edited atomically through the
[common parameter API](CONTROLNET_PARAMETERS.md), including every dynamic source state.
