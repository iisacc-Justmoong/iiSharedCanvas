# Layered timeline XML interchange

`exportTimelineInterchange` exports a native document as an editable, media-backed
timeline package. Its two XML representations describe the same visual layers,
hold exposures and independent audio clips; neither is a flattened movie or mix.
The package also retains `source.iisc`, because XML interchange cannot replace the
native editable vector, chunk, document, and provenance model.

`timeline.xml` uses legacy Final Cut Pro 7 XML (`xmeml` version 5), suitable for
applications that import that dialect, including Premiere Pro and DaVinci Resolve.
`timeline.fcpxml` uses Apple FCPXML 1.9, intended for Final Cut Pro and compatible
Resolve imports. These are different formats, not interchangeable filename
extensions. Import the appropriate file through the target editor's XML/timeline
import command. Importing one of the PNG files imports only that exposure.

## Timeline and layer mapping

The native frame rate is reduced before encoding. Exact integer rates and exact
integer × 1000/1001 rates can be represented in both files. Arbitrary fractional
rates, including the decimal approximation `2997/100`, return
`UnsupportedFeature`; they are never rounded to a nearby broadcast rate.
FCPXML times use reduced rational seconds with integer arithmetic. Legacy edit
positions use integer frames, a nominal `timebase`, and the corresponding `ntsc`
flag. Both sequences begin at frame zero, retain the complete native duration,
and use non-drop timecode display; display numbering does not change duration.

Each native layer has its own legacy video track in bottom-to-top order. Every
hold exposure is a separate clip with exact start/end positions, including gaps
before, between, and after exposures. Names are preserved on clips because
standard legacy XML has no portable track-name element. Hidden layers and their
clips are retained and disabled, not deleted. Their source PNGs retain pixels so
that an editor can enable them later.

FCPXML uses one full-duration primary gap as its timing foundation. Layer clips
are attached above it using positive lanes 1 through N, in native bottom-to-top
order. Each connected `clip` contains a `video` referencing a timeless PNG asset.
The asset has `duration="0s"` and a separate image format without a frame duration;
the wrapper and video carry the exposure duration. Stills are not incorrectly
declared as ordinary finite-duration `asset-clip` video files. An empty timeline
still retains its complete duration through the primary gap.

Every rendered PNG is the full canvas viewport with straight, non-premultiplied
alpha. Layer transforms and vector paths are rendered into those per-layer
pixels, but layer opacity, visibility, and blend mode remain separate timeline
properties. They are not applied a second time to the PNG. `source.iisc` retains
the native vector paths and transforms; PNG editing does not retain vector
editability. The XML format does not preserve native private metadata by itself.

| Native blend | Legacy `compositemode` | FCPXML `adjust-blend` mode |
| --- | --- | --- |
| SourceOver | `normal` | `0` |
| Multiply | `multiply` | `4` |
| Screen | `screen` | `10` |
| Overlay | `overlay` | `14` |

Legacy opacity is an independent `opacity` motion effect on a 0–100 scale;
FCPXML opacity uses `adjust-blend amount` on a 0–1 scale. Unsupported modes fail
closed. The legacy Apple element catalog omits Overlay from its older list,
although current Premiere/Resolve interoperability documentation lists Overlay
support. The emitted `overlay` token therefore requires target-editor validation;
FCPXML has an explicit documented numeric mapping. Different editors' color
management and compositing implementations can still produce different pixels.
FCPXML declares an SDR Rec.709 sequence and sRGB source-image overrides rather
than relying on an editor's current project or image-profile assumptions.

Legacy still **source** `file` and `file/media/video` durations are whole minutes
covering the sequence, as in Apple's still-image example. Sequence duration,
`clipitem` duration, and source/timeline edit positions remain frame-based. PNG
sources and clips explicitly declare `alphatype=straight` and `stillframe=TRUE`.
FCPXML has no equivalent generic alpha-type attribute; alpha is carried by the
PNG media. Spatial conform is disabled on its full-canvas video element.

## Audio timeline mapping

