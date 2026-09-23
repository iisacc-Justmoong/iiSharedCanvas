# iiSharedCanvas engineering contract

## Product boundary

iiSharedCanvas composes raster pixels, native vector paths, owned constant-rate
video and interpolated transform/opacity motion in one document.

iiSharedCanvas is the authoritative canvas document, rendering, editing, and
serialization standard for iisacc products. Consumer applications are
downstream adopters. A consumer's existing model, QML property names, tool
workflow, or release schedule must not define this library's public API.

When a consumer and iiSharedCanvas differ, adapt the consumer through its own
bridge or wrapper first. Change iiSharedCanvas only when the missing behavior
belongs to the reusable canvas domain and is specified and tested here without
product-specific names or assumptions.

iiPaintEngine remains bitmap-only. Brush input must become committed pixels
before crossing into iiSharedCanvas. Never persist a brush trajectory, raw
pointer sequence, curve, dab stream, or replay command.

## Dependency direction

Application -> iiSharedCanvas -> iiPaintEngine.
Application/domain SDK -> iiFileProvider -> Qt Core / SQLite.
File CRUD and SQLite execution belong to iiFileProvider. The provider must not
reference iiSharedCanvas or any other higher-level iisacc SDK. Canvas schemas,
validation, serialization and in-memory editing remain here.

iiPaintEngine must not depend on iiSharedCanvas. The only direct dependency in
the initial milestone is iiPaintEngine. Any additional archive, serialization,
vector, text, or codec library requires an explicit maintenance, license, and
dependency-size review.

SQLite is the reviewed storage engine, now owned privately by iiFileProvider;
see docs/DEPENDENCIES.md. File-bound edits commit synchronously through
DocumentFile, never a delayed autosave or whole-document dump. Read-only
render snapshots must not retain a writable file binding.

Media interchange uses the existing Qt codecs and the reviewed zlib dependency
for SVGZ and PNG integrity checks. FFmpeg/ffprobe are optional application-selected runtime executables,
never a hidden download or link dependency. See docs/MEDIA_IO.md and the media
review in docs/DEPENDENCIES.md. Imports return detached values; inserting them
into a working document must use its existing validated edit transaction.

Layer-preserving foreign-document import uses the reviewed libzip dependency
for OpenRaster ZIP data and the existing Qt/zlib primitives. PSD import is a
bounded, fail-closed pixel-layer subset. Do not silently substitute merged
previews, discard unsupported compositing, extract archive paths, or mutate
the source file. See docs/MEDIA_IO.md and docs/DEPENDENCIES.md.

PSD export snapshots native frame zero. Vectors must carry an embedded vector
PDF Smart Object, not just a raster preview labeled as one. Preserve source
documents, report animation/viewport losses, reuse existing Qt PDF primitives,
and publish completed outputs atomically. Smart Object export does not imply
Smart Object import or native timeline round-trip support.

Timeline interchange exports the full persisted canvas timeline through paired
legacy XML/FCPXML manifests and independent layer-state PNGs. Never substitute
a flattened movie for editable tracks. Keep source.iisc in the package, report
vector/transform projection losses, preserve exact frame intervals and publish
only new directories atomically. Native editor plug-in support and the separate
TimelineProject model are not implied by this interchange adapter.

Consumer adoption is sequential, not a parallel compatibility exercise:

1. Specify, implement, validate, version, and install iiSharedCanvas on its own.
2. Freeze that library contract for the adoption task.
3. Update each consumer to conform through consumer-owned integration code.

Do not edit a consumer application in order to discover or shape an unfinished
iiSharedCanvas API during the same implementation phase.

## Change rules

- Write or update tests before implementing behavior.
- Every source change updates the relevant document and test.
- Use only build/ for generated build output.
- Keep each public header beside its implementation in a module directory
  under src/. Never introduce a separate include/ source tree.
- After source changes, run a fresh configure, full build, all CTest tests,
  install staging, standalone installed-package consumption, and diff checks.
- Do not claim file serialization or rendering until round-trip or golden
  output tests prove it.
- Unknown format versions and invalid references fail closed.
- Prefer integer frames and rational rates over persisted floating timestamps.
- Add abstraction only after repeated concrete changes show the need.
- Keep source, tests, errors, QML types, and public documentation independent
  of any named consumer product.

## Stable Phase 0 semantics

- RasterAsset owns iiPaintEngine RasterLayer pixels.
- VectorAsset owns M/L/Q/C/Z paths with solid fill or stroke.
- A layer has exactly one static or keyframed source; video uses a static video reference.
- One keyframed source has one content kind and begins at frame zero.
- Asset-reference keyframe sampling is hold-only; motion property keys use their explicit interpolation.
- Layer order is bottom-to-top.
- Rendering never mutates source assets.
- Persisted model fields remain public aggregate data. Use `DocumentEditor` for
  validated structural edits; a rejected edit must preserve both document
  state and editor revision.
- BitmapEditor mutates only the explicitly bound RasterAsset and persists no
  pointer trajectory or replay command.
- BitmapItem is a selected-raster display/input adapter, not evidence that the
  mixed-layer frame renderer or serializer is complete.
- Qt Quick is consumed through the Qt targets exported transitively by
  iiPaintEngine; product QML continues to use LVRS. SQLite is private to iiFileProvider; direct SQLite use here is restricted to tests.

## Native video and motion (0.11.0)

VideoAsset owns bounded decoded frames; never require an external media path to
render a persisted native document. CanvasSampling is the shared rational-time
and motion evaluator. Keep media input warnings and resource limits explicit.
Video/motion fields require .iisc 1.6. PSD/XML adapters currently reject these
fields; do not silently discard animation or route video through a bitmap cast.
