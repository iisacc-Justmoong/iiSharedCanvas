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
| `media/*.wav` | PCM16 source audio, independent editable audio tracks |
| `manifest.json` | Version 1 for visual-only packages; version 2 adds audio mapping |
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

The separate in-memory `TimelineProject` remains independent of the persisted
canvas document. This adapter exports the canvas audio track subset described in
[AUDIO_TIMELINE.md](AUDIO_TIMELINE.md). It does not map arbitrary `TimelineProject`
movie clips, transitions, automation or effects. Native format 1.4 persists `Document::audioAssets` and `Document::audioTracks`;
working-file schema 1 is retained and the package API is version 0.9.0.

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

## Persisted audio tracks (0.9.0)

`Document::audioTracks` contains independent `AudioTrackLayer` values with ordered,
nonoverlapping clips; different tracks may overlap. Both XML outputs retain gaps,
clip positions/durations, mute/enabled state, source handles and combined track +
clip gain. PCM16 mono/stereo WAV media is written without lossy compression or
resampling. Legacy XML uses linked per-channel audio tracks for stereo; FCPXML
uses one audio asset clip per negative lane. Track and clip gain sum into the
receiving editor's editable clip gain; their separate values remain in the source.

The version 2 manifest adds `audioTracks`. Each clip records `sourceOffsetSamples`
(native), `mediaOffsetSamples` (XML), `mediaTrimSamples` (WAV origin shift), and
`sampleFrameCount` as decimal strings to avoid JSON double precision loss.
Sample offsets refer to per-channel sample frames. When legacy XML cannot exactly
represent a source offset at the editing frame rate, the WAV omits only the
minimal leading samples needed for exact frame alignment. It retains all trailing
source handles, and the entire original is always stored in `source.iisc`.
The offset mapping is exact: native = mediaTrimSamples + mediaOffsetSamples.
Repeated references to the same asset and origin reuse one WAV. No channel mix,
normalization, gain baking, playback engine or audio DSP is introduced.

Audio assets, tracks, clip metadata, WAV data and the native PCM snapshot all count
against the package limits. `maxLayers` counts visual and audio tracks together;
`maxClips` counts both kinds. Failure never publishes a partial package.

### Audio application check (2026-09-05)

A dedicated Final Cut Pro test library under `build/audio-finalcut-validation/`
imported the 1280x720, 24 fps, five-second audio package. Three stereo clips on
native two-track lanes appeared at 0s, 1s and 3s. The audio inspector showed the
opening clip's editable -9 dB combined gain and stereo channel configuration.
Final Cut re-exported FCPXML 1.14; the independent oracle confirmed both lanes,
all clip and source timing, mute/enabled state, gain, channel counts and identical
WAV bytes after Final Cut copied the media into its library. The test library was
closed after verification. Final Cut normalized custom metadata and role names;
separate native labels and edit values remain authoritative in `source.iisc` and
the manifest. Empty audio tracks have no FCPXML lane object until they have clips.

Both generated XML formats passed their published DTDs. A mixed PNG/audio fixture
at 30000/1001 fps also passed the independent oracle, including an exact one-sample
WAV origin shift; this fixture was not imported in Final Cut. Premiere Pro and
DaVinci Resolve are not installed on this host, so audio application acceptance
for those editors remains unverified. The fresh Release build, all 36 CTest
checks, staging installation and standalone 0.9.0 package consumer passed.
