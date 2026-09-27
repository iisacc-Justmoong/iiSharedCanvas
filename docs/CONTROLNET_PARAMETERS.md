# ControlNet detailed parameter API

Package 0.24.0 exposes a common typed parameter workflow for all 12 conditioning
kinds, including Reference and IP-Adapter. Native model remains 1.17: this API edits
existing persisted fields and introduces no new wire tags or inference dependencies.

## Query, modify, apply

`getControlNetParameters(document, layerId)` and
`DocumentEditor::controlNetParameters(layerId)` return a detached
`ControlNetParametersResult`. Check `ok()` before accessing `parameters`.
`parameters.layer` is the existing typed `Layer` variant. `parameters.assets` contains
ALL unique source assets, in first-reference order. For dynamic sources this includes
every keyframe state, even when the layer is disabled, hidden or outside its display
frame range. Repeated references to the same asset appear once.

Use `std::get<T>`/`std::get_if<T>` to adjust the complete concrete aggregates.
`controlNetSettings(layer)` provides common settings for any conditioning layer;
the const overload supports read-only access. Both return null for artwork.
Editing a returned detached copy never changes the document directly.

`DocumentEditor::setControlNetParameters(layerId, parameters)` validates and applies
the layer and all source assets in ONE transaction. Identity, concrete type and source
references must remain unchanged. Every referenced asset must appear exactly once;
the input asset order is arbitrary. Layer name, visibility, opacity, transform, blend
mode, frame range and motion may be changed together with conditioning data. Source
conversion, id changes and keyframe topology use the existing structural editor APIs.

The setter is a full replacement, not a merge or a concurrent-edit protocol. Reacquire
a snapshot after other edits. Each accepted setter call records one document edit,
including resubmission of identical values (matching `replaceLayer` semantics).
Assets shared with other layers change for every owner; full document validation
rejects any change that would invalidate another owner. Make an independent asset
with the structural APIs when separate ownership is required.

## Partial common settings

`patchControlNetSettings(layerId, ControlNetSettingsPatch)` changes only engaged
optional fields: `enabled`, `modelId`, `modelRevision`, `conditioningScale`,
`guidanceStart`, `guidanceEnd`. Explicit false, zero and empty strings are edits;
disengaged fields remain unchanged. Empty/equivalent patches do not increment the
editor revision. The complete final settings are validated together: scale is finite
and nonnegative; guidance obeys 0 <= start < end <= 1. IP-Adapter still requires
matching nonempty adapter identity/revision. Use the complete parameter transaction
when changing its layer binding and asset descriptors together.

## Detailed field coverage

| Kind | Editable object data | Specific layer data / output API |
| --- | --- | --- |
| Semantic Segment | `RasterAsset.pixels`: dimensions and exact identity colors | Taxonomy id/version/source; classes' id/key/name/description/category/parent/palette/external id/aliases; regions' class, mask color, instance, confidence, origin, generator, source, attributes; void colors |
| Pose | Viewport; people id/name/track/enabled; all body, hand, face, eye, mouth anchors (coordinates, z, confidence, visibility, lock); expressions id/name/weight/deltas | `PoseRenderOptions`: profile, confidence filter, occlusion, point radius, line width, output budget |
| Depth | Viewport and every normalized proximity sample [0,1] | Exact depth data remains independent of display opacity/transform |
| Line Art | Viewport and every coverage sample [0,1] | Prepared coverage, no implicit extraction |
| Canny | Viewport and every binary edge sample | Prepared edge mask, no implicit detector |
| Scribble | Viewport and every binary stroke sample | Prepared stroke mask, no pointer/brush trajectory |
| MLSD | Viewport; segment id, endpoints, confidence, enabled | `MlsdRenderOptions`: minimum confidence, pixel and raster-step budgets |
| Normal Map | Viewport; signed XYZ unit normals and validity | `NormalMapRenderOptions`: XYZ/ZYX order, flipY, output budget |
| Shuffle | Viewport and RGB samples | Explicit preparation UV mapping remains available through `makeShuffleAsset` |
| Tile | Viewport and RGB samples | Existing whole-image and bounded-region export APIs |
| Reference | Viewport and RGB samples | Attention/AdaIN/combined mode and style fidelity |
| IP-Adapter | Descriptor stage, encoder/adapter id and revisions, base model and preprocessing id; token/channel dimensions; conditional and optional unconditional float32 values for all states | Adapter binding and common scale/schedule; `IpAdapterExportOptions` controls CFG requirement and output budget |

All existing typed insert/replace/sample/anchor/expression APIs remain public.
Render/export option structures are supplied to their existing output functions;
they are not persisted as layer parameters. Preprocessor algorithms that the SDK
does not execute (for example Canny extraction thresholds) are not invented as
ineffective settings. Prepared pixel/geometry/tensor data is the native object contract.

## Atomic examples

Change a semantic region identity and its masks together:

```cpp
auto result = editor.controlNetParameters("segments");
if (!result.ok()) { /* present result.message */ return; }
auto parameters = std::move(*result.parameters);
auto &semantic = std::get<SemanticSegmentLayer>(parameters.layer);
const auto oldColor = semantic.segmentation.regions.at(0).maskColor;
const auto newColor = 0xff336699U; // Must not collide with another region or void.
semantic.segmentation.regions.at(0).maskColor = newColor;
for (auto &asset : parameters.assets) {
    auto &mask = std::get<RasterAsset>(asset).pixels;
    for (auto &pixel : mask.pixels) if (pixel == oldColor) pixel = newColor;
}
auto applied = editor.setControlNetParameters("segments", std::move(parameters));
// Check applied.ok(), applied.changed, applied.path and applied.message.
```

Change common parameters without replacing unspecified settings:

```cpp
ControlNetSettingsPatch patch;
patch.conditioningScale = 0.65;
patch.guidanceStart = 0.1;
patch.guidanceEnd = 0.85;
auto applied = editor.patchControlNetSettings("pose", patch);
```

Change detailed expression and multiple anchors via one pose asset snapshot, or
change an IP-Adapter model binding and every referenced state's descriptor in the
same manner. No direct mutation of the live document is required.

## Validation and persistence

Rejected edits preserve layer/assets, format version, authorship and editor revision.
All type-specific invariants remain enforced, including semantic palette consistency,
pose anchors/expressions, binary masks, unit normals, finite tensors and IP-Adapter
cross-frame compatibility. Invalid references, duplicate/missing assets, artwork and
expired file bindings fail explicitly. Successful file-bound edits synchronously
commit through the existing `DocumentFile` transaction. Unchanged asset records are
reused for common-only patches. Tests cover all 12 types in both snapshots and
working files, multi-state atomic edits and rollback.
