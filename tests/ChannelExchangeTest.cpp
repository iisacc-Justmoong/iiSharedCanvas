#include <Document/Document.h>
#include <Document/DocumentHistory.h>
#include <File/DocumentFile.h>
#include <Bitmap/TiledMask.h>
#include <Bitmap/BitmapCodec.h>
#include <Render/FrameComposite.h>
#include <Bitmap/ChannelExchange.h>
#include <Render/LayerExchange.h>
#include <Serialization/IiscCodec.h>

#include <array>
#include <cstdint>
#include <filesystem>
#include <iostream>
#include <string_view>
#include <vector>

using namespace iiSharedCanvas;
using namespace std::literals;
namespace {
int failures=0;
void expect(bool condition,std::string_view message)
{
    if (!condition) { ++failures; std::cerr << message << '\n'; }
}

std::vector<std::uint8_t> base64(std::string_view text)
{
    constexpr std::string_view alphabet="ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";
    std::vector<std::uint8_t> bytes;
    std::uint32_t bits=0; int available=0;
    for (const auto c:text) {
        if (c=='=') break;
        const auto value=alphabet.find(c);
        if (value==std::string_view::npos) return {};
        bits=(bits<<6)|static_cast<std::uint32_t>(value); available+=6;
        if (available>=8) { available-=8; bytes.push_back(static_cast<std::uint8_t>(bits>>available)); }
    }
    return bytes;
}

Document fixture()
{
    Document document; document.extent={2,1}; document.timeline.frameCount=2;
    document.assets.emplace_back(RasterAsset{"red",makeRasterLayer(2,1,0xffff0000U)});
    document.assets.emplace_back(RasterAsset{"blue",makeRasterLayer(1,1,0xff0000ffU)});
    document.assets.emplace_back(RasterAsset{"green",makeRasterLayer(1,1,0xff00ff00U)});
    document.layers.emplace_back(StaticBitmapLayer{{"base","Base"},StaticSource{"red"}});
    DynamicBitmapContent dynamic; dynamic.frameIndices={0,1};
    document.layers.emplace_back(DynamicBitmapLayer{{"top","Top"},dynamic});
    document.frames={{0,{{"top","blue"}}},{1,{{"top","green"}}}};
    return document;
}

void frameComposites()
{
    auto document=fixture();
    const FrameRenderTileRequest tile{{{0,0},{2,1}},{2,1},RasterSampling::Nearest};
    const auto first=renderFrameLayers(document,0,{tile});
    const auto second=renderFrameLayers(document,1,{tile});
    const std::array selection{"top"sv};
    const auto selected=SelectedFrameComposite{}(second,selection);
    const auto merged=MergedFrameComposite{}(second);
    expect(selected.ok() && selected.tiles.size()==1 &&
           selected.tiles[0].pixels.pixels==std::vector<std::uint32_t>{0xff00ff00U,0},
           "selected composite uses only the selected layer at the caller's frame");
    expect(merged.ok() && merged.tiles.size()==1 &&
           merged.tiles[0].pixels.pixels==std::vector<std::uint32_t>{0xff00ff00U,0xffff0000U},
           "merged composite includes all visible layers at the caller's frame");
    const auto selectedFirst=SelectedFrameComposite{}(first,selection);
    expect(selectedFirst.ok() && selectedFirst.tiles.size()==1 && selectedFirst.tiles[0].pixels.pixels.size()==2 && selectedFirst.tiles[0].pixels.pixels[0]==0xff0000ffU,
           "caller-selected frame, not frame zero, determines the output");

    const std::array masks{RasterMask{{2,1},{128,0}}};
    const auto selectedMasked=SelectedFrameComposite{}(second,selection,masks);
    const auto mergedMasked=MergedFrameComposite{}(second,masks);
    expect(selectedMasked.ok() && selectedMasked.tiles.size()==1 && selectedMasked.tiles[0].pixels.pixels==
               std::vector<std::uint32_t>{0x8000ff00U,0} &&
           mergedMasked.ok() && mergedMasked.tiles.size()==1 && mergedMasked.tiles[0].pixels.pixels==
               std::vector<std::uint32_t>{0x8000ff00U,0},
           "soft selection coverage multiplies output alpha");
    const std::array missing{"missing"sv};
    const std::array duplicate{"top"sv,"top"sv};
    const std::array badMask{RasterMask{{2,1},{255}}};
    expect(!SelectedFrameComposite{}(second,missing).ok() &&
           !SelectedFrameComposite{}(second,duplicate).ok() &&
           !SelectedFrameComposite{}(second,{}).ok() &&
           !SelectedFrameComposite{}(second,selection,{},1).ok() &&
           !MergedFrameComposite{}(second,badMask).ok(),
           "bad selection ids, masks, and work budgets fail without partial output");
    expect(second.layers.size()==2 && second.layers[0].visible && second.layers[1].visible,
           "composition leaves borrowed layer batches unchanged");

    auto clipped=fixture(); layerProperties(clipped.layers[1]).appearance.clippedToBelow=true;
    const auto clippedBatch=renderFrameLayers(clipped,1,{tile});
    const std::array both{"base"sv,"top"sv};
    expect(!SelectedFrameComposite{}(clippedBatch,selection).ok() &&
           SelectedFrameComposite{}(clippedBatch,both).ok(),
           "a clipped selection requires its original clipping base");
    auto replacedBase=fixture();
    replacedBase.assets.emplace_back(RasterAsset{"other",makeRasterLayer(2,1,0xffff00ffU)});
    replacedBase.layers.insert(replacedBase.layers.begin()+1,StaticBitmapLayer{{"other","Other"},StaticSource{"other"}});
    layerProperties(replacedBase.layers.back()).appearance.clippedToBelow=true;
    const auto replacedBatch=renderFrameLayers(replacedBase,1,{tile});
    expect(!SelectedFrameComposite{}(replacedBatch,both).ok(),
           "a selected earlier layer cannot replace an unselected original clipping base");

    Document board; board.extent={2,1};
    board.artboards.push_back({"board","Board",{{0,0},{2,1}},0xffffffffU,true});
    board.assets.emplace_back(RasterAsset{"green",makeRasterLayer(1,1,0xff00ff00U)});
    LayerProperties properties{"top","Top"}; properties.artboardId="board";
    board.layers.emplace_back(StaticBitmapLayer{properties,StaticSource{"green"}});
    const auto boardBatch=renderFrameLayers(board,0,{tile});
    const auto selectedBoard=SelectedFrameComposite{}(boardBatch,selection);
    const auto mergedBoard=MergedFrameComposite{}(boardBatch);
    expect(selectedBoard.ok() && selectedBoard.tiles.size()==1 && selectedBoard.tiles[0].pixels.pixels==
               std::vector<std::uint32_t>{0xff00ff00U,0} &&
           mergedBoard.ok() && mergedBoard.tiles.size()==1 && mergedBoard.tiles[0].pixels.pixels==
               std::vector<std::uint32_t>{0xff00ff00U,0xffffffffU},
           "selected output excludes artboard background while merged output retains it");
}

void channelOperations()
{
    RasterLayer source=makeRasterLayer(2,1,0);
    source.pixels={0x80402010U,0xffc86432U};
    RasterLayer target=makeRasterLayer(2,1,0);
    target.pixels={0xff102030U,0x80406080U};
    const RasterMask mask{{2,1},{255,0}};
    ChannelCompositeOptions options; options.selection=&mask;
    const ChannelSource green{&source,RasterChannel::Green,false};
    const auto applied=ApplyImageComposite{}(green,target,RasterChannel::Blue,options);
    expect(applied.ok() && applied.pixels.pixels==std::vector<std::uint32_t>{0xff102020U,0x80406080U},
           "applying a channel preserves the other components and unselected pixels");
    expect(source.pixels==std::vector<std::uint32_t>{0x80402010U,0xffc86432U} &&
           target.pixels==std::vector<std::uint32_t>{0xff102030U,0x80406080U},
           "channel calculations do not mutate their borrowed inputs");
    const auto alpha=ApplyImageComposite{}({&source,RasterChannel::Alpha,false},target,RasterChannel::Alpha);
    expect(alpha.ok() && alpha.pixels.pixels==std::vector<std::uint32_t>{0x80102030U,0xff406080U},
           "alpha application retains hidden RGB components");
    RasterLayer red=makeRasterLayer(1,1,0x80ff0000U),blue=makeRasterLayer(1,1,0xff0000ffU);
    const auto color=ApplyImageComposite{}({&red,RasterChannel::Composite,false},blue);
    expect(color.ok() && color.pixels.pixels==std::vector<std::uint32_t>{0xff80007fU},
           "full color application uses the source's straight alpha");
    const ChannelSource redChannel{&source,RasterChannel::Red,true};
    const ChannelSource alphaChannel{&target,RasterChannel::Alpha,false};
    ChannelCompositeOptions multiply; multiply.blendMode=RasterBlendMode::Multiply;
    const auto calculated=ChannelCalculations{}(redChannel,alphaChannel,false,multiply);
    expect(calculated.ok() && calculated.mask.alpha==std::vector<std::uint8_t>{191,28},
           "channel calculations invert and multiply individual channels");
    const RasterMask soft{{2,1},{128,0}};
    multiply.selection=&soft;
    const auto selected=ChannelCalculations{}(redChannel,alphaChannel,false,multiply);
    expect(selected.ok() && selected.mask.alpha==std::vector<std::uint8_t>{223,128},
           "soft selection interpolates the calculated channel over the second source");
    ChannelCompositeOptions invalid=multiply; invalid.opacity=2;
    const RasterMask malformed{{2,1},{255}};
    ChannelCompositeOptions badMask; badMask.selection=&malformed;
    expect(!ChannelCalculations{}(redChannel,alphaChannel,false,invalid).ok() &&
           !ChannelCalculations{}({&source,RasterChannel::Composite,false},alphaChannel).ok() &&
           !ApplyImageComposite{}(green,target,RasterChannel::Luminance).ok() &&
           !ApplyImageComposite{}(green,target,RasterChannel::Red,badMask).ok() &&
           !ApplyImageComposite{}({nullptr,RasterChannel::Red,false},target).ok(),
           "invalid channels, opacity, masks and sources are rejected");

#ifndef IISC_EXCHANGE_SKIP_HISTORY
    auto document=fixture(); DocumentHistory history(document);
    const auto updated=ApplyImageComposite{}(green,findRasterAsset(document,"red")->pixels,
                                              RasterChannel::Green,options);
    if (updated.ok()) {
        const std::array patches{BitmapPatch{{{0,0},2,1},updated.pixels.pixels}};
        expect(history.replaceBitmapPatches("red",patches).changed &&
               findRasterAsset(document,"red")->pixels.pixels[0]==0xffff2000U &&
               history.undo().changed && findRasterAsset(document,"red")->pixels.pixels[0]==0xffff0000U &&
               history.redo().changed && findRasterAsset(document,"red")->pixels.pixels[0]==0xffff2000U,
               "applied output publishes and restores through one document history entry");
    } else expect(false,"prepare an image application for history");

    if (calculated.ok()) {
        auto tiled=TiledMask::fromRaster({{0,0},{2,1}},{0,0},calculated.mask);
        auto relations=document.relations;
        const bool added=tiled.ok() && relations.selections.insert({"calc","Calculated",std::make_shared<TiledMask>(tiled.mask)});
        const bool stored=added && history.setSelectionLayerRelations(std::move(relations)).changed;
        expect(stored && document.relations.selections.find("calc") && history.undo().changed &&
               !document.relations.selections.find("calc") && history.redo().changed &&
               document.relations.selections.find("calc")->mask->coverage(0,0)==191,
               "calculated selection can be stored and undone as a named channel");
    } else expect(false,"prepare a calculated channel for history");
#endif
}

#ifndef IISC_EXCHANGE_SKIP_HISTORY
Document sharedFrameFixture()
{
    Document document; document.extent={1,1}; document.timeline.frameCount=3;
    document.assets.emplace_back(RasterAsset{"shared",makeRasterLayer(1,1,0xffff0000U)});
    document.layers.emplace_back(StaticBitmapLayer{{"target","Target"},StaticSource{"shared"}});
    document.layers.emplace_back(StaticBitmapLayer{{"other","Other"},StaticSource{"shared"}});
    return document;
}
std::uint32_t layerPixel(const Document &document,std::string_view id,FrameIndex frame)
{
    const auto *layer=findLayer(document,std::string(id));
    const auto *asset=layer ? resolveAssetAt(document,*layer,frame) : nullptr;
    const auto *raster=asset ? std::get_if<RasterAsset>(asset) : nullptr;
    return raster && !raster->pixels.pixels.empty() ? raster->pixels.pixels[0] : 0;
}
const RasterLayer &sharedPixels(const Document &document)
{
    return findRasterAsset(document,"shared")->pixels;
}
void isolatedFramePublication()
{
    auto document=sharedFrameFixture(); DocumentHistory history(document);
    const auto revision=document.authorship.revision();
    auto result=history.replaceLayerFrameRaster("target",1,sharedPixels(document),{"edited",makeRasterLayer(1,1,0xff00ff00U)},revision);
    expect(result.changed && layerPixel(document,"target",0)==0xffff0000U &&
           layerPixel(document,"target",1)==0xff00ff00U && layerPixel(document,"target",2)==0xffff0000U &&
           layerPixel(document,"other",1)==0xffff0000U && findRasterAsset(document,"shared"),
           "copy-on-write affects only the selected layer and frame");
    expect(history.undo().changed && layerPixel(document,"target",1)==0xffff0000U &&
           std::holds_alternative<StaticBitmapLayer>(document.layers[0]) && !findRasterAsset(document,"edited") &&
           history.redo().changed && layerPixel(document,"target",1)==0xff00ff00U,
           "one history entry restores the original static source and cloned frame");
    const auto encoded=encodeIisc(document);
    const auto reopened=encoded.ok() ? decodeIisc(encoded.bytes) : IiscDecodeResult{};
    expect(encoded.ok() && reopened.ok() && layerPixel(reopened.document,"target",0)==0xffff0000U &&
           layerPixel(reopened.document,"target",1)==0xff00ff00U &&
           layerPixel(reopened.document,"other",1)==0xffff0000U,
           "isolated frame references survive IISC serialization");

    auto dynamic=sharedFrameFixture();
    dynamic.layers[0]=DynamicBitmapLayer{{"target","Target"},KeyframedSource{{0}}};
    dynamic.frames={{0,{{"target","shared"}}}};
    DocumentHistory dynamicHistory(dynamic);
    expect(dynamicHistory.replaceLayerFrameRaster("target",1,sharedPixels(dynamic),{"new",makeRasterLayer(1,1,0xff0000ffU)},
               dynamic.authorship.revision()).changed &&
           layerPixel(dynamic,"target",0)==0xffff0000U && layerPixel(dynamic,"target",1)==0xff0000ffU &&
           layerPixel(dynamic,"target",2)==0xffff0000U && layerPixel(dynamic,"other",1)==0xffff0000U,
           "a held dynamic exposure is split for exactly one frame");
    auto keyed=sharedFrameFixture();
    keyed.assets.emplace_back(RasterAsset{"later",makeRasterLayer(1,1,0xff00ff00U)});
    keyed.layers[0]=DynamicBitmapLayer{{"target","Target"},KeyframedSource{{0,2}}};
    keyed.frames={{0,{{"target","shared"}}},{2,{{"target","later"}}}};
    DocumentHistory keyedHistory(keyed);
    expect(keyedHistory.replaceLayerFrameRaster("target",1,sharedPixels(keyed),{"new",makeRasterLayer(1,1,0xff0000ffU)},
               keyed.authorship.revision()).changed &&
           layerPixel(keyed,"target",0)==0xffff0000U && layerPixel(keyed,"target",1)==0xff0000ffU &&
           layerPixel(keyed,"target",2)==0xff00ff00U && keyedHistory.undo().changed &&
           layerPixel(keyed,"target",2)==0xff00ff00U,
           "a pre-existing next-frame exposure remains unchanged during edit and undo");
    for (const FrameIndex boundary : {FrameIndex{0},FrameIndex{2}}) {
        auto edge=sharedFrameFixture(); DocumentHistory edgeHistory(edge);
        expect(edgeHistory.replaceLayerFrameRaster("target",boundary,sharedPixels(edge),
                   {"new",makeRasterLayer(1,1,0xff0000ffU)},edge.authorship.revision()).changed &&
               layerPixel(edge,"target",boundary)==0xff0000ffU &&
               layerPixel(edge,"target",1)==0xffff0000U && layerPixel(edge,"other",boundary)==0xffff0000U &&
               edgeHistory.undo().changed && layerPixel(edge,"target",boundary)==0xffff0000U,
               "first and last frame edits preserve neighboring and shared exposures");
    }
    auto unchanged=sharedFrameFixture(); DocumentHistory unchangedHistory(unchanged);
    const auto noChange=unchangedHistory.replaceLayerFrameRaster("target",1,sharedPixels(unchanged),
        {"unused",sharedPixels(unchanged)},unchanged.authorship.revision());
    expect(noChange.ok() && !noChange.changed && !unchangedHistory.canUndo() && !findRasterAsset(unchanged,"unused"),
           "identical pixels do not create an asset or history entry");
    auto locked=sharedFrameFixture(); locked.relations.locks.setLayer("target",LayerLock::Pixels);
    DocumentHistory lockedHistory(locked);
    expect(!lockedHistory.replaceLayerFrameRaster("target",1,sharedPixels(locked),{"blocked",makeRasterLayer(1,1,0xff0000ffU)},
               locked.authorship.revision()).ok() && !findRasterAsset(locked,"blocked"),
           "pixel locks prevent isolated frame edits without inserting an asset");
    auto transparent=sharedFrameFixture(); transparent.relations.locks.setLayer("target",LayerLock::Transparency);
    DocumentHistory transparencyHistory(transparent);
    expect(!transparencyHistory.replaceLayerFrameRaster("target",1,sharedPixels(transparent),{"blocked",makeRasterLayer(1,1,0x800000ffU)},
               transparent.authorship.revision()).ok() && !findRasterAsset(transparent,"blocked"),
           "transparency locks reject an isolated edit that changes alpha");
    expect(transparencyHistory.replaceLayerFrameRaster("target",1,sharedPixels(transparent),{"allowed",makeRasterLayer(1,1,0xff0000ffU)},
               transparent.authorship.revision()).changed,
           "transparency locks allow a color change that preserves alpha");
    auto structural=sharedFrameFixture(); structural.relations.locks.setLayer("target",LayerLock::Structure);
    DocumentHistory structuralHistory(structural);
    expect(!structuralHistory.replaceLayerFrameRaster("target",1,sharedPixels(structural),{"blocked",makeRasterLayer(1,1,0xff0000ffU)},
               structural.authorship.revision()).ok() && !findRasterAsset(structural,"blocked"),
           "structure locks prevent splitting a held frame exposure");
    auto inherited=sharedFrameFixture();
    inherited.relations.groups.insert({"group","Group","",LayerGroupMode::PassThrough,true});
    inherited.relations.groups.assign("target","group");
    inherited.relations.locks.setGroup("group",LayerLock::Pixels);
    DocumentHistory inheritedHistory(inherited);
    expect(!inheritedHistory.replaceLayerFrameRaster("target",1,sharedPixels(inherited),{"blocked",makeRasterLayer(1,1,0xff0000ffU)},
               inherited.authorship.revision()).ok() && !findRasterAsset(inherited,"blocked"),
           "inherited group pixel locks prevent frame publication");
    auto otherLocked=sharedFrameFixture(); otherLocked.relations.locks.setLayer("other",LayerLock::Pixels);
    DocumentHistory otherHistory(otherLocked);
    expect(otherHistory.replaceLayerFrameRaster("target",1,sharedPixels(otherLocked),{"new",makeRasterLayer(1,1,0xff0000ffU)},
               otherLocked.authorship.revision()).changed && layerPixel(otherLocked,"other",1)==0xffff0000U,
           "an unrelated locked layer sharing the original asset remains unchanged");
    auto stale=sharedFrameFixture(); DocumentHistory staleHistory(stale);
    expect(staleHistory.replaceLayerFrameRaster("target",1,sharedPixels(stale),{"blocked",makeRasterLayer(1,1,0xff0000ffU)},
               stale.authorship.revision()+1).code==DocumentHistoryCode::Conflict && !findRasterAsset(stale,"blocked"),
           "stale source revisions reject isolated frame publication");
    auto untracked=sharedFrameFixture(); DocumentHistory untrackedHistory(untracked);
    const auto expected=sharedPixels(untracked);
    findRasterAsset(untracked,"shared")->pixels.pixels[0]=0xff101010U;
    expect(untrackedHistory.replaceLayerFrameRaster("target",1,expected,
               {"blocked",makeRasterLayer(1,1,0xff0000ffU)},untracked.authorship.revision()).code==DocumentHistoryCode::Conflict &&
           !findRasterAsset(untracked,"blocked"),
           "untracked source-pixel changes reject stale frame results");
    auto replay=sharedFrameFixture(); DocumentHistory replayHistory(replay);
    expect(replayHistory.replaceLayerFrameRaster("target",1,sharedPixels(replay),
               {"new",makeRasterLayer(1,1,0xff00ff00U)},replay.authorship.revision()).changed,
           "prepare source-conflict undo case");
    findRasterAsset(replay,"shared")->pixels.pixels[0]=0xff101010U;
    expect(replayHistory.undo().code==DocumentHistoryCode::Conflict && replayHistory.canUndo() &&
           layerPixel(replay,"target",1)==0xff00ff00U,
           "undo rejects untracked edits to the retained shared original");
    findRasterAsset(replay,"shared")->pixels.pixels[0]=0xffff0000U;
    expect(replayHistory.undo().changed,"undo succeeds once the shared original is restored");
    findRasterAsset(replay,"shared")->pixels.pixels[0]=0xff202020U;
    expect(replayHistory.redo().code==DocumentHistoryCode::Conflict && replayHistory.canRedo(),
           "redo rejects untracked edits to the retained shared original");
    findRasterAsset(replay,"shared")->pixels.pixels[0]=0xffff0000U;
    expect(replayHistory.redo().changed,"redo succeeds once the shared original is restored");
    auto limited=sharedFrameFixture(); DocumentHistoryLimits limits; limits.maximumBytes=0;
    DocumentHistory limitedHistory(limited,limits);
    expect(!limitedHistory.replaceLayerFrameRaster("target",1,sharedPixels(limited),{"blocked",makeRasterLayer(1,1,0xff0000ffU)},
               limited.authorship.revision()).ok() && !findRasterAsset(limited,"blocked"),
           "history budget rejection leaves the document intact");

    std::filesystem::create_directories(IISHAREDCANVAS_TEST_OUTPUT_DIR);
    iiFileProvider::native::TemporaryDirectory temporary{std::filesystem::path(IISHAREDCANVAS_TEST_OUTPUT_DIR)};
    expect(temporary.isValid(),"prepare isolated-frame working-file directory");
    if (temporary.isValid()) {
        const auto filePath=(temporary.path()/"isolated.iisc").string();
        DocumentFile file;
        if (file.create(filePath,sharedFrameFixture()).ok()) {
            {
                DocumentHistory fileHistory(file);
                expect(fileHistory.replaceLayerFrameRaster("target",1,sharedPixels(*file.document()),{"edited",makeRasterLayer(1,1,0xff00ff00U)},
                           file.document()->authorship.revision()).changed &&
                       fileHistory.undo().changed && fileHistory.redo().changed,
                       "working-file frame publication and undo/redo are atomic");
            }
            file.close(); Document restored;
            expect(DocumentFile::readSnapshot(filePath,restored).ok() &&
                   layerPixel(restored,"target",1)==0xff00ff00U && layerPixel(restored,"target",2)==0xffff0000U &&
                   layerPixel(restored,"other",1)==0xffff0000U,
                   "working-file reopen preserves the isolated frame and shared source");
        } else expect(false,"create isolated-frame working file");
    }
}
#endif

void cssAndSvg()
{
    LayerProperties properties{"shape","Shape"};
    properties.transform.translationX=3;
    properties.opacity=.5;
    FillLayerContent fill; fill.id="fill"; fill.extent={2,1}; fill.color=0x80ff0000U;
    const auto css=LayerCssEncoder{}(properties,{2,1},&fill);
    expect(css.ok() && css.losses.empty() && css.declarations.find("opacity: 0.5")!=std::string::npos &&
           css.declarations.find("matrix(1, 0, 0, 1, 3, 0)")!=std::string::npos &&
           css.declarations.find("#ff000080")!=std::string::npos,
           "CSS describes a solid fill, transform and opacity without claiming losses");
    properties.appearance.rasterMask=LayerMask{};
    const auto lossy=LayerCssEncoder{}(properties,{2,1},&fill);
    expect(lossy.ok() && !lossy.losses.empty(), "CSS reports unsupported masks instead of silently dropping them");
    LayerGroupTree groups;
    expect(groups.insert({"group","Group","",LayerGroupMode::PassThrough,true}) &&
           groups.assign("shape","group"),"prepare CSS group context");
    const auto grouped=LayerCssEncoder{}(properties,{2,1},&fill,&groups);
    expect(grouped.ok() && grouped.losses.size()>lossy.losses.size(),
           "CSS reports enclosing group composition when provided by the caller");
    fill.kind=FillLayerKind::LinearGradient;
    const auto gradientCss=LayerCssEncoder{}(properties,{2,1},&fill);
    expect(gradientCss.ok() && gradientCss.losses.size()>=2,
           "CSS explicitly reports gradient content that it cannot faithfully project");
    expect(!LayerCssEncoder{}(properties,{0,1},&fill).ok(), "CSS rejects invalid dimensions");

    Document document; document.extent={2,1}; document.pixelAspectRatio={2,1};
    VectorPath path;
    path.commands={MoveTo{{0,0}},LineTo{{1,0}},LineTo{{1,1}},ClosePath{}};
    path.fill=SolidPaint{0xff00ff00U};
    VectorAsset vector{"vector",{2,1},{path}};
    document.assets.emplace_back(vector);
    document.layers.emplace_back(StaticVectorLayer{{"shape","Shape"},StaticSource{"vector"}});
    const std::array selected{"shape"sv};
    const CanvasRegion region{{0,0},{2,1}};
    const auto svg=SelectedLayerSvgProjection{}(document,0,selected,region);
    expect(svg.ok() && !svg.rasterized && svg.svg.find("<path ")!=std::string::npos &&
           svg.svg.find("width=\"4\"")!=std::string::npos &&
           svg.svg.find("preserveAspectRatio=\"none\"")!=std::string::npos &&
           svg.svg.find("data:image/")==std::string::npos,
           "SVG preserves vector paths and reflects pixel aspect ratio");
    auto &props=layerProperties(document.layers.front());
    document.assets.emplace_back(RasterAsset{"mask",makeRasterLayer(2,1,0xffffffffU)});
    LayerMask documentMask; documentMask.assetId="mask";
    props.appearance.rasterMask=documentMask;
    const auto missing=SelectedLayerSvgProjection{}(document,0,selected,region);
    const auto raster=makeRasterLayer(2,1,0x80abcdefU);
    const auto fallback=SelectedLayerSvgProjection{}(document,0,selected,region,&raster);
    expect(!missing.ok() && fallback.ok() && fallback.rasterized &&
           fallback.svg.find("data:image/png;base64,")!=std::string::npos &&
           fallback.svg.find("<path ")==std::string::npos,
           "unsupported SVG features require and embed caller-composed fallback pixels");
    const auto marker=fallback.svg.find("data:image/png;base64,");
    if (marker!=std::string::npos) {
        const auto begin=marker+std::string_view("data:image/png;base64,").size();
        const auto end=fallback.svg.find('"',begin);
        const auto png=base64(std::string_view(fallback.svg).substr(begin,end-begin));
        const auto decoded=decodeBitmap(png);
        expect(decoded.ok() && decoded.asset.pixels.pixels==raster.pixels,
               "embedded PNG decodes to the supplied straight-alpha fallback pixels");
    } else expect(false,"SVG fallback contains embedded PNG bytes");
    const std::array absent{"absent"sv};
    expect(!SelectedLayerSvgProjection{}(document,0,absent,region,&raster).ok() &&
           !SelectedLayerSvgProjection{}(document,1,selected,region,&raster).ok(),
           "SVG rejects missing selection and invalid frame");
    MediaLimits small; small.maxOutputBytes=32;
    expect(!SelectedLayerSvgProjection{}(document,0,selected,region,&raster,small).ok(),
           "SVG rejects output that exceeds its byte budget");

    Document mixed; mixed.extent={2,1}; mixed.assets.emplace_back(vector);
    mixed.assets.emplace_back(RasterAsset{"mask",makeRasterLayer(2,1,0xffffffffU)});
    mixed.layers.emplace_back(StaticVectorLayer{{"plain","Plain"},StaticSource{"vector"}});
    LayerProperties maskedProperties{"masked","Masked"}; maskedProperties.appearance.rasterMask=documentMask;
    mixed.layers.emplace_back(StaticVectorLayer{maskedProperties,StaticSource{"vector"}});
    const std::array mixedIds{"plain"sv,"masked"sv};
    const std::array perLayer{SvgLayerRasterFallback{"masked",&raster}};
    const auto hybrid=SelectedLayerSvgProjection{}(mixed,0,mixedIds,region,&raster,{},perLayer);
    expect(hybrid.ok() && hybrid.rasterized && hybrid.rasterizedLayers==std::vector<std::string>{"masked"} &&
           hybrid.svg.find("<path ")!=std::string::npos && hybrid.svg.find("<image ")!=std::string::npos &&
           hybrid.svg.find("<path ")<hybrid.svg.find("<image "),
           "SVG keeps independent vector layers while rasterizing only the masked layer in stack order");
    layerProperties(mixed.layers.back()).blendMode=RasterBlendMode::Multiply;
    const auto flattened=SelectedLayerSvgProjection{}(mixed,0,mixedIds,region,&raster,{},perLayer);
    expect(flattened.ok() && flattened.rasterized && flattened.svg.find("<path ")==std::string::npos,
           "backdrop-dependent blending uses the complete selected composition");
    layerProperties(mixed.layers.back()).blendMode=RasterBlendMode::SourceOver;
    layerProperties(mixed.layers.back()).appearance.clippedToBelow=true;
    const std::array maskedOnly{"masked"sv};
    expect(!SelectedLayerSvgProjection{}(mixed,0,maskedOnly,region,&raster,{},perLayer).ok() &&
           SelectedLayerSvgProjection{}(mixed,0,mixedIds,region,&raster,{},perLayer).ok(),
           "SVG refuses clipped layers when their original selected base is missing");
    Document timed; timed.extent={2,1}; timed.timeline.frameCount=2; timed.assets.emplace_back(vector);
    LayerProperties temporal{"timed","Timed"}; temporal.frameRange=LayerFrameRange{0,0};
    timed.layers.emplace_back(StaticVectorLayer{temporal,StaticSource{"vector"}});
    const std::array timedId{"timed"sv};
    const auto outside=SelectedLayerSvgProjection{}(timed,1,timedId,region);
    expect(outside.ok() && !outside.rasterized && outside.svg.find("<path ")==std::string::npos,
           "a selected layer outside its frame range produces an empty SVG without a fallback");
    timed.layers[0]=StaticVectorLayer{{"timed","Timed"},StaticSource{"vector"}};
    timed.relations.groups.insert({"hidden","Hidden","",LayerGroupMode::PassThrough,false});
    timed.relations.groups.assign("timed","hidden");
    const auto hidden=SelectedLayerSvgProjection{}(timed,0,timedId,region);
    expect(hidden.ok() && !hidden.rasterized && hidden.svg.find("<path ")==std::string::npos,
           "an ancestor-hidden layer produces an empty SVG without a fallback");
}
}

int main()
{
    frameComposites();
    channelOperations();
#ifndef IISC_EXCHANGE_SKIP_HISTORY
    isolatedFramePublication();
#endif
    cssAndSvg();
    return failures ? 1 : 0;
}
