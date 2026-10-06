# Dynamic frame content (0.27.1)

One dynamic bitmap layer can contain different native pixels at every integer
frame. One dynamic vector layer can contain different native viewport dimensions,
path commands, geometry, fills and strokes at every frame. Content changes are
independent of interpolated layer transform/opacity motion. This uses the existing
`Frame`, `Keyframe`, `KeyframedSource`, `RasterAsset` and `VectorAsset` model; no new
document format or external frame files are required.

## Frame ownership and sampling

Each `Document::frames` entry owns the `{layerId, assetId}` keys at its exact frame.
`KeyframedSource::frameIndices` is the derived increasing index of those owners.
The first content key is required at frame zero, and all keys must be inside the
document timeline. Different dynamic layers may share the same frame owner while
retaining separate content references.

`resolveAssetAt`, synchronous/parallel rendering and `CanvasItem` select the last
content key at or before the requested frame (hold sampling). A key at every frame
provides frame-by-frame content. Missing keys intentionally repeat the previous
content until the next key; pixel/path data is not interpolated. Each layer keeps
its bitmap or vector kind across all content states. A bitmap layer cannot switch
to vector data inside the same track.

Distinct content keys can deliberately reference one shared asset. Editing that
asset affects every reference. To make each frame independently editable, give
each frame its own key and asset id. Hidden or out-of-range layers retain their
stored keys; layer visibility/frame ranges only control presentation. Artboard
ownership and clipping apply identically to every dynamic state.

## Atomic content authoring

The two typed overloads return the existing `DocumentEditResult`:

```cpp
DocumentEditResult DocumentEditor::setDynamicFrameContent(
    const std::string &layerId, FrameIndex frame, RasterAsset content);
DocumentEditResult DocumentEditor::setDynamicFrameContent(
    const std::string &layerId, FrameIndex frame, VectorAsset content);
```

They append the supplied native asset and insert or replace the exact-frame key in
one validated edit. This removes the intermediate asset-only commit previously
needed when using `insertRasterAsset`/`insertVectorAsset` and keyframe methods
separately. The derived source index, authorship and editor revision update once.
File-bound edits commit one synchronous `DocumentFile` transaction before returning.

The target must already be a valid dynamic layer of the matching content kind.
Static sources return `SourceNotKeyframed`; missing layers return `LayerNotFound`;
kind mismatches return `AssetKindMismatch`; frame indices outside the timeline
return `IndexOutOfRange`. The supplied id must be nonempty and absent from the asset
pool. An existing id returns `DuplicateAssetId`, including an id already selected
at that frame. For replacement, pass a fresh id such as `paint-12-revision-2`; this
ensures other frames and layers are never implicitly overwritten. The old asset
remains in the pool until explicit `removeAsset` cleanup.

Each successful call creates a new state/reference and counts as a real edit even
when the supplied pixels or paths equal prior content. Use `setKeyframeAsset` to
reuse an existing asset without copying it, `BitmapEditor`/`replaceRasterPixels` to
edit a known raster asset, and `replaceVectorData`/path methods to edit a known
vector asset. Those asset-oriented APIs retain intentional sharing semantics.

Malformed payloads, invalid keys and serialization-limit/storage failures preserve
the document, all earlier assets, frame owners, source index, authorship, editor
revision and durable file contents. Memory rollback copies only frame references
and indices, never existing raster/video payloads. No Qt types enter this API.

```cpp
using namespace iiSharedCanvas;
Document document;
document.extent = {640, 480};
document.timeline = {{24, 1}, 24};
DocumentEditor editor(document);
// Check every result before proceeding to dependent work.
auto seed = editor.insertRasterAsset("paint-0", makeRasterLayer(640, 480, 0xffff0000U));
auto layer = editor.insertDynamicLayer({"paint", "Animated painting"},
    LayerRepresentation::Bitmap, {{0, "paint-0"}});
for (FrameIndex frame = 1; frame < document.timeline.frameCount; ++frame) {
    RasterAsset content{"paint-" + std::to_string(frame),
                        makeRasterLayer(640, 480, 0xff000000U | frame)};
    auto result = editor.setDynamicFrameContent("paint", frame, std::move(content));
    if (!result.ok()) { /* handle result.code / result.message */ break; }
}
auto rendered = renderFrame(document, 12); // Exact frame-12 pixels.
// The same call with VectorAsset writes native geometry, not a raster preview.
```

## Persistence and verification

Native snapshot format is 1.19 with explicit layer type tags. Existing serialization already stores every
asset's complete pixels or paths and every frame-owned reference. Working-file
records persist a new/changed content key and its fresh asset without rewriting
all other frame payloads. Existing limits, checksums and transactions continue to
apply. Package version is 0.28.0 with ABI/SOVERSION 0.28; rebuild consumers against
the exact package version.

CTest prepends the selected build/package directory to `DYLD_LIBRARY_PATH` on
macOS or `LD_LIBRARY_PATH` on Unix, preserving other dependency paths. Source-tree
tests and staged/host consumer tests therefore exercise the selected package even
when the host shell forces an older installation with the same ABI library name.

`DynamicFrameContentTest` authors 24 consecutive frames through the public APIs.
Every bitmap frame has a distinct exact color; every vector frame has distinct
paint and changing path geometry. It compares all output pixels, verifies native
asset identities/geometry, snapshot canonical round trips, one-frame edits leaving
every other frame unchanged, reverse-order asynchronous rendering, sparse hold
boundaries, typed failures, one-revision edits, incremental working-file writes,
close/reopen and durable rollback on serialization-budget failure.

`CanvasItemRenderTest` checks actual bitmap/vector presentation and complete cached
pixels while scrubbing forwards and backwards. The standalone
`InstalledDynamicFrameContent` test repeats the content, persistence and rendering
checks against packaged headers and the installed library.

The dynamic layer alternatives are `DynamicBitmapLayer` and `DynamicVectorLayer`,
with separate `DynamicBitmapContent` and `DynamicVectorContent` types. See
[FOUR_LAYER_TYPES.md](FOUR_LAYER_TYPES.md) for static/dynamic conversion and migration.
