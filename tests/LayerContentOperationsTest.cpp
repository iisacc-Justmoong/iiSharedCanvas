#include <iiSharedCanvas>
#include <Render/Compositor.h>

#include <iostream>
#include <filesystem>
#include <array>
#include <cmath>
#include <limits>
#include <string_view>

using namespace iiSharedCanvas;

namespace {
int failures = 0;
void expect(bool value, std::string_view message)
{
    if (!value) { ++failures; std::cerr << message << '\n'; }
}

// Construct a real document so the new operations use the same renderer and
// validation rules as the SDK's other document-facing entry points.
Document sample()
{
    Document document;
    document.extent = {3, 2};
    document.assets.emplace_back(RasterAsset{"paint", makeRasterLayer(3, 2, 0xffff0000)});
    document.assets.emplace_back(RasterAsset{"mask", makeRasterLayer(3, 2, 0xffffffff)});
    document.assets.emplace_back(VectorAsset{"path", {3, 2}, {}});
    document.layers.emplace_back(StaticBitmapLayer{{"layer", "Layer"}, StaticSource{"paint"}});
    return document;
}

// An external controller resolves the source and executes the stages in order.
FrameRenderResult runRasterization(const Document &document, std::string_view id, FrameIndex frame,
    CanvasRegion region, CanvasExtent extent, RasterSampling sampling = RasterSampling::Nearest)
{
    const auto index=layerIndex(document,std::string(id));
    if (!index) return {region.origin,{},FrameRenderStatus::LayerOutOfRange,"missing layer"};
    const FrameRenderTileRequest request{region,extent,sampling};
    const auto rendered=renderFrameLayerTiles(document,frame,*index,{request});
    bool visible=true;
    auto group=document.relations.groups.groupOf(id);
    while (!group.empty()) {
        const auto *parent=document.relations.groups.find(group);
        if (!parent) return {region.origin,{},FrameRenderStatus::InvalidDocument,"missing group"};
        visible=visible && parent->visible;
        group=parent->parentId;
    }
    return LayerRasterizationPass{}(rendered,request,visible);
}

void masks()
{
    auto document = sample();
    iiSharedCanvas::LayerMask mask; mask.assetId = "mask"; mask.inverted = true; mask.density = .5;
    auto raster = LayerMaskOperations{}(document, "layer", mask);
    expect(raster.ok() && raster.appearance.rasterMask &&
           raster.appearance.rasterMask->inverted && raster.appearance.rasterMask->density == .5,
           "raster mask preserves editable parameters");
    expect(!layerProperties(document.layers[0]).appearance.rasterMask,
           "mask computation does not mutate the source document");
    DocumentHistory history(document);
    expect(history.setLayerAppearance("layer", raster.appearance).changed,
           "prepared appearance is published as a history entry");
    const auto encoded = encodeIisc(document);
    const auto decoded = encoded.ok() ? decodeIisc(encoded.bytes) : IiscDecodeResult{};
    expect(encoded.ok() && decoded.ok() &&
           layerProperties(decoded.document.layers[0]).appearance.rasterMask->inverted,
           "mask appearance survives a document round trip");
    expect(history.undo().changed && !layerProperties(document.layers[0]).appearance.rasterMask &&
           history.redo().changed && layerProperties(document.layers[0]).appearance.rasterMask,
           "mask publication can be undone and redone");
    auto locked=document.relations;
    locked.locks.setLayer("layer",LayerLock::Transparency);
    expect(history.setSelectionLayerRelations(locked).changed &&
           !history.setLayerAppearance("layer",LayerAppearance{}).ok(),
           "transparency protection blocks mask changes");
    expect(history.undo().changed,"removing the lock through history restores editability");
    auto constrained=sample(); DocumentHistoryLimits limits; limits.maximumBytes=1;
    DocumentHistory tiny(constrained,limits); const auto before=encodeIisc(constrained).bytes;
    expect(tiny.setLayerAppearance("layer",raster.appearance).code==DocumentHistoryCode::BudgetExceeded &&
           encodeIisc(constrained).bytes==before && !tiny.canUndo(),
           "appearance snapshot storage is budgeted before publication");
    mask.assetId = "path";
    expect(!LayerMaskOperations{}(document, "layer", mask).ok(),
           "a vector asset cannot silently become a raster mask");
    auto vector = VectorMaskOperations{}(document, "layer", mask);
    expect(vector.ok() && vector.appearance.vectorMask && vector.appearance.vectorMask->assetId == "path",
           "vector mask references editable vector geometry");
    expect(!VectorMaskOperations{}(document, "missing", mask).ok(), "missing layer is rejected");
    mask.density = 2;
    expect(!VectorMaskOperations{}(document, "layer", mask).ok(), "invalid mask density is rejected");
}

void workingFile()
{
    auto document=sample();
    document.layers.emplace_back(StaticBitmapLayer{{"second","Second"},StaticSource{"paint"}});
    document.formatVersion.minor=24;
    const auto path=std::filesystem::path(IISHAREDCANVAS_TEST_OUTPUT_DIR)/"content-operations.iisc";
    std::filesystem::create_directories(path.parent_path()); std::filesystem::remove(path);
    DocumentFile file;
    const auto created=file.create(path.string(),document);
    expect(created.ok(),"create a 1.24 working file");
    if (!created.ok()) return;
    DocumentHistory history(file);
    auto appearance=layerProperties(file.document()->layers[0]).appearance;
    LayerStyle effect; effect.kind=LayerStyleKind::Overlay; effect.color=0xff00ff00;
    appearance.styles.effects.push_back(effect);
    expect(history.setLayerAppearance("layer",appearance).changed,"upgrade and persist the first layer's style");
    expect(history.undo().changed && history.redo().changed,"file-bound style history is reversible");
    FillLayerContent fill; fill.id="file-fill"; fill.extent=document.extent; fill.color=0xff123456;
    expect(history.insertLayerWithAsset(fill,StaticBitmapLayer{{"fill","Fill"},StaticSource{"file-fill"}}).changed,
           "insert procedural asset and layer atomically in a working file");
    fill.color=0xff654321;
    expect(history.replaceProceduralContent(fill).changed && history.undo().changed && history.redo().changed,
           "file-bound procedural content is reversible");
    DocumentFile stale; const auto staleOpened=stale.open(path.string());
    expect(staleOpened.ok(),"open an independent working-file session");
    if (staleOpened.ok()) {
        DocumentHistory staleHistory(stale);
        expect(history.setLayerName("second","Changed by another writer").changed,
               "publish another writer's document edit");
        expect(staleHistory.replaceProceduralContent(fill).code==DocumentHistoryCode::Conflict &&
               staleHistory.statistics().undoEntries==0,
               "a file-bound content no-op still detects another writer");
        stale.close();
    }
    file.close();
    const auto opened=file.open(path.string());
    const auto *asset=opened.ok() ? findAsset(*file.document(),"file-fill") : nullptr;
    const auto *saved=asset ? std::get_if<FillLayerContent>(asset) : nullptr;
    const auto *layer=opened.ok() ? findLayer(*file.document(),"layer") : nullptr;
    expect(opened.ok() && file.document()->formatVersion.minor==CurrentFormatMinor &&
           file.document()->layers.size()==3 && saved && saved->color==fill.color && layer &&
           layerProperties(*layer).appearance.styles.effects.size()==1 && findLayer(*file.document(),"second"),
           "format upgrade retains every layer and procedural edit after reopening");
}

void slices()
{
    auto document = sample();
    layerProperties(document.layers[0]).opacity = .5;
    const CanvasRegion region{{1, 0}, {2, 2}};
    const FrameRenderTileRequest request{region,{2,2},RasterSampling::Nearest};
    const auto renderedLayer=renderFrameLayerTiles(document,0,0,{request});
    const auto raster=LayerRasterizationPass{}(renderedLayer,request,true);
    expect(raster.ok() && raster.origin == region.origin && raster.pixels.width == 2 &&
           raster.pixels.height == 2 && (raster.pixels.pixels[0] >> 24) == 128,
           "layer rasterization respects crop, origin and layer opacity");
    LayerSliceDescriptor descriptor;
    descriptor.frame = 0; descriptor.region = region; descriptor.outputExtent = {2, 2};
    const auto renderedComposite=renderFrameRegion(document,0,region,{2,2});
    auto composite=LayerSliceBuilder{}(renderedComposite,region,descriptor);
    expect(composite.ok() && composite.pixels.pixels == raster.pixels.pixels,
           "composite and single-layer slices agree for a one-layer document");
    descriptor.source = LayerSliceSource::Layer; descriptor.layerId = "layer";
    auto oneLayer=LayerSliceBuilder{}(raster,region,descriptor);
    expect(oneLayer.ok() && oneLayer.pixels.pixels == raster.pixels.pixels,
           "the external controller can supply a detached layer raster");
    oneLayer.pixels.pixels[0]=0;
    expect(raster.pixels.pixels[0]!=0,"slice output owns its pixels");
    auto hidden=LayerRasterizationPass{}(renderedLayer,request,false);
    expect(hidden.ok() && hidden.pixels.pixels[0]==0,"ancestor visibility is supplied as data");
    auto invalidLayer=renderedLayer; invalidLayer.tiles.clear();
    expect(!LayerRasterizationPass{}(invalidLayer,request,true).ok(),"missing tile data is rejected");
    invalidLayer=renderedLayer; invalidLayer.clippedToBelow=true;
    expect(!LayerRasterizationPass{}(invalidLayer,request,true).ok(),"backdrop-dependent clipping is rejected");
    expect(!runRasterization(document,"missing",0,region,{2,2}).ok(),"controller rejects missing layers");
    descriptor.region.extent.width=0;
    expect(!LayerSliceBuilder{}(raster,region,descriptor).ok(),"empty crop is rejected");
    descriptor.region={{0,0},{1,1}};
    expect(!LayerSliceBuilder{}(raster,region,descriptor).ok(),"crop outside supplied image is rejected");
    FrameRenderResult source{{10,20},makeRasterLayer(2,2,0),FrameRenderStatus::Success,{}};
    source.pixels.pixels={0xffff0000,0x000000ff,0x000000ff,0x000000ff};
    const CanvasRegion whole{{10,20},{2,2}};
    descriptor.source=LayerSliceSource::Composite; descriptor.region=whole; descriptor.outputExtent={1,1};
    descriptor.sampling=RasterSampling::Smooth;
    auto smooth=LayerSliceBuilder{}(source,whole,descriptor);
    expect(smooth.ok() && smooth.pixels.pixels[0]==0x40ff0000,
           "smooth reduction interpolates premultiplied alpha without transparent color bleed");
    descriptor.region={{11,20},{1,1}}; descriptor.sampling=RasterSampling::Nearest;
    auto crop=LayerSliceBuilder{}(source,whole,descriptor);
    expect(crop.ok() && crop.origin==descriptor.region.origin && crop.pixels.pixels[0]==0,
           "crop uses the supplied world origin and normalizes transparent pixels");
}

void proceduralLayers()
{
    Document document; document.extent = {3, 2};
    document.assets.emplace_back(RasterAsset{"pattern", makeRasterLayer(2, 1, 0xffff0000)});
    std::get<RasterAsset>(document.assets[0]).pixels.pixels[1] = 0xff0000ff;
    FillLayerContent fill; fill.id = "fill"; fill.extent = document.extent;
    fill.kind = FillLayerKind::Pattern; fill.patternAssetId = "pattern";
    document.assets.emplace_back(fill);
    document.layers.emplace_back(StaticBitmapLayer{{"fill-layer", "Fill"}, StaticSource{"fill"}});
    auto rendered = renderFrame(document, 0);
    expect(rendered.ok() && rendered.pixels.pixels[0] == 0xffff0000 &&
           rendered.pixels.pixels[1] == 0xff0000ff && rendered.pixels.pixels[2] == 0xffff0000,
           "pattern fill repeats pixels owned by the document");
    std::array<std::uint32_t,64*64> preview{}; preview[0]=0xff00ff00; preview[1]=0xff0000ff;
    const std::array tiles{BitmapPreviewTile{{{0,0},64,64},preview}};
    const std::array overlays{FrameBitmapOverlay{"pattern",tiles}};
    const auto previewed=renderFrame(document,0,overlays);
    expect(previewed.ok() && previewed.pixels.pixels[0]==0xff00ff00 &&
           previewed.pixels.pixels[2]==0xff00ff00 && renderFrame(document,0).pixels.pixels[0]==0xffff0000,
           "staged pattern edits preview at every repetition without mutating the document");
    {
        DocumentHistory protection(document); auto relations=document.relations;
        relations.locks.setLayer("fill-layer",LayerLock::Pixels);
        const std::array<std::uint32_t,1> pixel{0xff00ff00};
        const std::array patch{BitmapPatch{{{0,0},1,1},pixel}};
        expect(protection.setSelectionLayerRelations(relations).changed &&
               !protection.replaceBitmapPatches("pattern",patch).ok() && protection.undo().changed,
               "shared pattern edits honor the referencing fill layer's inherited pixel lock");
    }
    auto bytes = encodeIisc(document);
    auto restored = bytes.ok() ? decodeIisc(bytes.bytes) : IiscDecodeResult{};
    expect(bytes.ok() && restored.ok() && renderFrame(restored.document, 0).pixels.pixels == rendered.pixels.pixels,
           "fill content round trips with its owned pattern reference");
    DocumentEditor editor(document);
    expect(!editor.removeAsset("pattern").ok() && editor.renameAsset("pattern","renamed-pattern").changed &&
           std::get<FillLayerContent>(*findAsset(document,"fill")).patternAssetId=="renamed-pattern" &&
           renderFrame(document,0).pixels.pixels==rendered.pixels.pixels,
           "owned patterns cannot be deleted while referenced and survive asset renaming");
    fill.patternAssetId="renamed-pattern";
    auto supported = document; supported.formatVersion.minor = 25;
    expect(encodeIisc(supported).ok(), "the 1.25 format retains fill content");
    auto legacy = document; legacy.formatVersion.minor = 24;
    expect(!encodeIisc(legacy).ok(), "old format refuses to lose fill content");
    auto solid = fill; solid.kind = FillLayerKind::Solid; solid.color = 0xff204060;
    solid.patternAssetId.clear();
    auto malformed=solid; malformed.stops={{std::numeric_limits<double>::quiet_NaN(),0xff000000}};
    auto invalid=document; *findAsset(invalid,"fill")=malformed;
    expect(!validate(invalid).ok(),"inactive gradient settings cannot retain non-finite data");
    DocumentHistory history(document);
    expect(history.replaceProceduralContent(solid).changed, "replace fill settings through document history");
    rendered = renderFrame(document, 0);
    expect(rendered.ok() && rendered.pixels.pixels[0] == solid.color, "solid fill remains editable content");
    expect(history.undo().changed && renderFrame(document, 0).pixels.pixels[1] == 0xff0000ff &&
           history.redo().changed && renderFrame(document, 0).pixels.pixels[0] == solid.color,
           "fill settings are reversible without changing the pattern asset");
    auto gradient = solid; gradient.kind = FillLayerKind::LinearGradient;
    gradient.start = {0, 0}; gradient.end = {3, 0};
    gradient.stops = {{0, 0xff000000}, {1, 0xffffffff}};
    expect(history.replaceProceduralContent(gradient).changed, "replace fill with a gradient");
    rendered = renderFrame(document, 0);
    expect(rendered.ok() && rendered.pixels.pixels[0] != rendered.pixels.pixels[2],
           "gradient fill interpolates its stops across the layer");
    SerializationLimits noStops; noStops.maximumTotalFillGradientStops=0;
    expect(!encodeIisc(document,noStops).ok(),"gradient stop budgets are enforced on encode");
    const auto gradientBytes=encodeIisc(document);
    expect(gradientBytes.ok() && !decodeIisc(gradientBytes.bytes,noStops).ok(),
           "gradient stop budgets are enforced before decode allocations");
    auto radial=gradient; radial.kind=FillLayerKind::RadialGradient;
    expect(history.replaceProceduralContent(radial).changed && renderFrame(document,0).ok(),
           "radial gradient is rendered through the same content contract");
    expect(history.undo().changed,"radial gradient change is reversible");
    auto appearance=layerProperties(document.layers[0]).appearance;
    LayerFilter filter; filter.operation=ColorAdjustment{.exposure=1}; appearance.filters.push_back(filter);
    expect(history.setLayerAppearance("fill-layer",appearance).changed &&
           renderFrame(document,0).pixels.pixels!=rendered.pixels.pixels && history.undo().changed,
           "procedural fills participate in the existing non-destructive filter pipeline");

    document.assets.emplace_back(AdjustmentLayerContent{"adjust", document.extent, ColorAdjustment{.exposure = 1}});
    document.layers.emplace_back(StaticBitmapLayer{{"adjust-layer", "Adjustment"}, StaticSource{"adjust"}});
    auto adjusted = renderFrame(document, 0);
    expect(adjusted.ok() && adjusted.pixels.pixels != rendered.pixels.pixels,
           "adjustment changes the already-composited pixels below it");
    auto unsupported=document;
    unsupported.assets.emplace_back(RasterAsset{"clipped-pixels",makeRasterLayer(3,2,0xffffffff)});
    unsupported.layers.emplace_back(StaticBitmapLayer{{"clipped","Clipped"},StaticSource{"clipped-pixels"}});
    layerProperties(unsupported.layers.back()).appearance.clippedToBelow=true;
    expect(!validate(unsupported).ok(),"adjustments cannot silently become clipping bases");
    auto delayed=document; delayed.timeline.frameCount=2;
    layerProperties(delayed.layers.back()).frameRange=LayerFrameRange{1,1};
    layerProperties(delayed.layers.back()).blendMode=RasterBlendMode::Multiply;
    expect(!validate(delayed).ok(),"invalid adjustment combinations are rejected even outside frame zero");
    const std::vector<FrameRenderTileRequest> requests={{{{0,0},{3,2}},{3,2}}};
    const auto batch=renderFrameLayers(document,0,requests);
    const auto recomposed=composeFrameLayers(batch);
    expect(recomposed.ok() && recomposed.tiles[0].pixels.pixels==adjusted.pixels.pixels,
           "detached layer composition retains backdrop adjustments");
    LayerGroup group; group.id = "group"; group.mode = LayerGroupMode::PassThrough;
    expect(document.relations.groups.insert(group) && document.relations.groups.assign("adjust-layer", "group"),
           "attach adjustment to a pass-through group");
    auto pass = renderFrame(document, 0);
    document.relations.groups.setMode("group", LayerGroupMode::Isolated);
    auto isolated = renderFrame(document, 0);
    expect(pass.ok() && isolated.ok() && pass.pixels.pixels == adjusted.pixels.pixels &&
           isolated.pixels.pixels == rendered.pixels.pixels,
           "adjustments honor pass-through and isolated group backdrops");
    expect(!runRasterization(document, "adjust-layer", 0, {{0, 0}, {3, 2}}, {3, 2}).ok(),
           "an adjustment cannot be rasterized without a backdrop");
}

void styles()
{
    auto pixels=makeRasterLayer(5,5,0); pixels.pixels[2*5+2]=0xffffffffU;
    LayerStyle style; style.kind=LayerStyleKind::Shadow; style.color=0xff000000U;
    style.offsetX=1; style.radius=0;
    LayerStyleStack stack; stack.effects.push_back(style);
    auto evaluated=LayerStyleEvaluator{}(pixels,stack,0);
    expect(evaluated.ok() && (evaluated.pixels.pixels[2*5+3]>>24)>0 &&
           (evaluated.pixels.pixels[2*5+2]>>24)==0,
           "shadow uses the original alpha even when fill opacity is zero");
    for (const auto kind:{LayerStyleKind::Glow,LayerStyleKind::Stroke}) {
        stack.effects[0].kind=kind; stack.effects[0].offsetX=0; stack.effects[0].radius=1;
        evaluated=LayerStyleEvaluator{}(pixels,stack,1);
        expect(evaluated.ok() && (evaluated.pixels.pixels[2*5+1]>>24)>0,
               "glow and stroke extend outside the original alpha");
    }
    stack.effects[0].kind=LayerStyleKind::Overlay; stack.effects[0].color=0xffff0000;
    evaluated=LayerStyleEvaluator{}(pixels,stack,1);
    expect(evaluated.ok() && evaluated.pixels.pixels[2*5+2]==0xffff0000,
           "overlay colors the original shape");
    auto translucent=pixels; translucent.pixels[2*5+2]=0x80ffffff;
    const auto overlay=LayerStyleEvaluator{}(translucent,stack,1);
    expect(overlay.ok() && overlay.pixels.pixels[2*5+2]==0x80ff0000,
           "an overlay changes color without increasing partially transparent shape alpha");
    stack.effects[0].kind=LayerStyleKind::Bevel; stack.effects[0].color=0xff000000;
    evaluated=LayerStyleEvaluator{}(pixels,stack,1);
    expect(evaluated.ok() && evaluated.pixels.pixels[2*5+2]!=0xffffffff,
           "bevel shades an edge of the original shape");
    const auto plain=LayerStyleEvaluator{}(translucent,LayerStyleStack{},.5);
    expect(plain.ok() && plain.pixels.pixels[2*5+2]==
           compositeRasterPixel(0x80ffffffU,0,RasterBlendMode::SourceOver,.5,0),
           "style evaluation matches straight-alpha source-over for partial opacity");

    // Compare the evaluator's local arithmetic with the SDK compositor over
    // partially transparent shadow backdrops, including fractional opacity.
    for (const auto value:{0x00808080U,0x017f80f0U,0x407f80f0U,0x807f80f0U,0xff7f80f0U}) {
        auto point=makeRasterLayer(1,1,value);
        LayerStyle shadow; shadow.kind=LayerStyleKind::Shadow; shadow.color=0x80402010U;
        shadow.opacity=.5; shadow.radius=0; shadow.offsetX=0; shadow.offsetY=0;
        LayerStyleStack effects; effects.effects.push_back(shadow);
        const auto actual=LayerStyleEvaluator{}(point,effects,.63);
        const auto a=static_cast<std::uint32_t>(std::lround(double(value>>24)/255.*128*.5));
        const auto background=a ? (a<<24)|0x00402010U : 0;
        const auto expected=compositeRasterPixel(value,background,RasterBlendMode::SourceOver,.63,0);
        expect(actual.ok() && actual.pixels.pixels[0]==expected,
               "local style compositing matches the SDK reference for partial alpha");
    }

    Document document; document.extent={5,5};
    document.assets.emplace_back(RasterAsset{"paint",pixels});
    document.layers.emplace_back(StaticBitmapLayer{{"layer","Layer"},StaticSource{"paint"}});
    auto appearance=layerProperties(document.layers[0]).appearance;
    stack.effects[0].kind=LayerStyleKind::Shadow; stack.effects[0].radius=1; stack.effects[0].offsetX=1;
    appearance.styles=stack;
    DocumentHistory history(document);
    expect(history.setLayerAppearance("layer",appearance).changed, "publish style stack with history");
    const auto full=renderFrame(document,0);
    const std::vector<FrameRenderTileRequest> requests={{{{0,0},{2,5}},{2,5}},{{{2,0},{3,5}},{3,5}}};
    const auto tiles=renderFrameTiles(document,0,requests);
    bool equal=full.ok() && tiles.ok();
    if (equal) for (int y=0;y<5;++y) for (int x=0;x<5;++x)
        equal &= full.pixels.pixels[y*5+x]==tiles.tiles[x<2?0:1].pixels.pixels[y*(x<2?2:3)+(x<2?x:x-2)];
    expect(equal,"style halos remain identical across tile boundaries");
    const auto encoded=encodeIisc(document); const auto decoded=encoded.ok()?decodeIisc(encoded.bytes):IiscDecodeResult{};
    expect(encoded.ok() && decoded.ok() && layerProperties(decoded.document.layers[0]).appearance.styles==stack,
           "style stack survives IISC encoding");
    auto legacy=document; legacy.formatVersion.minor=24;
    SerializationLimits noStyles; noStyles.maximumTotalLayerStyles=0;
    expect(!encodeIisc(legacy).ok() && !encodeIisc(document,noStyles).ok() &&
           encoded.ok() && !decodeIisc(encoded.bytes,noStyles).ok(),
           "legacy versions and bounded decoders refuse unsupported style data");
    expect(history.undo().changed && layerProperties(document.layers[0]).appearance.styles.effects.empty() &&
           history.redo().changed, "style stack participates in undo and redo");
}
} // namespace

int main()
{
    masks(); slices(); proceduralLayers(); styles(); workingFile();
    return failures ? 1 : 0;
}
