# Raster presentation quality (0.25.0)

Source pixels, asset dimensions and transforms remain authoritative. Presentation filtering never changes `.iisc` assets. Binary snapshots and working files round-trip the original ARGB array, including transparency.

`FrameRenderTileRequest::sampling` selects `RasterSampling::Nearest` or `RasterSampling::Smooth`. Existing requests and the four-argument `renderFrameRegion` retain nearest sampling. The five-argument overload selects a policy explicitly. The policy reaches isolated layer tiles, composed frames and decoded video frames. Unknown values are rejected.

Smooth sampling uses pixel-center bilinear interpolation for enlargement and exact area-weighted integration for axis-aligned reduction, including partial edge coverage. Colors are accumulated with alpha premultiplication and returned as straight ARGB; transparent colors cannot cause fringes. Rotated or sheared reduction uses bounded stratified integration (at most 16 × 16 samples per output pixel), rather than exact polygon-area integration. Vector paths keep their direct coverage rasterizer.

`CanvasItem::smoothRendering` defaults to true. Setting false restores nearest sampling and texture presentation for deliberate pixel inspection. Changing the option invalidates presentation tiles and in-flight results without editing assets, writing the working file, or advancing the document revision. Both scene-graph and QPainter presentation follow this setting.

LOD uses logical zoom multiplied by `QQuickWindow::effectiveDevicePixelRatio()`. Its power-of-two reduction is rounded toward retaining detail. At 45% zoom on a 2× display it retains native-resolution tiles; the old logical-only calculation reduced them fourfold. Window attachment and pixel-ratio changes schedule visible rendering again. `renderDevicePixelRatio()` and `renderLevelOfDetail()` expose the effective values for consumer verification. A detached item uses a ratio of one.

Window tracking is disconnected before derived members are destroyed. The later `QQuickItem` detachment cannot reschedule rendering through an expired renderer or window reference. Mounted-canvas destruction is covered by the presentation test and consumer project replacement tests.

No schema migration, preview substitution or source-image resize is introduced. Consumers mount the actual `CanvasItem` in the destination window and select the published presentation policy.

`RasterSamplingTest` checks lossless binary and working-file round trips, checkerboard reduction, identity rendering, tiled parity, alpha filtering, magnification, layer transforms, video frames and rejected policies. `CanvasItemRenderTest` checks presentation, unchanged revisions, display output and mounted-window pixel ratio. Installed-consumer tests exercise the public API from the installed package.
