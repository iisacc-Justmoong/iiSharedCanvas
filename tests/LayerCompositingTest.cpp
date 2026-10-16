#include <iiSharedCanvas.h>
#include <array>
#include <algorithm>
#include <cmath>
#include <filesystem>
#include <iostream>
#include <limits>

using namespace iiSharedCanvas;
namespace {
int failures = 0;
void expect(bool ok, const char *message) { if (!ok) { std::cerr << message << '\n'; ++failures; } }
Document document(int width = 2, int height = 1)
{
    Document d; d.extent = {width, height};
    d.assets.emplace_back(RasterAsset{"pixels", makeRasterLayer(width, height, 0xff804020)});
    d.layers.emplace_back(StaticBitmapLayer{{"layer", "Layer"}, StaticSource{"pixels"}});
    return d;
}
LayerAppearance &appearance(Document &d) { return layerProperties(d.layers[0]).appearance; }
const LayerAppearance &appearance(const Document &d) { return layerProperties(d.layers[0]).appearance; }
std::uint32_t pixel(const Document &d, int x = 0, int y = 0)
{
    const auto result = renderFrame(d, 0);
    expect(result.ok(), "render failed");
    return result.ok() ? result.pixels.pixels[std::size_t(y) * result.pixels.width + x] : 0;
}
LayerMask bitmapMask(Document &d)
{
    d.assets.emplace_back(RasterAsset{"mask", makeRasterLayer(d.extent.width, d.extent.height, 0xffffffff)});
    auto &p = std::get<RasterAsset>(d.assets.back()).pixels;
    p.pixels[0] = 0xff000000;
    LayerMask mask; mask.assetId = "mask"; mask.outsideCoverage = 0;
    return mask;
}
}
int main()
{
    const std::array modes{RasterBlendMode::SourceOver, RasterBlendMode::Multiply,
        RasterBlendMode::Screen, RasterBlendMode::Overlay, RasterBlendMode::Darken,
        RasterBlendMode::Lighten, RasterBlendMode::ColorBurn, RasterBlendMode::LinearBurn,
        RasterBlendMode::DarkerColor, RasterBlendMode::ColorDodge, RasterBlendMode::LinearDodge,
        RasterBlendMode::LighterColor, RasterBlendMode::SoftLight, RasterBlendMode::HardLight,
        RasterBlendMode::VividLight, RasterBlendMode::LinearLight, RasterBlendMode::PinLight,
        RasterBlendMode::HardMix, RasterBlendMode::Difference, RasterBlendMode::Exclusion,
        RasterBlendMode::Subtract, RasterBlendMode::Divide, RasterBlendMode::Hue,
        RasterBlendMode::Saturation, RasterBlendMode::Color, RasterBlendMode::Luminosity,
        RasterBlendMode::Dissolve};
    for (auto mode : modes) {
        auto d = document(); layerProperties(d.layers[0]).blendMode = mode;
        const auto encoded = encodeIisc(d); expect(encoded.ok(), "all layer blend modes must encode");
        if (!encoded.ok()) continue;
        const auto decoded = decodeIisc(encoded.bytes);
        expect(decoded.ok() && layerProperties(decoded.document.layers[0]).blendMode == mode,
            "all layer blend modes must round trip without substitution");
        expect(renderFrame(d, 0).ok(), "all modes must render");
        const auto psd = encodePsd(d);
        expect(psd.ok(), "all pixel-layer blend modes must export to PSD");
        if (psd.ok()) {
            const auto imported = decodeLayeredDocument(psd.bytes);
            expect(imported.ok() && layerProperties(imported.document.layers[0]).blendMode == mode,
                "all 27 PSD keys must survive import/export");
        }
    }
    {
        auto d = document(256);
        auto &pixels = std::get<RasterAsset>(d.assets[0]).pixels.pixels;
        for (unsigned i = 0; i < 256; ++i) pixels[i] = 0x80000000U | (i*0x010101U);
        LayerFilter solar; solar.operation = RasterEffect{RasterEffectKind::Solarize};
        appearance(d).filters = {solar};
        const auto result = renderFrame(d,0);
        expect(result.ok(), "Solarize full byte domain must render");
        if (result.ok()) for (unsigned i = 0; i < 256; ++i) {
            const auto value = i < 128 ? i : 255-i;
            expect(result.pixels.pixels[i] == (0x80000000U | (value*0x010101U)),
                "Solarize preserves alpha and the recovered rule for every byte");
        }
    }
    {
        auto d = document(); std::fill(std::get<RasterAsset>(d.assets[0]).pixels.pixels.begin(),
            std::get<RasterAsset>(d.assets[0]).pixels.pixels.end(),0xff000000);
        d.assets.emplace_back(RasterAsset{"white",makeRasterLayer(2,1,0xffffffff)});
        LayerProperties p; p.id = "hard-mix"; p.blendMode = RasterBlendMode::HardMix;
        d.layers.emplace_back(StaticBitmapLayer{p,StaticSource{"white"}});
        expect(pixel(d) == 0xffffffff, "Hard Mix includes the exact 255 channel-sum boundary");
    }
    {
        auto d = document(16,4); auto &pixels = std::get<RasterAsset>(d.assets[0]).pixels.pixels;
        std::fill(pixels.begin(),pixels.end(),0x80804020);
        layerProperties(d.layers[0]).blendMode = RasterBlendMode::Dissolve;
        const auto full = renderFrame(d,0);
        const auto tiles = renderFrameTiles(d,0,{{{{0,0},{8,4}},{8,4}}, {{{8,0},{8,4}},{8,4}}});
        expect(full.ok() && tiles.ok(), "Dissolve split tiles must render");
        if (full.ok() && tiles.ok()) for (int y = 0; y < 4; ++y) for (int x = 0; x < 16; ++x)
            expect(full.pixels.pixels[y*16+x] == tiles.tiles[x/8].pixels.pixels[y*8+x%8],
                "Dissolve samples must use stable document coordinates across tiles");
    }
    {
        auto d = document(); appearance(d).rasterMask = bitmapMask(d);
        const auto source = std::get<RasterAsset>(d.assets[0]).pixels.pixels;
        expect(pixel(d, 0) == 0 && pixel(d, 1) == 0xff804020, "black hides, white reveals");
        appearance(d).rasterMask->density = 0.5;
        expect((pixel(d, 0) >> 24) == 128 && (pixel(d, 1) >> 24) == 255, "mask density restores coverage");
        appearance(d).rasterMask->density = 1; appearance(d).rasterMask->inverted = true;
        expect(pixel(d, 0) == 0xff804020 && pixel(d, 1) == 0, "mask inversion");
        appearance(d).rasterMask->enabled = false;
        expect(pixel(d, 1) == 0xff804020, "disabled mask retains editable state");
        expect(std::get<RasterAsset>(d.assets[0]).pixels.pixels == source, "rendering cannot change source pixels");
        expect(!encodePsd(d).ok(), "PSD export cannot silently discard native mask state");
    }
    {
        auto d = document(4); appearance(d).rasterMask = bitmapMask(d);
        appearance(d).rasterMask->featherRadius = 1;
        const auto full = renderFrame(d, 0);
        const auto tiles = renderFrameTiles(d, 0, {{{{0,0},{2,1}},{2,1}}, {{{2,0},{2,1}},{2,1}}});
        expect(full.ok() && tiles.ok() && tiles.tiles[0].pixels.pixels[1] == full.pixels.pixels[1]
            && tiles.tiles[1].pixels.pixels[0] == full.pixels.pixels[2], "feathering must have no tile seams");
        expect((full.pixels.pixels[0] >> 24) > 0 && (full.pixels.pixels[0] >> 24) < 255,
            "feather radius must soften actual mask coverage");
    }
    {
        auto d = document(4); appearance(d).rasterMask = bitmapMask(d);
        layerProperties(d.layers[0]).transform.translationX = 1;
        expect(pixel(d, 1) == 0 && pixel(d, 2) == 0xff804020, "linked mask follows layer transform");
        appearance(d).rasterMask->linkedToLayer = false;
        expect(pixel(d, 1) == 0xff804020, "unlinked mask stays in document space");
        VectorPath path; path.commands = {MoveTo{{2,0}}, LineTo{{3,0}}, LineTo{{3,1}}, LineTo{{2,1}}, ClosePath{}};
        path.fill = SolidPaint{0xffffffff}; d.assets.emplace_back(VectorAsset{"vector-mask", {4,1}, {path}});
        LayerMask mask; mask.assetId = "vector-mask"; mask.channel = MaskChannel::Alpha;
        mask.linkedToLayer = false; mask.outsideCoverage = 0; appearance(d).vectorMask = mask;
        expect(pixel(d, 1) == 0 && pixel(d, 2) == 0xff804020, "pixel and vector masks multiply");
    }
    {
        auto d = document(); auto &p = std::get<RasterAsset>(d.assets[0]).pixels;
        p.pixels = {0xff102030, 0xfff0e0d0};
        LayerFilter average; average.operation = RasterEffect{RasterEffectKind::Average};
        appearance(d).filters = {average};
        expect(pixel(d, 0) == 0xff808080 && pixel(d, 1) == 0xff808080, "recovered 8-bit Average kernel");
        LayerFilter solarize; solarize.operation = RasterEffect{RasterEffectKind::Solarize};
        appearance(d).filters = {average, solarize};
        expect(pixel(d) == 0xff7f7f7f, "filter order is application order");
        appearance(d).filters = {solarize, average};
        expect(pixel(d) == 0xff102030, "reordering filters changes output");
        appearance(d).filters = {average}; appearance(d).filterMask = bitmapMask(d);
        expect(pixel(d, 0) == 0xff102030 && pixel(d, 1) == 0xff808080,
            "filter mask hides effects while retaining layer opacity");
        appearance(d).filterMask.reset(); appearance(d).filters[0].opacity = 0.5;
        expect(pixel(d, 0) == 0xff485058, "per-filter opacity blends processed colors with input");
        appearance(d).filters[0].enabled = false;
        expect(pixel(d, 0) == 0xff102030, "disabled filters retain original content");
    }
    {
        auto d = document(); appearance(d).fillOpacity = 0.5;
        expect((pixel(d) >> 24) == 128, "fill opacity attenuates layer content");
        layerProperties(d.layers[0]).opacity = 0.5;
        expect((pixel(d) >> 24) == 64, "overall and fill opacity remain independent");
    }
    {
        auto d = document(); auto &p = std::get<RasterAsset>(d.assets[0]).pixels;
        p.pixels = {0x8000ff00, 0};
        d.assets.emplace_back(RasterAsset{"top", makeRasterLayer(2,1,0xffff0000)});
        d.layers.emplace_back(StaticBitmapLayer{{"clip", "Clipped"}, StaticSource{"top"}});
        layerProperties(d.layers[1]).appearance.clippedToBelow = true;
        expect(pixel(d,0) == 0x80ff0000 && pixel(d,1) == 0, "clipping preserves base shape without alpha inflation");
        d.assets.emplace_back(RasterAsset{"background", makeRasterLayer(2,1,0xff0000ff)});
        d.layers.insert(d.layers.begin(), StaticBitmapLayer{{"background-layer", "Background"}, StaticSource{"background"}});
        expect(pixel(d,1) == 0xff0000ff, "clipping never uses accumulated background alpha");
        layerProperties(d.layers[1]).visible = false;
        expect(pixel(d,0) == 0xff0000ff, "hidden base hides its complete clipping chain");
        d.layers.erase(d.layers.begin(), d.layers.begin()+2);
        expect(!validate(d).ok(), "orphaned clipping must be rejected");
    }
    {
        auto d = document(); BlendIf ranges; ranges.channel = BlendIfChannel::Red;
        ranges.source = {0, 0.5, 1, 1}; appearance(d).blendIf = ranges;
        expect((pixel(d) >> 24) == 255, "split Blend If reaches full coverage above ramp");
        std::get<RasterAsset>(d.assets[0]).pixels.pixels[0] = 0xff400000;
        expect((pixel(d) >> 24) == 128, "split Blend If interpolates source coverage");
    }
    {
        auto d = document(); d.formatVersion.minor = 19;
        const auto old = encodeIisc(d); expect(old.ok(), "legacy document must still encode");
        const auto decoded = decodeIisc(old.bytes);
        expect(decoded.ok() && encodeIisc(decoded.document).bytes == old.bytes, "legacy canonical bytes preserved");
        DocumentEditor editor(d); LayerAppearance a; a.rasterMask = bitmapMask(d);
        expect(editor.setLayerAppearance("layer", a).ok() && d.formatVersion.minor == CurrentFormatMinor,
            "appearance edit upgrades document format");
        const auto revision = editor.revision(); a.rasterMask->assetId = "missing";
        expect(!editor.setLayerAppearance("layer", a).ok() && editor.revision() == revision
            && appearance(d).rasterMask->assetId == "mask", "invalid appearance edit rolls back state and revision");
        expect(!editor.removeAsset("mask").ok(), "mask assets cannot be deleted while referenced");
        const auto references = assetReferences(d,"mask");
        expect(references.size() == 1 && references[0].kind == AssetReferenceKind::RasterMask,
            "mask reference kind is distinguished from layer content");
        expect(editor.renameAsset("mask", "renamed-mask").ok()
            && appearance(d).rasterMask->assetId == "renamed-mask", "renaming updates mask references");
        const auto encoded = encodeIisc(d); const auto reopened = decodeIisc(encoded.bytes);
        expect(encoded.ok() && reopened.ok() && appearance(reopened.document) == appearance(d)
            && pixel(reopened.document,1) == pixel(d,1), "appearance and rendering round trip together");
        SerializationLimits limits; limits.maximumFiltersPerLayer = 0;
        appearance(d).filters.emplace_back();
        expect(!encodeIisc(d, limits).ok(), "filter resource limits apply to encode");
        const auto filtered = encodeIisc(d);
        expect(!decodeIisc(filtered.bytes, limits).ok(), "filter resource limits apply to decode");
    }
    {
        auto d = document(); d.formatVersion.minor = 19;
        auto replacement = d.layers[0]; layerProperties(replacement).appearance.fillOpacity = 0.5;
        DocumentEditor editor(d);
        expect(editor.replaceLayer("layer",replacement).ok() && d.formatVersion.minor == CurrentFormatMinor,
            "replacing an old layer with appearance upgrades atomically");
        d = document(); d.formatVersion.minor = 19;
        layerProperties(replacement).id = "inserted";
        expect(editor.insertLayer(replacement).ok() && d.formatVersion.minor == CurrentFormatMinor,
            "inserting appearance-bearing artwork into a legacy document upgrades atomically");
        d = document(); d.formatVersion.minor = 19; d.timeline.frameCount = 2;
        DynamicBitmapLayer dynamic{{"dynamic","Dynamic"},KeyframedSource{}};
        dynamic.properties.appearance.fillOpacity = 0.5;
        expect(editor.insertKeyframedLayer(dynamic,{{0,"pixels"},{1,"pixels"}}).ok()
            && d.formatVersion.minor == CurrentFormatMinor, "keyframed appearance insertion upgrades atomically");
        d = document(); d.formatVersion.minor = 19; const auto revision = editor.revision();
        layerProperties(replacement).appearance.rasterMask = LayerMask{"missing"};
        expect(!editor.insertLayer(replacement).ok() && d.layers.size() == 1
            && d.formatVersion.minor == 19 && editor.revision() == revision,
            "failed legacy insertion restores version, collection and revision");
    }
    {
        auto d = document(4); d.timeline.frameCount = 2;
        appearance(d).rasterMask = bitmapMask(d);
        auto &p = layerProperties(d.layers[0]);
        p.motion = {{0,MotionValue{}},{1,MotionValue{.position={1,0},.opacity=0.5}}};
        const auto linked = renderFrame(d,1);
        expect(linked.ok() && linked.pixels.pixels[1] == 0 && (linked.pixels.pixels[2] >> 24) == 128,
            "linked masks follow sampled motion while motion opacity is applied once");
        appearance(d).rasterMask->linkedToLayer = false;
        const auto unlinked = renderFrame(d,1);
        expect(unlinked.ok() && (unlinked.pixels.pixels[1] >> 24) == 128,
            "unlinked masks stay in document space during motion");
    }
    {
        auto d = document(); auto &a = appearance(d);
        a.rasterMask = bitmapMask(d); a.rasterMask->inverted = true; a.rasterMask->linkedToLayer = false;
        a.rasterMask->density = 0.67; a.rasterMask->featherRadius = 1.25;
        a.rasterMask->outsideCoverage = 0.3; a.rasterMask->transform.translationX = 0.25;
        LayerFilter effect; effect.name = "Motion filter"; effect.enabled = false; effect.opacity = 0.6;
        effect.blendMode = RasterBlendMode::SoftLight;
        effect.operation = RasterEffect{.kind=RasterEffectKind::MotionBlur,.amount=0.4,.radius=2,.angle=45,
            .scale=0.7,.detail=0.8,.threshold=0.03,.midpoint=0.3,.roundness=0.9,.feather=0.6,
            .monochromatic=false,.edgeAware=true,.natural=true,.seed=314};
        ColorAdjustment color; color.exposure = 0.75; color.offset = 0.1; color.contrast = 0.2;
        color.pivot = 0.4; color.highlights = -0.1; color.highlightRange = 0.6; color.rolloff = 0.3;
        color.shadows = 0.2; color.shadowRange = 0.3; color.blackProtection = 0.4;
        color.whites = 0.04; color.whiteClip = 0.9; color.blacks = 0.05; color.blackClip = 0.1;
        color.temperature = 0.2; color.tint = -0.1; color.vibrance = 0.3; color.lowSaturationBias = 0.8;
        color.saturation = 0.4; color.channelSaturation = -0.2; color.skinProtection = 0.2;
        color.gradeHue = 30; color.gradeBalance = 0.2; color.saturationChannel = 2; color.gradeRange = 2;
        color.curveChannel = 0; color.curve = -0.2; color.protectHighlights = true; color.preserveColor = false;
        color.linearLight = true; color.adaptive = true; color.liftBlacks = true; color.gamutLimit = false;
        color.colorize = true; color.smoothCurve = false;
        LayerFilter adjustment; adjustment.name = "Color adjustment"; adjustment.opacity = 0.4;
        adjustment.blendMode = RasterBlendMode::Color; adjustment.operation = color;
        a.filters = {effect,adjustment}; a.blendIf = BlendIf{BlendIfChannel::Green,{0,0.1,0.9,1},{0.2,0.3,0.7,0.8}};
        const auto encoded = encodeIisc(d); const auto decoded = decodeIisc(encoded.bytes);
        expect(encoded.ok() && decoded.ok() && appearance(decoded.document) == a,
            "all non-default mask, effect and color parameters survive persistence");
        SerializationLimits limits; limits.maximumTotalLayerFilters = 1;
        expect(!encodeIisc(d,limits).ok() && !decodeIisc(encoded.bytes,limits).ok(),
            "aggregate filter budgets are enforced in both directions");
    }
    {
        auto d = document(); appearance(d).rasterMask = bitmapMask(d);
        appearance(d).filters.emplace_back(); appearance(d).filters[0].operation = ColorAdjustment{.exposure = 1};
        const auto output = std::filesystem::path{IISHAREDCANVAS_TEST_OUTPUT_DIR} / "layer-compositing.iisc";
        std::filesystem::create_directories(output.parent_path()); std::filesystem::remove(output);
        DocumentFile file; expect(file.create(output.string(), d).ok(), "create persisted appearance document");
        DocumentEditor editor(file); auto a = appearance(d); a.rasterMask->density = 0.25;
        expect(editor.setLayerAppearance("layer", a).ok(), "file-bound appearance commits synchronously");
        const auto committed = file.revision(); a.fillOpacity = std::numeric_limits<double>::quiet_NaN();
        expect(!editor.setLayerAppearance("layer", a).ok() && file.revision() == committed,
            "invalid file-bound edit cannot reach durable storage");
        file.close(); expect(file.open(output.string()).ok(), "reopen committed appearance document");
        expect(file.document() && layerProperties(file.document()->layers[0]).appearance.rasterMask->density == 0.25,
            "mask/filter state must be durable");
        if (file.document()) expect(renderFrame(*file.document(),0).ok(), "persisted appearance renders");
        file.close(); std::filesystem::remove(output);
    }
    return failures == 0 ? 0 : 1;
}
