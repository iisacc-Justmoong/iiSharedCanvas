# Native layer compositing (0.30.0 / IISC 1.20)

`LayerProperties::appearance` is persistent public aggregate data. Set the complete detached value with `DocumentEditor::setLayerAppearance(layerId, value)`. It validates references and parameter bounds, commits through the existing `DocumentFile` transaction when file-bound, and preserves document state, format and revision on rejection. A real appearance edit, or insertion/replacement of appearance-bearing static/keyframed artwork, upgrades an older model to 1.20. Rendering never changes source assets.

## Pixel-layer blend modes

The 27 modes are Normal (`SourceOver`), Dissolve, Darken, Multiply, ColorBurn, LinearBurn, DarkerColor, Lighten, Screen, ColorDodge, LinearDodge, LighterColor, Overlay, SoftLight, HardLight, VividLight, LinearLight, PinLight, HardMix, Difference, Exclusion, Subtract, Divide, Hue, Saturation, Color and Luminosity. The iiPaintEngine 0.4.1 compositor owns their CPU RGB math. It uses straight ARGB32 and encoded RGB channels with correct source-only/overlap/backdrop-only alpha weighting. Hard Mix thresholds the sum of the two 8-bit channel values at 255, including equality. Dissolve uses a stable layer-ID/document-coordinate key, independent of tile subdivision. It is a repeatable native policy, not a reproduction of Photoshop's private random sequence.

`fillOpacity` multiplies content coverage. Layer `opacity` and sampled motion opacity apply during composition. These values remain separately editable and serializable. There are no layer-style effects in this model; Photoshop's eight special Fill-slider blend algorithms and style-specific Fill behavior are not claimed.

## Masks

`rasterMask`, `vectorMask` and `filterMask` are optional `LayerMask` values. Pixel/filter masks reference a `RasterAsset`; vector masks reference a `VectorAsset`. A vector mask uses its path geometry, with opaque white fill/stroke coverage, retaining the source vector paths. `channel` selects alpha or alpha-weighted Rec.709 luminance. Black is zero coverage and white is full coverage.

`enabled`는 마스크를 삭제하지 않고 우회하고 `inverted`는 커버리지를 반전한다. `density`는 `1-density + density*coverage`이며, `outsideCoverage`는 자산 밖의 커버리지를 지정한다. `featherRadius`는 마스크 자산 픽셀 단위의 Gaussian sigma이고 범위는 0..128, 후광은 3 sigma이다. 0.35.6부터 전체 마스크를 복제하지 않고 [필요한 영역의 타일](MASK_REGIONS.md)을 계산한다.

연결된 마스크는 모션을 포함한 레이어 변환을 따르고, 연결되지 않은 마스크는 문서 또는 소속 아트보드 좌표에 남는다. 래스터와 벡터 마스크는 레이어 alpha를 곱하고, 필터 마스크는 전체 필터 결과와 원본을 혼합한다. 마스크는 자산 원래 해상도에서 샘플링하므로 인접 타일도 같은 feather 값을 사용한다. Smooth 요청은 커버리지를 적분한다. 자산 이름 변경은 참조 ID를 갱신하며 사용 중인 자산의 삭제는 거부한다. `AssetReference::kind`는 Content, RasterMask, VectorMask, FilterMask를 구분한다.

## Non-destructive filters

`filters` stores up to 64 ordered `LayerFilter` values per layer. First-to-last is application order. Each entry preserves a name, enabled state, opacity, blend mode and a variant of `RasterEffect` or `ColorAdjustment`. A filter is processed from the current native-resolution input and blended with it; disabled/zero-opacity entries preserve state. The layer alpha is retained by these photographic RGB operations.

The 18 effects are GaussianBlur, MotionBlur, LensBlur, Texture, Clarity, Dehaze, Vignette, Grain, Sharpen, Noise, LuminanceDenoise, ColorDenoise, LensDistortion, ChromaticAberration, Average, Solarize, Invert and Grayscale. ColorAdjustment retains all existing exposure, tonal, white-balance, saturation, grading and curve parameters. Average uses per-channel integer mean with half-up rounding. Solarize uses the recovered 8-bit rule: values below 128 remain unchanged; other values become 255-value. Invert and Grayscale are independent native operations.

