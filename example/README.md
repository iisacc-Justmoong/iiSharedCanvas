# iiSharedCanvas native examples

The runnable contracts and examples are maintained in [`tests/`](../tests/). Build them in the repository `build/` directory. Use the installed native SDK kit documented in `docs/`; the Congregation integration uses C++23 and contains no Qt runtime.

`VisualCodecVideoDecoder.h/.cpp` shows how an application can compose the
native frame-decoder interface with iiVisualCodec 0.4.2. It handles canonical
right-angle display matrices while rejecting other display or timing normalization; see
[the video decoder contract](../docs/VIDEO_FRAME_DECODER.md) for its limits and
the installed-package integration test.

`VisualCodecVideoEncoder.h/.cpp` composes the matching native frame-encoder
interface with iiVisualCodec's in-memory video codec. It preserves alpha with
FFV1 and explicitly mattes alpha for opaque codecs. See [the video encoder
contract](../docs/VIDEO_FRAME_ENCODER.md) for memory, cancellation, and format
limits.

`GuiFramePresenter.h/.cpp` converts a detached native canvas frame to an
immutable iiGUI image while preserving straight alpha and the frame origin.
The application owns widget updates, input and scheduling; this does not replace
the complete Qt Quick editor. See [the frame presentation contract](../docs/GUI_FRAME_PRESENTATION.md).

`XmlSvgDecoder.h/.cpp` shows an application composing the native canvas with
iiXml and iiCompression to recover the editable paths emitted by the native
SVG/SVGZ exporter. It
rejects constructs outside that narrow subset. See [the SVG adapter
contract](../docs/XML_SVG_IMPORT.md) and its installed-package consumer test.

`GuiSvgRasterizer.h/.cpp` demonstrates explicit bitmap import using iiGUI,
iiXml and iiCompression. It supports a restricted SVG/SVGZ subset, warns that
the result is flattened, and rejects unsupported constructs. It does not
rasterize PDF. See the same [SVG contract](../docs/XML_SVG_IMPORT.md) for
the exact supported syntax, resource limits and installed-consumer test.
