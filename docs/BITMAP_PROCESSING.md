# Bitmap processing and native brush configuration (0.26.0)

`Bitmap/BitmapProcessing.h` provides detached, bounded C++23 operations for color
adjustment, spatial effects, deterministic noise, solid/gradient/pattern fills,
coverage-mask feathering and combinations, connected/global color selection,
and masked pixel replacement. Inputs remain unchanged. Invalid dimensions,
storage, mismatched masks and nonfinite parameters return an explicit error.
RGB operations preserve source alpha; mask operations multiply alpha and clear
RGB where alpha becomes zero. Gaussian blur uses three running-window separable
box convolutions. It is an approximation, not a frequency-domain exact Gaussian.

These operations do not infer faces, objects, depth or semantic regions. Such
data must be supplied by a real detector or an existing conditioning asset.
They do not manage UI state, files or transactions. Consumers commit resulting
pixels through `DocumentFile::edit`, `DocumentEditor` or `CanvasItem` edits.

`BitmapBrush::engineState` optionally carries iiPaintEngine's native tip,
dynamics, color and material configuration. The existing scalar size/color/flow/
hardness/spacing controls remain authoritative, and erasing remains destination
out. `CanvasItem::setBrushEngineState` applies this to both finite and chunked
bitmap editors; `clearBrushEngineState` restores legacy behavior. Brush input
remains committed raster pixels; no pointer sequence is serialized.

This release changes the C++ package ABI to SOVERSION 0.26. The `.iisc` schema
remains 1.17. Processing and independent installed-consumer tests cover exact
neutral output, alpha, deterministic seeds, gradient stops, mask algebra,
connected/global selection, rejected data and all effect kinds.
# Alpha and parameter boundaries

Masked replacement interpolates premultiplied color and alpha, then stores
unassociated ARGB. Feathering colored content over transparency retains its
color instead of introducing a dark fringe. Zero/full mask coverage preserves
the corresponding input byte exactly. Dehaze strength is bounded to keep its
transmission divisor positive for every accepted public parameter.
