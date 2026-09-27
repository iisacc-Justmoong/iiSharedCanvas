# Depth ControlNet layers (0.15.0 / native format 1.9)

`DepthLayer` is a dedicated `LayerRole::ControlNet` / `ControlNetKind::Depth`
layer backed by `DepthAsset`. Its representation is bitmap. A `StaticSource`
holds one depth field at every frame; a `KeyframedSource` selects a different
field through the existing timeline frame/hold and layer-range rules.

## Value contract

Depth is authored **normalized proximity**, not metric distance or a computed
inverse-distance formula. `0` means empty/open space, `1` means contact with
the canvas camera. Positive intermediate values represent occupied space,
with larger values closer to the camera. There is no far-plane geometry at
zero and no additional unknown-value sentinel. No camera calibration, clipping
planes, units, depth estimation model, or perspective reconstruction is implied.

`DepthAsset` contains an id, positive `CanvasExtent viewport`, and exactly
`width * height` finite `double` values in row-major order. The domain is [0,1].
Coordinates address native asset pixels (x right, y down). Values outside the
range, NaN/infinity, mismatched sizes, missing assets, and wrong source kinds
are rejected transactionally. Do not encode empty space using transparency.

The native field retains IEEE binary64 precision. `depthRasterPreview` and
`renderDepthControlMap` map each value to `round(value * 255)` in all RGB
channels, with alpha 255. Thus 0 = opaque black, 0.5 = RGB(128,128,128), and
1 = opaque white. This is direct linear numeric encoding: no sRGB gamma,
per-frame min/max normalization, inversion, interpolation, or alpha blending
is applied to the control output. Very small positive values can quantize to
black in RGB8; the output's original `values` and `occupiedPixels` (value > 0)
retain their distinction from truly empty space. Consumers needing full
precision should use `values` rather than recover depth from the preview.

## Editing and output

```cpp
Document document;
document.extent = {2, 2};
DocumentEditor editor(document);
auto assetResult = editor.insertDepthAsset(
    {"depth.frame0", {2, 2}, {0.0, 0.25, 0.5, 1.0}});
DepthLayer layer;
layer.properties = {"depth.layer", "Depth"};
layer.source = StaticSource{"depth.frame0"};
auto layerResult = editor.insertDepthLayer(layer);
auto editResult = editor.setDepthSample("depth.frame0", 1, 0, 0.375);
auto map = renderDepthControlMap(document, "depth.layer", 0);
// Check edit results and map.ok() before using data.
```

Use `replaceDepthAsset` for bulk edits and `setDepthSample` for an individual
pixel. Each edit uses the same validation, rollback, revision and file-bound
transaction rules as other document operations. A shared source asset edit
updates every layer/frame that references it; insert a separate asset first
when independent content is required. Colored `BitmapEditor` brushes cannot
bind a depth asset. A default-created field must explicitly size its values;
fill with 0.0 for an empty canvas.

Use `insertDepthLayer(layer, {{0,"a"},{1,"b"}})` with `KeyframedSource{}` or
`setKeyframedSource` for dynamic depth. Frame sources may have distinct native
extents; exported map dimensions are those of the selected asset. No temporal
interpolation or automatic depth normalization occurs on frame changes.

`ControlNetSettings` is shared with Semantic Segment and Pose. The control map
honors `enabled`, timeline bounds, and layer frame range. Display visibility,
opacity, blend mode and transforms do not alter native control values. Normal
artwork composition excludes the layer; explicit layer previews remain
available and can use display transforms. Output defaults to at most 16 Mi
pixels; the budget is checked before allocating RGB, scalar and occupancy
buffers (13 bytes/sample excluding allocator overhead). `depthRasterPreview`
throws for invalid data/limits; `renderDepthControlMap` reports a message.

## Persistence and interchange

Package 0.15.0 changes public variant layout (six asset and six layer variants),
so consumers must rebuild. Native format 1.9 adds asset tag 5, keyframed content
tag 3, and layer-role tag 3. Earlier formats reject depth rather than discard
it. Existing 1.0–1.8 documents remain readable.

A depth asset stores id, i32 width, i32 height, u64 sample count, then row-major
little-endian f64 samples. A depth layer uses normal properties/source plus
ControlNet enabled/modelId/modelRevision/conditioningScale/guidanceStart/
guidanceEnd fields before motion. Count, input-byte availability, address space,
and the global `SerializationLimits::maximumDepthSamples` (default 64 Mi
samples across all assets) are checked before allocation. Invalid values are
rejected by document validation on encode/decode.

Snapshot and SQLite working-file paths both preserve exact values. Working
files write the changed asset and reuse unchanged records; schema version
remains 1. PSD and timeline interchange reject unsupported ControlNet content,
including orphan depth assets in timeline packages. No external model
compatibility or generated-image quality is asserted by this scalar contract.

Detailed layer and object fields can also be edited atomically through the
[common parameter API](CONTROLNET_PARAMETERS.md), including every dynamic source state.
