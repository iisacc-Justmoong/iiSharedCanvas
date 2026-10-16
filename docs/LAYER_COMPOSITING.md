# Native layer compositing (0.30.0 / IISC 1.20)

`LayerProperties::appearance` is persistent public aggregate data. Set the complete detached value with `DocumentEditor::setLayerAppearance(layerId, value)`. It validates references and parameter bounds, commits through the existing `DocumentFile` transaction when file-bound, and preserves document state, format and revision on rejection. A real appearance edit, or insertion/replacement of appearance-bearing static/keyframed artwork, upgrades an older model to 1.20. Rendering never changes source assets.

## Pixel-layer blend modes

The 27 modes are Normal (`SourceOver`), Dissolve, Darken, Multiply, ColorBurn, LinearBurn, DarkerColor, Lighten, Screen, ColorDodge, LinearDodge, LighterColor, Overlay, SoftLight, HardLight, VividLight, LinearLight, PinLight, HardMix, Difference, Exclusion, Subtract, Divide, Hue, Saturation, Color and Luminosity. The iiPaintEngine 0.4.1 compositor owns their CPU RGB math. It uses straight ARGB32 and encoded RGB channels with correct source-only/overlap/backdrop-only alpha weighting. Hard Mix thresholds the sum of the two 8-bit channel values at 255, including equality. Dissolve uses a stable layer-ID/document-coordinate key, independent of tile subdivision. It is a repeatable native policy, not a reproduction of Photoshop's private random sequence.

`fillOpacity` multiplies content coverage. Layer `opacity` and sampled motion opacity apply during composition. These values remain separately editable and serializable. There are no layer-style effects in this model; Photoshop's eight special Fill-slider blend algorithms and style-specific Fill behavior are not claimed.

## Masks

`rasterMask`, `vectorMask` and `filterMask` are optional `LayerMask` values. Pixel/filter masks reference a `RasterAsset`; vector masks reference a `VectorAsset`. A vector mask uses its path geometry, with opaque white fill/stroke coverage, retaining the source vector paths. `channel` selects alpha or alpha-weighted Rec.709 luminance. Black is zero coverage and white is full coverage.

`enabled` bypasses a mask without deleting it. `inverted` complements coverage. `density` uses `1-density + density*coverage`, so reducing density restores hidden content. `outsideCoverage` explicitly controls the area outside the mask asset. `featherRadius` is a separable Gaussian sigma in mask-asset pixels, truncated at three sigma, in [0,128]. Padding includes the feather halo and uses outside coverage. Prepared mask storage is bounded to 16 million pixels, including padding.

A linked mask's transform is local to the sampled layer transform, including motion. An unlinked mask stays in document or owning-artboard coordinates. Raster and vector masks multiply the final layer alpha. The filter mask mixes the complete processed filter stack with the original layer; it does not hide layer pixels. All masks are prepared at their own native resolution before region sampling, so neighboring tiles use the same feather field. Smooth requests integrate mask coverage. Renaming a referenced asset updates mask IDs; deleting an in-use mask asset is rejected. `AssetReference::kind` distinguishes Content, RasterMask, VectorMask and FilterMask references.

## Non-destructive filters

`filters` stores up to 64 ordered `LayerFilter` values per layer. First-to-last is application order. Each entry preserves a name, enabled state, opacity, blend mode and a variant of `RasterEffect` or `ColorAdjustment`. A filter is processed from the current native-resolution input and blended with it; disabled/zero-opacity entries preserve state. The layer alpha is retained by these photographic RGB operations.

The 18 effects are GaussianBlur, MotionBlur, LensBlur, Texture, Clarity, Dehaze, Vignette, Grain, Sharpen, Noise, LuminanceDenoise, ColorDenoise, LensDistortion, ChromaticAberration, Average, Solarize, Invert and Grayscale. ColorAdjustment retains all existing exposure, tonal, white-balance, saturation, grading and curve parameters. Average uses per-channel integer mean with half-up rounding. Solarize uses the recovered 8-bit rule: values below 128 remain unchanged; other values become 255-value. Invert and Grayscale are independent native operations.

Filters support raster assets, rasterized vector assets and native video frames. Source raster/vector processing is bounded to 64 million pixels; sparse chunked or conditioning content rejects a filter stack. The existing GaussianBlur/LensBlur implementations approximate RGB blur with repeated box passes and preserve source alpha. They do not reproduce Photoshop's entire filter host, blur transparency expansion, 16-bit scale, HDR behavior, lens model or every named native filter.

## Clipping and Blend If

Clipped artwork uses a preceding base in the same artboard composition; missing bases are invalid. A hidden/timeline-absent base suppresses its chain. By default, `blendClippedAsGroup` mixes consecutive clipped layers inside the base's masked/fill-scaled shape without increasing alpha, then applies the base blend mode and opacity once. Disabling the flag blends each clipped layer against the composition, gated by the base shape and opacity. Unrelated background alpha never creates clipping coverage.

`BlendIf` has source and backdrop split ranges and a Gray/Red/Green/Blue channel. Each range satisfies `black <= blackFade <= whiteFade <= white` within [0,1]. Coverage ramps linearly at split edges; the two coverages multiply. Gray uses encoded-RGB luminance weights 0.30/0.59/0.11. This is the native contract, not a verified Photoshop color-managed gray-channel algorithm.

`renderFrame`, `renderFrameRegion`, `renderFrameTiles`, `renderFrameLayers` and `composeFrameLayers` share this composition. `CanvasItem` uses composed frame tiles for appearance-bearing artwork, retaining native CPU semantics through its Qt presentation adapter. This does not establish running consumer-app or physical-device adoption.

## Persistence and interchange

IISC 1.20 persists all fields with explicit tags and little-endian scalars. Unsupported modes, tags, references, nonfinite parameters and resource excesses reject the operation. Older 1.0-1.19 layouts retain their canonical bytes; extended state cannot be encoded under an older version. Serialization limits include per-layer and total filter counts and all filter/mask strings.

PSD pixel-layer import/export maps all 27 documented keys. OpenRaster reads the 15 baseline source-over blend functions. PSD export and timeline interchange reject appearance-bearing documents until those adapters can preserve the state; they cannot silently drop masks, filters, clipping, Fill or Blend If. PSD mask/style/group import remains outside the bounded pixel-layer profile.

## Verification

`LayerCompositingTest` covers all native/PSD blend tags, real mask pixels, density, inversion, disabled and linked/unlinked masks, combined vector masks, feather seams, filter ordering and filter masks, Fill/Opacity multiplication, clipping over opaque unrelated backgrounds, Blend If, legacy canonical bytes, edit rollback, asset reference protection, configurable filter limits, and durable file-bound reopens. iiPaintEngine's `LayerBlendModesTest` supplies independent opaque/partial-alpha reference pixels and repeatable Dissolve checks. See [Photoshop evidence comparison](PHOTOSHOP_LAYER_COMPARISON.md) for provenance and remaining differences.
