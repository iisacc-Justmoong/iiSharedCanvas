# iiSharedCanvas data and mutation API

iiSharedCanvas exposes its canonical canvas data as C++20 aggregate types and
adds validated editors for callers that do not want to maintain cross-reference
invariants manually. The aggregates are the serialized truth; the editors are
convenience and safety boundaries over the same data rather than a second model.

## Camera RAW authoring objects

`Camera/CameraRaw.h` is an import-side, format-neutral model. It does not add a
third content kind to `Document`, and it does not claim that a manufacturer RAW
file has been decoded merely because these aggregates can represent its common
sensor data.

| Data | Public fields | Meaning |
| --- | --- | --- |
| `CameraRawData` | `image`, `color`, `camera`, `lens`, `capture` | One decoded RAW payload and its processing metadata |
| `CameraRawSensorImage` | `kind`, `extent`, `bitsPerSample`, `samplesPerPixel`, `samples`, `orientation`, `activeArea`, `defaultCrop`, `colorChannels`, `cfaPattern`, `blackLevel`, `whiteLevel` | Integer sensor codes and their spatial/sample interpretation |
| `CameraRawCfaPattern` | `columns`, `rows`, `channelIndices` | Row-column repeating references into `colorChannels` |
| `CameraRawLevelPattern` | `columns`, `rows`, `values` | Repeating zero-light values in row-column-plane order |
| `CameraRawColorProfile` | `asShotNeutral`, `calibrations` | Optional capture white balance and XYZ-to-camera matrices |
| `CameraRawColorCalibration` | `illuminant`, `xyzToCamera` | One row-major `colorChannels × 3` matrix and its illuminant label |
| `CameraRawCameraMetadata` | `manufacturer`, `model`, `uniqueModel`, `serialNumber` | Camera identity without manufacturer-specific tags |
| `CameraRawLensMetadata` | identity plus focal and f-number limits | Lens identity and optional positive optical ranges |
| `CameraRawCaptureMetadata` | exposure, f-number, ISO, focal length, focus distance, exposure compensation | Optional exposure-time settings for the captured image |

Samples are unsigned integers from one through 32 significant bits and use
row-pixel-plane interleaving. A CFA or monochrome image has one sample per
pixel; a linear RAW image has one interleaved plane for every color channel.
The active area and crop use absolute sensor coordinates. When omitted, the
active area is the complete sensor and the crop is the active area. CFA and
black-level pattern origins are the active-area origin. An omitted black level
means zero and an omitted white level means the maximum integer code for the
declared bit depth.

`cameraRawSampleAt` reads stored sensor codes. `cameraRawChannelIndexAt` maps a
CFA cell or linear plane to its indexed channel. `cameraRawBlackLevelAt` and
`cameraRawWhiteLevelAt` expose processing bounds without applying them.
Out-of-range coordinates, malformed patterns, or invalid planes return
`std::nullopt`.

`validateCameraRaw` validates the Camera RAW aggregate independently from
`validate(Document)`. It rejects size multiplication or sample-count mismatch,
samples beyond the declared bit depth, invalid active/crop regions, mismatched
CFA/channel maps, malformed black and white levels, non-positive as-shot
neutral values, non-finite or wrongly sized color matrices, inverted lens
ranges, and invalid exposure metadata. Validation never demosaics, normalizes,
color-converts, renders, or mutates the source values.

`CameraRawData` is not encoded by `.iisc` version 1.1 and remains import-only
in version 1.2. A caller that wants a canvas bitmap must explicitly decode a
file through its chosen adapter,
validate the aggregate, perform its chosen RAW processing, and commit the
resulting ARGB pixels as a `RasterAsset`. The aggregate owns its vectors and
strings but provides no internal synchronization; one owner must coordinate
mutation while other threads read it.

## Stable Diffusion generation metadata

`Metadata/StableDiffusionMetadata.h` defines an optional document-level
generation recipe. It is metadata for provenance and interoperability, not a
third layer type and not an inference or model-loading interface.

| Data | Public fields | Meaning |
| --- | --- | --- |
| `StableDiffusionMetadata` | prompts, output settings, `samplingPasses`, `models`, `loras`, software/provenance strings, `automatic1111Parameters`, `comfyUi`, `extraParameters` | One complete generation or refinement recipe |
| `StableDiffusionSamplingPass` | `nodeId`, `seed`, `steps`, `cfgScale`, `samplerName`, `scheduler`, `denoiseStrength`, `startStep`, `endStep` | One independently identified sampler invocation; multiple passes are preserved |
| `StableDiffusionModelResource` | `role`, `name`, `hash`, `hashType`, `uri` | Checkpoint, VAE, ControlNet, or other named model reference without embedded model bytes |
| `StableDiffusionLora` | `name`, `hash`, `modelStrength`, `clipStrength` | One LoRA and its two application strengths |
| `ComfyUiMetadata` | `promptJson`, `workflowJson`, `extraPngInfo` | Exact API execution graph, UI-restoration graph, and extension JSON |
| `StableDiffusionMetadataEntry` | `key`, `value` | Ordered extension value; ComfyUI values are JSON and generic values are opaque UTF-8 |
| `Automatic1111Infotext` | `rawInfotext`, prompts, ordered `parameters` | Lossless extracted AUTOMATIC1111 text plus its decoded key/value view |
| `Automatic1111ParseResult` | `infotext`, common `metadata`, typed `issues` | Fail-closed compatibility read and reusable common projection |

