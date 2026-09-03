# iiSharedCanvas data and mutation API

iiSharedCanvas exposes its canonical canvas data as C++20 aggregate types and
adds validated editors for callers that do not want to maintain cross-reference
invariants manually. The aggregates are the serialized truth; the editors are
convenience and safety boundaries over the same data rather than a second model.

## Media import and export

`Bitmap/BitmapCodec.h`, `Vector/VectorCodec.h`, `Video/VideoCodec.h` and
`Media/MediaIo.h` are exported by the umbrella header and installed package.
Options and results are public aggregates with inline `ok()` inspectors.
`MediaIoResult` distinguishes invalid data/options, unsupported features,
missing runtime dependencies, limits, collisions, I/O failure, cancellation
and timeouts; `warnings` describes successful but lossy conversions.

Bitmap byte/file readers return `BitmapImportResult::asset` (`RasterAsset`),
detected format and text carriers. SVG readers return
`VectorImportResult::asset` (`VectorAsset`). Video import returns a complete
`MediaDocumentResult::document` with one typed bitmap layer and frame-owned
keys. None mutates an existing document; commit returned values inside the
existing `DocumentFile::edit` boundary. Byte exporters return
`MediaBytesResult`; file exporters return `MediaIoResult` and publish only a
completed output, with explicit `overwrite` and working-file protection.

Full format limits, options and usage examples: [MEDIA_IO.md](MEDIA_IO.md).

## Working-file authoring

`DocumentFile` owns the committed canvas and exposes only `const Document *`.
`create(path, document, limits)` refuses existing paths; `open(path, limits)`
validates a working file. Each editor's `bind(DocumentFile &)` overload writes
accepted edits immediately. `DocumentFile::edit` provides a validated atomic
callback for custom aggregate changes. Neither close nor refresh performs a
save. `DocumentFileResult` reports rejection, I/O, conflict, schema and limit
errors; `lastWriteStatistics` exposes logical incremental write counts.

`DocumentEditor` and `CanvasItem` deliberately return null from their mutable
`document()` overload when file-bound. Use the const overload for a view; use
file-bound editors or `file.edit` to mutate. Standalone aggregate APIs still
operate in memory. New file-binding overloads, `CanvasItem::createFile`,
`openFile`, `filePath`, lifetime rules, stroke/undo failure behavior, and legacy
import are specified in [PERSISTENCE.md](PERSISTENCE.md).

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
through version 1.3. A caller that wants a canvas bitmap must explicitly decode a
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
| `StableDiffusionMetadata` | prompts, output settings, `samplingPasses`, `models`, `loras`, software/provenance strings, `generationParametersText`, `comfyUi`, `extraParameters` | One complete generation or refinement recipe |
| `StableDiffusionSamplingPass` | `nodeId`, `seed`, `steps`, `cfgScale`, `samplerName`, `scheduler`, `denoiseStrength`, `startStep`, `endStep` | One independently identified sampler invocation; multiple passes are preserved |
| `StableDiffusionModelResource` | `role`, `name`, `hash`, `hashType`, `uri` | Checkpoint, VAE, ControlNet, or other named model reference without embedded model bytes |
| `StableDiffusionLora` | `name`, `hash`, `modelStrength`, `clipStrength` | One LoRA and its two application strengths |
| `ComfyUiMetadata` | `promptJson`, `workflowJson`, `extraPngInfo` | Exact API execution graph, UI-restoration graph, and extension JSON |
| `StableDiffusionMetadataEntry` | `key`, `value` | Ordered extension value; ComfyUI values are JSON and generic values are opaque UTF-8 |
| `StableDiffusionGenerationParameters` | `rawText`, prompts, ordered `parameters` | Lossless Stable Diffusion generation text plus its decoded key/value view |
| `StableDiffusionGenerationParametersParseResult` | `generationParameters`, common `metadata`, typed `issues` | Fail-closed compatibility read and reusable common projection |

`parseStableDiffusionGenerationParameters` accepts the UTF-8 infotext after a host extracts
it from PNG `parameters`, EXIF `UserComment`, or another image carrier. It does
not open an image file. The parser follows the upstream terminal-line rule: at
least three key/value fields distinguish a parameter line from another prompt
line. It trims prompt lines, splits at `Negative prompt:`, decodes JSON-quoted
parameter strings, and preserves the raw source and ordered pairs. Duplicate
pairs remain in `StableDiffusionGenerationParameters::parameters`; typed conversion and
`findStableDiffusionGenerationParameter` use the final occurrence.

