# Tile ControlNet layer

Package 0.21.0 adds dedicated Tile asset/layer variants and requires consumers
to rebuild. Native model version is 1.15; SQLite working-file schema remains 1.

## Meaning and data contract

ControlNet Tile uses spatial image context to guide detail regeneration. The
[official Tile description](https://github.com/lllyasviel/ControlNet-v1-1-nightly#controlnet-11-tile)
describes local-context conditioning and detail replacement, including use with
separate tiled diffusion workflows. This SDK models that RGB reference. It does
not infer missing detail or choose a diffusion/upscaling algorithm.

- `TileColor { uint8_t red, green, blue }` stores exact encoded RGB8 values.
- `TileAsset { id, viewport, vector<TileColor> colors }` owns positive dimensions
  and exactly width*height colors in row-major spatial order. Black is valid
  content. There is no alpha, mask, repeated pattern or hidden shuffle operation.
- `TileLayer { properties, source, control }` references only TileAsset and has
  `LayerRole::ControlNet`, `ControlNetKind::Tile`. It is not a line-control layer.
  StaticSource is StaticBitmap; KeyframedSource is DynamicBitmap, with hold-only
  source selection at integer frames and a first keyframe at zero.

The reference image may use a different native resolution from the canvas.
Its pixels are stored as authored; model-specific resizing, blur/downsampling,
color conversion, tiled inference, overlap blending and output merging belong to
an explicit consuming adapter. No implicit preprocessing occurs on render/open.

## Preparing and editing

`makeTileAsset(id, opaqueRaster, maxPixels=16Mi)` copies the source RGB without
changing spatial layout, detail or resolution. Transparent input is rejected so
the caller chooses its compositing background explicitly. Malformed dimensions
or an empty id throw invalid_argument, and output budget overflow throws
length_error before allocating colors. Direct aggregate construction is also
supported for prepared RGB data.

Use `insertTileAsset`, `replaceTileAsset`, `insertTileLayer`, `setTileSample`,
`setKeyframedSource` and `setControlNetSettings` for validated edits. Asset
replacement preserves id. Invalid coordinates, dimensions, references and
settings leave state/revision unchanged. Bound DocumentEditor operations commit
synchronously to DocumentFile. BitmapEditor cannot bind the dedicated Tile asset.
Common model id/revision, conditioning scale and guidance interval apply.

```cpp
Document doc;
doc.extent = {2,2};
DocumentEditor editor(doc);
RasterLayer input = {2,2,{0xffff0000,0xff00ff00,0xff0000ff,0xffffffff}};
auto assetResult = editor.insertTileAsset(makeTileAsset("reference",input));
TileLayer layer;
layer.properties = {"detail-control", "Tile reference"};
layer.source = StaticSource{"reference"};
auto layerResult = editor.insertTileLayer(layer);
auto full = renderTileControlMap(doc,"detail-control",0);
auto region = renderTileControlRegion(doc,"detail-control",0,{1,0,1,2});
// Check each edit result and output's ok() before consuming its data.
```

## Full and region output

`tileRasterPreview` produces an opaque RGB preview. `renderTileControlMap`
returns the full current-frame reference, with `region={0,0,width,height}`.
`renderTileControlRegion` accepts `TileRegion { x,y,width,height }` in native
asset pixels and copies only that rectangle. The result owns `pixels`, exact
`colors`, source `region`, and failure `message`. Overlapping requests return
identical colors for the same source coordinates. Region dimensions must be
positive and wholly in bounds; no clipping, padding, repetition or resizing is
performed. Bounds arithmetic uses widened sums to reject oversized rectangles.

Both document exports honor control.enabled, layer frame range and timeline.
Display visibility, opacity, affine transforms and motion do not alter control
pixels. The default 16 Mi pixel budget applies to the requested output, so a
small region can be read with a smaller budget than the full image requires.
Region export does not allocate a temporary full-image output. Export failures
return a non-ok result; the direct preview throws invalid_argument or length_error.

General artwork/CanvasItem/PDF composition excludes ControlNet layers; explicit
per-layer previews remain available with display properties. PSD and timeline
interchange reject unsupported Tile semantics, including orphan Tile assets in
timeline packages. Snapshot and working-file round trips preserve all pixels and
source keyframes. Unchanged image records are reused for control-only edits.

## Verification boundary

TileTest verifies exact full/region RGB, overlap consistency, small crop budgets,
invalid and overflow-sized regions, opaque import, typed/static/dynamic sources,
control/display separation, rejection rollback, snapshot version/tag/count
failures with valid checksums, working-file reopening and record reuse, and
unsupported foreign exports. The installed-package consumer runs the same test.
Actual diffusion, super-resolution quality and tiled-output blending are outside
this SDK object and export verification.

Detailed layer and object fields can also be edited atomically through the
[common parameter API](CONTROLNET_PARAMETERS.md), including every dynamic source state.
