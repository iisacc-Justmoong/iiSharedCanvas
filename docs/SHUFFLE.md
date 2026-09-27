# Shuffle ControlNet layer

Package 0.20.0 adds dedicated Shuffle variants; consumers must rebuild. Native
model version is 1.14, SQLite working-file schema remains 1. No new dependency,
model download or inference backend is introduced.

## Data and layer identity

Shuffle conditions generation on an RGB content/reference image. The native
object is the **prepared conditioning image**, with no hidden preprocessing:

- `ShuffleColor { uint8_t red, green, blue }` stores exact RGB8 channels.
- `ShuffleAsset { id, viewport, vector<ShuffleColor> colors }` owns a positive
  width/height and exactly width*height row-major colors. No alpha or color
  normalization is applied; an all-black image is valid content.
- `ShuffleLayer { properties, source, control }` references ShuffleAsset only.
  It is `LayerRole::ControlNet`, `ControlNetKind::Shuffle`, and either
  `StaticBitmap` or `DynamicBitmap`. It is not a line-control layer.

Static layers retain one image. Keyframed sources hold the selected image until
the next asset-reference keyframe; ordinary frame selection never randomizes
content. All shared model id/revision, conditioning scale and guidance settings
are available. Source dimensions may differ from the canvas dimensions.

## Preparing a shuffled image

The official [ContentShuffleDetector](https://github.com/lllyasviel/ControlNet-v1-1-nightly/blob/main/annotator/shuffle/__init__.py)
remaps the source through noise-derived X/Y coordinates using linear sampling.
This SDK provides the explicit remapping operation, not a bit-identical copy of
the NumPy/OpenCV random preprocessing pipeline.

`makeShuffleAsset(id, source, outputExtent, coordinates, maxPixels=16Mi)` accepts
an opaque RasterLayer and one `ShuffleCoordinate { double u, v }` per output
pixel. Coordinates must be finite within [0,1]. They map to source pixel centers
`(u*(width-1), v*(height-1))`; four surrounding RGB values are interpolated
bilinearly, then rounded to the nearest integer. Endpoints and one-pixel sources
are supported. Interpolation is in encoded RGB channel space, not linear light.
Transparent input is rejected; the caller explicitly composites its background.

The caller supplies the field, including any seeded noise. The returned detached
asset contains final colors and owns no external source reference. Neither the
source nor coordinate field is changed. Persist it with insertShuffleAsset or
replaceShuffleAsset. Prepared maps from an external detector can also be stored
directly. No random seed, source trajectory or regeneration command is persisted.

```cpp
Document doc;
doc.extent = {2,1};
DocumentEditor editor(doc);
RasterLayer source = {2,1,{0xffff0000,0xff0000ff}};
auto prepared = makeShuffleAsset("colors", source, {2,1}, {{1,0},{0,0}});
auto assetResult = editor.insertShuffleAsset(std::move(prepared));
ShuffleLayer layer;
layer.properties = {"shuffle", "Shuffle reference"};
layer.source = StaticSource{"colors"};
auto layerResult = editor.insertShuffleLayer(layer);
auto map = renderShuffleControlMap(doc,"shuffle",0); // blue, red
// Check edit results and map.ok() before using their outputs.
```

## Editing, rendering and persistence

`setShuffleSample` edits one color through a validated DocumentEditor transaction.
`replaceShuffleAsset` replaces a full image preserving id. `insertShuffleLayer`,
`setKeyframedSource` and `setControlNetSettings` follow shared contracts. Failed
coordinates, dimensions, references or settings preserve data and revision.
DocumentFile-bound edits commit synchronously; unchanged color data is reused
for control-only edits. BitmapEditor cannot bind this dedicated asset type.

`shuffleRasterPreview` returns exact opaque RGB pixels. `renderShuffleControlMap`
returns those pixels, exact colors and message/ok status. Both default to a 16 Mi
output pixel budget, checked before output allocation. Control export honors
enabled, frame range and timeline but ignores display visibility, opacity and
affine/motion transforms. It does not reshuffle, normalize or resize the image.
The remapping and direct-preview APIs throw invalid_argument for malformed input
and length_error for an exceeded output budget; document export returns failure.

General artwork/CanvasItem/PDF composition excludes ControlNet layers. Explicit
per-layer preview remains available. PSD and timeline interchange reject Shuffle
content to avoid silent loss, including orphan assets in timeline interchange.
Native snapshots and working files preserve every RGB channel and source keyframe.

## Verification

ShuffleTest covers exact channel output, remap identity/corner/center/one-pixel
sampling, invalid coordinates/alpha/counts, output and accumulated serialization
budgets, static/dynamic classification and selection, control/display separation,
transaction rollback, malformed checksum-correct snapshots, working-file reopening,
record reuse and unsupported foreign export. The installed consumer runs the same
contract test. This does not establish actual ControlNet inference quality.

Detailed layer and object fields can also be edited atomically through the
[common parameter API](CONTROLNET_PARAMETERS.md), including every dynamic source state.
