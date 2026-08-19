# iiSharedCanvas data and mutation API

iiSharedCanvas exposes its canonical canvas data as C++20 aggregate types and
adds validated editors for callers that do not want to maintain cross-reference
invariants manually. The aggregates are the serialized truth; the editors are
convenience and safety boundaries over the same data rather than a second model.

## Data ownership and identity

`Document` owns all persisted canvas state:

| Data | Public fields | Meaning |
| --- | --- | --- |
| `FormatVersion` | `major`, `minor` | Physical/model compatibility version |
| `CanvasExtent` | `width`, `height` | Positive output size in pixels |
| `Timeline` | `frameRate`, `frameCount` | Rational playback rate and integer frame domain |
| `RasterAsset` | `id`, `pixels` | Stable id and iiPaintEngine ARGB pixel storage |
| `VectorAsset` | `id`, `viewport`, `paths` | Stable id and native vector paint data |
| `VectorPath` | `commands`, `fill`, `stroke` | M/L/Q/C/Z geometry and solid paints |
| `Layer` | `id`, `name`, `visible`, `opacity`, `transform`, `blendMode`, `source` | One bottom-to-top compositing entry |
| `StaticSource` | `assetId` | One durable asset reference |
| `KeyframedSource` | `kind`, `keyframes` | Hold-sampled raster or vector track |
| `Keyframe` | `frame`, `assetId` | Exact integer-frame asset switch |

Asset and layer ids are stable identities. Vector indices are storage or paint
order and can change after insertion, removal, or movement. Do not retain an
element pointer across a collection mutation; resolve the stable id again.
`BitmapEditor` already follows this rule by resolving its bound raster asset id
on every operation.

`RasterAsset` is the document's image/pixel asset and `VectorAsset` is its
native shape asset. These names describe the persisted representation rather
than an importing application's file format. A decoded PNG, JPEG, or brush
result therefore exposes the canonical `RasterLayer` dimensions and ARGB
pixels; `.iisc` does not retain the source codec bytes or brush trajectory. A
shape exposes its viewport, ordered paths, every M/L/Q/C/Z command and control
point, fill color, stroke color, and stroke width without flattening.

## Detailed document traversal

`decodeIisc` returns the same public aggregate model used for authoring. It does
not return an opaque document handle or a reduced summary. Callers may inspect
every serialized layer, asset, path, pixel, source reference, and keyframe:

~~~cpp
const IiscDecodeResult decoded = decodeIisc(bytes);
if (!decoded.ok()) {
    // Surface decoded.error.code, offset, and message.
}

for (const Layer &layer : decoded.document.layers) {
    const std::string &id = layer.id;
    const double opacity = layer.opacity;
    const AffineTransform &transform = layer.transform;

    if (const auto *source = std::get_if<StaticSource>(&layer.source)) {
        const Asset *asset = findAsset(decoded.document, source->assetId);
        if (const auto *image = asset ? std::get_if<RasterAsset>(asset) : nullptr) {
            const std::int32_t width = image->pixels.width;
            const std::vector<std::uint32_t> &argb = image->pixels.pixels;
        } else if (const auto *shape = asset ? std::get_if<VectorAsset>(asset) : nullptr) {
            for (const VectorPath &path : shape->paths) {
                const std::vector<PathCommand> &commands = path.commands;
                const std::optional<SolidPaint> &fill = path.fill;
                const std::optional<StrokeStyle> &stroke = path.stroke;
            }
        }
    }
}
~~~

`FrameRenderResult::ok()`, `IiscEncodeResult::ok()`, and
`IiscDecodeResult::ok()` are inline aggregate inspectors. Their success test is
available identically to static and shared-library consumers, including a
Windows DLL build, without adding a separate exported member ABI.

The references above are views into `decoded.document`; they remain valid only
until the owning collection is structurally changed. Direct aggregate mutation
is allowed, but callers must run `validate(decoded.document)` before rendering
or re-encoding. `DocumentEditor` is the atomic alternative when an edit must
preserve cross-reference invariants automatically.

## Lookup API

`Document/Document.h` exposes mutable and const overloads where applicable:

- `findAsset`, `findRasterAsset`, and `findVectorAsset` resolve stable asset ids;
- `assetIndex` exposes the asset's current storage position;
- `findLayer` and `layerIndex` resolve layer identity and bottom-to-top position;
- `findKeyframe` and `keyframeIndex` perform exact-frame lookup, unlike
  `resolveAssetAt`, which performs timeline hold sampling;
- `assetReferences` returns every referencing layer and optional keyframe index.

These helpers return pointers or `std::optional` values and never throw. Missing
ids and exact frames return null or `std::nullopt`.

## DocumentEditor lifecycle

`DocumentEditor` binds only a currently valid `Document`. It keeps a non-owning
pointer, so the document must outlive the editor and must not move in memory.
`unbind()` releases that relationship. `document()` deliberately exposes the
same aggregate for advanced batch work.

Each operation returns `DocumentEditResult`:

- `ok()` is true for applied edits and valid no-ops;
- `changed` is true only when the document was changed;
- `code` identifies lookup, type, reference, index, source, keyframe, input, or
  validation rejection;
