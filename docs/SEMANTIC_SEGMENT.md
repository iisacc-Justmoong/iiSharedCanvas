# ControlNet semantic segment layers (0.13.0)

`Layer` owns a dedicated `SemanticSegmentLayer` alternative. Its domain hierarchy
is Layer -> ControlNet role -> SemanticSegment kind. This is composition, not
inheritance or a bitmap with arbitrary metadata attached. `layerRole()` separates
artwork from conditioning; `controlNetKind()` identifies the specialized control.
The earlier timing/representation axes remain independent: semantic layers are
StaticBitmap or DynamicBitmap according to their source.

## Object contract

| Object | Fields and purpose |
| --- | --- |
| `ControlNetSettings` | `enabled`, model identifier/revision, nonnegative finite conditioning scale, normalized guidance start/end |
| `SemanticTaxonomy` | Stable taxonomy id, version, provenance URI, class collection |
| `SemanticClass` | Stable numeric id, unique machine key, display name, description, category, optional parent, external dataset id, aliases, exact model-palette ARGB color |
| `SemanticRegion` | Stable nonzero id, class reference, unique identity-mask ARGB color, name, description, optional instance id/confidence, annotation origin, generator, source reference, named attributes |
| `SemanticSegmentation` | Taxonomy, regions, explicit void identity color and void conditioning color |
| `SemanticRegionGeometry` | Derived per-frame pixel count, tight integer bounds, pixel-center centroid; absent regions have zero geometry |
| `SemanticControlMapResult` | Exact conditioning pixels, dense class ids, region ids, valid-pixel flags, region geometry and error message |

Class ids are taxonomy-local and may be zero. Region ids and optional instance
ids are nonzero. Multiple regions may refer to one class; multiple regions may
share an instance id when they describe the same object. A region can contain
multiple disconnected components; region identity does not claim connectivity.
The same region definitions apply to all frame masks and may be absent in some
frames. Parent classes form an acyclic hierarchy. Attributes describe annotations;
they are not automatically inserted into text prompts or executed as instructions.

## Identity colors versus model colors

An identity mask is an owned dense `RasterAsset`. Each opaque pixel is either a
region's exact `maskColor` or `voidMaskColor`. Two people can have separate red
and green identity colors while both reference the same person class. The
conditioning export recolors both regions to that class's `controlColor`.
This retains independent object editing without pretending different colors are
different semantic classes. Output class colors may repeat if a target palette
requires it; region colors must be unique. There is no approximate matching,
alpha blending, antialiasing, lossy mask import, or automatic meaning inference.

A taxonomy must explicitly describe the palette expected by the selected model.
No built-in palette is claimed compatible with every ControlNet. The original
segmentation model's example maps label ids through a palette; see the primary
[ControlNet segmentation model card](https://huggingface.co/lllyasviel/control_v11p_sd15_seg).
The SDK neither downloads models nor runs a segmentation/inference model.

## Creation, editing and frame behavior

```cpp
SemanticSegmentation semantics;
semantics.taxonomy.id = "my-scene";
semantics.taxonomy.version = "1";
SemanticClass person;
person.id = 1; person.key = "person"; person.name = "Person";
person.controlColor = 0xffaabbcc; // Supply the actual model's palette value.
semantics.taxonomy.classes.push_back(person);
SemanticRegion region;
region.id = 10; region.classId = 1; region.maskColor = 0xffff0000;
region.name = "Left person"; region.instanceId = 100;
semantics.regions.push_back(region);

SemanticSegmentLayer layer;
layer.properties = {"segments", "Semantic segmentation"};
layer.source = StaticSource{"identity-mask"}; // Insert its RasterAsset first.
layer.segmentation = semantics;
editor.insertSemanticSegmentLayer(layer);
auto controlMap = renderSemanticControlMap(document, "segments", currentFrame);
```

For animation, set `source = KeyframedSource{}` and supply frame placements to
`insertSemanticSegmentLayer`. Frame-zero content is mandatory; content is held
until the next key. All referenced masks must have identical dimensions.
`setStaticSource` / `setKeyframedSource` change timing without losing semantics.
`setSemanticSegmentation` and `setControlNetSettings` atomically update definitions
and control parameters. Existing asset replacement validates all referencing
semantic masks. Rejected edits preserve state/revision, also for `DocumentFile`.
`isSemanticMaskAsset` lets editors detect categorical ownership. `BitmapEditor`
rejects soft-brush binding to these masks (including preexisting bindings after
semantic ownership is added); use exact pixels with `replaceRasterPixels`.
To change mask colors and definitions together, construct a valid replacement
using a new mask asset and `replaceLayer`, then remove the unused old asset.

## Rendering and coordinate contract

`renderSemanticControlMap` samples the current source frame and emits opaque
class colors at native mask resolution. It honors the timeline/range and
`control.enabled`. Display visibility, opacity, motion and transforms are not
applied: output coordinates are explicitly local mask coordinates. Its default
16 Mi-pixel budget bounds output allocation; callers can supply `maxPixels`.
The valid-pixel buffer distinguishes void from a valid class id zero. Geometry
uses pixel centers (x+0.5,y+0.5), and covers every declared region in definition
order. Class/region lookups are available by stable id.

Ordinary frame composition, CanvasItem artwork tiles and PDF artwork output omit
ControlNet layers. Explicit per-layer rendering still provides an identity-color
preview and carries its ControlNet role. Display previews may be transformed;
never use a display preview as the lossless conditioning map. Model scale and
guidance fields are persisted intent for the consuming inference runtime.

## Persistence and limits

Snapshots require `.iisc` 1.7 and working-file records reuse the exact same
versioned layer extension; SQLite schema remains 1. All class/region/control
fields persist, including semantics-only incremental edits. Snapshot 1.0–1.6
remains readable. Saving semantic content under an older version fails closed.
Package ABI 0.13 requires consumers to rebuild for the new Layer alternative.

Document validation permits up to 65,536 classes and 1,048,576 regions per layer.
Serialization separately bounds document-wide classes, regions, aliases and
attributes, as well as existing string/container/pixel budgets. Decoding verifies
collection sizes against available bytes before allocation. Unknown role/origin
tags, cycles, duplicate ids/keys/mask colors, invalid class references, invalid
confidence/scale/ranges and unmapped mask pixels are rejected. Void colors and
all mask/model palette colors must be opaque.

PSD and timeline XML export reject semantic layers rather than silently discard
class, instance, provenance and control data. Use the native document for editable
interchange and the dedicated exact map for model input. Automatic segmentation,
model palette discovery, GPU inference and capture of live sources are outside
this layer's contract.

Since 0.14.0, ControlNetSettings lives in ControlNet/ControlNet.h and is shared
with PoseLayer. This header includes it for existing semantic API consumers.

Detailed layer and object fields can also be edited atomically through the
[common parameter API](CONTROLNET_PARAMETERS.md), including every dynamic source state.
