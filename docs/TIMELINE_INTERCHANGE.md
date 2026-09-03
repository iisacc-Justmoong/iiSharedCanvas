# Editable timeline interchange

`exportTimelineInterchange(document, directory, options)` creates a new local
package for importing a native canvas timeline into video editors. It exports
the complete `Document::timeline`, not the PSD frame-zero projection. It does
not install native `.iisc` importer plug-ins into other applications.

| File | Purpose |
| --- | --- |
| `timeline.xml` | Legacy Final Cut Pro XML timeline for Premiere and Resolve |
| `timeline.fcpxml` | FCPXML timeline for Final Cut Pro and compatible editors |
| `media/*.png` | Independently rendered, straight-alpha layer states |
| `manifest.json` | Version 1 mapping of native layer/asset IDs to clips and media |
| `source.iisc` | Canonical native snapshot retaining original editable data |

Import the appropriate XML, not `source.iisc` or the PNG files individually.
The package never flattens all layers into a single movie or image sequence.
XML references use percent-encoded absolute local file URLs pointing to the
completed destination directory, never a temporary path. Keep the package at
that location; after moving it, relink media in the editor or regenerate it at
the new location. No network references or external codecs are used.

## Preserved editing structure

Every native layer becomes one bottom-to-top track or connected lane, including
hidden layers. Every hold-key interval becomes a separately trimmable clip.
Inclusive native `LayerFrameRange` endpoints become exclusive clip ends without
off-by-one changes. Repeated references reuse PNG media but retain all key cuts.
Sequence duration, rational frame rate, layer names, visibility, opacity and
supported compositing are represented in XML. PNGs contain neither layer opacity
nor layer blend application; the target editor applies those attributes.

SourceOver, Multiply, Screen and Overlay are the supported blend modes. Editor
color management, blend implementations and version-specific XML translation
can affect appearance; schema validity alone is not application acceptance.
See [XML details](TIMELINE_XML.md) and the validation record below.

Legacy XML represents integer rates and supported nominal rates multiplied by
1000/1001. Both outputs must represent the rate exactly; unsupported rates fail
without publishing a partial package. Rates are reduced before conversion.
This adapter defaults `limits.maxFrames` to 1,000,000 (rather than the movie
adapter's 4096); hold clips avoid per-frame media generation. All other media
limits retain their common defaults. Callers may adjust the limits explicitly.

## Deliberate boundaries

Vector layers remain independent clips but are rendered to transparent PNG for
NLE playback. Their editable vector paths remain in `source.iisc`; they do not
become native NLE vector objects or PSD Smart Objects. Spatial transforms are
baked into full-canvas PNGs, while timing and compositing remain separate. The
source canvas region defines the output frame; infinite-canvas content outside
that region is not included in clip pixels. These conversions return warnings.

The separate in-memory `TimelineProject` audio/video editing model is not stored
in `.iisc` 1.3 and is not silently conflated with the canvas document. This API
does not invent audio, movie clips, transitions, Bezier automation or effects
that do not exist in the persisted canvas timeline. Native format 1.3 and
working-file schema 1 are unchanged; the library package API is version 0.8.0.

## Limits and publishing

`TimelineInterchangeOptions` contains a neutral sequence name (`iisc Timeline`),
`MediaLimits`, `maxLayers` (4096) and `maxClips` (65536). Dimensions are limited to
16384 pixels per axis as well as the configured pixel budget. Output bytes are
charged across every package file, including the source snapshot. Source copy,
XML/manifest and PNG conversion, vector tessellation, and native chunked render
pieces have conservative decoded-memory preflights. Sequence names and output
paths are budget-checked before text conversion; even unused native assets and
nested metadata are checked before snapshot serialization. This is a bounded adapter,
not a guarantee that arbitrary host allocations equal a fixed peak RSS.

Existing paths, directories and destination symlinks are always refused; there
is intentionally no overwrite option. The destination parent must already
exist. A private sibling staging directory holds complete outputs before one
exclusive directory rename publishes them. Failure cleans only that private
directory and leaves both existing destinations and the input document intact.
Exclusive publishing currently supports macOS, Linux with `renameat2`, and
Windows; other platforms fail closed. This ensures atomic visibility, not a
power-loss durability guarantee. Names must be valid UTF-8/XML text.

The CLI accepts native canonical snapshots and live SQLite working files using
a shared read-only backup loader; see [CLI usage](TIMELINE_INTERCHANGE_CLI.md).

## Verification

Tests independently parse both XML structures, map every clip to a real PNG,
reconstruct every frame of an animated fixture, and compare pixels with native
rendering. They cover hidden layers, Unicode names, repeated states, frame
ranges, rational rates, source preservation, resource limits, failure cleanup
and collisions. An installed-package consumer exercises the same public API.
The read-only `tests/verify_timeline_interchange.py` development oracle uses only
Python's standard library. It independently compares both XML files to the
manifest, resolves and hashes PNG references, and optionally checks a receiving
Final Cut application's re-export with `--fcpxml-roundtrip FILE`. No Python
dependency is added to the library or installed tools.

### Application check (2026-09-03)

Final Cut Pro imported the generated 1280x720, 24 fps, five-second package into
a separate test library under `build/`. Its timeline retained three layers:
the five-second background, three separate one-second clips at 1s/2s/3s on
the keyed layer, and the five-second disabled hidden layer. The inspector
showed editable opacity of 50% on a keyed clip. Final Cut then exported the
project as FCPXML 1.14; the independent oracle confirmed identical layer lanes,
clip timing, names, visibility, opacity, media identity and frame rate. The
test library was closed; user projects were not opened or modified.

Both generated XML dialects also passed their Apple DTDs. Premiere and Resolve
were not available for application-level validation on this host. Their import
path is implemented against published XML support and independently checked
syntax/structure, not claimed as a live application pass. Single-frame exposures,
all compositing modes, every editor/version and pixel-exact target color output
still require an application-specific acceptance matrix. This verification does
not constitute importing changed editor projects back into `.iisc`.