Native PCM16 mono/stereo audio is referenced through package WAV files. Source
sample rates from 8,000 through 192,000 Hz are retained; both sequences declare
48 kHz stereo output so receiving editors can perform their normal output sample
rate conversion. The source WAV bytes are not resampled, normalized or mixed.
Source media duration is derived from interleaved PCM sample-frame count and the
source sample rate, independent of the sequence's video timebase.

Legacy `media/audio` contains native tracks in their original order. Stereo
clips become two linked mono channel clips, using distinct `sourcetrack`
indices, a shared `file` and matching `link/groupindex` values. A native track
containing stereo uses two adjacent legacy tracks; mono clips use its first
track. Explicit Audio Pan values put stereo source channel 1 on the left and
channel 2 on the right, and mono clips in the center. The source `file` includes
16-bit depth, sample rate, channel count, layout and source channel labels.
Both the track and its clips carry mute/enabled state. Clip `in/out` values are
source frame positions; `start/end` are independent sequence frame positions.

FCPXML uses finite-duration audio-only `asset` resources with exact sample-clock
duration, `hasAudio="1"`, `hasVideo="0"`, `audioChannels` and `audioRate` values.
Each native clip becomes an `asset-clip` below the primary gap, on negative lanes
-1 through -N. `start` is the source offset; `offset` and `duration` use exact
rational sequence time. Stereo components explicitly map source channels `1,2`
to `L,R`. Muted tracks and individually disabled clips remain present with
`enabled="0"`. Native track grouping is represented by lane order and combined
track/clip labels. Receiving editors may normalize role names. Empty audio tracks remain in the
native snapshot and manifest; a trackless FCPXML timeline has no empty lane object.

Native track and clip gain add in dB and become one editable clip adjustment:
legacy `Audio Levels/Level` encodes linear amplitude `10^(gainDb/20)`, while
FCPXML `adjust-volume` carries the sum with a `dB` suffix. Individual values and
mute controls remain separately editable in `source.iisc`. XML clip names include
both native track and clip labels; legacy logging information and FCPXML metadata
also retain the separate labels.

Sample-accurate native trims must not be rounded to video frames. Legacy
`subframeoffset` lacks a sufficiently specified cross-editor sample-clock contract.
The package therefore omits only a minimal WAV prefix when necessary, remapping
the source offset onto an exact video frame. For source sample rate `s` and video
rate `n/d`, the alignment quantum is `s*d/gcd(n,s*d)` sample frames. The omitted
prefix is the native offset modulo this quantum; the remaining XML source offset
is exact, including NTSC rates. The native offset equals the WAV-prefix trim plus
the XML offset. Trailing source handles remain available, and the complete
original PCM stays in `source.iisc`. The version 2 manifest records all three
offsets as decimal strings. The private writer rejects unaligned offsets rather
than quietly changing playback. Audio-only documents use the same full-duration
primary gap and do not require a visual layer.

## Bounds and publication

XML media URLs are absolute `file:` URLs, encoded with Qt's URL encoder and then
escaped by its XML writer. They reference the final published directory, never
the temporary staging directory. Non-ASCII names, spaces, hash signs, ampersands,
and percent signs are preserved. Moving a completed package may require relinking
media in the receiving editor. No external media is copied or fetched by the XML
writer, and no network location is emitted.

The private XML writer validates positive dimensions/frame rate/duration,
UTF-8/XML-safe names, finite in-range opacity, safe relative media paths, and
ordered non-overlapping in-range clips with valid media references. Audio also
validates channel layout, sample rate, source ranges, finite bounded gains and
exact representable source offsets, using checked integer arithmetic. Traversal,
absolute media paths, NUL/control characters, invalid UTF-8, unsupported blends,
and unsupported rates fail closed. Resource and clip identifiers are generated
locally rather than deriving XML IDs from user text.

Both XML byte arrays share one aggregate output limit. A conservative initial
size check precedes string/URL allocation; a bounded output device enforces the
actual serialized byte count. Exact-size limits are accepted. Allocation failure,
writer errors, and limit failures return no partial XML. The surrounding package
exporter separately enforces rendering, media, clip/layer, native snapshot, and
whole-package limits and publishes a new directory atomically. It does not
overwrite an existing output directory or mutate the native source document.

