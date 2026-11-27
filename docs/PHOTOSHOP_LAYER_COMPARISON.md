# Photoshop layer evidence and native coverage comparison

The reference is the local reverse-engineering project at `Engineering/Projects/Adobe/Photoshop`, inspected on 2026-10-14. The recovered evidence reports Photoshop 2026 27.10.0; the currently installed bundle is 27.11.0. Seven of the eight layer-related recovered text files are byte-identical to the current bundle. The differing mask-properties layout adds an Enhance Edge control and tooltip without changing the observed density/feather/inversion declarations. This is a UI declaration, not proof of the Enhance Edge algorithm. Recovered JavaScript and Eve/EXV text expose shipped interfaces and state declarations. They do not recover the full original C++ layer compositor. Implementations below are independently written C++23 code; no proprietary layer implementation is linked or copied into iiSharedCanvas.

## Evidence inspected

- `recovered/javascript/shipped/Required/Plug-ins/Generator/assets.generate/lib/dom/layer.js`: layer bounds, effects bounds, visibility, clipped state, masks, blend options, protection, vector paths, parent groups, typed adjustment/pixel/shape/smart-object layers and dependency traversal. The adjustment layer depends on underlying layers unless clipped.
- The adjacent `mask.js`: enabled state, bounds and extend-with-white behavior, preserved separately from mask removal.
- The adjacent `layereffects.js`: independently enabled bevel/emboss, strokes, satin, shadows, solid/gradient/pattern overlays and glows, including multiple effect entries.
- `recovered/interface/Required/drover_layouts/layers/panels/layers_panel.eve` and `Required/layouts/Layers/Panels/layers-2490.exv`: layer panel structure and controls.
- `recovered/interface/Required/layouts/Masks/Properties/masks-2662.exv`: separate pixel, vector and filter mask controls, density, feather and inversion.
- `recovered/interface/Required/layouts/Filters/Dialogs/filterBlendingOptions-3061.exv`: per-filter opacity/blend controls.
- `recovered/interface/Required/layouts/Styles/Dialogs/tLayerEffectsDialog-3230.exv`: Fill, clipped-group blending, knockout, channel restrictions, mask/effect relationships and split source/backdrop Blend If controls.
- `docs/Filters.md`, `recovered/native/filters/Average` and `recovered/native/filters/Solarize`: isolated reconstructed kernel rules and their stated native replay boundary. Clouds/NTSC Colors exports are observations, not verified buildable kernels.

The table separates source declarations from verified native behavior. See `build/photoshop-layer-source-evidence.json` for current recovered/source byte identities when available.

## Coverage delta

- Area: Layer blends; Before this change: Normal, Multiply, Screen, Overlay; Native contract after this change: 27 pixel-layer modes; corrected partial-alpha overlap; all PSD keys and 15 baseline ORA blend functions; Remaining Photoshop-specific work: Color-managed precision, 16-bit/HDR, private Dissolve sequence and special Fill arithmetic
- Area: Fill; Before this change: Only overall opacity; Native contract after this change: Distinct stored Fill and layer/motion opacity; Remaining Photoshop-specific work: Styles with independent Fill and the eight special blend formulas
- Area: Pixel/vector masks; Before this change: Detached bitmap mask operations only; Native contract after this change: Asset-backed masks, enable/invert/density/Gaussian feather, linked/unlinked transforms, outside coverage and combined coverage; Remaining Photoshop-specific work: Editable PSD mask-channel interchange, selection UI and Photoshop edge-refinement algorithms
- Area: Filter masks; Before this change: No layer-owned filter mask; Native contract after this change: Shared non-destructive filter-stack coverage; Remaining Photoshop-specific work: Full Smart Object/filter host equivalence
- Area: Filters; Before this change: Detached 14 photographic effects and color adjustment; Native contract after this change: Persistent ordered stack, individual enable/blend/opacity, all existing adjustments; Average/Solarize/Invert/Grayscale added; Remaining Photoshop-specific work: Other native filter kernels, transparent blur expansion and exact Photoshop algorithms for existing approximations
- Area: Composition; Before this change: Flat artwork plus isolated artboards; Native contract after this change: Correct base-shape clipping chains, optional grouped blending, split source/backdrop Blend If; Remaining Photoshop-specific work: General hierarchical layer groups, Pass Through, knockout, channel exclusions and backdrop adjustment layers
- Area: Layer styles; Historical status at the comparison: no native style model. The subsequent 1.25 native implementation covers bounded shadows, glows, strokes, color overlays and a basic bevel; Photoshop-specific variants and parity remain unverified. See [layer content operations](LAYER_CONTENT_OPERATIONS.md).
- Area: Persistence; Before this change: IISC 1.19; Native contract after this change: IISC 1.20, validated atomic/file-bound edits, resource limits, old-byte preservation; Remaining Photoshop-specific work: PSD appearance serialization and richer interchange profiles

Nested groups, backdrop adjustments and a bounded native style model were subsequently added. PSD appearance interchange and Photoshop-specific style, blending and mask parity remain open. None of the CPU fixture or installed-package checks establish full Photoshop UI parity, consumer application adoption or another platform's GPU/device behavior.

## Primary specifications used

- [Adobe Photoshop layer API](https://developer.adobe.com/photoshop/uxp/ps_reference/classes/layer/): public layer/mask state.
- [Adobe Photoshop blend descriptions](https://helpx.adobe.com/photoshop/desktop/repair-retouch/adjust-light-tone/blending-mode-descriptions.html): mode semantics and depth limitations.
- [Adobe Photoshop file format](https://www.adobe.com/devnet-apps/photoshop/fileformatashtml/): the exact PSD four-byte blend keys.
- [Adobe filter masks](https://helpx.adobe.com/photoshop/desktop/effects-filters/smart-filters/mask-smart-filter-effects.html) and [filter blending](https://helpx.adobe.com/photoshop/desktop/effects-filters/get-started-with-filters/blend-and-fade-filter-effects.html): mask and filter-blend intent.
- [W3C Compositing and Blending](https://www.w3.org/TR/compositing-1/): straight/premultiplied alpha accounting and separable/nonseparable definitions.
- [OpenRaster layer stack](https://www.openraster.org/baseline/layer-stack-spec.html): baseline source-over blend functions and isolated-group semantics.