The common projection uses `stable-diffusion.main` and, when applicable,
`stable-diffusion.hires` sampling ids. Missing schedule and CLIP skip values map
to upstream defaults `Automatic` and 1. Old Hires records inherit the main
sampler and scheduler, and a Hires step value of zero reuses the main step
count. `Size`, `Batch size`, numeric sampler fields, checkpoint,
VAE, Hires checkpoint, refiner, and `Version` map to their typed equivalents.
Current 10-hex checkpoint/VAE hashes use `sha256-prefix-10`; 64-hex hashes use
`sha256`; the older collision-prone eight-character model hash is labeled
`sha256-partial-prefix-8` rather than misrepresented as a full digest.
All unprojected and future fields remain in `extraParameters` with the last
value, and the ordered/raw views retain every occurrence.

`StableDiffusionGenerationParametersParseCode` distinguishes empty or invalid UTF-8, a missing
parameter line, malformed quoted/key-value syntax, invalid integers, numbers,
sizes and ranges, and a projection that violates the common metadata contract.
A failed result may be inspected for diagnostics and raw recovery but must not
be accepted as a trusted generation recipe. A successful result always passes
`validateStableDiffusionMetadata`.

The parser does not infer `StableDiffusionMetadata::software` from syntax.
AUTOMATIC1111 is the reference implementation for the compatible text format,
but another producer can emit the same shape. A carrier adapter sets software
provenance only when it has separate evidence of the writer.

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

## Video editing timeline objects

`TimelineProject` is a standalone public aggregate and is not a member of
`Document`. It contains project metadata, `TimelineMediaSource` entries,
`TimelineSequence` entries, bins, and `TimelineRenderProfile` delivery
configurations. A media source may retain multiple original, proxy, optimized,
or custom `TimelineMediaRepresentation` objects. Each representation owns a
container descriptor and a `TimelineMediaStream` variant of video, audio,
subtitle, or data streams.

All media and edit positions use signed ticks. `TimelineTimeBase` determines
seconds per tick, while `TimelineFrameRate` independently expresses an exact
rational FPS. `timelineTicksToSeconds` is a checked convenience conversion;
the integer tick value and rational remain authoritative. Video streams can
retain variable-frame-rate sample timing, and sequence, source, timecode, and
output frame rates are intentionally separate.

`TimelineTrack` is a variant of `TimelineVideoTrack`, `TimelineAudioTrack`,
`TimelineSubtitleTrack`, and `TimelineDataTrack`. Each track stores only clips
of the corresponding concrete type. Common clip properties identify the
source stream, timeline and source ranges, playback rate, link group, role,
time remapping, effects, automation, and markers. Video clips add transform and
crop data; audio clips add gain, pan, fades, and a channel matrix; subtitle
clips add text and styling. Stable-id lookup is provided by
`findTimelineMediaSource`, `findTimelineMediaRepresentation`,
`findTimelineMediaStream`, `findTimelineSequence`, `findTimelineTrack`,
`findTimelineClip`, and `findTimelineRenderProfile`.

Source ranges use the referenced media stream, nested sequence, or generated
source `TimelineTimeBase`; timeline ranges use the owning sequence time base.
For a non-looping clip without a time map, validation compares these exact
rationals and requires `playbackRate` to account for the complete duration.
When `timeMap` is present it alone maps the full clip from offset zero through
the clip duration, so `playbackRate` must be 1/1. Negative constant rates model
reverse playback.

Clip lookup returns a non-owning typed view; an empty view has no stream kind,
and any mutation that can relocate its project invalidates the view. A caller
must resolve it again after every successful `TimelineEditor` commit.

Representations use a shared stream id as a logical media identity. Validation
requires every repeated id to keep its stream kind, time base, start tick, and
duration, while codec, resolution, pixel format, and other representation
details may differ. This makes one clip source range stable when active proxy
selection changes.

`validateTimelineProject` returns `TimelineValidationResult` with all observed
issues. It verifies numeric domains, uniqueness, references, stream kinds,
clip ranges, variable-frame-rate samples, automation curves, transitions, and
delivery profiles. It validates structural meaning only; an adapter decides
whether an installed codec implementation can handle a requested setting.
Two-sided transitions use an adjacent `from` end/`to` start as their cut and
must obey start, centered, end, or custom alignment. One-sided transitions are
explicit incoming or outgoing fades. Visual and audio clips require their
sequence canvas or mix format respectively. Unknown media duration does not
disable signed-tick overflow checks, and known sample byte ranges must fit the
representation file size. Subtitle image-resource ids resolve only to
attachments in the same representation.

