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
        KeyframedSource{{{0, "vector-a"}, {6, "vector-b"}}},
    });
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
    const auto &initialSource = std::get<KeyframedSource>(layerSource(document.layers[1]));
    expect(findKeyframe(initialSource, 6) != nullptr
               && keyframeIndex(initialSource, 6) == std::optional<std::size_t>{1},
           "exact keyframe lookup must be distinct from hold sampling");
    expect(assetReferences(document, "vector-a").size() == 1
               && assetReferences(document, "raster-a").front().keyframeIndex == std::nullopt,
           "callers must be able to inspect every static and keyframed asset reference");

    expect(editor.setCanvasExtent({16, 12}).changed
               && document.extent.width == 16
               && document.extent.height == 12,
           "canvas extent must be directly editable");
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
    expect(editor.insertKeyframe("highlights", {4, "vector-c"}).code
               == DocumentEditCode::SourceNotKeyframed,
           "keyframe operations must identify a static source precisely");
    expect(editor.setKeyframedSource("highlights",
                                     {{0, "vector-a"}, {8, "vector-b"}}).changed,
           "a layer source must be replaceable with a validated keyframe track");

    const std::uint64_t beforeKeyframeKindMismatch = editor.revision();
    const DocumentEditResult keyframeKindMismatch = editor.insertKeyframe(
        "highlights", {4, "raster-b"});
    expect(!keyframeKindMismatch.ok()
               && keyframeKindMismatch.code == DocumentEditCode::AssetKindMismatch
               && editor.revision() == beforeKeyframeKindMismatch
               && findKeyframe(std::get<KeyframedSource>(
                                   layerSource(*findLayer(document, "highlights"))), 4)
                    == nullptr,
           "a vector layer must reject a bitmap keyframe without partial insertion");
    expect(editor.insertKeyframe("highlights", {4, "vector-c"}).changed
               && keyframeIndex(std::get<KeyframedSource>(
                                    layerSource(*findLayer(document, "highlights"))), 4)
                    == std::optional<std::size_t>{1},
           "keyframes must be inserted in chronological order from frame values");
    expect(editor.insertKeyframe("highlights", {4, "vector-c"}).code
               == DocumentEditCode::DuplicateKeyframe,
           "duplicate exact-frame insertion must have a typed rejection");
    expect(editor.setKeyframeAsset("highlights", 4, "vector-b").changed
               && findKeyframe(std::get<KeyframedSource>(
                                   layerSource(*findLayer(document, "highlights"))), 4)->assetId
                    == "vector-b",
           "a keyframe asset reference must be independently replaceable");
    expect(editor.moveKeyframe("highlights", 4, 5).changed
               && findKeyframe(std::get<KeyframedSource>(
                                   layerSource(*findLayer(document, "highlights"))), 5),
           "a non-initial keyframe must be moveable to another unoccupied frame");
    expect(editor.removeKeyframe("highlights", 5).changed
               && findKeyframe(std::get<KeyframedSource>(
                                   layerSource(*findLayer(document, "highlights"))), 5) == nullptr,
           "non-initial keyframes must be removable");
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
               && editor.removeAsset("raster-b").changed,
           "unreferenced layers and assets must be removable explicitly");
    expect(validate(document).ok(),
           "every successful structural edit must leave the whole document valid");

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
