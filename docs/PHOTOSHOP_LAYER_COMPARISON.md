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

| Area | Before this change | Native contract after this change | Remaining Photoshop-specific work |
|---|---|---|---|
| Layer blends | Normal, Multiply, Screen, Overlay | 27 pixel-layer modes; corrected partial-alpha overlap; all PSD keys and 15 baseline ORA blend functions | Color-managed precision, 16-bit/HDR, private Dissolve sequence and special Fill arithmetic |
| Fill | Only overall opacity | Distinct stored Fill and layer/motion opacity | Styles with independent Fill and the eight special blend formulas |
| Pixel/vector masks | Detached bitmap mask operations only | Asset-backed masks, enable/invert/density/Gaussian feather, linked/unlinked transforms, outside coverage and combined coverage | Editable PSD mask-channel interchange, selection UI and Photoshop edge-refinement algorithms |
| Filter masks | No layer-owned filter mask | Shared non-destructive filter-stack coverage | Full Smart Object/filter host equivalence |
| Filters | Detached 14 photographic effects and color adjustment | Persistent ordered stack, individual enable/blend/opacity, all existing adjustments; Average/Solarize/Invert/Grayscale added | Other native filter kernels, transparent blur expansion and exact Photoshop algorithms for existing approximations |
| Composition | Flat artwork plus isolated artboards | Correct base-shape clipping chains, optional grouped blending, split source/backdrop Blend If | General hierarchical layer groups, Pass Through, knockout, channel exclusions and backdrop adjustment layers |
| Layer styles | No native style model | Explicitly retained as a known gap | Shadows, glows, strokes, overlays, bevel/emboss and style/mask precedence |
| Persistence | IISC 1.19 | IISC 1.20, validated atomic/file-bound edits, resource limits, old-byte preservation | PSD appearance serialization and richer interchange profiles |

The highest-value remaining dependencies are a real nested group/adjustment-layer model, then layer styles with tested Fill/mask precedence, and PSD appearance interchange. None of the new CPU fixture or installed-package checks establish full Photoshop UI parity, consumer application adoption or another platform's GPU/device behavior.

## Primary specifications used

- [Adobe Photoshop layer API](https://developer.adobe.com/photoshop/uxp/ps_reference/classes/layer/): public layer/mask state.
- [Adobe Photoshop blend descriptions](https://helpx.adobe.com/photoshop/desktop/repair-retouch/adjust-light-tone/blending-mode-descriptions.html): mode semantics and depth limitations.
- [Adobe Photoshop file format](https://www.adobe.com/devnet-apps/photoshop/fileformatashtml/): the exact PSD four-byte blend keys.
- [Adobe filter masks](https://helpx.adobe.com/photoshop/desktop/effects-filters/smart-filters/mask-smart-filter-effects.html) and [filter blending](https://helpx.adobe.com/photoshop/desktop/effects-filters/get-started-with-filters/blend-and-fade-filter-effects.html): mask and filter-blend intent.
- [W3C Compositing and Blending](https://www.w3.org/TR/compositing-1/): straight/premultiplied alpha accounting and separable/nonseparable definitions.
- [OpenRaster layer stack](https://www.openraster.org/baseline/layer-stack-spec.html): baseline source-over blend functions and isolated-group semantics.
