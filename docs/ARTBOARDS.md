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

- API: `insertArtboard(board, index)`; Behavior: Insert an empty group; default index appends.
- API: `setArtboardName(id, name)`; Behavior: Rename the display name, preserving the stable id.
- API: `setArtboardRegion(id, region)`; Behavior: Move/resize the board; local artwork moves with it and is cropped to the new extent. Sources are not resampled or scaled.
- API: `setArtboardBackground(id, argb)`; Behavior: Set an opaque, translucent or transparent background.
- API: `setArtboardVisible(id, visible)`; Behavior: Hide/show both background and owned content.
- API: `moveArtboard(id, index)`; Behavior: Change bottom-to-top group order.
- API: `setLayerArtboard(layerId, optionalId)`; Behavior: Reparent while preserving world placement at every motion frame; `nullopt` detaches to the loose stack.
- API: `duplicateArtboard(id, newId, origin, index)`; Behavior: Clone the board, all owned layers, all referenced source states and keyframes, preserving editable independence.
- API: `removeArtboard(id, KeepLayers)`; Behavior: Default: detach its layers while preserving world placement.
- API: `removeArtboard(id, DeleteLayers)`; Behavior: Remove owned visual/conditioning layers and their keyframes.

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
# Cropped isolated rendering (0.31.4)

`renderArtboardRegion(document, frame, artboardId, region, outputExtent, sampling)`
renders only the requested world-space region within the identified artboard.
The result origin is `region.origin`. A native-size crop uses `Nearest` by
default; `Smooth` is an explicit presentation option for scaled output.

The region must be positive and wholly contained by that artboard, including
when its origin is negative. Invalid geometry, output size or sampling policy
returns `InvalidRegion`; missing artboards and invalid frames retain their typed
errors. No raster output is allocated before validation succeeds.

Composition uses only this artboard's authored visibility, background and owned
layers. Other overlapping artboards and loose artwork are excluded. Layer order,
transforms, frame ranges, dynamic assets, raster/vector masks, filters, clipping
and world-coordinate Dissolve behavior use the same native renderer as
`renderArtboard`. Full-artboard rendering delegates to this path and validates
the document once. The operation never mutates the input document.

Only requested output pixels are allocated by the composition. A source-wide
filter or source/mask preparation can still require native source-sized work;
this API is not a constant-time guarantee or an asynchronous owner. Consumers
must schedule potentially expensive analysis independently of their UI thread.

The native and installed-package region tests compare every pixel of small
mixed compositions with the full native artboard oracle, including negative
origins, overlapping boards, loose artwork, masks, vectors, nontrivial transforms,
partial/dynamic timing and Dissolve. A 32768 by 32768 artboard separately verifies
that a corner request produces exactly one output pixel.

## 중첩 레이어 그룹을 가진 아트보드

현재 아트보드 복제는 중첩 레이어 그룹을 복제하지 못하므로, 그룹에 속한 레이어가 있는 아트보드는 조용히 평탄화하지 않고 거부한다. 그룹과 레이어 잠금은 아트보드에 다시 소속시키거나 아트보드를 삭제하는 편집에도 적용한다. [선택 채널과 레이어 관계](SELECTION_LAYER_RELATIONSHIPS.md)를 참조한다.