- `path` identifies the rejected data location;
- `message` is a human-readable diagnostic, not a localization key; branch on
  `code` rather than message text.

A rejected edit never advances `revision()` and never leaves a partial change.
A successful change advances it exactly once. A valid no-op succeeds without
advancing it. Before every operation the editor detects direct external changes
that made the document invalid and rejects further mutation as
`InvalidDocument`.

## Document and timeline operations

| Method | Operation |
| --- | --- |
| `setCanvasExtent` | Replace the positive output extent; it does not resample assets |
| `setFrameRate` | Replace the non-zero rational frame rate |
| `setFrameCount` | Resize the integer frame domain; rejects a shrink that would exclude a keyframe |

## Asset operations

| Method | Operation |
| --- | --- |
| `insertRasterAsset` | Insert an id and complete `RasterLayer` at an index or at `AppendDocumentIndex` |
| `insertVectorAsset` | Insert an id, viewport, and ordered path collection |
| `replaceRasterPixels` | Atomically replace raster dimensions and ARGB storage |
| `replaceVectorData` | Atomically replace vector viewport and all paths |
| `renameAsset` | Rename identity and rewrite all static/keyframe references |
| `moveAsset` | Change storage order without changing identity or render order |
| `removeAsset` | Remove only an unreferenced asset; never cascades into layers |

## Layer operations

| Method | Operation |
| --- | --- |
| `insertLayer` | Insert a complete layer at a bottom-to-top index |
| `replaceLayer` | Atomically replace one layer and validate all references |
| `renameLayer` | Replace stable layer identity |
| `setLayerName` | Replace the user-visible name |
| `setLayerVisible` | Toggle rendering participation |
| `setLayerOpacity` | Set finite opacity from zero through one |
| `setLayerTransform` | Replace the complete finite affine transform |
| `setLayerBlendMode` | Select a supported document compositing mode |
| `setStaticSource` | Replace the source with one asset reference |
| `setKeyframedSource` | Replace the source with a validated typed track |
| `moveLayer` | Change bottom-to-top compositing order |
| `removeLayer` | Remove the layer without deleting its assets |

## Keyframe operations

`insertKeyframe`, `setKeyframeAsset`, `moveKeyframe`, and `removeKeyframe`
address a keyframed layer by stable layer id and exact frame. Insertion and
movement restore chronological order automatically. The editor rejects duplicate
frames, out-of-range frames, content-kind mismatches, an empty track, and any
edit that would remove or move the required frame-zero keyframe.

## Vector path operations

`insertVectorPath`, `replaceVectorPath`, `moveVectorPath`, and
`removeVectorPath` address a vector asset by stable id. Their indices are native
paint order. Each inserted or replaced path must start with `MoveTo`, contain
finite coordinates, have a fill or stroke, and use a finite positive stroke
width when a stroke exists.

Path command data remains directly editable through `VectorPath::commands` for
batch algorithms. After direct edits call `validate(document)` before rendering
or encoding; use the path methods when atomic rollback is required.

## Raster pixel operations

Structural raster replacement belongs to `DocumentEditor::replaceRasterPixels`.
Fine-grained pixels belong to `BitmapEditor`, which exposes `pixelAt`,
`setPixel`, `replacePatch`, `replacePixels`, `clear`, streamed brush/eraser
input, dirty bounds, revision, and pixel-snapshot undo/redo. Brush input always
commits pixels and never becomes retained document geometry.

## CanvasItem integration

`CanvasItem::document()` returns the bound aggregate and
`CanvasItem::documentEditor()` returns its persistent structural editor. For a
single structural change, prefer `editDocument`:

~~~cpp
const DocumentEditResult result = canvas.editDocument(
    [](DocumentEditor &editor) {
        return editor.setLayerOpacity("ink", 0.65);
    });
~~~

On success the item rerenders, resolves or clears the selected raster layer,
emits its normal change signals, and clamps the displayed frame if the timeline
became shorter. Code that mutates `*canvas.document()` directly must call
`validate` and `canvas.refresh()` itself.

## Standalone example

~~~cpp
Document canvas;
canvas.extent = {1920, 1080};
canvas.timeline = {{24, 1}, 48};
DocumentEditor canvasEditor(canvas);

canvasEditor.insertRasterAsset(
    "paint.pixels", makeRasterLayer(1920, 1080, 0x00000000U));
canvasEditor.insertLayer({
    "paint.layer", "Paint", true, 1.0, {},
    RasterBlendMode::SourceOver, StaticSource{"paint.pixels"},
});
canvasEditor.renameAsset("paint.pixels", "paint.frame.0");

BitmapEditor pixels(canvas, "paint.frame.0");
pixels.setPixel(10, 10, 0xffffcc00U);
~~~

## Threading and persistence

`Document`, `DocumentEditor`, `BitmapEditor`, and a bound `CanvasItem` are not
internally synchronized. Mutate one document from one owning thread; Qt Quick
items remain GUI-thread objects. Stable-id lookup prevents collection relocation
from becoming a dangling cached asset pointer, but it does not make concurrent
mutation safe.

Call `validate(document)` before `renderFrame` or `encodeIisc` after any direct
aggregate mutation. The serializer persists only aggregate state; editor
revision counters, undo stacks, selection, and callbacks are runtime state.
