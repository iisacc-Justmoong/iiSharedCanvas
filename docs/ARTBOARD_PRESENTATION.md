# Artboard presentation focus (0.29.0)

`CanvasItem::renderArtboardId` controls the visible composition of the Qt Quick
adapter without changing its bound document. It is a writable `QString` QML
property with `renderArtboardIdChanged` notification. C++ callers use
`renderArtboardId()` and `setRenderArtboardId(const QString &)`. The default empty
string renders the complete document, including loose artwork.

A nonempty stable artboard ID renders only that artboard's existing background
and owned visible artwork. Other artboards and loose layers are excluded even
when their persisted regions overlap. Hidden artboards and hidden owned layers
remain hidden. An unknown ID produces a transparent composition while preserving
document readiness; it does not set an editing error. Clearing the ID restores
the full composition. The value is retained across document binding, so a host
must select an ID present in the newly bound document or clear the focus.

Focus is presentation state. It changes the adapter's immutable worker snapshot,
tile caches and asynchronous presentation generation. It does not increment the
document revision, rewrite visibility or geometry in the source document, modify
selection, cancel brush input, reset undo, or persist an extra `.iisc` field.
Repeated assignment of the same value does not emit a notification or schedule
another render. An old in-flight render cannot overwrite a newer focus.

The workspace origin, dimensions, pan, zoom and input mapping continue to use the
complete document. Pixels outside the focused region are transparent. A complete
`framePixels()` compatibility tile retains the complete workspace dimensions,
including those transparent pixels; large tiled workspaces may still return null.
Hosts choose their own layout, cropping and navigation without moving artboards.
Changing focus does not select a layer or restrict editing to a particular board.

The existing snapshot copy retains asset and frame references and hides excluded
content only in that detached render snapshot. Frame changes, document refresh,
brush commits and undo rebuild the snapshot using the current focus. This keeps
all native artwork, blend modes, clipping and timeline sampling on the existing
renderer and at the display's physical texture resolution.

`iiSharedCanvas.CanvasItemArtboardFocus` verifies overlapping and loose content,
backgrounds, workspace coordinates, property reflection and notification, missing
IDs, rapid focus changes, document immutability, brush input, selection and undo,
frame scrubbing and document replacement. The standalone installed-package suite
runs the same contract against the selected installed library. These offscreen
tests verify renderer pixels; they do not claim a native window capture.
