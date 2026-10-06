# Artboards (0.27.0, native format 1.18)

An artboard is an independent rectangular canvas group in one `Document` and one
`.iisc` file. This follows the Photoshop artboard concept: owned artwork is clipped
to the board, boards can have different sizes and positions, and loose artwork
remains outside board ownership. Boards cannot contain other boards. No Qt types,
UI selection state, or product-specific workflow enter the persisted artboard model.

## Model and coordinates

`Document::artboards` is a bottom-to-top ordered vector of `Artboard` aggregates.
Each board stores a stable nonempty `id`, UTF-8 `name`, `CanvasRegion region`,
`backgroundArgb` (white by default; alpha zero is transparent), and `visible`.
Negative world positions and overlapping rectangles are allowed. Extents must be
positive; rectangle endpoints and the complete view must fit signed 32-bit geometry.
Artboard ids have their own namespace, separate from layer and asset ids.

`LayerProperties::artboardId` is optional. When present it must reference an existing
board, and the layer's base transform and motion are local to that board's origin.
Without membership, the layer uses world coordinates and is not board-clipped.
All existing spatial asset and layer kinds use the same ownership and clipping
contract. Nonspatial IP-Adapter data retains ownership but contributes no pixels.
Boards share the document timeline, authorship and asset pool. They do not introduce
independent frame rates, nested documents, or another persistence owner.

`canvasOrigin` / `canvasRegion` retain their existing allocated-canvas meaning, which
is needed for sparse raster editing. `documentViewRegion` returns the union of that
region and every board, including hidden boards so visibility does not move the
camera. Invalid or overflowing union geometry yields an empty region; validation
rejects it. `renderFrame` and `CanvasItem` use this complete workspace region.

## Validated editing

Use `DocumentEditor` for both memory and file-bound edits:

| API | Behavior |
| --- | --- |
| `insertArtboard(board, index)` | Insert an empty group; default index appends. |
| `setArtboardName(id, name)` | Rename the display name, preserving the stable id. |
| `setArtboardRegion(id, region)` | Move/resize the board; local artwork moves with it and is cropped to the new extent. Sources are not resampled or scaled. |
| `setArtboardBackground(id, argb)` | Set an opaque, translucent or transparent background. |
| `setArtboardVisible(id, visible)` | Hide/show both background and owned content. |
| `moveArtboard(id, index)` | Change bottom-to-top group order. |
| `setLayerArtboard(layerId, optionalId)` | Reparent while preserving world placement at every motion frame; `nullopt` detaches to the loose stack. |
| `duplicateArtboard(id, newId, origin, index)` | Clone the board, all owned layers, all referenced source states and keyframes, preserving editable independence. |
| `removeArtboard(id, KeepLayers)` | Default: detach its layers while preserving world placement. |
| `removeArtboard(id, DeleteLayers)` | Remove owned visual/conditioning layers and their keyframes. |

Duplication generates layer ids as `newId + ":" + oldLayerId` and source ids as
`newId + ":asset:" + oldAssetId`. Each unique source is copied once, so sharing
within the duplicate is preserved; edits to copied pixels/geometry do not affect
the original. Collisions fail atomically. Source assets remain in the document
after deletion: existing explicit `removeAsset` handles unreferenced asset cleanup.
Audio remains document-wide and is not duplicated or deleted with an artboard.

Rejected edits preserve the document, format version, editor revision, authorship
and durable file state. Equivalent edits are no-ops. A real board/membership edit
upgrades legacy documents to 1.18. File-bound edits commit synchronously through
`DocumentFile`; no additional save method is required.

```cpp
using namespace iiSharedCanvas;
Document document;
document.extent = {1280, 720};
DocumentEditor editor(document);
auto first = editor.insertArtboard({"desktop", "Desktop", {{0, 0}, {1280, 720}}});
auto second = editor.insertArtboard({"phone", "Phone", {{1400, 0}, {390, 844}}});
// Check each DocumentEditResult before subsequent work.
LayerProperties layer;
layer.id = "phone-background";
layer.artboardId = "phone"; // A newly inserted layer starts in board-local coordinates.
auto source = editor.insertRasterAsset("background", makeRasterLayer(390, 844, 0xff202020U));
auto inserted = editor.insertLayer(StaticBitmapLayer{layer, StaticSource{"background"}});
auto phone = renderArtboard(document, 0, "phone"); // 390 x 844 pixels, world origin (1400, 0).
auto overview = renderFrame(document, 0);         // Complete side-by-side workspace.
```