`TimelineEditor` binds to one caller-owned project and exposes stable-id CRUD
for media sources, sequences, render profiles, typed tracks, and typed clips.
It also provides `setSequenceFrameRate`, `setRenderContainer`,
`setRenderVideoCodec`, and `setRenderAudioCodec`. Every operation validates a
candidate copy before committing it. A rejected edit never advances
`revision()` and never partially mutates the project. Public aggregate edits
remain allowed, but an externally invalid project blocks subsequent editor
operations until repaired or rebound. Binding a different invalid project is
rejected without discarding the current valid binding or its revision.

No function in this module opens media, probes a container, decodes or encodes
a codec, renders a sequence, or writes a project file. `TimelineProject` is not
encoded by `.iisc` version 1.3.

Actual canvas-animation probing and interchange live in `Video/VideoCodec.h`,
not in the timeline authoring model.

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
| `LayerFrameRange` | `firstFrame`, `lastFrame` | Optional inclusive layer existence boundaries |
| `LayerProperties` | `id`, `name`, `visible`, `opacity`, `transform`, `blendMode`, `frameRange` | Presentation and timeline-existence state shared by both layer types |
| `BitmapLayer` | `properties`, `source` | Raster-only finite or chunked bitmap layer |
| `VectorLayer` | `properties`, `source` | Native-vector-only layer |
| `Layer` | `BitmapLayer \| VectorLayer` | Type-distinguished bottom-to-top compositing entry |
| `StaticSource` | `assetId` | One durable asset reference |
| `KeyframedSource` | `frameIndices` | Derived increasing index of the exact frames that own this layer's keys; it owns no `Keyframe` |
| `Frame` | `index`, `keyframes` | Sparse integer frame that directly owns all keys at that position |
| `Keyframe` | `layerId`, `assetId` | Layer-to-asset switch owned by one `Frame` |
| `Document` | `frames` | Possibly empty collection; every stored frame is non-empty and indices are strictly increasing |

`Frame::keyframes` uses canonical ascending `layerId` order. Address a key by
`layerId`; `.iisc` reconstructs simultaneous keys in this canonical in-memory
order while preserving document layer order on the layer-major wire. The frame
index and `(layerId, assetId)` mappings are the durable values.

`KeyframedSource::frameIndices` is a derived secondary index used for bounded
hold lookup. It must be strictly increasing, begin at zero, and equal the exact
set of `Document::frames` entries containing that layer id. It duplicates no
asset reference and gives the layer no keyframe ownership. Direct aggregate
mutation must keep both sides synchronized and then call `validate`;
`DocumentEditor` and `decodeIisc` maintain the index automatically.

`LayerProperties::frameRange` is either absent or an inclusive
`LayerFrameRange`. An absent value means the layer exists throughout the current
document timeline. A present value requires format 1.3, satisfies
`firstFrame <= lastFrame < timeline.frameCount`, and includes both boundary
frames. The range controls layer existence during lookup and rendering; it does
not delete or invalidate frame-owned keyframes outside the range.

Asset and layer ids are stable identities. Vector indices are storage or paint
order and can change after insertion, removal, or movement. Do not retain an
element pointer across a collection mutation; resolve the stable id again.
`BitmapEditor` already follows this rule by resolving its bound raster asset id
on every operation.

`Frame` and `Keyframe` pointers are likewise views into `Document::frames`.
Any frame/keyframe insertion, movement, removal, source conversion, or layer
removal may invalidate them. Reacquire a frame by exact `FrameIndex` and then a
key by stable `layerId` after every successful structural edit. An
`AssetReference::frameIndex` is a storage index, not the persisted frame number.

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