`parseAutomatic1111Infotext` accepts the UTF-8 infotext after a host extracts
it from PNG `parameters`, EXIF `UserComment`, or another image carrier. It does
not open an image file. The parser follows the upstream terminal-line rule: at
least three key/value fields distinguish a parameter line from another prompt
line. It trims prompt lines, splits at `Negative prompt:`, decodes JSON-quoted
parameter strings, and preserves the raw source and ordered pairs. Duplicate
pairs remain in `Automatic1111Infotext::parameters`; typed conversion and
`findAutomatic1111Parameter` use the final occurrence.

The common projection uses `automatic1111.main` and, when applicable,
`automatic1111.hires` sampling ids. Missing schedule and CLIP skip values map
to upstream defaults `Automatic` and 1. Old Hires records inherit the main
sampler and scheduler, and a Hires step value of zero reuses the main step
count. `Size`, `Batch size`, numeric sampler fields, checkpoint,
VAE, Hires checkpoint, refiner, and `Version` map to their typed equivalents.
Current 10-hex checkpoint/VAE hashes use `sha256-prefix-10`; 64-hex hashes use
`sha256`; the older collision-prone eight-character model hash is labeled
`automatic1111-legacy-model-hash` rather than misrepresented as a full digest.
All unprojected and future fields remain in `extraParameters` with the last
value, and the ordered/raw views retain every occurrence.

`Automatic1111ParseCode` distinguishes empty or invalid UTF-8, a missing
parameter line, malformed quoted/key-value syntax, invalid integers, numbers,
sizes and ranges, and a projection that violates the common metadata contract.
A failed result may be inspected for diagnostics and raw recovery but must not
be accepted as a trusted generation recipe. A successful result always passes
`validateStableDiffusionMetadata`.

ComfyUI `promptJson` and `workflowJson` are optional independently. When
present, each must be a complete JSON object. They remain exact strings during
`.iisc` round-trip; the library does not reorder object keys or normalize
numbers. `extraPngInfo` rejects the reserved `prompt` and `workflow` keys,
requires valid JSON values, and requires unique keys. Generic
`extraParameters` requires non-empty unique keys and can retain application
fields that do not justify changing the typed model.

`validateStableDiffusionMetadata` rejects an attached but empty object, zero
output/batch/CLIP values, empty or numerically invalid sampling passes,
incomplete model identity, non-finite LoRA strengths, malformed ComfyUI JSON,
and duplicate extension keys. `validate(Document)` additionally rejects this
metadata on a format older than 1.2. JSON and all generation metadata are
untrusted data: validation proves structure, not that a node, model URI, hash,
or recipe is safe or available.

`DocumentEditor::setStableDiffusionMetadata` validates and installs the entire
recipe atomically. It upgrades the document version to 1.2 only when the edit
succeeds and restores both metadata and version on rejection.
`clearStableDiffusionMetadata` removes the recipe without downgrading the
physical format. Neither operation changes layers, assets, or rendered pixels.

## Data ownership and identity

`Document` owns all persisted canvas state:

| Data | Public fields | Meaning |
| --- | --- | --- |
| `FormatVersion` | `major`, `minor` | Physical/model compatibility version |
| `CanvasExtent` | `width`, `height` | Positive output size in pixels |
| `CanvasMode` | `Finite`, `Infinite` | Whether the allocated region is a boundary or a movable window into world space |
| `InfiniteCanvas` | `origin`, `chunkSize` | Allocated world origin and sparse raster chunk dimension |
| `Timeline` | `frameRate`, `frameCount` | Rational playback rate and integer frame domain |
| `StableDiffusionMetadata` | `Document::stableDiffusionMetadata` | Optional format-1.2 generation recipe and ComfyUI compatibility payload |
| `RasterAsset` | `id`, `pixels` | Stable id and iiPaintEngine ARGB pixel storage |
| `ChunkedRasterAsset` | `id`, `chunks` | Stable id and canonical sparse raster chunks for an infinite canvas |
| `VectorAsset` | `id`, `viewport`, `paths` | Stable id and native vector paint data |
| `VectorPath` | `commands`, `fill`, `stroke` | M/L/Q/C/Z geometry and solid paints |
| `LayerProperties` | `id`, `name`, `visible`, `opacity`, `transform`, `blendMode` | Presentation state shared by both layer types |
| `BitmapLayer` | `properties`, `source` | Raster-only finite or chunked bitmap layer |
| `VectorLayer` | `properties`, `source` | Native-vector-only layer |
| `Layer` | `BitmapLayer \| VectorLayer` | Type-distinguished bottom-to-top compositing entry |
| `StaticSource` | `assetId` | One durable asset reference |
| `KeyframedSource` | `keyframes` | Hold-sampled track whose type comes from its owning layer |
| `Keyframe` | `frame`, `assetId` | Exact integer-frame asset switch |

