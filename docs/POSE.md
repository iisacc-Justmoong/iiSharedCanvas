# Native full-body and expression-dense Pose ControlNet layers (0.14.0)

`PoseLayer` is a dedicated ControlNet layer referencing an owned `PoseAsset`.
A PoseAsset contains a viewport and up to 64 independently identified people.
It stores editable joint/face geometry, not a baked preview or generic vector
paths. `layerRole` returns ControlNet and `controlNetKind` returns Pose.
The independent four-way kind is StaticVector or DynamicVector depending on the
source. `ContentKind::Pose` identifies the stored domain asset.

## Complete native topology

Native topology V1 contains **590 anchors per person**, including **523 face
anchors**. Arrays have a stable named-group order, exposed through `poseAnchors`.

| Part | Count | Detail |
| --- | ---: | --- |
| Body | 25 | BODY_25 ordering: nose, neck, shoulders, elbows, wrists, mid-hip, hips, knees, ankles, eyes, ears, toes and heels |
| Left/right hands | 21 each | Wrist plus four anchors per thumb/index/middle/ring/little finger; joint names available through PoseHandJoint |
| Face outline | 33 | Image-left temple through chin to image-right temple |
| Eyebrows | 17 each | Independent brows |
| Nose | 26 | Nine bridge and 17 contour anchors |
| Each eye | 68 | 17 upper-lid, 17 lower-lid, 17 crease, 16 iris-ring and one pupil anchor |
| Mouth | 143 | 25 each for upper/lower outer and inner lips; 17 upper and 17 lower teeth; nine tongue anchors |
| Cheeks/forehead | 51 | 17 each for left cheek, right cheek and forehead |
| Nasolabial folds | 25 each | Separate left/right expression folds |
| Under-eye contours | 25 each | Separate left/right skin contours |

Left/right means the person's anatomical side, never the camera side. Face arcs
run image-left to image-right; the iris starts at its image-right point and runs
clockwise in y-down coordinates. The neutral template mirrors anatomical thumb direction and uses a frontal stance and
independently editable face curves, not an anatomically calibrated 3D model.
`makeNeutralPosePerson(id)` supplies all 590 visible anchors so a caller can edit
immediately. Bare PosePerson aggregates initialize anchors to Missing instead.

## Coordinates and identity

Each `PoseAnchor` owns normalized viewport x/y, optional relative z, confidence
in [0,1], explicit Missing/Visible/Occluded state and an editor lock. Coordinates
must be finite in [-16,16], permitting bounded off-canvas joints. z uses the same
relative scale and is not a metric 3D reconstruction. (0,0) remains a valid visible
anchor; missingness never relies on zero coordinates. A person's id must be
nonempty and unique within the asset; name and trackId are optional. Track ids
can associate the same person across frame assets. Person.enabled controls output.

## Expression deformation

`PoseExpression` owns stable id/name, a [0,1] weight, and sparse `PoseAnchorDelta`
entries addressed by named group and index. Each delta has dx/dy/dz. Evaluation
adds weighted deltas to authored anchors without modifying them; multiple targets
combine. This supports smiles, jaw opening, lip closure, tongue/teeth exposure,
winks, brow movement, pupil/iris movement and asymmetric expressions by explicit
geometry. Targets are authored deformations, not an inferred emotional classifier.

Each person permits 128 expression targets; a target may address up to 590 unique
anchors. Duplicate addresses, unknown groups, invalid indices/weights and combined
coordinate overflow are rejected. Setting an anchor lock prevents targeted editor
movement until unlocked. Locks do not freeze expression evaluation; wholesale
asset replacement is a deliberate lower-level operation.

```cpp
PoseAsset asset;
asset.id = "pose-neutral";
asset.viewport = {1024, 1024};
asset.people = {makeNeutralPosePerson("person-1")};
PoseExpression smile;
smile.id = "smile"; smile.name = "Smile"; smile.weight = 0.6;
smile.deltas = {
    {PoseGroup::MouthOuterUpper, 0, 0, -0.01, 0},
    {PoseGroup::MouthOuterUpper, 24, 0, -0.01, 0},
};
asset.people[0].expressions.push_back(smile);
editor.insertPoseAsset(asset);
PoseLayer layer;
layer.properties = {"pose-control", "Pose control"};
layer.source = StaticSource{"pose-neutral"};
editor.insertPoseLayer(layer);
editor.setPoseExpressionWeight("pose-neutral", "person-1", "smile", 1.0);
auto map = renderPoseControlMap(document, "pose-control", currentFrame);
```

