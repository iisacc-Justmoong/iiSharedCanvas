# Layered timeline XML interchange

`exportTimelineInterchange` exports a native document as an editable, media-backed
timeline package. Its two XML representations describe the same layer tracks and
hold exposures; neither representation is a flattened movie or image sequence.
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

## Bounds and publication

XML media URLs are absolute `file:` URLs, encoded with Qt's URL encoder and then
escaped by its XML writer. They reference the final published directory, never
the temporary staging directory. Non-ASCII names, spaces, hash signs, ampersands,
and percent signs are preserved. Moving a completed package may require relinking
media in the receiving editor. No external media is copied or fetched by the XML
writer, and no network location is emitted.

The private XML writer validates positive dimensions/frame rate/duration,
UTF-8/XML-safe names, finite in-range opacity, safe relative media paths, and
ordered non-overlapping in-range clips with valid media references. Traversal,
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
inputs, and aggregate output boundaries. Successful tests write XML fixtures
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

The adapter uses the existing Qt XML/URL writers rather than introducing a new
XML runtime. The two small domain-to-schema mappings remain separate so one
dialect's compatibility rules do not redefine the other dialect.

Authoritative schema and timing references are Apple's
[legacy XML DTD](https://developer.apple.com/library/archive/documentation/AppleApplications/Reference/FinalCutPro_XML/DTD/DTD.html),
[legacy element catalog](https://developer.apple.com/library/archive/documentation/AppleApplications/Reference/FinalCutPro_XML/Elements/Elements.html),
[still-image example](https://developer.apple.com/library/archive/documentation/AppleApplications/Reference/FinalCutPro_XML/Applications/Applications.html),
[FCPXML reference](https://developer.apple.com/documentation/professional-video-applications/fcpxml-reference),
[format description](https://developer.apple.com/documentation/professional-video-applications/format),
and [numeric blend mapping](https://developer.apple.com/documentation/professional-video-applications/adjust-blend).
The still-image structure was also compared with an actual Final Cut export in
[OpenFCPXMLKit's ImageSample fixture](https://github.com/TheAcharya/OpenFCPXMLKit/blob/main/Tests/FCPXML%20Samples/FCPXML/ImageSample.fcpxml).
Current target support is described by
[Adobe's XML interchange table](https://helpx.adobe.com/premiere/desktop/organize-media/transfer-files/supported-elements-for-final-cut-pro-x.html)
and the [DaVinci Resolve reference manual](https://documents.blackmagicdesign.com/UserManuals/DaVinci_Resolve_21_Reference_Manual.pdf).
