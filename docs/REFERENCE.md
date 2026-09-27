# Reference conditioning layer

Package 0.22.0 introduces native Reference asset/layer variants. Consumers must
rebuild; snapshot model is 1.16 and SQLite working-file schema remains 1.

## Meaning

A Reference layer supplies an image and a reference-application contract to an
inference adapter. The original extension describes [reference-only attention
control](https://github.com/Mikubill/sd-webui-controlnet/discussions/1236) and
[AdaIN / attention+AdaIN](https://github.com/Mikubill/sd-webui-controlnet/discussions/1280).
These modes need not load a dedicated ControlNet checkpoint. `control.modelId`
is optional and may be empty; no model name is invented or downloaded.

The layer belongs to the SDK's ControlNet conditioning family so it shares
selection, enable/weight/guidance, timeline, persistence and export contracts.
It stores RGB reference pixels rather than embeddings or learned feature maps.
It does not implement attention hooks, AdaIN, IP-Adapter, face identity matching,
style extraction, VAE encoding or model inference. A consuming adapter must
explicitly support the requested mode; it must not silently substitute another
conditioning technique. Model-specific support and effective fidelity behavior
(including CFG/control-mode interactions) remain adapter responsibilities.

## Objects

`ReferenceColor { uint8_t red, green, blue }` stores RGB8. `ReferenceAsset { id,
viewport, colors }` owns positive dimensions and exactly width*height row-major
colors. Its native resolution can differ from the document canvas. All channels
are preserved exactly; opaque black is valid reference content.

`ReferenceSettings` contains:

| Field | Contract |
| --- | --- |
| mode | Attention (default), AdaIN, or AttentionAdaIN |
| styleFidelity | finite [0,1], default 0.5; reference-style fidelity requested from the adapter |

Attention maps to `reference_only`, AdaIN to `reference_adain`, and AttentionAdaIN
to `reference_adain+attn`. Fidelity is not an opacity, RGB filter or pixel blend.
Unknown enum values, NaN/infinite fidelity and out-of-range values are invalid.

`ReferenceLayer { properties, source, control, reference }` owns both common
ControlNet settings and reference-specific settings. Role is ControlNet, kind is
Reference; it is not a line-control layer. StaticSource produces StaticBitmap,
KeyframedSource produces DynamicBitmap. Integer-frame source changes are hold-only
and begin at frame zero. Reference settings are layer-wide; only image content
changes through source keyframes.

## Editing and output

`makeReferenceAsset(id, opaqueRaster, maxPixels=16Mi)` copies native RGB without
resizing, cropping, color normalization or feature extraction. Transparent inputs
are rejected so the caller chooses its compositing background. Invalid inputs
throw invalid_argument; exceeding the output budget throws length_error before
allocation. Aggregate construction supports prepared RGB images directly.

Use insertReferenceAsset, replaceReferenceAsset, insertReferenceLayer,
setReferenceSample and setReferenceSettings through DocumentEditor. Common
setKeyframedSource and setControlNetSettings also apply. Invalid edits preserve
state/revision. File-bound edits commit synchronously; changing either set of
layer settings reuses unchanged image records.

`referenceRasterPreview` returns opaque RGB pixels. `renderReferenceControlMap`
returns pixels, exact colors, `reference` settings, `control` settings and message/
ok status. It selects the current-frame image and honors enabled/frame range/
timeline. Display visibility, opacity, affine transforms and motion do not alter
conditioning. Style fidelity never changes the exported RGB. Both outputs default
to a 16 Mi pixel allocation budget. The direct preview throws invalid_argument or
length_error; document export returns a non-ok result.

```cpp
Document doc;
doc.extent = {2,1};
DocumentEditor editor(doc);
RasterLayer input = {2,1,{0xffbb8844,0xff226699}};
auto assetResult = editor.insertReferenceAsset(makeReferenceAsset("reference",input));
ReferenceLayer layer;
layer.properties = {"reference-layer", "Style reference"};
layer.source = StaticSource{"reference"};
layer.reference = {ReferenceMode::AttentionAdaIN,0.7};
auto layerResult = editor.insertReferenceLayer(layer);
auto output = renderReferenceControlMap(doc,"reference-layer",0);
// Check edit results/output.ok(); send output pixels and settings to the adapter.
```

General artwork/CanvasItem/PDF composition excludes the layer; explicit per-layer
preview is available. PSD and timeline interchange reject unsupported Reference
semantics, including orphan reference assets in timeline packages. BitmapEditor
cannot bind this dedicated data type. Native snapshots and working files preserve
RGB, frame references, both settings objects and exact binary64 fidelity.

## Verification

ReferenceTest covers exact RGB, opaque import, typed static/dynamic sources,
all three mode round trips, fidelity bounds and mode rejection with transaction
rollback, malformed checksum-correct mode/value/tag/count data, serialization/output
budgets, working-file reopening and settings-only record reuse. The installed
consumer runs the same contract. Actual reference-guided generation is not tested.

Detailed layer and object fields can also be edited atomically through the
[common parameter API](CONTROLNET_PARAMETERS.md), including every dynamic source state.
