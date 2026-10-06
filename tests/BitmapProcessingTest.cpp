#include <iiSharedCanvas.h>
#include <cmath>
#include <iostream>
#include <limits>

namespace {
int failures = 0;
void check(bool ok, const char *message) { if (!ok) { std::cerr << message << '\n'; ++failures; } }
}
int main() {
    using namespace iiSharedCanvas;
    auto source = makeRasterLayer(8, 8);
    for (int y = 0; y < 8; ++y) for (int x = 0; x < 8; ++x)
        source.pixels[y * 8 + x] = 0x80000000U | (x * 32U << 16) | (y * 32U << 8) | 64U;
    const auto original = source.pixels;
    ColorAdjustment neutral;
    auto same = adjustRaster(source, neutral);
    check(same.ok() && same.pixels.pixels == original, "neutral adjustment must be byte exact");
    neutral.exposure = 1;
    auto exposed = adjustRaster(source, neutral);
    check(exposed.ok() && exposed.pixels.pixels != original, "exposure must change RGB");
    for (auto p : exposed.pixels.pixels) check((p >> 24) == 128, "color processing must preserve source alpha");
    neutral.exposure = std::numeric_limits<double>::quiet_NaN();
    check(!adjustRaster(source, neutral).ok(), "nonfinite color parameters must reject");
    RasterEffect blur; blur.kind = RasterEffectKind::GaussianBlur; blur.radius = 2;
    check(effectRaster(source, blur).ok(), "blur must produce valid pixels");
    RasterEffect noise; noise.kind = RasterEffectKind::Noise; noise.amount = 0.4; noise.seed = 27;
    auto first = effectRaster(source, noise), second = effectRaster(source, noise);
    check(first.ok() && first.pixels.pixels == second.pixels.pixels && first.pixels.pixels != original, "noise must be deterministic and effective");
    noise.seed = 28;
    check(effectRaster(source, noise).pixels.pixels != first.pixels.pixels, "noise seed must affect output");
    RasterFill fill; fill.kind = RasterFillKind::Linear; fill.firstArgb = 0xffff0000; fill.secondArgb = 0xff0000ff;
    auto gradient = fillRaster({8, 8}, fill);
    check(gradient.ok() && gradient.pixels.pixels.front() != gradient.pixels.pixels[7], "gradient must interpolate its stops");
    fill.kind = RasterFillKind::Pattern; fill.scale = 2;
    check(fillRaster({8, 8}, fill).pixels.pixels != gradient.pixels.pixels, "pattern must have actual pixels");
    RasterMask mask{{8, 8}, std::vector<std::uint8_t>(64, 0)};
    mask.alpha[0] = 255;
    auto clipped = maskRaster(source, mask);
    check(clipped.ok() && clipped.pixels.pixels.front() == original.front() && clipped.pixels.pixels.back() == 0, "mask must multiply alpha and clear transparent RGB");
    auto feathered = featherMask(mask, 2);
    check(feathered.ok() && feathered.mask.alpha[1] > 0 && feathered.mask.alpha[0] < 255, "feather must spread mask coverage");
    auto solid = makeRasterLayer(3, 1); solid.pixels = {0xffff0000, 0xff0000ff, 0xffff0000};
    auto connected = floodMask(solid, {0, 0}, 0, true), global = floodMask(solid, {0, 0}, 0, false);
    check(connected.ok() && connected.mask.alpha == std::vector<std::uint8_t>({255, 0, 0}), "contiguous wand must stop at an unmatched neighbor");
    check(global.ok() && global.mask.alpha == std::vector<std::uint8_t>({255, 0, 255}), "global wand must select disconnected matching pixels");
    auto merged = combineMasks(connected.mask, global.mask, MaskOperation::Subtract);
    check(merged.ok() && merged.mask.alpha == std::vector<std::uint8_t>({0, 0, 0}), "subtract must implement coverage subtraction");
    auto reconstructed = blendRaster(source, exposed.pixels, mask);
    check(reconstructed.ok() && reconstructed.pixels.pixels[0] == exposed.pixels.pixels[0] && reconstructed.pixels.pixels[1] == source.pixels[1], "masked edit must preserve unselected pixels");
    auto transparent = makeRasterLayer(1, 1), red = makeRasterLayer(1, 1);
    red.pixels[0] = 0xffff0000;
    auto translucent = blendRaster(transparent, red, {{1, 1}, {128}});
    check(translucent.ok() && translucent.pixels.pixels[0] == 0x80ff0000,
          "feathered edits over transparent pixels must retain unassociated source color");
    RasterEffect extremeHaze; extremeHaze.kind = RasterEffectKind::Dehaze;
    extremeHaze.amount = 4; extremeHaze.detail = 1;
    check(effectRaster(source, extremeHaze).ok(), "dehaze at supported extremes must not divide by zero");
    RasterMask wrong{{1, 1}, {255}};
    check(!maskRaster(source, wrong).ok(), "mismatched masks must reject");
    check(source.pixels == original, "all processing must leave input untouched");
    Document document; document.extent = {32, 32};
    document.assets.emplace_back(RasterAsset{"paint", makeRasterLayer(32, 32)});
    document.layers.emplace_back(StaticBitmapLayer{{"layer", "Paint"}, StaticSource{"paint"}});
    BitmapEditor editor(document, "paint"); BitmapBrush brush;
    brush.size = 16; brush.engineState = BrushState{};
    brush.engineState->rasterizer.shape.kind = BrushTipShape::Square;
    check(editor.setBrush(brush), "valid native brush state must apply");
    check(editor.beginStroke({16, 16}) && editor.endStroke({16, 16}), "native configured brush must paint");
    const auto painted = editor.pixels()->pixels;
    check(painted != std::vector<std::uint32_t>(32 * 32), "brush configuration must produce committed pixels");
    check(editor.undo() && editor.pixels()->pixels == std::vector<std::uint32_t>(32 * 32), "configured brush must remain undoable");
    brush.engineState->dynamics.pressureToSize = std::numeric_limits<double>::quiet_NaN();
    check(!editor.setBrush(brush), "invalid native brush dynamics must reject before painting");
    for (auto kind : {RasterEffectKind::MotionBlur, RasterEffectKind::Sharpen, RasterEffectKind::Texture,
        RasterEffectKind::Clarity, RasterEffectKind::Dehaze, RasterEffectKind::Vignette,
        RasterEffectKind::Grain, RasterEffectKind::LuminanceDenoise, RasterEffectKind::ColorDenoise,
        RasterEffectKind::LensDistortion, RasterEffectKind::ChromaticAberration}) {
        RasterEffect effect; effect.kind = kind; effect.amount = 0.5; effect.radius = 2;
        check(effectRaster(source, effect).ok(), "each effect must return valid output");
    }
    return failures ? 1 : 0;
}