`setPoseAnchor` edits one addressed anchor; `replacePoseAsset` replaces a complete
person collection with validation and atomic rollback. File-bound edits persist
synchronously. `setControlNetSettings` applies to both semantic and pose layers.
Dynamic insertion uses KeyframedSource and frame-zero-first placements; ordinary
source conversion/keyframe APIs remain available. Frame content uses hold sampling.
No automatic IK, body-part parenting or expression inference is implied: joint
and face movement is authored directly or through sparse expression targets.

## Rendering and OpenPose projection

The native detailed profile draws the complete body/hand connectivity and every
face contour. Output is an opaque black-background conditioning bitmap at the
PoseAsset viewport resolution. It honors control.enabled, person.enabled,
confidence, occlusion options and the source frame range. Layer display transforms,
visibility and opacity are not baked into model input. The ordinary canvas omits
ControlNet content; explicit layer previews still support display transforms.
`poseVectorPreview` throws invalid_argument for invalid assets/options; validated
frame rendering supplies valid values.

Two export paths are deliberately distinct:

- `exportOpenPoseKeypoints`: pixel-coordinate x/y/confidence arrays in BODY_25,
  left/right hand 21, and face 70 ordering. The 70 face anchors sample native
  contours: outline every second point; brows every fourth; bridge indices
  0/3/5/8; nose contour every fourth; each eye upper 0/5/11/16 then lower 11/5;
  outer lips upper 0/4/.../24 then lower 20/16/.../4; inner lips upper 0/6/.../24
  then lower 18/12/6; right and left pupils at 68 and 69. Missing anchors emit
  zero confidence. This is structured C++ data for downstream JSON/tensor adapters.
- `PoseRenderProfile::OpenPose`: COCO18 body + 21+21 hands + 70 face raster
  projection, at most 130 emitted anchors per enabled complete person. Foot joints
  and dense face detail are omitted from this raster. NativeDetailed retains all
  590. Result counters and warnings disclose projection loss; native persistence
  always retains all geometry, depth, locks and expression definitions.

The original [OpenPose output contract](https://github.com/CMU-Perceptual-Computing-Lab/openpose/blob/master/doc/02_output.md)
and [70-point face layout](https://github.com/CMU-Perceptual-Computing-Lab/openpose/blob/master/include/openpose/face/faceParameters.hpp)
anchor the interchange indexing. Raster colors/connectivity follow an OpenPose-style
projection but are **not pixel-identical** to a model's annotator. The original
[ControlNet body/hand annotator](https://github.com/lllyasviel/ControlNet/blob/main/annotator/openpose/util.py)
uses its own drawing widths/blending. A consumer must choose a compatible model
and validate inference quality; this SDK neither downloads nor executes one.

Point radius/line width must be finite in (0,64]; output defaults to a 16 Mi-pixel
budget. Dense faces need adequate output resolution or zoom for individual editing.
Anchors below confidence threshold or excluded occlusion state are omitted from
raster output, not deleted. Projection counters count participating slots/anchors,
not clipped raster pixels.

## Persistence and evidence

Package 0.14 changes Asset and Layer variant ABI. Snapshot 1.8 adds native PoseAsset
tag 4, pose source kind 2 and ControlNet layer role tag 2. The working SQLite schema
remains 1 and reuses these versioned asset/layer records. Older snapshots remain
readable; saving pose data under 1.7 or older fails closed. Unchanged pose assets
can be reused by incremental writes. Serialization separately caps document-wide
people, expressions and deltas and checks minimal payload size before allocation.

PSD and XML timeline interchange reject Pose layers; timeline export also rejects
unreferenced PoseAssets before any legacy vector cast. Ordinary bitmap/PDF artwork
output excludes ControlNet layers. Pose tests cover complete topology, actual
expression/eye rendering, missingness, editing rollback, native snapshots, dynamic
sources, working files, budgets and installed-package consumption. They do not
constitute an actual ControlNet inference or face-tracking evaluation.

Detailed layer and object fields can also be edited atomically through the
[common parameter API](CONTROLNET_PARAMETERS.md), including every dynamic source state.