## Verification and source references

`TimelineXmlWriterTest` parses actual emitted documents independently with
`QXmlStreamReader`. It checks timing/gaps, names and URL round-trips, bottom-to-top
tracks/lanes, hidden clips, all four blends, separate opacity, straight alpha,
timeless PNG resources, rational-rate reduction, empty timelines, unsupported
inputs, and aggregate output boundaries. Audio tests cover linked stereo and
mono channels, source trims at NTSC rates, independent gaps, disabled/muted clips,
combined gains, sample-clock durations and audio validation failures. Successful tests write XML fixtures
under `build/test-output/` for additional schema inspection. A well-formed XML
test is not itself proof of a successful import or identical rendered output in
every commercial editor.

When Final Cut Pro is installed, its bundled 1.9 DTD can independently validate
the FCPXML fixture without adding a runtime dependency:

```sh
xmllint --noout --nonet --dtdvalid \
  'file:///Applications/Final%20Cut%20Pro.app/Contents/Frameworks/Interchange.framework/Versions/A/Resources/FCPXMLv1_9.dtd' \
  build/test-output/timeline-writer.fcpxml
```

The audio fixture is `build/test-output/timeline-audio-writer.fcpxml`; its legacy
counterpart is `timeline-audio-writer.xml`. Both fixtures also pass the installed
FCPXML 1.9 DTD and Apple's published legacy v5 DTD. The stdlib development oracle
`tests/verify_timeline_interchange.py` independently checks complete package
manifests, WAV channels/sample counts and hashes, exact source mapping, gains,
mute state, linked legacy channels and negative FCPXML lanes. It can apply those
same assertions to a Final Cut application re-export; schema checks alone remain
distinct from a verified application import.

On 2026-09-05, Final Cut imported an audio-only 24 fps / five-second package and
re-exported FCPXML 1.14. The independent oracle confirmed two negative lanes,
three clips, exact source and timeline trims, enabled/muted states, -9 dB and
-3 dB editable gains, stereo channel mapping and identical WAV bytes copied into
the test library. Final Cut normalized custom audio roles and omitted custom clip
metadata while preserving combined display names. Separate native labels, gain
controls and IDs remain authoritative in `source.iisc` and the manifest. This
audio application check does not establish a Premiere or Resolve application pass.

The adapter uses the existing Qt XML/URL writers rather than introducing a new
XML runtime. The two small domain-to-schema mappings remain separate so one
dialect's compatibility rules do not redefine the other dialect.

Authoritative schema and timing references are Apple's
[legacy XML DTD](https://developer.apple.com/library/archive/documentation/AppleApplications/Reference/FinalCutPro_XML/DTD/DTD.html),
[legacy element catalog](https://developer.apple.com/library/archive/documentation/AppleApplications/Reference/FinalCutPro_XML/Elements/Elements.html),
[legacy timing and audio gain conventions](https://developer.apple.com/library/archive/documentation/AppleApplications/Reference/FinalCutPro_XML/Topics/Topics.html),
[still-image example](https://developer.apple.com/library/archive/documentation/AppleApplications/Reference/FinalCutPro_XML/Applications/Applications.html),
[FCPXML reference](https://developer.apple.com/documentation/professional-video-applications/fcpxml-reference),
[format description](https://developer.apple.com/documentation/professional-video-applications/format),
and [numeric blend mapping](https://developer.apple.com/documentation/professional-video-applications/adjust-blend).
The still-image structure was also compared with an actual Final Cut export in
[OpenFCPXMLKit's ImageSample fixture](https://github.com/TheAcharya/OpenFCPXMLKit/blob/main/Tests/FCPXML%20Samples/FCPXML/ImageSample.fcpxml).
Current target support is described by
[Adobe's XML interchange table](https://helpx.adobe.com/premiere/desktop/organize-media/transfer-files/supported-elements-for-final-cut-pro-x.html)
and the [DaVinci Resolve reference manual](https://documents.blackmagicdesign.com/UserManuals/DaVinci_Resolve_21_Reference_Manual.pdf).
