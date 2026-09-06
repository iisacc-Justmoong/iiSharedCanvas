#include <iiSharedCanvas.h>

#include <iostream>
#include <limits>
#include <string>
#include <utility>

namespace {

int failures = 0;

void expect(bool condition, const std::string &message)
{
    if (!condition) {
        std::cerr << message << '\n';
        ++failures;
    }
}

iiSharedCanvas::VectorPath rectangle(std::uint32_t argb)
{
    using namespace iiSharedCanvas;
    VectorPath path;
    path.commands = {
        MoveTo{{0.0, 0.0}},
        LineTo{{4.0, 0.0}},
        LineTo{{4.0, 4.0}},
        LineTo{{0.0, 4.0}},
        ClosePath{},
    };
    path.fill = SolidPaint{argb};
    return path;
}

iiSharedCanvas::Document makeDocument()
{
    using namespace iiSharedCanvas;
    Document document;
    document.extent = {8, 8};
    document.timeline = {{24, 1}, 12};
    document.assets.emplace_back(
        RasterAsset{"raster-a", makeRasterLayer(8, 8, 0xff102030U)});
    document.assets.emplace_back(
        VectorAsset{"vector-a", {8, 8}, {rectangle(0xff00ff00U)}});
    document.assets.emplace_back(
        VectorAsset{"vector-b", {8, 8}, {rectangle(0xff0000ffU)}});
    document.layers.emplace_back(BitmapLayer{
        {"paint", "Paint", true, 1.0, {}, RasterBlendMode::SourceOver},
        StaticSource{"raster-a"},
    });
    document.layers.emplace_back(VectorLayer{
        {"motion", "Motion", true, 1.0, {}, RasterBlendMode::SourceOver},
        KeyframedSource{{0, 6}},
    });
    document.frames = {
        {0, {{"motion", "vector-a"}}},
        {6, {{"motion", "vector-b"}}},
    };
    return document;
}

} // namespace