Asset and layer ids are stable identities. Vector indices are storage or paint
order and can change after insertion, removal, or movement. Do not retain an
element pointer across a collection mutation; resolve the stable id again.
`BitmapEditor` already follows this rule by resolving its bound raster asset id
on every operation.

`RasterAsset` is a finite document's contiguous image/pixel asset,
`ChunkedRasterAsset` is an infinite document's sparse image/pixel asset, and
`VectorAsset` is its native shape asset. These names describe the persisted representation rather
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
    const LayerProperties &properties = layerProperties(layer);
    const std::string &id = properties.id;
    const double opacity = properties.opacity;
    const AffineTransform &transform = properties.transform;

    if (const auto *source = std::get_if<StaticSource>(&layerSource(layer))) {
        const Asset *asset = findAsset(decoded.document, source->assetId);
        if (std::holds_alternative<BitmapLayer>(layer)) {
            if (const auto *image = asset ? std::get_if<RasterAsset>(asset) : nullptr) {
                const std::int32_t width = image->pixels.width;
                const std::vector<std::uint32_t> &argb = image->pixels.pixels;
            } else if (const auto *sparse = asset
                           ? std::get_if<ChunkedRasterAsset>(asset)
                           : nullptr) {
                const std::vector<RasterChunk> &chunks = sparse->chunks;
            }
        } else {
            const auto *shape = asset ? std::get_if<VectorAsset>(asset) : nullptr;
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
- `findChunkedRasterAsset` resolves sparse raster identity, while
  `findRasterChunk` resolves signed `(column, row)` coordinates;
- `canvasOrigin` and `canvasRegion` expose zero-origin finite geometry or the
  currently allocated infinite world region through one query;
- `assetIndex` exposes the asset's current storage position;
- `findLayer` and `layerIndex` resolve layer identity and bottom-to-top position;
- `findBitmapLayer` and `findVectorLayer` return only the requested concrete layer type;
- `layerProperties` and `layerSource` expose the common fields without erasing
  `Layer`'s bitmap/vector variant identity;
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
| `ensureInfiniteCanvasRegion` | Union a requested world region into an infinite canvas and align growth outwards to chunk boundaries |
| `setFrameRate` | Replace the non-zero rational frame rate |
| `setFrameCount` | Resize the integer frame domain; rejects a shrink that would exclude a keyframe |
| `setStableDiffusionMetadata` | Validate and atomically attach the complete recipe, upgrading legacy format to 1.2 |
| `clearStableDiffusionMetadata` | Remove the optional recipe without changing render content or downgrading format |

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
| `setKeyframedSource` | Replace the source with a validated track of the owning layer's type |
| `moveLayer` | Change bottom-to-top compositing order |
| `removeLayer` | Remove the layer without deleting its assets |

## Keyframe operations

`insertKeyframe`, `setKeyframeAsset`, `moveKeyframe`, and `removeKeyframe`
address a keyframed layer by stable layer id and exact frame. Insertion and
movement restore chronological order automatically. The editor rejects duplicate
frames, out-of-range frames, layer/asset type mismatches, an empty track, and any
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

`ChunkedBitmapEditor` provides the corresponding authoring boundary for a
`ChunkedRasterAsset`. World coordinates, including negative coordinates, are
mapped to signed chunk coordinates. It stores only chunks touched by non-empty
replacement pixels or brush output; a missing chunk reads as transparent.
Brush gestures, clear, full allocated-region replacement, dirty bounds, and
undo/redo follow the same committed-pixel contract. Its current history policy
snapshots the sparse chunk collection and never serializes input trajectories.

`CanvasItem::selectedRasterPixels()` is a contiguous compatibility view. It
returns finite raster storage directly and may materialize a sparse allocated
region only up to 16,777,216 pixels. It returns null above that budget; large
sparse callers traverse `ChunkedRasterAsset::chunks` or request a bounded
`renderFrameRegion` instead of forcing a monolithic allocation.

An infinite canvas is conceptually unbounded but operates on a finite allocated
region at any instant. A camera owner asks `ensureInfiniteCanvasRegion` to cover
the visible world region plus its chosen prefetch margin. The document origin
and extent grow only when that demand crosses a chunk boundary. Existing chunk
coordinates and pixels never move.

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

On success the item schedules a rerender, resolves or clears the selected raster layer,
emits its normal change signals, and clamps the displayed frame if the timeline
became shorter. Code that mutates `*canvas.document()` directly must call
`validate` and `canvas.refresh()` itself.

`createInfiniteRasterDocument(width, height, chunkSize)` creates one selected,
empty `ChunkedRasterAsset`. QML hosts read `infiniteCanvas`, `canvasOriginX`,
`canvasOriginY`, and `canvasChunkSize`, then call
`ensureInfiniteCanvasRegion(x, y, width, height)` as the camera moves. The
returned map contains `changed` and exact `left`, `top`, `right`, and `bottom`
growth in pixels so a host can resize its display item while preserving camera
and overlay positions.

`CanvasItem::refresh()` validates and snapshots the current document, then
queues visible tiles; its boolean result means the request was accepted, not
that pixels were already produced. `refreshAsync()` returns the new content
revision. Observe `rendering` or `renderCompleted(requestId)` when a caller must
wait for presentation. `residentTileCount` is capped at 64 composed tiles;
`residentLayerTileCount` reports the independently cached layer tiles and is
capped at 256. Every texture is at most 512 by 512 pixels. `framePixels()` remains a compatibility view only
when one full-resolution tile covers the entire document; it deliberately
returns null for a large tiled frame.

`gpuAccelerated` reports whether the active Qt Quick renderer is Metal,
Vulkan, Direct3D, or OpenGL, while `graphicsBackend` exposes its name. The CPU
worker still owns deterministic vector rasterization and iiPaintEngine blend
semantics; the GPU owns texture upload, composition into the Qt Quick scene,
nearest-neighbor sampling, pan, and zoom. A software Qt Quick backend continues
to work but reports no GPU acceleration.

For non-QML callers, `renderFrameLayerTiles` renders one layer for a bounded
request batch. `renderFrameLayers` returns all independent layer batches in
bottom-to-top document order, and `composeFrameLayers` applies their opacity and
blend metadata to produce final spatial tiles. `renderFrameRegion` and
`renderFrameTiles` use that same boundary, so their output remains compatible
with callers that only need a composed frame. Hidden layers keep their ordered
identity and metadata in the batch but allocate no pixel tiles.

`AsyncFrameRenderer::request` takes a value snapshot or shared immutable
snapshot, distributes its layer indices across a bounded set of global
thread-pool workers, coalesces queued work to the newest request, and emits
`finished` on its owning thread. `lastLayerResult()` inspects the isolated
layer batch and `lastResult()` inspects the composed batch. `takeLayerResult()`
and `takeResult()` transfer their pixel storage without a GUI-thread pixel copy.
During a live stroke, `livePreviewFrameIntervalMs` is the coalescing delay
before the next immutable snapshot; the final commit still schedules the newest
document state.

## Standalone example

~~~cpp
Document canvas;
canvas.extent = {1920, 1080};
canvas.timeline = {{24, 1}, 48};
DocumentEditor canvasEditor(canvas);

canvasEditor.insertRasterAsset(
    "paint.pixels", makeRasterLayer(1920, 1080, 0x00000000U));
canvasEditor.insertLayer(BitmapLayer{
    {"paint.layer", "Paint", true, 1.0, {}, RasterBlendMode::SourceOver},
    StaticSource{"paint.pixels"},
});
canvasEditor.renameAsset("paint.pixels", "paint.frame.0");

BitmapEditor pixels(canvas, "paint.frame.0");
pixels.setPixel(10, 10, 0xffffcc00U);
~~~

## Threading and persistence

`Document`, `DocumentEditor`, `BitmapEditor`, `ChunkedBitmapEditor`, and a bound
`CanvasItem` are not general-purpose synchronized mutation objects. Mutate one
document from one owning thread; Qt Quick items remain GUI-thread objects.
`CanvasItem` copies a validated immutable render snapshot before a worker sees
it, so the worker never races caller mutation. `AsyncFrameRenderer` likewise
owns or shares a const snapshot for the lifetime of a request. Stable-id lookup
prevents collection relocation from becoming a dangling cached asset pointer,
but it does not make concurrent mutation of the source document safe.

Call `validate(document)` before `renderFrame`, `renderFrameRegion`, or
`encodeIisc` after any direct
aggregate mutation. The serializer persists only aggregate state; editor
revision counters, undo stacks, selection, and callbacks are runtime state.