for (const Frame &frame : decoded.document.frames) {
    for (const Keyframe &keyframe : frame.keyframes) {
        const Layer *layer = findLayer(decoded.document, keyframe.layerId);
        const Asset *asset = findAsset(decoded.document, keyframe.assetId);
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
- `layerExistsAt` reports whether a layer exists at an in-timeline frame after
  applying its optional inclusive range;
- `findFrame` and `frameIndex` resolve an exact sparse frame record;
- `findKeyframe(Frame, layerId)` and `keyframeIndex` inspect direct frame
  ownership, while `findKeyframe(Document, layerId, frame)` combines both exact
  lookups;
- `resolveAssetAt` performs timeline hold sampling through the keyframed
  source's derived owner-frame index and exact frame/key binary lookups;
- `assetReferences` returns every referencing layer plus optional frame and
  keyframe storage indices.

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
Binding a different invalid document is also rejected without discarding the
current valid binding or its revision.

## Document and timeline operations

| Method | Operation |
| --- | --- |
| `setCanvasExtent` | Replace the positive output extent; it does not resample assets |
| `ensureInfiniteCanvasRegion` | Union a requested world region into an infinite canvas and align growth outwards to chunk boundaries |
| `setFrameRate` | Replace the non-zero rational frame rate |
| `setFrameCount` | Resize the integer frame domain; rejects a shrink that would exclude a keyframe or explicit layer boundary |
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
| `insertKeyframedLayer` | Insert a keyframed layer and all `KeyframePlacement` values atomically at one bottom-to-top index |
| `replaceLayer` | Atomically replace one layer and validate all references |
| `renameLayer` | Replace stable layer identity |
| `setLayerName` | Replace the user-visible name |
| `setLayerVisible` | Toggle rendering participation |
| `setLayerOpacity` | Set finite opacity from zero through one |
| `setLayerTransform` | Replace the complete finite affine transform |
| `setLayerBlendMode` | Select a supported document compositing mode |
| `setLayerFrameRange` | Set or clear the optional inclusive existence range, upgrading a committed range to format 1.3 |
| `setStaticSource` | Replace the source with one asset reference |
| `setKeyframedSource` | Mark the layer animated and atomically distribute `KeyframePlacement` inputs into their owning sparse frames |
| `moveLayer` | Change bottom-to-top compositing order |
| `removeLayer` | Remove the layer without deleting its assets |

## Keyframe operations

`insertKeyframe`, `setKeyframeAsset`, `moveKeyframe`, and `removeKeyframe`
address a keyframed layer by stable layer id and exact frame. Insertion creates
a sparse `Frame` when needed; movement transfers the key between frame owners;
removal also deletes an owner that becomes empty. The editor rejects duplicate
layer keys in one frame, out-of-range frames, layer/asset type mismatches, an
empty keyframed layer, and any edit that would remove or move its required
frame-zero key. Layer and asset renames rewrite frame-owned stable references,
while switching to a static source or removing a layer removes only that
layer's keys. A layer range limits existence and rendering only: keyframes may
remain before `firstFrame` or after `lastFrame`, preserving hold state and data
for a later range extension.

## Vector path operations

`insertVectorPath`, `replaceVectorPath`, `moveVectorPath`, and
`removeVectorPath` address a vector asset by stable id. Their indices are native
paint order. Each inserted or replaced path must start with `MoveTo`, contain
finite coordinates, have a fill or stroke, and use a finite positive stroke
width when a stroke exists.

`VectorEditor` is the fine-grained native geometry boundary. It binds one
`VectorAsset` by stable id, exposes read-only path inspection, and resolves the
asset again for each edit instead of caching a pointer into `Document::assets`.
`createPath` starts a styled path with `MoveTo`; `insertPath`, `movePath`, and
`removePath` manage native paint order, while `setViewport` replaces the bound
asset viewport without touching another asset.

`appendMoveTo` starts another subpath. `appendLineTo` adds a linear segment;
`appendQuadraticBezierTo` and `appendCubicBezierTo` add one- and two-control-point
Bezier segments. `insertCommand`, `replaceCommand`, and `removeCommand` address
the native M/L/Q/C/Z command sequence directly. `setAnchorPoint` edits the
point of M/L or the endpoint of Q/C, and `setControlPoint` accepts index zero for
quadratic commands and zero or one for cubic commands. `closePath` and
`openPath` manage a trailing `ClosePath`; `setPathPaint` changes optional solid
fill and stroke together.

The editor copies one path, applies the proposed change, and delegates the
replacement to `DocumentEditor`, so complete-document validation is the commit
boundary. A rejected edit retains geometry, paint, and `revision()` exactly;
valid idempotent edits return `ok() == true` with `changed == false`. The bound
document must outlive the editor. Path command data remains directly editable
through `VectorPath::commands` for batch algorithms, but direct edits require
`validate(document)` before rendering or encoding.

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
with callers that only need a composed frame. Hidden layers and layers outside
their optional inclusive `frameRange` keep ordered identity and metadata in the
batch but report effective invisibility and allocate no pixel tiles. Both range
boundaries render; an absent range covers the whole timeline.

`AsyncFrameRenderer::request` takes a value snapshot or shared immutable
snapshot, validates that snapshot once in a thread-pool preflight, and only then
distributes its layer indices across a bounded set of global workers. A rejected
preflight returns the same error as `renderFrameLayers` with no partial layer
batch. Requests coalesce to the newest pending work, and the renderer emits
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

`Document`, `DocumentFile`, `DocumentEditor`, `BitmapEditor`, `ChunkedBitmapEditor`,
`VectorEditor`, and a bound `CanvasItem` are not general-purpose synchronized
mutation objects. Mutate one
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
For file-backed authoring, validation and direct disk commit are automatic at
the editing boundary. Render snapshots remain detached and cannot write files.