int main()
{
    using namespace iiSharedCanvas;

    Document document = makeDocument();
    DocumentEditor editor(document);
    expect(editor.isBound() && editor.lastResult().ok(),
           "a valid document must bind to the structural editor");
    expect(editor.document() == &document,
           "the editor must expose the exact non-owning aggregate it edits");
    expect(findRasterAsset(document, "raster-a") != nullptr
               && findVectorAsset(document, "vector-a") != nullptr
               && findLayer(document, "motion") != nullptr,
           "typed asset and layer lookup must expose mutable document data");
    expect(assetIndex(document, "vector-a") == std::optional<std::size_t>{1}
               && layerIndex(document, "motion") == std::optional<std::size_t>{1},
           "stable ids must resolve to current collection indices");
    const Frame *initialFrame = findFrame(document, 6);
    expect(initialFrame
               && findKeyframe(*initialFrame, "motion") != nullptr
               && keyframeIndex(*initialFrame, "motion")
                    == std::optional<std::size_t>{0}
               && findKeyframe(document, "motion", 6)
                    == findKeyframe(*initialFrame, "motion"),
           "exact keyframe lookup must be distinct from hold sampling");
    const std::vector<AssetReference> vectorReferences = assetReferences(
        document, "vector-a");
    expect(vectorReferences.size() == 1
               && vectorReferences.front().frameIndex
                    == std::optional<std::size_t>{0}
               && vectorReferences.front().keyframeIndex
                    == std::optional<std::size_t>{0}
               && assetReferences(document, "raster-a").front().frameIndex
                    == std::nullopt
               && assetReferences(document, "raster-a").front().keyframeIndex
                    == std::nullopt,
           "callers must be able to inspect every static and keyframed asset reference");

    expect(editor.setCanvasExtent({16, 12}).changed
               && document.extent.width == 16
               && document.extent.height == 12,
           "canvas extent must be directly editable");
    Document invalidRebind;
    const std::uint64_t beforeRejectedRebind = editor.revision();
    const DocumentEditResult rejectedRebind = editor.bind(invalidRebind);
    expect(!rejectedRebind.ok()
               && rejectedRebind.code == DocumentEditCode::InvalidDocument
               && editor.isBound()
               && editor.document() == &document
               && editor.revision() == beforeRejectedRebind,
           "a rejected rebind must preserve the current document and revision");
    expect(editor.setFrameRate({30, 1}).changed
               && document.timeline.frameRate.numerator == 30,
           "rational frame rate must be directly editable");
    const std::uint64_t beforeRejectedFrameCount = editor.revision();
    const DocumentEditResult rejectedFrameCount = editor.setFrameCount(6);
    expect(!rejectedFrameCount.ok()
               && rejectedFrameCount.code == DocumentEditCode::ValidationRejected
               && document.timeline.frameCount == 12
               && editor.revision() == beforeRejectedFrameCount,
           "shrinking a timeline across an existing keyframe must fail without mutation");
    expect(editor.setFrameCount(18).changed && document.timeline.frameCount == 18,
           "a valid timeline expansion must apply");

    const DocumentEditResult layerRangeEdit = editor.setLayerFrameRange(
        "paint", LayerFrameRange{1, 16});
    expect(layerRangeEdit.ok() && layerRangeEdit.changed
               && layerProperties(*findLayer(document, "paint")).frameRange
                    == std::optional<LayerFrameRange>{{1, 16}}
               && document.formatVersion.minor == CurrentFormatMinor,
           "setting a layer range must atomically migrate the document to the current format");
    const std::uint64_t beforeRangeNoOp = editor.revision();
    expect(editor.setLayerFrameRange("paint", LayerFrameRange{1, 16}).ok()
               && !editor.lastResult().changed
               && editor.revision() == beforeRangeNoOp,
           "setting the same layer range must preserve the editor revision");
    const DocumentEditResult rejectedRange = editor.setLayerFrameRange(
        "paint", LayerFrameRange{17, 16});
    expect(!rejectedRange.ok()
               && rejectedRange.code == DocumentEditCode::ValidationRejected
               && layerProperties(*findLayer(document, "paint")).frameRange
                    == std::optional<LayerFrameRange>{{1, 16}}
               && editor.revision() == beforeRangeNoOp,
           "an invalid layer range must preserve the prior range and revision");
    const DocumentEditResult rejectedRangeShrink = editor.setFrameCount(16);
    expect(!rejectedRangeShrink.ok()
               && document.timeline.frameCount == 18
               && editor.revision() == beforeRangeNoOp,
           "shrinking a timeline across an inclusive layer lastFrame must fail atomically");
    expect(editor.setFrameCount(24).changed
               && document.timeline.frameCount == 24
               && layerProperties(*findLayer(document, "paint")).frameRange
                    == std::optional<LayerFrameRange>{{1, 16}},
           "timeline expansion must preserve an explicit layer range instead of widening it");
    expect(editor.setLayerFrameRange("paint", std::nullopt).changed
               && !layerProperties(*findLayer(document, "paint")).frameRange
               && document.formatVersion.minor == CurrentFormatMinor,
           "clearing a layer range must restore whole-timeline existence without downgrading format");

    StableDiffusionMetadata generation;
    generation.positivePrompt = "architectural concept art";
    generation.negativePrompt = "watermark";
    generation.samplingPasses.push_back({
        "3", 42, 24, 7.0, "euler", "normal", 1.0, std::nullopt, std::nullopt,
    });
    generation.software = "ComfyUI";
    generation.comfyUi.promptJson =
        R"json({"3":{"class_type":"KSampler","inputs":{"seed":42,"steps":24,"cfg":7.0}}})json";
    const DocumentEditResult metadataEdit =
        editor.setStableDiffusionMetadata(generation);
    expect(metadataEdit.ok() && metadataEdit.changed
               && document.stableDiffusionMetadata == generation,
           "Stable Diffusion generation metadata must be atomically editable");
    const std::uint64_t beforeMetadataNoOp = editor.revision();
    expect(!editor.setStableDiffusionMetadata(generation).changed
               && editor.revision() == beforeMetadataNoOp,
           "setting identical generation metadata must not advance revision");
    StableDiffusionMetadata invalidGeneration = generation;
    invalidGeneration.samplingPasses.front().cfgScale =
        std::numeric_limits<double>::quiet_NaN();
    const DocumentEditResult rejectedMetadata =
        editor.setStableDiffusionMetadata(invalidGeneration);
    expect(!rejectedMetadata.ok()
               && rejectedMetadata.code == DocumentEditCode::ValidationRejected
               && document.stableDiffusionMetadata == generation
               && editor.revision() == beforeMetadataNoOp,
           "invalid generation metadata must preserve the prior metadata and revision");
    expect(editor.clearStableDiffusionMetadata().changed
               && !document.stableDiffusionMetadata,
           "generation metadata must be explicitly removable without touching canvas content");

    Document legacyRangedLayerDocument = makeDocument();
    legacyRangedLayerDocument.formatVersion = {1, 2};
    DocumentEditor legacyRangedLayerEditor(legacyRangedLayerDocument);
    Layer rangedAnimation = VectorLayer{
        {"ranged-animation", "Ranged animation", true, 1.0, {},
         RasterBlendMode::SourceOver, LayerFrameRange{3, 8}},
        KeyframedSource{},
    };
    expect(legacyRangedLayerEditor.insertKeyframedLayer(
                    rangedAnimation,
                    {KeyframePlacement{0, "vector-a"},
                     KeyframePlacement{6, "vector-b"}}).changed
               && legacyRangedLayerDocument.formatVersion.minor == CurrentFormatMinor
               && findKeyframe(legacyRangedLayerDocument,
                               "ranged-animation", 0)
               && layerProperties(*findLayer(legacyRangedLayerDocument,
                                             "ranged-animation")).frameRange
                    == std::optional<LayerFrameRange>{{3, 8}},
           "inserting a ranged keyframed layer must preserve pre-range keys and atomically migrate format 1.2 to the current format");

    Document rejectedLegacyRangeDocument = makeDocument();
    rejectedLegacyRangeDocument.formatVersion = {1, 2};
    DocumentEditor rejectedLegacyRangeEditor(rejectedLegacyRangeDocument);
    Layer invalidRangedLayer = VectorLayer{
        {"invalid-range", "Invalid range", true, 1.0, {},
         RasterBlendMode::SourceOver, LayerFrameRange{3, 12}},
        StaticSource{"vector-a"},
    };
    const DocumentEditResult rejectedLegacyRange =
        rejectedLegacyRangeEditor.insertLayer(invalidRangedLayer);
    expect(!rejectedLegacyRange.ok()
               && rejectedLegacyRangeDocument.formatVersion.minor == 2
               && findLayer(rejectedLegacyRangeDocument, "invalid-range") == nullptr
               && rejectedLegacyRangeEditor.revision() == 0,
           "a rejected ranged-layer insertion must restore the legacy format, layer collection, and revision");

    Document rejectedRangedKeyedDocument = makeDocument();
    rejectedRangedKeyedDocument.formatVersion = {1, 2};
    DocumentEditor rejectedRangedKeyedEditor(rejectedRangedKeyedDocument);
    Layer invalidRangedAnimation = VectorLayer{
        {"invalid-ranged-animation", "Invalid ranged animation", true, 1.0, {},
         RasterBlendMode::SourceOver, LayerFrameRange{2, 8}},
        KeyframedSource{},
    };
    const DocumentEditResult rejectedRangedKeyed =
        rejectedRangedKeyedEditor.insertKeyframedLayer(
            invalidRangedAnimation,
            {KeyframePlacement{2, "vector-a"}});
    expect(!rejectedRangedKeyed.ok()
               && rejectedRangedKeyedDocument.formatVersion.minor == 2
               && findLayer(rejectedRangedKeyedDocument,
                            "invalid-ranged-animation") == nullptr
               && findFrame(rejectedRangedKeyedDocument, 2) == nullptr
               && findKeyframe(rejectedRangedKeyedDocument, "motion", 0)
               && findKeyframe(rejectedRangedKeyedDocument, "motion", 6)
               && rejectedRangedKeyedEditor.revision() == 0,
           "a failed ranged keyframed insertion must restore legacy format, shared frame owners, layers, and revision");

    Document rejectedRangedReplacementDocument = makeDocument();
    rejectedRangedReplacementDocument.formatVersion = {1, 2};
    DocumentEditor rejectedRangedReplacementEditor(
        rejectedRangedReplacementDocument);
    Layer invalidRangedReplacement = VectorLayer{
        {"motion", "Invalid replacement", true, 1.0, {},
         RasterBlendMode::SourceOver, LayerFrameRange{1, 8}},
        StaticSource{"raster-a"},
    };
    const DocumentEditResult rejectedRangedReplacement =
        rejectedRangedReplacementEditor.replaceLayer(
            "motion", invalidRangedReplacement);
    const Layer *restoredMotion = findLayer(
        rejectedRangedReplacementDocument, "motion");
    expect(!rejectedRangedReplacement.ok()
               && rejectedRangedReplacementDocument.formatVersion.minor == 2
               && restoredMotion
               && std::holds_alternative<VectorLayer>(*restoredMotion)
               && !layerProperties(*restoredMotion).frameRange
               && std::holds_alternative<KeyframedSource>(
                   layerSource(*restoredMotion))
               && findKeyframe(rejectedRangedReplacementDocument,
                               "motion", 0)
               && findKeyframe(rejectedRangedReplacementDocument,
                               "motion", 6)
               && rejectedRangedReplacementEditor.revision() == 0,
           "a failed ranged replacement must restore the original layer, keyframes, legacy format, and revision");

    Document legacyMetadataDocument = makeDocument();
    legacyMetadataDocument.formatVersion = {1, 1};
    DocumentEditor legacyMetadataEditor(legacyMetadataDocument);
    expect(legacyMetadataEditor.setStableDiffusionMetadata(generation).changed
               && legacyMetadataDocument.formatVersion.minor == 2
               && legacyMetadataDocument.stableDiffusionMetadata == generation,
           "adding generation metadata to an older document must atomically migrate it to format 1.2");

    expect(editor.insertRasterAsset("raster-b", makeRasterLayer(4, 4, 0xffaabbccU), 1).changed
               && assetIndex(document, "raster-b") == std::optional<std::size_t>{1},
           "raster assets must be insertable at an explicit collection index");
    expect(editor.insertVectorAsset("vector-c", {4, 4}, {rectangle(0xffff0000U)}).changed
               && findVectorAsset(document, "vector-c") != nullptr,
           "vector assets and their path data must be insertable");
    const std::uint64_t beforeDuplicateAsset = editor.revision();
    const DocumentEditResult duplicateAsset = editor.insertRasterAsset(
        "raster-b", makeRasterLayer(1, 1, 0U));
    expect(!duplicateAsset.ok()
               && duplicateAsset.code == DocumentEditCode::DuplicateAssetId
               && editor.revision() == beforeDuplicateAsset,
           "duplicate asset ids must be rejected without advancing revision");
    expect(editor.replaceRasterPixels("raster-b", makeRasterLayer(2, 3, 0xff010203U)).changed
               && findRasterAsset(document, "raster-b")->pixels.width == 2,
           "raster pixel storage must be replaceable as one validated value");
    expect(editor.replaceVectorData("vector-c", {10, 10}, {rectangle(0xffabcdefU)}).changed
               && findVectorAsset(document, "vector-c")->viewport.width == 10,
           "vector viewport and paths must be replaceable together");
    expect(editor.replaceVectorData("raster-b", {10, 10}, {}).code
               == DocumentEditCode::AssetKindMismatch,
           "typed replacement methods must reject the wrong asset content kind");

    expect(editor.renameAsset("raster-a", "paint-pixels").changed
               && findAsset(document, "raster-a") == nullptr
               && std::get<StaticSource>(layerSource(*findLayer(document, "paint"))).assetId
                    == "paint-pixels",
           "renaming an asset must atomically rewrite every layer reference");
    expect(editor.moveAsset("paint-pixels", document.assets.size() - 1).changed
               && assetIndex(document, "paint-pixels") == document.assets.size() - 1,
           "asset storage order must be controllable without changing references");
    const DocumentEditResult referencedRemoval = editor.removeAsset("vector-a");
    expect(!referencedRemoval.ok()
               && referencedRemoval.code == DocumentEditCode::AssetReferenced,
           "referenced assets must not be silently removed");

    Layer overlay = VectorLayer{
        {"overlay", "Overlay", true, 0.75, {}, RasterBlendMode::Screen},
        StaticSource{"vector-c"},
    };
    expect(editor.insertLayer(overlay, 1).changed
               && layerIndex(document, "overlay") == std::optional<std::size_t>{1},
           "layers must be insertable at an explicit bottom-to-top position");
    expect(editor.insertLayer(overlay).code == DocumentEditCode::DuplicateLayerId,
           "layer identity must remain unique across insertion APIs");
    Layer replacement = overlay;
    layerProperties(replacement).name = "Replacement overlay";
    layerProperties(replacement).opacity = 0.625;
    expect(editor.replaceLayer("overlay", replacement).changed
               && layerProperties(*findLayer(document, "overlay")).name
                    == "Replacement overlay",
           "a complete layer must be replaceable through one validated operation");
    expect(editor.setLayerName("overlay", "Highlights").changed
               && editor.setLayerVisible("overlay", false).changed
               && editor.setLayerOpacity("overlay", 0.5).changed
               && editor.setLayerBlendMode("overlay", RasterBlendMode::Multiply).changed,
           "layer presentation fields must be independently editable");
    const std::uint64_t beforeNoOp = editor.revision();
    const DocumentEditResult noOp = editor.setLayerName("overlay", "Highlights");
    expect(noOp.ok() && !noOp.changed && editor.revision() == beforeNoOp,
           "a value-preserving edit must not advance structural revision");
    const std::uint64_t beforeInvalidOpacity = editor.revision();
    const DocumentEditResult invalidOpacity = editor.setLayerOpacity("overlay", 1.5);
    expect(!invalidOpacity.ok()
               && invalidOpacity.code == DocumentEditCode::ValidationRejected
               && layerProperties(*findLayer(document, "overlay")).opacity == 0.5
               && editor.revision() == beforeInvalidOpacity,
           "invalid layer values must restore the prior field and revision");
    AffineTransform transform;
    transform.translationX = 3.0;
    transform.translationY = 2.0;
    expect(editor.setLayerTransform("overlay", transform).changed
               && layerProperties(*findLayer(document, "overlay")).transform.translationX == 3.0,
           "the complete affine transform must be replaceable");
    expect(editor.renameLayer("overlay", "highlights").changed
               && findLayer(document, "overlay") == nullptr,
           "layer ids must be renameable independently of display names");
    expect(editor.moveLayer("highlights", document.layers.size() - 1).changed
               && layerIndex(document, "highlights") == document.layers.size() - 1,
           "bottom-to-top layer order must be explicitly mutable");
    const std::uint64_t beforeKindMismatch = editor.revision();
    const DocumentEditResult kindMismatch = editor.setStaticSource("highlights", "raster-b");
    expect(!kindMismatch.ok()
               && kindMismatch.code == DocumentEditCode::AssetKindMismatch
               && editor.revision() == beforeKindMismatch
               && std::holds_alternative<VectorLayer>(*findLayer(document, "highlights"))
               && std::get<StaticSource>(layerSource(*findLayer(document, "highlights"))).assetId
                    == "vector-c",
           "a vector layer must reject a bitmap source without changing type, source, or revision");
    expect(editor.setStaticSource("highlights", "vector-b").changed
               && std::get<StaticSource>(layerSource(*findLayer(document, "highlights"))).assetId
                    == "vector-b",
           "a layer source must be replaceable with a static asset reference");
    expect(editor.insertKeyframe("highlights", 4, "vector-c").code
               == DocumentEditCode::SourceNotKeyframed,
           "keyframe operations must identify a static source precisely");
    expect(editor.setKeyframedSource("highlights",
                                     {KeyframePlacement{0, "vector-a"},
                                      KeyframePlacement{8, "vector-b"}}).changed,
           "a layer source must be replaceable with a validated keyframe track");
    expect(findKeyframe(document, "highlights", 0)
               && findKeyframe(document, "highlights", 8)
               && findFrame(document, 0)->keyframes.size() == 2
               && std::get<KeyframedSource>(
                      layerSource(*findLayer(document, "highlights"))).frameIndices
                    == std::vector<FrameIndex>{0, 8},
           "setting a keyframed source must distribute its keys into shared frame owners");
    const std::uint64_t beforeReorderedNoOp = editor.revision();
    expect(editor.setKeyframedSource(
                    "highlights",
                    {KeyframePlacement{8, "vector-b"},
                     KeyframePlacement{0, "vector-a"}}).ok()
               && !editor.lastResult().changed
               && editor.revision() == beforeReorderedNoOp,
           "placement input order must not create a different frame-owned track");
    const std::uint64_t beforeRejectedTrack = editor.revision();
    const DocumentEditResult rejectedTrack = editor.setKeyframedSource(
        "highlights", {KeyframePlacement{1, "vector-c"}});
    expect(!rejectedTrack.ok()
               && rejectedTrack.code == DocumentEditCode::ValidationRejected
               && editor.revision() == beforeRejectedTrack
               && findKeyframe(document, "highlights", 0)->assetId == "vector-a"
               && findKeyframe(document, "highlights", 8)->assetId == "vector-b"
               && findFrame(document, 1) == nullptr,
           "a rejected track replacement must restore every frame owner and revision");
    expect(editor.renameLayer("highlights", "keyed-highlights").changed
               && findKeyframe(document, "highlights", 0) == nullptr
               && findKeyframe(document, "keyed-highlights", 0)
               && findKeyframe(document, "keyed-highlights", 8),
           "renaming a keyframed layer must rewrite every frame-owned stable reference");
    expect(editor.renameLayer("keyed-highlights", "highlights").changed,
           "a rewritten keyframed layer id must remain independently renameable");

    const std::uint64_t beforeKeyframeKindMismatch = editor.revision();
    const DocumentEditResult keyframeKindMismatch = editor.insertKeyframe(
        "highlights", 4, "raster-b");
    expect(!keyframeKindMismatch.ok()
               && keyframeKindMismatch.code == DocumentEditCode::AssetKindMismatch
               && editor.revision() == beforeKeyframeKindMismatch
               && findKeyframe(document, "highlights", 4) == nullptr
               && findFrame(document, 4) == nullptr,
           "a vector layer must reject a bitmap keyframe without partial insertion");
    expect(editor.insertKeyframe("highlights", 4, "vector-c").changed
               && frameIndex(document, 4) == std::optional<std::size_t>{1}
               && findKeyframe(document, "highlights", 4)
               && std::get<KeyframedSource>(
                      layerSource(*findLayer(document, "highlights"))).frameIndices
                    == std::vector<FrameIndex>{0, 4, 8},
           "keyframes must create sparse frame owners in chronological order");
    expect(editor.insertKeyframe("highlights", 4, "vector-c").code
               == DocumentEditCode::DuplicateKeyframe,
           "duplicate exact-frame insertion must have a typed rejection");
    expect(editor.setKeyframeAsset("highlights", 4, "vector-b").changed
               && findKeyframe(document, "highlights", 4)->assetId
                    == "vector-b",
           "a keyframe asset reference must be independently replaceable");
    expect(editor.moveKeyframe("highlights", 4, 5).changed
               && findKeyframe(document, "highlights", 4) == nullptr
               && findFrame(document, 4) == nullptr
               && findKeyframe(document, "highlights", 5)
               && std::get<KeyframedSource>(
                      layerSource(*findLayer(document, "highlights"))).frameIndices
                    == std::vector<FrameIndex>{0, 5, 8},
           "moving a keyframe must transfer ownership and remove an empty source frame");
    expect(editor.removeKeyframe("highlights", 5).changed
               && findKeyframe(document, "highlights", 5) == nullptr
               && findFrame(document, 5) == nullptr
               && std::get<KeyframedSource>(
                      layerSource(*findLayer(document, "highlights"))).frameIndices
                    == std::vector<FrameIndex>{0, 8},
           "removing a frame's last keyframe must remove the empty sparse frame");
    const DocumentEditResult initialKeyframeRemoval = editor.removeKeyframe("highlights", 0);
    expect(!initialKeyframeRemoval.ok()
               && initialKeyframeRemoval.code == DocumentEditCode::ValidationRejected,
           "removing the required frame-zero keyframe must fail without mutation");

    VectorPath cyan = rectangle(0xff00ffffU);
    expect(editor.insertVectorPath("vector-c", cyan, 0).changed
               && findVectorAsset(document, "vector-c")->paths.size() == 2,
           "vector paths must be insertable at explicit paint-order positions");
    VectorPath yellow = rectangle(0xffffff00U);
    expect(editor.replaceVectorPath("vector-c", 0, yellow).changed
               && editor.moveVectorPath("vector-c", 0, 1).changed
               && editor.removeVectorPath("vector-c", 0).changed,
           "vector paths must support replace, reorder, and remove operations");
    VectorPath invalidPath;
    invalidPath.fill = SolidPaint{0xffffffffU};
    const std::size_t pathCountBeforeFailure = findVectorAsset(document, "vector-c")->paths.size();
    const DocumentEditResult invalidPathInsert = editor.insertVectorPath("vector-c", invalidPath);
    expect(!invalidPathInsert.ok()
               && invalidPathInsert.code == DocumentEditCode::ValidationRejected
               && findVectorAsset(document, "vector-c")->paths.size() == pathCountBeforeFailure,
           "invalid path mutations must roll back exactly");

    expect(editor.removeLayer("highlights").changed
               && findKeyframe(document, "highlights", 0) == nullptr
               && findFrame(document, 8) == nullptr
               && findFrame(document, 0)
               && findFrame(document, 0)->keyframes.size() == 1
               && editor.removeAsset("raster-b").changed,
           "removing a layer must erase only its frame-owned keys and empty frames");
    expect(validate(document).ok(),
           "every successful structural edit must leave the whole document valid");

    Document sourceCleanup = makeDocument();
    DocumentEditor sourceCleanupEditor(sourceCleanup);
    expect(sourceCleanupEditor.setStaticSource("motion", "vector-a").changed
               && sourceCleanup.frames.empty()
               && std::holds_alternative<StaticSource>(
                   layerSource(*findLayer(sourceCleanup, "motion"))),
           "switching an animated layer to static must remove all of its global keyframes");

    Document keyedRollback = makeDocument();
    DocumentEditor keyedRollbackEditor(keyedRollback);
    const std::uint64_t beforeInvalidStatic = keyedRollbackEditor.revision();
    expect(!keyedRollbackEditor.setStaticSource("motion", "raster-a").ok()
               && keyedRollbackEditor.revision() == beforeInvalidStatic
               && std::holds_alternative<KeyframedSource>(
                   layerSource(*findLayer(keyedRollback, "motion")))
               && findKeyframe(keyedRollback, "motion", 0)
               && findKeyframe(keyedRollback, "motion", 6),
           "an invalid keyed-to-static conversion must restore the marker and every frame-owned key");

    Document staticRollback = makeDocument();
    DocumentEditor staticRollbackEditor(staticRollback);
    expect(staticRollbackEditor.setStaticSource("motion", "vector-a").changed
               && staticRollback.frames.empty(),
           "the static rollback fixture must begin without frame-owned motion keys");
    const std::uint64_t beforeInvalidKeyed = staticRollbackEditor.revision();
    expect(!staticRollbackEditor.setKeyframedSource(
                    "motion", {KeyframePlacement{0, "raster-a"}}).ok()
               && staticRollbackEditor.revision() == beforeInvalidKeyed
               && std::holds_alternative<StaticSource>(
                   layerSource(*findLayer(staticRollback, "motion")))
               && staticRollback.frames.empty(),
           "an invalid static-to-keyed conversion must restore the static source and frame collection");

    Document layerCleanup = makeDocument();
    DocumentEditor layerCleanupEditor(layerCleanup);
    expect(layerCleanupEditor.removeLayer("motion").changed
               && layerCleanup.frames.empty()
               && validate(layerCleanup).ok(),
           "removing a keyframed layer must not leave dangling frame references");

    Document atomicLayerInsert = makeDocument();
    DocumentEditor atomicLayerInsertEditor(atomicLayerInsert);
    Layer insertedAnimation = VectorLayer{
        {"inserted-animation", "Inserted animation", true, 1.0, {},
         RasterBlendMode::SourceOver},
        KeyframedSource{},
    };
    const std::uint64_t beforeAtomicLayerInsert = atomicLayerInsertEditor.revision();
    expect(atomicLayerInsertEditor.insertKeyframedLayer(
                    insertedAnimation,
                    {KeyframePlacement{0, "vector-a"},
                     KeyframePlacement{3, "vector-b"}},
                    1).changed
               && atomicLayerInsertEditor.revision() == beforeAtomicLayerInsert + 1
               && layerIndex(atomicLayerInsert, "inserted-animation")
                    == std::optional<std::size_t>{1}
               && findKeyframe(atomicLayerInsert, "inserted-animation", 0)
               && findKeyframe(atomicLayerInsert, "inserted-animation", 3)
               && std::get<KeyframedSource>(layerSource(
                      *findLayer(atomicLayerInsert, "inserted-animation"))).frameIndices
                    == std::vector<FrameIndex>{0, 3},
           "a keyframed layer and its frame-owned keys must be insertable in one commit");
    const std::uint64_t beforeRejectedLayerInsert = atomicLayerInsertEditor.revision();
    Layer rejectedAnimation = VectorLayer{
        {"rejected-animation", "Rejected animation", true, 1.0, {},
         RasterBlendMode::SourceOver},
        KeyframedSource{},
    };
    expect(!atomicLayerInsertEditor.insertKeyframedLayer(
                    rejectedAnimation,
                    {KeyframePlacement{2, "vector-a"}}).ok()
               && atomicLayerInsertEditor.revision() == beforeRejectedLayerInsert
               && findLayer(atomicLayerInsert, "rejected-animation") == nullptr
               && findKeyframe(atomicLayerInsert, "rejected-animation", 2) == nullptr,
           "an invalid atomic keyframed-layer insertion must preserve layers, frames, and revision");

    Document assetRename = makeDocument();
    DocumentEditor assetRenameEditor(assetRename);
    expect(assetRenameEditor.renameAsset("vector-a", "vector-origin").changed
               && findKeyframe(assetRename, "motion", 0)
               && findKeyframe(assetRename, "motion", 0)->assetId == "vector-origin"
               && findAsset(assetRename, "vector-a") == nullptr,
           "renaming an asset must rewrite its frame-owned keyframe references");

    Document invalid;
    DocumentEditor rejected(invalid);
    expect(!rejected.isBound()
               && rejected.lastResult().code == DocumentEditCode::InvalidDocument,
           "an invalid document must not bind to the safe mutation API");

    Document externallyChanged = makeDocument();
    DocumentEditor guarded(externallyChanged);
    externallyChanged.extent.width = 0;
    const DocumentEditResult invalidDocument = guarded.setLayerName("paint", "Ignored");
    expect(!invalidDocument.ok()
               && invalidDocument.code == DocumentEditCode::InvalidDocument
               && layerProperties(*findLayer(externallyChanged, "paint")).name == "Paint"
               && guarded.revision() == 0,
           "the editor must detect and avoid extending an externally invalidated aggregate");
    guarded.unbind();
    expect(!guarded.isBound() && guarded.document() == nullptr,
           "unbind must release the document without touching its data");

    return failures == 0 ? 0 : 1;
}
