#include <iiSharedCanvas>
#include <Document/CanvasOperations.h>
#include <Bitmap/TiledMask.h>
#include <Video/VideoFrameEncoder.h>
#include <Timeline/TimelineInterchange.h>

#include <algorithm>
#include <array>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <limits>
#include <string_view>

using namespace iiSharedCanvas;
namespace {
int failures=0;
void expect(bool value,std::string_view message)
{
    if (!value) { ++failures; std::cerr << message << '\n'; }
}
void transparentBounds()
{
    auto image=makeRasterLayer(5,4,0x0000ff77U);
    image.pixels[1*5+1]=0x01010203U;
    image.pixels[2*5+3]=0x80000000U;
    const auto all=TransparentBoundsScanner{}(image,{-7,9},0);
    expect(all.ok() && all.bounds==CanvasRegion{{-6,10},{3,2}},
           "nonzero alpha defines exact half-open world bounds, including low alpha");
    const auto strong=TransparentBoundsScanner{}(image,{-7,9},1);
    expect(strong.ok() && strong.bounds==CanvasRegion{{-4,11},{1,1}},
           "an explicit threshold excludes coverage at or below it");
    const auto empty=TransparentBoundsScanner{}(image,{-7,9},128);
    expect(empty.ok() && !empty.bounds,"fully transparent or thresholded images have no bounds");
    auto malformed=image; malformed.pixels.pop_back();
    expect(!TransparentBoundsScanner{}(malformed,{},0).ok(),"truncated pixel buffers fail");
    expect(!TransparentBoundsScanner{}(image,{std::numeric_limits<std::int32_t>::max(),0},0).ok(),
           "coordinate overflow fails before scanning");
}
void cloneDocument()
{
    Document source; source.extent={2,1};
    source.assets.emplace_back(RasterAsset{"pixels",makeRasterLayer(2,1,0xff123456U)});
    source.layers.emplace_back(StaticBitmapLayer{{"layer","Original"},StaticSource{"pixels"}});
    StableDiffusionMetadata generation; generation.positivePrompt="original prompt";
    source.stableDiffusionMetadata=generation;
    PublicationMetadata publication; publication.title="Original title";
    source.publicationMetadata=publication;
    constexpr auto timestamp="2026-11-26T00:00:00.000Z";
    const auto author=iiFileProvider::native::FileAuthor::fromIisaccAccount(
        {{"sub","canvas-clone"},{"email","clone@example.com"},{"displayName","Author"}},
        "https://iisacc.com",timestamp);
    expect(bool(author),"create an explicit test author");
    if (!author) return;
    source.authorship.setAuthor(*author,timestamp);
    DocumentHistory original(source);
    expect(original.setLayerName("layer","Edited").changed && original.canUndo(),"source has independent history");
    auto result=DocumentCloneTransaction{}(source);
    expect(result.ok() && result.document.stableDiffusionMetadata==source.stableDiffusionMetadata &&
           result.document.publicationMetadata==source.publicationMetadata &&
           authorshipDump(result.document.authorship)==authorshipDump(source.authorship) &&
           !result.document.authorship.hasActiveAuthor(),
           "clone preserves metadata while clearing the active editing session");
    if (!result.ok()) return;
    DocumentHistory cloned(result.document);
    expect(!cloned.canUndo() && original.canUndo(),"clone starts with a fresh undo history");
    findRasterAsset(result.document,"pixels")->pixels.pixels[0]=0xffabcdefU;
    expect(findRasterAsset(source,"pixels")->pixels.pixels[0]==0xff123456U,
           "clone owns independently editable pixels");
    SerializationLimits limits; limits.maximumContainerBytes=1;
    expect(!DocumentCloneTransaction{}(source,limits).ok() && original.canUndo(),
           "clone resource failure leaves the source and its history unchanged");
}
void canvasGeometry()
{
    Document source; source.extent={10,8};
    source.assets.emplace_back(RasterAsset{"pixels",makeRasterLayer(2,2,0xffffffffU)});
    source.layers.emplace_back(StaticBitmapLayer{{"layer","Layer"},StaticSource{"pixels"}});
    const auto centered=CanvasResizePlan{}(source,{14,12},CanvasAnchor::Center);
    expect(centered.ok() && centered.plan.target.extent==CanvasExtent{14,12} &&
           centered.plan.contentOffset==CanvasOrigin{2,2},"center resize computes an explicit content shift");
    const auto crop=CanvasCropTransaction{}(source,{{2,3},{4,3}});
    expect(crop.ok() && crop.plan.contentOffset==CanvasOrigin{-2,-3},"crop maps the selected area to the finite origin");
    expect(!CanvasCropTransaction{}(source,{{-1,0},{4,4}}).ok() &&
           !CanvasResizePlan{}(source,{0,3},CanvasAnchor::Center).ok(),"invalid geometry is rejected");
    const auto original=findRasterAsset(source,"pixels")->pixels.pixels;
    DocumentHistory history(source);
    expect(crop.ok() && history.applyCanvasGeometry(crop.plan).changed && source.extent==CanvasExtent{4,3} &&
           layerProperties(source.layers[0]).transform.translationX==-2 &&
           findRasterAsset(source,"pixels")->pixels.pixels==original &&
           history.undo().changed && source.extent==CanvasExtent{10,8} &&
           layerProperties(source.layers[0]).transform.translationX==0 &&
           history.redo().changed && source.extent==CanvasExtent{4,3},
           "non-destructive crop is one reversible geometry edit");

    Document withMask; withMask.extent={10,8};
    withMask.assets.emplace_back(RasterAsset{"pixels",makeRasterLayer(2,2,0xffffffffU)});
    withMask.layers.emplace_back(StaticBitmapLayer{{"layer","Layer"},StaticSource{"pixels"}});
    auto selection=TiledMask::rectangle({{0,0},{10,8}},{3,4},{4,5},false);
    expect(selection.ok() && withMask.relations.selections.insert({"selection","Selection",std::make_shared<const TiledMask>(selection.mask)}),
           "create a named selection");
    DocumentHistory maskHistory(withMask);
    const auto maskCrop=CanvasCropTransaction{}(withMask,{{2,3},{4,3}});
    expect(maskCrop.ok() && maskHistory.applyCanvasGeometry(maskCrop.plan).changed &&
           withMask.relations.selections.find("selection")->mask->coverage(1,1)==255 &&
           maskHistory.undo().changed && withMask.relations.selections.find("selection")->mask->coverage(3,4)==255 &&
           maskHistory.redo().changed && withMask.relations.selections.find("selection")->mask->coverage(1,1)==255,
           "selection channels translate with the crop and are restored by undo and redo");
    auto locked=withMask; locked.relations.locks.setLayer("layer",LayerLock::Transform);
    DocumentHistory lockedHistory(locked);
    const auto lockedPlan=CanvasResizePlan{}(locked,{6,5},CanvasAnchor::Center);
    expect(lockedPlan.ok() && !lockedHistory.applyCanvasGeometry(lockedPlan.plan).ok() &&
           locked.extent==CanvasExtent{4,3} && !lockedHistory.canUndo(),
           "a transform lock rejects the entire canvas shift");
    auto stale=withMask; DocumentHistory staleHistory(stale);
    const auto stalePlan=CanvasResizePlan{}(stale,{6,5},CanvasAnchor::TopLeft);
    expect(staleHistory.setLayerName("layer","New").changed &&
           staleHistory.applyCanvasGeometry(stalePlan.plan).code==DocumentHistoryCode::Conflict,
           "a stale geometry plan cannot overwrite a later edit");
    Document infinite; infinite.extent={8,8}; infinite.canvasMode=CanvasMode::Infinite;
    infinite.infiniteCanvas.origin={-5,-7};
    const auto view=CanvasCropTransaction{}(infinite,{{-3,-4},{3,2}});
    DocumentHistory viewHistory(infinite);
    expect(view.ok() && viewHistory.applyCanvasGeometry(view.plan).changed &&
           canvasRegion(infinite)==CanvasRegion{{-3,-4},{3,2}} && viewHistory.undo().changed &&
           canvasRegion(infinite)==CanvasRegion{{-5,-7},{8,8}},
           "infinite crop changes only the viewport and is reversible");
    const auto path=std::filesystem::path(IISHAREDCANVAS_TEST_OUTPUT_DIR)/"canvas-geometry.iisc";
    std::filesystem::create_directories(path.parent_path()); std::filesystem::remove(path);
    Document originalFile; originalFile.extent={10,8};
    originalFile.assets.emplace_back(RasterAsset{"pixels",makeRasterLayer(2,2,0xffffffffU)});
    originalFile.layers.emplace_back(StaticBitmapLayer{{"layer","Layer"},StaticSource{"pixels"}});
    DocumentFile file; expect(file.create(path.string(),originalFile).ok(),"create canvas geometry working file");
    if (file.document()) {
        auto plan=CanvasCropTransaction{}(*file.document(),{{2,3},{4,3}});
        DocumentHistory fileHistory(file);
        expect(plan.ok() && fileHistory.applyCanvasGeometry(plan.plan).changed && fileHistory.undo().changed &&
               fileHistory.redo().changed,"file-bound crop is one reversible edit");
    }
    file.close(); expect(file.open(path.string()).ok() && file.document()->extent==CanvasExtent{4,3} &&
                        findRasterAsset(*file.document(),"pixels"),"file reopen retains crop and original pixels");
    file.close();
}
void contentBounds()
{
    Document document; document.extent={2,2}; document.timeline.frameCount=2;
    document.assets.emplace_back(RasterAsset{"visible",makeRasterLayer(1,1,0xff112233U)});
    document.assets.emplace_back(RasterAsset{"hidden",makeRasterLayer(1,1,0xff445566U)});
    document.layers.emplace_back(StaticBitmapLayer{{"visible-layer","Visible"},StaticSource{"visible"}});
    LayerProperties hidden{"hidden-layer","Hidden"}; hidden.visible=false;
    hidden.transform.translationX=3; hidden.transform.translationY=1;
    document.layers.emplace_back(StaticBitmapLayer{hidden,StaticSource{"hidden"}});
    const auto before=encodeIisc(document);
    CanvasContentBoundsRequest visible;
    const auto actual=queryCanvasContentBounds(document,visible);
    expect(actual.ok() && actual.bounds==CanvasRegion{{0,0},{1,1}},
           "visible content bounds use the composited alpha");
    CanvasContentBoundsRequest all; all.scope=CanvasContentBoundsScope::AllLayers;
    all.region=CanvasRegion{{-2,-2},{8,6}};
    const auto expanded=queryCanvasContentBounds(document,all);
    expect(expanded.ok() && expanded.bounds==CanvasRegion{{0,0},{4,2}},
           "explicit off-canvas search includes hidden layer pixels");
    all.maximumPixels=1;
    expect(!queryCanvasContentBounds(document,all).ok() && encodeIisc(document).bytes==before.bytes,
           "bounded content scan fails without mutating the document");
}
void orientedArtboard()
{
    Document document; document.extent={4,4};
    Artboard board{"board","Board",{{0,0},{4,4}},0xff112233U,true};
    board.geometry=ArtboardGeometry{{2,2},{1,1,-1,1,2,0}};
    document.artboards.push_back(board);
    auto rendered=renderFrame(document,0);
    expect(rendered.ok() && (rendered.pixels.pixels[0]>>24)==0 &&
           (rendered.pixels.pixels[1*4+1]>>24)>0,
           "oriented artboard background is clipped to its rotated boundary");
    const auto bytes=encodeIisc(document); const auto restored=decodeIisc(bytes.bytes);
    expect(bytes.ok() && restored.ok() && restored.document.artboards==document.artboards,
           "artboard's local rectangle and transform round-trip exactly");
    auto older=document; older.formatVersion.minor=26;
    expect(!encodeIisc(older).ok(),"legacy files cannot discard oriented artboard geometry");
    auto edited=document;
    DocumentEditor editor(edited);
    auto geometry=*edited.artboards[0].geometry; geometry.transform.translationX+=1;
    expect(editor.setArtboardGeometry("board",geometry).changed &&
           edited.artboards[0].region.origin.x==1 && edited.artboards[0].geometry->transform.translationX==3,
           "an oriented artboard can be edited through the document interface");
    DocumentHistory history(document);
    const auto resize=CanvasResizePlan{}(document,{6,6},CanvasAnchor::Center);
    expect(resize.ok() && history.applyCanvasGeometry(resize.plan).changed &&
           document.artboards[0].geometry->transform.translationX==3 &&
           history.undo().changed && document.artboards[0].geometry->transform.translationX==2,
           "canvas shifts and undo carry oriented artboards");
    const auto path=std::filesystem::path(IISHAREDCANVAS_TEST_OUTPUT_DIR)/"oriented-artboard.iisc";
    std::filesystem::remove(path);
    DocumentFile file; expect(file.create(path.string(),document).ok(),"create oriented artboard working file");
    if (file.document()) {
        DocumentHistory fileHistory(file);
        const auto move=CanvasResizePlan{}(*file.document(),{6,6},CanvasAnchor::Center);
        expect(move.ok() && fileHistory.applyCanvasGeometry(move.plan).changed && fileHistory.undo().changed &&
               fileHistory.redo().changed,"oriented artboard working-file edit has undo and redo");
    }
    file.close();
    expect(file.open(path.string()).ok() && file.document()->artboards[0].geometry &&
           file.document()->artboards[0].geometry->transform.translationX==3,
           "working-file reopen restores oriented artboard geometry");
    file.close();
    const auto legacyPath=std::filesystem::path(IISHAREDCANVAS_TEST_OUTPUT_DIR)/"legacy-artboard-upgrade.iisc";
    std::filesystem::remove(legacyPath);
    Document legacy; legacy.extent={4,4}; legacy.formatVersion.minor=26;
    legacy.artboards.push_back({"board","Board",{{0,0},{2,2}},0xffffffffU,true});
    expect(file.create(legacyPath.string(),legacy).ok(),"create 1.26 artboard working file");
    if (file.document()) {
        DocumentHistory upgraded(file);
        expect(upgraded.setPixelAspectRatio({2,1}).changed && upgraded.undo().changed && upgraded.redo().changed,
               "legacy artboard record upgrades with pixel-aspect edits and history");
    }
    file.close();
    expect(file.open(legacyPath.string()).ok() && file.document()->artboards.size()==1 &&
           !file.document()->artboards[0].geometry && file.document()->pixelAspectRatio==DocumentPixelAspectRatio{2,1},
           "upgraded artboard record and aspect survive reopening");
    file.close();
}
void documentTransform()
{
    Document document; document.extent={2,1}; document.timeline.frameCount=2;
    auto pixels=makeRasterLayer(2,1,0xff000000U);
    pixels.pixels={0xffff0000U,0xff0000ffU};
    document.assets.emplace_back(RasterAsset{"pixels",pixels});
    document.layers.emplace_back(StaticBitmapLayer{{"layer","Layer"},StaticSource{"pixels"}});
    auto selected=TiledMask::rectangle({{0,0},{2,1}},{0,0},{1,1},false);
    document.relations.selections.insert({"selection","Selection",std::make_shared<const TiledMask>(selected.mask)});
    DocumentHistory history(document);
    const auto scale=DocumentResamplingPlan{}(document,{4,2},RasterResamplingMode::PreserveSource);
    expect(scale.ok() && history.applyCanvasAffine(scale.plan).changed && document.extent==CanvasExtent{4,2} &&
           findRasterAsset(document,"pixels")->pixels.pixels==pixels.pixels &&
           document.relations.selections.find("selection")->mask->coverage(0,0)>0,
           "non-destructive resampling scales document geometry and preserves source pixels");
    const auto scaled=renderFrame(document,1);
    expect(scaled.ok() && scaled.pixels.width==4 && scaled.pixels.height==2 &&
           scaled.pixels.pixels[0]==0xffff0000U && scaled.pixels.pixels[3]==0xff0000ffU,
           "resampling preserves every timeline frame's content");
    expect(history.undo().changed && document.extent==CanvasExtent{2,1},"resampling is one undo step");
    const auto rotate=ImageRotationPlan{}(document,90);
    expect(rotate.ok() && history.applyCanvasAffine(rotate.plan).changed && document.extent==CanvasExtent{1,2},
           "90 degree rotation expands the document and is undoable");
    const auto rotated=renderFrame(document,0);
    expect(rotated.ok() && rotated.pixels.pixels==std::vector<std::uint32_t>{0xffff0000U,0xff0000ffU} &&
           history.undo().changed && document.extent==CanvasExtent{2,1},
           "rotation changes the actual composite and undo restores its geometry");
    const auto physical=DocumentResamplingPlan{}(document,{4,2},RasterResamplingMode::ReplacePixels);
    expect(physical.ok() && history.applyCanvasAffine(physical.plan).changed &&
           findRasterAsset(document,"pixels")->pixels.width==4 &&
           findRasterAsset(document,"pixels")->pixels.height==2 &&
           renderFrame(document,0).pixels.pixels[0]==0xffff0000U &&
           history.undo().changed && findRasterAsset(document,"pixels")->pixels.pixels==pixels.pixels &&
           history.redo().changed && findRasterAsset(document,"pixels")->pixels.width==4,
           "physical resampling replaces owned pixels as a single undoable edit");
    Document incompatible; incompatible.extent={2,1};
    incompatible.assets.emplace_back(RasterAsset{"odd",makeRasterLayer(1,1,0xffffffffU)});
    incompatible.layers.emplace_back(StaticBitmapLayer{{"layer","Layer"},StaticSource{"odd"}});
    auto preserved=incompatible; DocumentHistory preservedHistory(preserved);
    const auto reference=DocumentResamplingPlan{}(preserved,{3,1},RasterResamplingMode::PreserveSource);
    expect(reference.ok() && preservedHistory.applyCanvasAffine(reference.plan).changed,
           "prepare fractional reference with preserved source");
    DocumentHistory incompatibleHistory(incompatible);
    const auto fractional=DocumentResamplingPlan{}(incompatible,{3,1},RasterResamplingMode::ReplacePixels);
    expect(fractional.ok() && incompatibleHistory.applyCanvasAffine(fractional.plan).changed &&
           findRasterAsset(incompatible,"odd")->pixels.width==2 &&
           renderFrame(incompatible,0).pixels.pixels==renderFrame(preserved,0).pixels.pixels &&
           incompatibleHistory.undo().changed && findRasterAsset(incompatible,"odd")->pixels.width==1,
           "fractional physical asset extent is rounded and its placement is preserved");
    Document locked; locked.extent={2,1}; locked.assets.emplace_back(RasterAsset{"pixels",pixels});
    locked.layers.emplace_back(StaticBitmapLayer{{"layer","Layer"},StaticSource{"pixels"}});
    locked.relations.locks.setLayer("layer",LayerLock::Pixels);
    DocumentHistory lockedHistory(locked);
    const auto lockedPlan=DocumentResamplingPlan{}(locked,{4,2},RasterResamplingMode::ReplacePixels);
    expect(!lockedHistory.applyCanvasAffine(lockedPlan.plan).ok() && !lockedHistory.canUndo() &&
           findRasterAsset(locked,"pixels")->pixels.pixels==pixels.pixels,
           "physical resampling rejects locked assets atomically");
    Document art; art.extent={4,4}; art.artboards.push_back({"board","Board",{{1,1},{2,2}},0xffffffffU,true});
    DocumentHistory artHistory(art);
    const auto angle=ImageRotationPlan{}(art,45);
    expect(angle.ok() && artHistory.applyCanvasAffine(angle.plan).changed &&
           art.artboards[0].geometry && artHistory.undo().changed && !art.artboards[0].geometry,
           "arbitrary rotation stores an oriented artboard and undo restores it");
    const auto path=std::filesystem::path(IISHAREDCANVAS_TEST_OUTPUT_DIR)/"resampled.iisc";
    std::filesystem::remove(path);
    Document original; original.extent={2,1}; original.assets.emplace_back(RasterAsset{"pixels",pixels});
    original.layers.emplace_back(StaticBitmapLayer{{"layer","Layer"},StaticSource{"pixels"}});
    DocumentFile file; expect(file.create(path.string(),original).ok(),"create resampling working file");
    if (file.document()) {
        DocumentHistory fileHistory(file);
        const auto plan=DocumentResamplingPlan{}(*file.document(),{4,2},RasterResamplingMode::ReplacePixels);
        expect(plan.ok() && fileHistory.applyCanvasAffine(plan.plan).changed &&
               fileHistory.undo().changed && fileHistory.redo().changed,
               "physical resampling working file supports undo and redo");
    }
    file.close();
    expect(file.open(path.string()).ok() && findRasterAsset(*file.document(),"pixels")->pixels.width==4 &&
           file.document()->extent==CanvasExtent{4,2},"physical resampling survives file reopen");
    file.close();
    for (const bool linked:{false,true}) {
        Document animated; animated.extent={4,2}; animated.timeline.frameCount=2;
        animated.assets.emplace_back(RasterAsset{"image",makeRasterLayer(2,1,0xffff0000U)});
        auto maskPixels=makeRasterLayer(2,1,0xffffffffU); maskPixels.pixels[1]=0xff000000U;
        animated.assets.emplace_back(RasterAsset{"mask",maskPixels});
        LayerProperties props{"animated","Animated"};
        LayerMask mask; mask.assetId="mask"; mask.linkedToLayer=linked; mask.outsideCoverage=0;
        props.appearance.rasterMask=mask;
        MotionKeyframe first; first.frame=0; first.value.anchor={0.5,0.5};
        MotionKeyframe second=first; second.frame=1; second.value.position={1,0};
        props.motion={first,second};
        animated.layers.emplace_back(StaticBitmapLayer{props,StaticSource{"image"}});
        const auto before=renderFrame(animated,1);
        DocumentHistory animatedHistory(animated);
        const auto plan=DocumentResamplingPlan{}(animated,{8,4},RasterResamplingMode::ReplacePixels);
        const auto applied=animatedHistory.applyCanvasAffine(plan.plan);
        const auto after=renderFrame(animated,1);
        bool same=before.ok() && after.ok() && applied.changed;
        if (same) for (int y=0;y<2;++y) for (int x=0;x<4;++x)
            same=same && before.pixels.pixels[y*4+x]==after.pixels.pixels[(2*y)*8+2*x];
        expect(same && animatedHistory.undo().changed && findRasterAsset(animated,"mask")->pixels.width==2,
               linked ? "physical resampling preserves linked masks and motion" :
                        "physical resampling preserves unlinked masks and motion");
    }
    Document nonsquare; nonsquare.extent={2,1}; nonsquare.pixelAspectRatio={2,1};
    nonsquare.assets.emplace_back(RasterAsset{"pixels",pixels});
    nonsquare.layers.emplace_back(StaticBitmapLayer{{"layer","Layer"},StaticSource{"pixels"}});
    DocumentHistory nonsquareHistory(nonsquare);
    const auto physicalRotation=ImageRotationPlan{}(nonsquare,90);
    const auto physicalApplied=physicalRotation.ok() ? nonsquareHistory.applyCanvasAffine(physicalRotation.plan) : DocumentHistoryResult{DocumentHistoryCode::InvalidArgument};
    const auto physicalFrame=renderFrame(nonsquare,0);
    expect(physicalRotation.ok() && physicalRotation.plan.target.extent==CanvasExtent{1,4} &&
           physicalApplied.changed && physicalFrame.ok() && physicalFrame.pixels.pixels.size()==4 &&
           physicalFrame.pixels.pixels[1]!=0,
           "rotation uses physical geometry for nonsquare pixels and retains visible content");
    Document videoDoc; videoDoc.extent={2,1}; videoDoc.timeline.frameCount=2;
    videoDoc.assets.emplace_back(VideoAsset{"video",{24,1},{pixels,makeRasterLayer(2,1,0xff00ff00U)}});
    videoDoc.layers.emplace_back(VideoLayer{{"video-layer","Video"},StaticSource{"video"},{}});
    DocumentHistory videoHistory(videoDoc);
    const auto videoPlan=DocumentResamplingPlan{}(videoDoc,{4,2},RasterResamplingMode::ReplacePixels);
    const auto videoChange=videoHistory.applyCanvasAffine(videoPlan.plan);
    const auto *videoAsset=findVideoAsset(videoDoc,"video");
    const auto videoFrame=renderFrame(videoDoc,1);
    expect(videoChange.changed && videoAsset && videoAsset->frames[0].width==4 &&
           videoAsset->frames[1].width==4 && videoFrame.ok() && !videoFrame.pixels.pixels.empty() &&
           videoFrame.pixels.pixels[0]==0xff00ff00U &&
           videoHistory.undo().changed && findVideoAsset(videoDoc,"video")->frames[1].width==2,
           "physical resampling transforms every video frame and restores it on undo");

    Document effects; effects.extent={2,1};
    effects.assets.emplace_back(RasterAsset{"pixels",pixels});
    LayerProperties styled{"styled","Styled"};
    LayerStyle disabledStyle; disabledStyle.enabled=false;
    styled.appearance.styles.effects.push_back(disabledStyle);
    LayerFilter disabledFilter; disabledFilter.enabled=false; disabledFilter.operation=RasterEffect{};
    LayerFilter adjustment; adjustment.operation=ColorAdjustment{};
    std::get<ColorAdjustment>(adjustment.operation).exposure=0.5;
    styled.appearance.filters={disabledFilter,adjustment};
    effects.layers.emplace_back(StaticBitmapLayer{styled,StaticSource{"pixels"}});
    auto effectReference=effects;
    DocumentHistory effectReferenceHistory(effectReference);
    const auto effectReferencePlan=DocumentResamplingPlan{}(effectReference,{4,2},RasterResamplingMode::PreserveSource);
    const bool referenceChanged=effectReferencePlan.ok() && effectReferenceHistory.applyCanvasAffine(effectReferencePlan.plan).changed;
    DocumentHistory effectHistory(effects);
    const auto effectPlan=DocumentResamplingPlan{}(effects,{4,2},RasterResamplingMode::ReplacePixels);
    const bool effectChanged=effectPlan.ok() && effectHistory.applyCanvasAffine(effectPlan.plan).changed;
    const auto referencePixels=renderFrame(effectReference,0), effectPixels=renderFrame(effects,0);
    expect(referenceChanged && effectChanged && referencePixels.ok() && effectPixels.ok() &&
           referencePixels.pixels.pixels==effectPixels.pixels.pixels &&
           layerProperties(effects.layers[0]).appearance==styled.appearance && effectHistory.undo().changed,
           "physical resampling retains inactive effects and coordinate-independent color filters");

    for (const auto kind:{RasterEffectKind::Invert,RasterEffectKind::Grayscale,
                         RasterEffectKind::Solarize,RasterEffectKind::Dehaze}) {
        Document filtered; filtered.extent={2,1};
        filtered.assets.emplace_back(RasterAsset{"pixels",pixels});
        LayerProperties properties{"filtered","Filtered"};
        LayerFilter pointwise; pointwise.operation=RasterEffect{kind};
        properties.appearance.filters.push_back(pointwise);
        filtered.layers.emplace_back(StaticBitmapLayer{properties,StaticSource{"pixels"}});
        auto preservedFilter=filtered; DocumentHistory preservedFilterHistory(preservedFilter);
        const auto preservedPlan=DocumentResamplingPlan{}(preservedFilter,{4,2},RasterResamplingMode::PreserveSource);
        const bool preservedChanged=preservedPlan.ok() && preservedFilterHistory.applyCanvasAffine(preservedPlan.plan).changed;
        DocumentHistory filteredHistory(filtered);
        const auto filteredPlan=DocumentResamplingPlan{}(filtered,{4,2},RasterResamplingMode::ReplacePixels);
        const bool filteredChanged=filteredPlan.ok() && filteredHistory.applyCanvasAffine(filteredPlan.plan).changed;
        const auto preservedFrame=renderFrame(preservedFilter,0),filteredFrame=renderFrame(filtered,0);
        expect(preservedChanged && filteredChanged && preservedFrame.ok() && filteredFrame.ok() &&
               preservedFrame.pixels.pixels==filteredFrame.pixels.pixels && filteredHistory.undo().changed,
               "coordinate-independent raster effects commute with physical resampling");
    }
    Document overlay; overlay.extent={2,1};
    overlay.assets.emplace_back(RasterAsset{"pixels",pixels});
    LayerProperties overlaid{"overlay","Overlay"};
    LayerStyle overlayStyle; overlayStyle.kind=LayerStyleKind::Overlay; overlayStyle.color=0xff00ff00U; overlayStyle.opacity=0.5;
    overlaid.appearance.styles.effects.push_back(overlayStyle);
    overlay.layers.emplace_back(StaticBitmapLayer{overlaid,StaticSource{"pixels"}});
    const auto beforeOverlay=renderFrame(overlay,0);
    DocumentHistory overlayHistory(overlay);
    const auto overlayPlan=ImageRotationPlan{}(overlay,90);
    const bool overlayChanged=overlayPlan.ok() && overlayHistory.applyCanvasAffine(overlayPlan.plan).changed;
    const auto afterOverlay=renderFrame(overlay,0);
    expect(overlayChanged && beforeOverlay.ok() && afterOverlay.ok() &&
           beforeOverlay.pixels.pixels==afterOverlay.pixels.pixels && overlayHistory.undo().changed &&
           layerProperties(overlay.layers[0]).appearance.styles.effects[0]==overlayStyle,
           "color overlay remains editable and rotates with source coverage");
    auto transparentStyle=overlay;
    auto &transparent=layerProperties(transparentStyle.layers[0]).appearance.styles.effects[0];
    transparent.kind=LayerStyleKind::Shadow; transparent.opacity=0;
    DocumentHistory transparentHistory(transparentStyle);
    const auto transparentPlan=ImageRotationPlan{}(transparentStyle,90);
    expect(transparentPlan.ok() && transparentHistory.applyCanvasAffine(transparentPlan.plan).changed &&
           transparentHistory.undo().changed,"zero-opacity geometry styles do not prevent rotation");
    for (const bool dissolve:{false,true}) {
        Document blocked; blocked.extent={2,1};
        blocked.assets.emplace_back(RasterAsset{"pixels",pixels});
        LayerProperties properties{"blocked","Blocked"};
        LayerFilter filter;
        filter.operation=RasterEffect{dissolve ? RasterEffectKind::Invert : RasterEffectKind::GaussianBlur};
        if (dissolve) filter.blendMode=RasterBlendMode::Dissolve;
        properties.appearance.filters.push_back(filter);
        blocked.layers.emplace_back(StaticBitmapLayer{properties,StaticSource{"pixels"}});
        DocumentHistory blockedHistory(blocked);
        const auto blockedPlan=DocumentResamplingPlan{}(blocked,{4,2},RasterResamplingMode::ReplacePixels);
        expect(blockedPlan.ok() && !blockedHistory.applyCanvasAffine(blockedPlan.plan).ok() &&
               !blockedHistory.canUndo() && findRasterAsset(blocked,"pixels")->pixels.width==2,
               dissolve ? "position-seeded filter blending rejects physical resampling atomically" :
                          "spatial effects reject physical resampling atomically");
    }

    Document vectorControls; vectorControls.extent={2,2};
    vectorControls.assets.emplace_back(PoseAsset{"pose",{2,2},{}});
    vectorControls.assets.emplace_back(MlsdAsset{"lines",{2,2},{{"line",0,0,1,1,1,true}}});
    PoseLayer pose; pose.properties={"pose-layer","Pose"}; pose.source=StaticSource{"pose"};
    pose.control.modelId="test/pose";
    MlsdLayer lines; lines.properties={"lines-layer","Lines"}; lines.source=StaticSource{"lines"};
    lines.control.modelId="test/mlsd";
    vectorControls.layers.emplace_back(pose); vectorControls.layers.emplace_back(lines);
    DocumentHistory controlsHistory(vectorControls);
    const auto controlsPlan=DocumentResamplingPlan{}(vectorControls,{4,4},RasterResamplingMode::ReplacePixels);
    expect(controlsPlan.ok() && controlsHistory.applyCanvasAffine(controlsPlan.plan).changed &&
           std::get<PoseAsset>(vectorControls.assets[0]).viewport==CanvasExtent{2,2} &&
           std::get<MlsdAsset>(vectorControls.assets[1]).segments[0].x2==1 &&
           layerProperties(vectorControls.layers[0]).transform.m11==2 && controlsHistory.undo().changed,
           "physical resampling retains editable pose and line assets while scaling their layers");

    Document sparse; sparse.extent={4,2};
    auto negative=makeRasterLayer(256,256,0); negative.pixels.back()=0xffaabbccU;
    auto positive=makeRasterLayer(256,256,0); positive.pixels[0]=0xff123456U;
    sparse.assets.emplace_back(ChunkedRasterAsset{"chunks",{{-1,-1,negative},{0,0,positive}}});
    sparse.layers.emplace_back(StaticBitmapLayer{{"sparse","Sparse"},StaticSource{"chunks"}});
    auto sparseReference=sparse; DocumentHistory sparseReferenceHistory(sparseReference);
    const auto sparseReferencePlan=DocumentResamplingPlan{}(sparseReference,{8,6},RasterResamplingMode::PreserveSource);
    const bool sparseReferenceChanged=sparseReferencePlan.ok() && sparseReferenceHistory.applyCanvasAffine(sparseReferencePlan.plan).changed;
    DocumentHistory sparseHistory(sparse);
    const auto sparsePlan=DocumentResamplingPlan{}(sparse,{8,6},RasterResamplingMode::ReplacePixels);
    const bool sparseChanged=sparsePlan.ok() && sparseHistory.applyCanvasAffine(sparsePlan.plan).changed;
    const auto sparsePixels=renderFrame(sparse,0), sparseReferencePixels=renderFrame(sparseReference,0);
    const auto &newChunks=std::get<ChunkedRasterAsset>(sparse.assets[0]).chunks;
    const auto outside=std::find_if(newChunks.begin(),newChunks.end(),[](const RasterChunk &chunk) {
        return chunk.column==-1 && chunk.row==-1;
    });
    expect(sparseReferenceChanged && sparseChanged && sparsePixels.ok() && sparseReferencePixels.ok() &&
           sparsePixels.pixels.pixels==sparseReferencePixels.pixels.pixels && outside!=newChunks.end() &&
           outside->pixels.pixels[255*256+255]==0xffaabbccU &&
           outside->pixels.pixels[253*256+254]==0xffaabbccU && sparseHistory.undo().changed &&
           std::get<ChunkedRasterAsset>(sparse.assets[0]).chunks[0].pixels.pixels.back()==0xffaabbccU,
           "physical resampling preserves sparse off-canvas chunks and supports undo");

    Document fractionalChunks; fractionalChunks.extent={3,1};
    auto leftChunk=makeRasterLayer(256,256,0); leftChunk.pixels[254]=0xff445566U; leftChunk.pixels[255]=0x00112233U;
    auto rightChunk=makeRasterLayer(256,256,0); rightChunk.pixels[0]=0xffff0000U;
    rightChunk.pixels[1]=0xff00ff00U; rightChunk.pixels[2]=0xff0000ffU;
    fractionalChunks.assets.emplace_back(ChunkedRasterAsset{"chunks",{{-1,0,leftChunk},{0,0,rightChunk}}});
    fractionalChunks.layers.emplace_back(StaticBitmapLayer{{"chunks-layer","Chunks"},StaticSource{"chunks"}});
    auto constrained=fractionalChunks; DocumentHistoryLimits narrow; narrow.maximumBitmapInputBytes=100;
    DocumentHistory constrainedHistory(constrained,narrow);
    const auto constrainedPlan=DocumentResamplingPlan{}(constrained,{5,1},RasterResamplingMode::ReplacePixels);
    expect(constrainedPlan.ok() && !constrainedHistory.applyCanvasAffine(constrainedPlan.plan).ok() &&
           !constrainedHistory.canUndo() && std::get<ChunkedRasterAsset>(constrained.assets[0]).chunks[0].pixels.pixels[255]==0x00112233U,
           "sparse resampling budget failures leave off-canvas source pixels unchanged");
    DocumentHistory fractionalHistory(fractionalChunks);
    const auto chunkPlan=DocumentResamplingPlan{}(fractionalChunks,{5,1},RasterResamplingMode::ReplacePixels);
    const bool chunkChanged=chunkPlan.ok() && fractionalHistory.applyCanvasAffine(chunkPlan.plan).changed;
    const auto &fractionalOutput=std::get<ChunkedRasterAsset>(fractionalChunks.assets[0]).chunks;
    const auto negativeOutput=std::find_if(fractionalOutput.begin(),fractionalOutput.end(),[](const RasterChunk &c) {
        return c.row==0 && c.column==-1;
    });
    const auto positiveOutput=std::find_if(fractionalOutput.begin(),fractionalOutput.end(),[](const RasterChunk &c) {
        return c.row==0 && c.column==0;
    });
    expect(chunkChanged && negativeOutput!=fractionalOutput.end() && positiveOutput!=fractionalOutput.end() &&
           negativeOutput->pixels.pixels[253]==0xff445566U && negativeOutput->pixels.pixels[254]==0x00112233U &&
           negativeOutput->pixels.pixels[255]==0x00112233U && positiveOutput->pixels.pixels[0]==0xffff0000U &&
           positiveOutput->pixels.pixels[2]==0xff00ff00U && positiveOutput->pixels.pixels[4]==0xff0000ffU &&
           fractionalHistory.undo().changed && fractionalHistory.redo().changed,
           "fractional sparse resampling uses signed nearest samples and preserves transparent RGB");

    Document dense; dense.extent={2,1};
    dense.assets.emplace_back(DepthAsset{"depth",{2,1},{0.25,0.75}});
    dense.assets.emplace_back(LineArtAsset{"lineart",{2,1},{0.0,1.0}});
    dense.assets.emplace_back(CannyAsset{"canny",{2,1},{0,1}});
    dense.assets.emplace_back(ScribbleAsset{"scribble",{2,1},{1,0}});
    dense.assets.emplace_back(NormalMapAsset{"normal",{2,1},{{0,0,1,true},{1,0,0,true}}});
    dense.assets.emplace_back(ShuffleAsset{"shuffle",{2,1},{{1,2,3},{4,5,6}}});
    dense.assets.emplace_back(TileAsset{"tile",{2,1},{{1,2,3},{4,5,6}}});
    dense.assets.emplace_back(ReferenceAsset{"reference",{2,1},{{1,2,3},{4,5,6}}});
    const auto addControl=[&](auto layer,std::string id) {
        layer.properties={id+"-layer",id}; layer.source=StaticSource{id};
        layer.control.modelId="test/"+id; dense.layers.emplace_back(std::move(layer));
    };
    addControl(DepthLayer{},"depth"); addControl(LineArtLayer{},"lineart");
    addControl(CannyLayer{},"canny"); addControl(ScribbleLayer{},"scribble");
    addControl(NormalMapLayer{},"normal"); addControl(ShuffleLayer{},"shuffle");
    addControl(TileLayer{},"tile"); addControl(ReferenceLayer{},"reference");
    expect(validate(dense).ok(),"dense control-map resampling fixture is valid");
    const auto denseOriginal=dense;
    DocumentHistory denseHistory(dense);
    const auto densePlan=DocumentResamplingPlan{}(dense,{4,2},RasterResamplingMode::ReplacePixels);
    const bool denseChanged=densePlan.ok() && denseHistory.applyCanvasAffine(densePlan.plan).changed;
    const auto &depth=std::get<DepthAsset>(dense.assets[0]);
    const auto &edges=std::get<CannyAsset>(dense.assets[2]);
    const auto &normals=std::get<NormalMapAsset>(dense.assets[4]);
    const auto &referenceMap=std::get<ReferenceAsset>(dense.assets[7]);
    expect(denseChanged && depth.viewport==CanvasExtent{4,2} &&
           depth.values==std::vector<double>{0.25,0.25,0.75,0.75,0.25,0.25,0.75,0.75} &&
           edges.mask==std::vector<std::uint8_t>{0,0,1,1,0,0,1,1} &&
           normals.samples.size()==8 && normals.samples[3].x==1 &&
           std::get<LineArtAsset>(dense.assets[1]).coverage[3]==1 &&
           std::get<ScribbleAsset>(dense.assets[3]).mask[3]==0 &&
           std::get<ShuffleAsset>(dense.assets[5]).colors[3].red==4 &&
           std::get<TileAsset>(dense.assets[6]).colors[3].red==4 &&
           referenceMap.colors.size()==8 && referenceMap.colors[3].red==4 &&
           denseHistory.undo().changed && std::get<DepthAsset>(dense.assets[0]).viewport==CanvasExtent{2,1} &&
           denseHistory.redo().changed && std::get<CannyAsset>(dense.assets[2]).mask.size()==8,
           "physical resampling retains every dense control-map type and its undo and redo");
    const auto densePath=std::filesystem::path(IISHAREDCANVAS_TEST_OUTPUT_DIR)/"resampled-control-maps.iisc";
    std::filesystem::remove(densePath);
    DocumentFile denseFile;
    expect(denseFile.create(densePath.string(),denseOriginal).ok(),"create dense control-map working file");
    if (denseFile.document()) {
        DocumentHistory fileHistory(denseFile);
        const auto filePlan=DocumentResamplingPlan{}(*denseFile.document(),{4,2},RasterResamplingMode::ReplacePixels);
        expect(filePlan.ok() && fileHistory.applyCanvasAffine(filePlan.plan).changed &&
               fileHistory.undo().changed && fileHistory.redo().changed,
               "dense control-map working-file history remains reversible");
    }
    denseFile.close();
    expect(denseFile.open(densePath.string()).ok() && denseFile.document() &&
           std::get<NormalMapAsset>(denseFile.document()->assets[4]).samples.size()==8,
           "resampled dense control maps survive working-file reopen");
    denseFile.close();

    Document longId; longId.extent={1,1};
    const std::string largeIdentifier(1024*1024+1,'d');
    longId.assets.emplace_back(DepthAsset{largeIdentifier,{1,1},{0.5}});
    DocumentHistory longIdHistory(longId);
    const auto longIdPlan=DocumentResamplingPlan{}(longId,{2,1},RasterResamplingMode::ReplacePixels);
    const bool longIdApplied=longIdPlan.ok() && longIdHistory.applyCanvasAffine(longIdPlan.plan).changed;
    expect(longIdApplied && longIdHistory.undo().changed &&
           std::get<DepthAsset>(longId.assets[0]).viewport==CanvasExtent{1,1} &&
           longIdHistory.redo().changed && std::get<DepthAsset>(longId.assets[0]).viewport==CanvasExtent{2,1},
           "dense history restores its bounded owned data even when asset ids exceed default exchange limits");

    Document conflict; conflict.extent={1,1};
    conflict.assets.emplace_back(LineArtAsset{"samples",{1,1},{0.0}});
    DocumentHistory conflictHistory(conflict);
    const auto conflictPlan=DocumentResamplingPlan{}(conflict,{2,1},RasterResamplingMode::ReplacePixels);
    const bool conflictApplied=conflictPlan.ok() && conflictHistory.applyCanvasAffine(conflictPlan.plan).changed;
    std::get<LineArtAsset>(conflict.assets[0]).coverage[0]=-0.0;
    const auto conflictResult=conflictHistory.undo();
    bool restored=false;
    if (conflictResult.code==DocumentHistoryCode::Conflict) {
        std::get<LineArtAsset>(conflict.assets[0]).coverage[0]=0.0;
        restored=conflictHistory.undo().changed;
    }
    expect(conflictApplied && conflictResult.code==DocumentHistoryCode::Conflict && restored,
           "dense history detects bitwise changes before overwriting source samples");
}
class RecordingEncoder final : public VideoFrameEncoder {
public:
    int width=0,height=0;
    MediaBytesResult encode(std::span<const VideoEncodeFrame> frames,FrameRate,const VideoEncodeRequest &) override {
        if (!frames.empty()) { width=frames[0].width; height=frames[0].height; }
        return {{1},{}};
    }
};
void pixelAspect()
{
    Document document; document.extent={2,1};
    document.assets.emplace_back(RasterAsset{"pixels",makeRasterLayer(2,1,0xff123456U)});
    document.layers.emplace_back(StaticBitmapLayer{{"layer","Layer"},StaticSource{"pixels"}});
    DocumentHistory history(document);
    expect(history.setPixelAspectRatio({2,1}).changed && document.pixelAspectRatio==DocumentPixelAspectRatio{2,1},
           "pixel aspect changes as an undoable rational document property");
    const auto display=renderDisplayFrame(document,0,RasterSampling::Nearest);
    expect(display.ok() && display.pixels.width==4 && display.pixels.height==1,
           "display rendering accounts for nonsquare pixels");
    const auto encoded=encodeIisc(document); const auto decoded=decodeIisc(encoded.bytes);
    expect(encoded.ok() && decoded.ok() && decoded.document.pixelAspectRatio==DocumentPixelAspectRatio{2,1},
           "nonsquare pixel aspect survives IISC serialization");
    const auto imagePath=std::filesystem::path(IISHAREDCANVAS_TEST_OUTPUT_DIR)/"pixel-aspect.png";
    std::filesystem::create_directories(imagePath.parent_path()); std::filesystem::remove(imagePath);
    const auto exported=exportBitmapFrame(document,0,imagePath.string());
    const auto imported=importBitmap(imagePath.string());
    expect(exported.ok() && imported.result.ok() && imported.asset.pixels.width==4 && imported.asset.pixels.height==1,
           "bitmap export preserves the physical aspect in square-pixel output");
    RecordingEncoder encoder;
    const auto video=encodeVideoFrames(document,encoder,{});
    expect(video.ok() && encoder.width==4 && encoder.height==1,
           "video export sends aspect-corrected square-pixel frames to the encoder");
    const auto psd=encodePsd(document);
    constexpr std::array<std::uint8_t,24> aspectResource{
        '8','B','I','M',0x04,0x28,0,0,0,0,0,12,0,0,0,2,0x40,0,0,0,0,0,0,0};
    expect(psd.ok() && std::search(psd.bytes.begin(),psd.bytes.end(),aspectResource.begin(),aspectResource.end())!=psd.bytes.end(),
           "PSD export carries its pixel aspect image resource");
    const auto pdfPath=std::filesystem::path(IISHAREDCANVAS_TEST_OUTPUT_DIR)/"pixel-aspect.pdf";
    std::filesystem::remove(pdfPath);
    const auto pdf=exportPdf(document,pdfPath.string());
    std::ifstream pdfFile(pdfPath,std::ios::binary);
    const std::string pdfBytes((std::istreambuf_iterator<char>(pdfFile)),{});
    expect(pdf.ok() && pdfBytes.find("/MediaBox [0 0 3 0.75]")!=std::string::npos &&
           pdfBytes.find("1.5 0 0 -0.75")!=std::string::npos,
           "PDF page geometry and drawing transform use the physical pixel aspect");
    const auto package=std::filesystem::path(IISHAREDCANVAS_TEST_OUTPUT_DIR)/"pixel-aspect-timeline";
    std::filesystem::remove_all(package);
    const auto timeline=exportTimelineInterchange(document,package.string());
    const auto media=importBitmap((package/"media/layer-0001-state-0001.png").string());
    std::ifstream xmlFile(package/"timeline.fcpxml");
    const std::string xml((std::istreambuf_iterator<char>(xmlFile)),{});
    expect(timeline.ok() && media.result.ok() && media.asset.pixels.width==4 && media.asset.pixels.height==1 &&
           xml.find("width=\"4\"")!=std::string::npos && xml.find("paspH=\"1\"")!=std::string::npos,
           "timeline media and square-pixel sequence reflect the physical aspect ratio");
    auto legacy=document; legacy.formatVersion.minor=26;
    expect(!encodeIisc(legacy).ok(),"older versions cannot silently discard pixel aspect");
    expect(history.undo().changed && document.pixelAspectRatio==DocumentPixelAspectRatio{1,1} &&
           history.redo().changed && document.pixelAspectRatio==DocumentPixelAspectRatio{2,1},
           "pixel aspect is undoable and redoable");
    expect(!history.setPixelAspectRatio({0,1}).ok() && document.pixelAspectRatio==DocumentPixelAspectRatio{2,1},
           "zero pixel aspect is rejected atomically");
    const auto path=std::filesystem::path(IISHAREDCANVAS_TEST_OUTPUT_DIR)/"pixel-aspect.iisc";
    std::filesystem::create_directories(path.parent_path()); std::filesystem::remove(path);
    auto old=document; old.formatVersion.minor=26; old.pixelAspectRatio={1,1};
    DocumentFile file; expect(file.create(path.string(),old).ok(),"create legacy pixel-aspect working file");
    if (file.document()) {
        DocumentHistory fileHistory(file);
        expect(fileHistory.setPixelAspectRatio({3,2}).changed && fileHistory.undo().changed &&
               fileHistory.redo().changed,"file-bound pixel aspect is reversible");
    }
    file.close();
    expect(file.open(path.string()).ok() && file.document()->pixelAspectRatio==DocumentPixelAspectRatio{3,2},
           "pixel aspect and legacy upgrade survive working-file reopen");
    file.close();
}
}
int main() { transparentBounds(); cloneDocument(); canvasGeometry(); contentBounds(); orientedArtboard(); documentTransform(); pixelAspect(); return failures ? 1 : 0; }
