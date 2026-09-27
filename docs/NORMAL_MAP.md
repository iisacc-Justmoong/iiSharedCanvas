# Normal Map ControlNet layer

Package 0.19.0 adds `NormalMapAsset` and `NormalMapLayer` to the public variants
and requires consumers to rebuild. Native model version is 1.13; SQLite working
file schema remains 1. No new dependency, normal estimator or inference runtime
is included.

## Native data contract

`NormalMapSample { double x=0, y=0, z=1; bool valid=true; }` describes a surface
normal in a fixed canvas-camera basis: +X right, +Y down, +Z toward the viewer.
These are signed direction components, not RGB colors, metric depth, or a
mesh tangent-space texture. Negative Z is supported. Each valid component is
finite and within [-1,1], with absolute vector-length error <= 1e-6. A zero
vector is not a valid normal. Missing samples use `{0,0,0,false}` exclusively.
The SDK rejects invalid data instead of silently normalizing or filling holes.

`NormalMapAsset { id, viewport, samples }` owns a positive width/height and
exactly width*height row-major samples. It preserves all authored binary64
components and validity through snapshot and working-file round trips.
`NormalMapLayer { properties, source, control }` references only NormalMapAsset.
`StaticSource` produces `StaticBitmap`; `KeyframedSource` produces
`DynamicBitmap` with hold-only source changes at integer timeline frames.
It has `LayerRole::ControlNet` and `ControlNetKind::NormalMap`, and is not a
line-control layer. All shared ControlNet settings are supported.

## Rendering and model adapters

`normalMapRasterPreview(asset, options={})` and
`renderNormalMapControlMap(document, layerId, frame, options={})` produce opaque
RGB8 pixels. Each selected signed component maps by `round((n+1)*127.5)`.
The default RGB order is X,Y,Z; a front-facing `(0,0,1)` becomes `#8080FF`.
Missing samples become opaque black. `NormalMapControlMapResult` additionally
returns exact native `samples`, `validPixels` (0/1), and a failure `message`.
Consumers must use the validity mask to distinguish missing content.

`NormalMapRenderOptions` supports `channelOrder = Xyz | Zyx`, `flipY = false`,
and `maximumPixels = 16*1024*1024`. Y inversion happens before quantization;
channel order changes output RGB only. Exact returned samples remain untouched.
Unknown channel enums and output sizes exceeding the budget are rejected.
Preview throws invalid_argument for invalid input/options and length_error for
budget failures; document control export returns a non-ok result.

Channel convention must be chosen by the consuming model adapter. The original
[ControlNet MiDaS annotator](https://github.com/lllyasviel/ControlNet/blob/main/annotator/midas/__init__.py)
normalizes XYZ and converts it to image channels, while the original
[normal2image pipeline](https://github.com/lllyasviel/ControlNet/blob/main/gradio_normal2image.py)
reverses those channels before constructing its conditioning tensor. `Zyx`
allows that explicit reversal. This is not a universal model preset or a claim
of bit-identical MiDaS preprocessing: the reference truncates channels and this
SDK rounds them; estimators, basis conversion, resizing and background policies
remain adapter responsibilities.

Exports use the native asset grid and honor `control.enabled`, timeline and
layer frame range. Display visibility, opacity, affine transforms and motion
do not rotate/resample normals or alter conditioning. Ordinary artwork/CanvasItem
composition and PDF exclude the layer; explicit layer preview is available and
uses the default XYZ colors with normal display transforms. PSD and timeline
interchange reject unsupported normal content rather than discarding it.

## Editing example

```cpp
Document doc;
doc.extent = {2, 1};
doc.timeline.frameCount = 2;
DocumentEditor editor(doc);
auto assetResult = editor.insertNormalMapAsset(
    {"normals", {2,1}, {{0,0,1}, {0.6,0,0.8}}});
NormalMapLayer layer;
layer.properties = {"normal-layer", "Surface normals"};
layer.source = StaticSource{"normals"};
auto layerResult = editor.insertNormalMapLayer(layer);
auto editResult = editor.setNormalMapSample("normals", 1, 0, {0,1,0});
auto map = renderNormalMapControlMap(doc, "normal-layer", 0);
// Check each edit result's ok() and map.ok() before consuming the output.
```

`replaceNormalMapAsset` replaces the complete sample grid while preserving id.
`setNormalMapSample` replaces one coordinate through the same validated edit
transaction. `setKeyframedSource` switches a layer to per-frame assets;
`setControlNetSettings` edits conditioning configuration. Invalid vectors,
coordinates, references and mixed asset kinds leave state/revision unchanged.
When DocumentEditor is bound to DocumentFile, these operations synchronously
persist the same transaction. `BitmapEditor` cannot bind or paint this data.

## Verification

`NormalMapTest.cpp` covers exact RGB values, signed components, missing samples,
channel conversion, invalid data rollback, source typing, static/dynamic
resolution, output/serialization budgets, snapshot and working-file round trips,
checksum-correct malformed payload rejection, display separation and unsupported
foreign export. The installed-package consumer runs the same contract test.

Detailed layer and object fields can also be edited atomically through the
[common parameter API](CONTROLNET_PARAMETERS.md), including every dynamic source state.
