# Canny and Scribble ControlNet layers (0.17.0 / format 1.11)

This document covers three pixel-based line controls. `isLineControlNetLayer(layer)` identifies
Line Art, Canny, Scribble and (since 0.18.0) MLSD without grouping Depth, Pose
or Semantic Segment. MLSD uses editable vector segments; see MLSD.md.
All retain independent `ControlNetKind` identities and normal shared settings.

| Type | Source | Native values | Control output |
| --- | --- | --- | --- |
| LineArtLayer | LineArtAsset | f64 coverage [0,1] | black ink on white, soft levels retained |
| CannyLayer | CannyAsset | u8 mask, exactly 0 or 1 | white edges on opaque black |
| ScribbleLayer | ScribbleAsset | u8 mask, exactly 0 or 1 | white strokes on opaque black |

The binary source types have public aggregate fields `id`, positive `viewport`
and `std::vector<std::uint8_t> mask`, containing exactly width * height row-major
samples. Zero means background and one means a line. A default sized field
filled with zero is blank black. Values 2–255 are invalid: this is a binary
logical field, not an image-byte array containing 0 and 255. Convert incoming
0/255 image bytes explicitly before inserting an asset. Do not use transparency
for background or save raw input/pointer trajectories as strokes.

The two source types are not aliases and cannot substitute for each other or
for raster/Line Art data. Validation rejects wrong source kinds, missing assets,
invalid dimensions and nonbinary samples. Shared implementation of binary mask
validation/rendering is an internal detail, not a public base class.

## Authoring and timeline

```cpp
Document document;
document.extent = {2, 2};
DocumentEditor editor(document);
auto a = editor.insertCannyAsset({"edges", {2, 2}, {0, 1, 1, 0}});
CannyLayer edges;
edges.properties = {"edges.layer", "Canny"};
edges.source = StaticSource{"edges"};
auto b = editor.insertCannyLayer(edges);
auto c = editor.setCannySample("edges", 0, 0, 1);
auto output = renderCannyControlMap(document, "edges.layer", 0);
// Check a/b/c results and output.ok().
```

Scribble offers the corresponding `insertScribbleAsset`, `replaceScribbleAsset`,
`insertScribbleLayer`, `setScribbleSample`, `renderScribbleControlMap` APIs.
Canny also provides `replaceCannyAsset` for bulk edits. Set-sample APIs accept a
u8 logical value; callers must not narrow unvalidated wider input. Rejected
edits preserve state/revision. Bound DocumentEditor operations synchronously
persist through DocumentFile. Shared source edits affect all references; create
a separate asset for independent content.

StaticSource keeps one asset; KeyframedSource selects assets by existing hold
sampling, e.g. `insertCannyLayer(layer, {{0,"a"},{1,"b"}})` with a keyframed
source. Both types support `StaticBitmap` and `DynamicBitmap`. Frames may use
different native extents. Frame range/timeline and control enabled are honored.
No temporal interpolation is applied to binary masks.

Control output returns opaque RGB32 pixels plus the exact native binary `mask`.
There is no resizing, smoothing, thresholding, inversion or normalization in
this output. Display opacity, visibility, transforms and blending do not alter
conditioning data. Normal artwork excludes ControlNet layers; explicit layer
previews are available. Colored BitmapEditor brushes cannot bind typed masks.

Raster preview throws for invalid input/budget; render-control-map returns an
error message. The default output limit is 16 Mi pixels, checked before dense
allocation (4 bytes RGB + 1 byte mask per pixel, excluding allocator overhead).
Serialization independently caps each kind at 64 Mi total samples through
maximumCannySamples/maximumScribbleSamples, checked with remaining byte and
address-space bounds before allocation.

## Persistence and scope

Package 0.17.0 has nine asset and nine layer variants and requires rebuilding
consumers. Format 1.11 adds Canny/Scribble asset tags 7/8 and content/role tags
5/6. After id, an asset contains i32 dimensions, u64 count and raw u8 logical
samples. Layers store the common ControlNet settings before motion. Older
formats reject these types. Existing 1.0–1.10 records remain readable. Native
snapshot and SQLite working-file round trips preserve type, settings, timeline
and masks; unchanged records are reused. SQLite schema stays 1. PSD and timeline
interchange reject unsupported conditioning content, including orphan binary
assets in timeline packages, rather than silently dropping it.

This implementation is for authored/stored conditioning objects. It does not
run the Canny detector, derive scribbles from photos, or perform diffusion
inference; no detector thresholds are persisted as if extraction had occurred.

The black-background/white-line convention follows the original ControlNet
conditioning path (its gallery preview is inverted separately):
- https://github.com/lllyasviel/ControlNet/blob/main/gradio_canny2image.py
- https://github.com/lllyasviel/ControlNet/blob/main/gradio_scribble2image.py
No model-specific generation quality is implied by the data contract.

Detailed layer and object fields can also be edited atomically through the
[common parameter API](CONTROLNET_PARAMETERS.md), including every dynamic source state.
