# Line Art ControlNet layers (0.16.0 / native format 1.10)

`LineArtLayer` is a dedicated `LayerRole::ControlNet` / `ControlNetKind::LineArt`
layer. It references a `LineArtAsset`, whose id, positive viewport dimensions
and row-major `std::vector<double> coverage` describe the authored line image.
The source must contain exactly width * height finite samples in [0,1].

## Ink and background

Coverage 0 is clean white background; 1 is full black ink. Intermediate values
represent partial ink coverage/line intensity, retaining antialiased edges and
soft contours. This is a line-art field, not distance, a class mask, or a list
of input brush movements. A filled dark area is legal; no edge detection,
thinning, thresholding, automatic inversion, gamma conversion or per-frame
normalization is performed.

Native coverage retains binary64 precision. `lineArtRasterPreview` and
`renderLineArtControlMap` compute `round((1 - coverage) * 255)` for each RGB8
channel with alpha 255. Coverage 0/0.5/1 therefore produces white/128-gray/black.
Blank regions are opaque white, not transparent. A very small positive coverage
may round to white, so the control result also carries original `coverage` and
`inkPixels` (coverage > 0). These preserve weak authored lines independently of
preview quantization. Do not interpret `inkPixels` as a thresholded edge map.

## Static/dynamic editing

```cpp
Document document;
document.extent = {2, 2};
DocumentEditor editor(document);
auto assetResult = editor.insertLineArtAsset(
    {"lines.frame0", {2, 2}, {0.0, 0.5, 1.0, 0.0}});
LineArtLayer layer;
layer.properties = {"lines", "Line Art"};
layer.source = StaticSource{"lines.frame0"};
auto layerResult = editor.insertLineArtLayer(layer);
auto editResult = editor.setLineArtSample("lines.frame0", 1, 0, 0.75);
auto map = renderLineArtControlMap(document, "lines", 0);
// Check operation results and map.ok() before using the output.
```

Static content keeps one asset throughout the timeline. Dynamic content uses
`KeyframedSource{}` plus `insertLineArtLayer(layer, {{0,"a"},{1,"b"}})` or
`setKeyframedSource`; reference sampling follows existing hold-only timeline
rules. These map to `StaticBitmap` and `DynamicBitmap`. Each frame asset has
its own native dimensions. No temporal interpolation is introduced.

Use `setLineArtSample` for individual pixels and `replaceLineArtAsset` for bulk
updates. All edits validate and roll back atomically, including revisions and
file-bound transactions. An asset shared by multiple frames/layers changes for
all references; create a separate asset when independent edits are needed.
Fill a newly sized coverage array with 0.0 to create a blank white field.
`BitmapEditor` cannot bind this typed scalar asset. New domain code has no Qt
dependency; previews reuse the existing rendering stack.

Control output honors `ControlNetSettings::enabled`, timeline and frame range,
and ignores display visibility, opacity, transform and blending. Ordinary
artwork composition excludes ControlNet layers; explicit layer previews remain
available. Output allocation defaults to at most 16 Mi pixels (RGB8, binary64
coverage and u8 presence: 13 bytes/sample before allocator overhead).
`lineArtRasterPreview` throws invalid_argument/length_error for invalid input or
budget violations; `renderLineArtControlMap` returns an error message.

## Files and boundaries

Package 0.16.0 has seven asset and layer alternatives, requiring a consumer
rebuild. Format 1.10 adds asset tag 6, keyframed content tag 4, layer role tag 4.
The asset stores id, i32 width/height, u64 count and little-endian f64 coverage.
The layer stores shared ControlNet enabled/modelId/modelRevision/scale/guidance
fields before normal motion data. Earlier formats reject Line Art; existing
1.0–1.9 data remains readable. Unknown tags and invalid source kinds fail closed.

`SerializationLimits::maximumLineArtSamples` defaults to 64 Mi samples across
all assets. Counts, available bytes and address-space bounds are checked before
allocation. Snapshot and SQLite working-file edits preserve original coverage,
reuse unchanged records, and retain SQLite schema 1. PSD and timeline exchange
reject unsupported ControlNet semantics, including orphan line-art assets in
timeline packages, rather than silently flatten or discard them.

This API supplies authored conditioning data. It does not run an image-to-line
annotator, trace vectors, execute a diffusion model, or assert compatibility
with a particular model's input convention. The canonical output is black ink
on white; model-specific conversion belongs to an explicit consumer adapter.

Detailed layer and object fields can also be edited atomically through the
[common parameter API](CONTROLNET_PARAMETERS.md), including every dynamic source state.
