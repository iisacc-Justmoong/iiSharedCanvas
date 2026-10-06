# Four concrete layer/content types (0.28.0)

The artwork model has exactly four concrete C++ alternatives. Each owns its own
content type; there is no mutable static/dynamic source variant inside them.

| Layer | Content | Allowed payload | Timeline behavior |
| --- | --- | --- | --- |
| `StaticBitmapLayer` | `StaticBitmapContent` | `RasterAsset` or `ChunkedRasterAsset` | One asset for all frames |
| `StaticVectorLayer` | `StaticVectorContent` | `VectorAsset` with native paths/paint | One asset for all frames |
| `DynamicBitmapLayer` | `DynamicBitmapContent` | Frame-selected raster or chunked raster assets | Each content key selects pixels |
| `DynamicVectorLayer` | `DynamicVectorContent` | Frame-selected native vector assets | Each content key selects paths/paint |

`content.assetId` belongs to either static content type. `content.frameIndices`
belongs to either dynamic content type. The common source bases only share
reference/index storage. The four content types cannot be substituted for one
another. The document owns full asset payloads and `Document::frames` owns exact
`{layerId, assetId}` keys; content does not duplicate those payloads or keys.
Validation checks every referenced asset against the concrete layer type and
checks the complete increasing frame index against the frame-owned keys.

Static means the same content across the timeline, rather than immutable pixels.
Motion and an existence range are independent of content identity. Dynamic keys
use hold sampling, begin at frame zero, and may be present on every frame. A
one-key dynamic layer retains its dynamic type. `setDynamicFrameContent` commits a
fresh raster/vector payload and the exact-frame key atomically. Editing one
independent asset does not modify other frame assets.

```cpp
Document document;
document.extent = {640, 480};
document.timeline.frameCount = 24;
DocumentEditor editor(document);
editor.insertAsset(RasterAsset{"background", makeRasterLayer(640, 480, 0xff000000U)});
editor.insertLayer(StaticBitmapLayer{{"background-layer"}, StaticBitmapContent{"background"}});
editor.insertDynamicLayer({"animation"}, LayerRepresentation::Bitmap, {{0, "background"}});
editor.setDynamicFrameContent("animation", 1,
    RasterAsset{"frame-1", makeRasterLayer(640, 480, 0xffff0000U)});
const auto *animation = findDynamicBitmapLayer(document, "animation");
// animation->content.frameIndices == {0, 1}
```

## Editing and migration

Use `DocumentEditor::setStaticSource` or `setKeyframedSource` to convert timing.
They replace the concrete layer alternative and its content together, retain
properties, rebuild/remove frame-owned keys, and validate atomically. Failure
restores the previous type, frame records, revision and authorship. File-bound
edits commit synchronously through the existing incremental transaction.

`findStaticBitmapLayer`, `findStaticVectorLayer`, `findDynamicBitmapLayer` and
`findDynamicVectorLayer` return only their exact alternative. `layerKind` returns
`std::optional<LayerKind>` containing one of the four values for artwork.
`layerTiming` and `layerRepresentation` remain independent queries.

This is a breaking C++ model/API change: rebuild against package **0.28.0**, ABI
**0.28**. Replace `BitmapLayer`/`VectorLayer` with the appropriate concrete type and
replace `.source` with `.content`. `layerSource` now returns a detached source
value; assigning to it does not mutate a document. `staticLayerSource` and
`keyframedLayerSource` borrow reference/index storage without allocating.
`makeBitmapLayer`/`makeVectorLayer` convert a source carrier into the correct
concrete aggregate. `setLayerSource` performs aggregate conversion but does not
validate or manage document-owned keys; use the editor for structural edits.

Video and existing ControlNet specializations retain their media/conditioning
contracts. Video reports dynamic bitmap identity; spatial conditioning maps its
timing and representation to the same four visual kinds. IP-Adapter tensor
conditioning has `LayerRepresentation::Embedding` and `layerKind == nullopt`;
it does not add artwork kinds or masquerade as bitmap/vector pixels.

## Persistence and verification

Native **1.19** records store the explicit visual type byte immediately after
artboard membership. Tags 0/1/2/3 mean static bitmap/static vector/dynamic bitmap/
dynamic vector. Tag 255 identifies nonspatial conditioning. Decoders reject an
unknown tag or a declared type that disagrees with source/asset/role, even when
the checksum is valid. Working-file schema remains 1 and stores the same typed
layer record in its existing incremental store.

All legacy 1.0–1.18 versions infer concrete memory types from their existing
source/asset/role fields. Re-encoding an untouched legacy document retains its
version and canonical bytes. An accepted editor change advances to 1.19.

`iiSharedCanvas.FourLayerType` verifies distinct compile-time content types,
all four rendered behaviors, canonical current/legacy round trips, valid-checksum
type corruption rejection, successful/failed concrete conversions, and working
file reopen. Existing per-frame content, rendering, conditioning, layered media,
artboard and installed-package suites cover the migrated paths. The standalone
consumer also runs `iiSharedCanvas.InstalledFourLayerType` against the installed
headers and shared library.