Filters support raster assets, rasterized vector assets and native video frames. Source raster/vector processing is bounded to 64 million pixels; sparse chunked or conditioning content rejects a filter stack. The existing GaussianBlur/LensBlur implementations approximate RGB blur with repeated box passes and preserve source alpha. They do not reproduce Photoshop's entire filter host, blur transparency expansion, 16-bit scale, HDR behavior, lens model or every named native filter.

## Clipping and Blend If

Clipped artwork uses a preceding base in the same artboard composition; missing bases are invalid. A hidden/timeline-absent base suppresses its chain. By default, `blendClippedAsGroup` mixes consecutive clipped layers inside the base's masked/fill-scaled shape without increasing alpha, then applies the base blend mode and opacity once. Disabling the flag blends each clipped layer against the composition, gated by the base shape and opacity. Unrelated background alpha never creates clipping coverage.

`BlendIf` has source and backdrop split ranges and a Gray/Red/Green/Blue channel. Each range satisfies `black <= blackFade <= whiteFade <= white` within [0,1]. Coverage ramps linearly at split edges; the two coverages multiply. Gray uses encoded-RGB luminance weights 0.30/0.59/0.11. This is the native contract, not a verified Photoshop color-managed gray-channel algorithm.

`renderFrame`, `renderFrameRegion`, `renderFrameTiles`, `renderFrameLayers` and `composeFrameLayers` share this composition. `CanvasItem` uses composed frame tiles for appearance-bearing artwork, retaining native CPU semantics through its Qt presentation adapter. This does not establish running consumer-app or physical-device adoption.

레이어 영역이 불투명도 100%의 일반 혼합 조각 하나로 만들어지는 경우에는 그
소유 픽셀을 결과로 옮기고 완전 투명 픽셀을 0으로 정규화한다. 여러 청크가
겹치는 경우에는 기존 순서대로 모든 조각을 합성한다. 이 경계는 투명도, 색상,
샘플링, 변환 및 실시간 미리보기 결과를 바꾸지 않는다.

소유된 레이어 결과를 내부에서 바로 합성할 때 첫 레이어가 일반 혼합이고 레이어·
채움 불투명도가 100%이며 Blend If, 아트보드, 클리핑 그룹의 기반이 없으면 해당 타일을 출력 버퍼로
재사용한다. 완전 투명 픽셀은 기존 소스 오버 결과와 같이 정규화한다. 외부에서
빌린 `composeFrameLayers` 입력이나 이 조건에 해당하지 않는 문서는 기존 합성을
사용한다. 단일 영역 결과는 소유된 출력 픽셀을 복사하지 않고 옮긴다.
`FrameCompositionReuse`는 두 경로의 픽셀과 클리핑·투명도·벡터 배경 및 할당을
비교한다. 이는 CPU 합성 비용에 대한 개선이며 GPU 제출이나 실제 화면 지연을
측정한 결과는 아니다.

## Persistence and interchange

IISC 1.20 persists all fields with explicit tags and little-endian scalars. Unsupported modes, tags, references, nonfinite parameters and resource excesses reject the operation. Older 1.0-1.19 layouts retain their canonical bytes; extended state cannot be encoded under an older version. Serialization limits include per-layer and total filter counts and all filter/mask strings.

PSD pixel-layer import/export maps all 27 documented keys. OpenRaster reads the 15 baseline source-over blend functions. PSD export and timeline interchange reject appearance-bearing documents until those adapters can preserve the state; they cannot silently drop masks, filters, clipping, Fill or Blend If. PSD mask/style/group import remains outside the bounded pixel-layer profile.

## Verification

`LayerCompositingTest` covers all native/PSD blend tags, real mask pixels, density, inversion, disabled and linked/unlinked masks, combined vector masks, feather seams, filter ordering and filter masks, Fill/Opacity multiplication, clipping over opaque unrelated backgrounds, Blend If, legacy canonical bytes, edit rollback, asset reference protection, configurable filter limits, and durable file-bound reopens. iiPaintEngine's `LayerBlendModesTest` supplies independent opaque/partial-alpha reference pixels and repeatable Dissolve checks. See [Photoshop evidence comparison](PHOTOSHOP_LAYER_COMPARISON.md) for provenance and remaining differences.