## Rendering and adapters

`sampleLayerAt` returns world-space transforms, including board origin and board
visibility after local motion evaluation. Individual layer tiles clip to their
own board. `renderFrameLayers` retains original layer indices and includes detached
board composition metadata; `composeFrameLayers` composes each board background
and owned layer stack into an isolated group, then composites boards in document
order. Loose artwork is above all boards in its existing bottom-to-top layer order.
ControlNet layers remain excluded from final artwork while their spatial preview
tiles obey the same clip. Background and clip masks cover the rectangular output
pixel footprint, including scaled tile requests.

`renderArtboard(document, frame, id)` returns only that board's background and
owned artwork at its native extent. The overload with `outputExtent` and
`RasterSampling` supports a scaled board view. Loose artwork and other boards are
excluded even if they overlap. Hidden boards produce transparent output.

The parallel `AsyncFrameRenderer` carries the same board metadata into composition.
`CanvasItem` exposes complete workspace dimensions/origin, shows the grouped
composed tiles, and maps brush input through the world-space sampled transform.
Host code can edit boards through `CanvasItem::editDocument`. UI selection,
board handles, labels and property panels belong to consuming applications.

`exportBitmapFrame` and `exportVideo` flatten the complete workspace using its
actual output dimensions. To export one board as an image, call `renderArtboard`
then `exportBitmap(result.pixels, ...)`. PSD artboard tags, AI documents, multipage
artboard PDF, and layered timeline interchange are not implemented by this change.
Existing PSD/PDF/timeline exporters reject board documents with `UnsupportedFeature`
instead of discarding ownership, backgrounds or clipping.

## Persistence, limits and compatibility

Snapshot format 1.18 adds a membership flag/string immediately after each layer's
blend mode and appends the artboard collection after authorship. Each board stores
id/name, i32 x/y/width/height, u32 ARGB and bool visible. Pre-1.18 documents decode
with empty board collections and absent memberships and retain canonical bytes on
re-encoding. Encoding artboards with an older model version is rejected.

SQLite schema stays 1. Record kinds 10/11 store the board count and individual board
records; layer records store membership. Stable record ids and positions allow
renames, resizes, background edits and order changes without rewriting source
assets. Existing checksums and atomic transactions protect all affected records.
Version/record-count/record-identity mismatches fail closed.

`SerializationLimits::maximumArtboards` defaults to 65,536. Board extents obey
`maximumCanvasPixels`; names/ids/membership strings share the existing UTF-8 and
aggregate string budgets. Decode checks counts and remaining payload before
allocation. The package ABI changes to 0.27: consumers must rebuild against 0.27.0.

## Verification

`ArtboardTest` covers independent geometry, negative origins, clipping/backgrounds,
native and scaled renders, dynamic content, legacy canonical bytes, resource limits,
invalid references, no-op/rejected edits, clone independence, removal policies,
file-bound incremental persistence/reopen and asynchronous composition.
`CanvasItemRenderTest` covers workspace presentation and board-local brush input.
`VideoCodecTest` exports a workspace with negative artboard origins and a transparent
gap through the real FFmpeg encoder, then probes its complete dimensions and checks
every decoded frame against native rendering (skipped only when FFmpeg is unavailable).
The standalone installed consumer runs `InstalledArtboard` against packaged headers
and the installed library, without source-tree include paths.

Reference semantics: [Adobe Photoshop artboards](https://helpx.adobe.com/photoshop/desktop/create-manage-layers/layout-design-tools/get-started-artboards.html)
and [Adobe Illustrator artboards](https://helpx.adobe.com/illustrator/desktop/create-manage-artboards/add-edit-artboards/introduction-to-artboards.html).
