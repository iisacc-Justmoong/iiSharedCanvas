#include <Bitmap/BitmapEditor.h>
#include <Bitmap/ChunkedBitmapEditor.h>
#include <Document/DocumentHistory.h>
#include <array>
#include <iiBrushTools/Gesture.h>
#include <iostream>
int main()
{
    using namespace iiSharedCanvas;
    ii::brush::Controls controls;
    controls.set(ii::brush::Parameter::Size, 24);
    controls.set(ii::brush::Parameter::Opacity, 65);
    controls.set(ii::brush::Parameter::Start, 15);
    controls.set(ii::brush::Parameter::End, 30);
    controls.set(ii::brush::Parameter::TaperStrength, 100);
    std::array<StrokePoint, 4> samples;
    for (std::size_t i = 0; i < samples.size(); ++i)
    {
        samples[i].position = {10. + 130. * i, 128};
        samples[i].pressure = 1;
        samples[i].time = 1. + i;
    }
    Document doc;
    doc.extent = {512, 256};
    doc.assets.emplace_back(RasterAsset{"paint", makeRasterLayer(512, 256, 0x804488cc)});
    doc.layers.emplace_back(StaticBitmapLayer{{"layer", "Paint"}, StaticSource{"paint"}});
    auto *raster = &findRasterAsset(doc, "paint")->pixels;
    const auto original = raster->pixels;
    ii::brush::RenderTarget target;
    target.clip = {{0, 0}, 512, 256};
    target.argb = 0xffcc5522;
    target.source = {raster,
                     [](const void *p, DevicePixelPoint q)
                     { return rasterLayerPixelAt(*static_cast<const RasterLayer *>(p), q); },
                     {},
                     512,
                     256};
    const auto output = ii::brush::renderGesture(samples, ii::brush::Tool::Paint, controls, target);
    if (!output || output->tiles.size() < 3)
        return 1;
    std::vector<BitmapPatch> patches;
    for (const auto &tile : output->tiles)
        patches.push_back({tile.bounds, tile.pixels.pixels});
    BitmapEditor editor(doc, "paint");
    if (!editor.replacePatches(patches) || editor.revision() != 1 ||
        editor.historyStatistics().undoEntries != 1)
        return 2;
    for (const auto &tile : output->tiles)
        for (int y = 0; y < tile.bounds.height; ++y)
            for (int x = 0; x < tile.bounds.width; ++x)
                if (editor.pixelAt(tile.bounds.origin.x + x, tile.bounds.origin.y + y) !=
                    tile.pixels.pixels[std::size_t(y) * tile.bounds.width + x])
                    return 3;
    const auto final = raster->pixels;
    if (!editor.undo() || raster->pixels != original || !editor.redo() || raster->pixels != final)
        return 4;
    const auto replacePreview = [](auto &editor, const auto &rendered)
    {
        std::vector<BitmapPatch> view;
        for (const auto &tile : rendered.tiles)
            view.push_back({tile.bounds, tile.pixels.pixels});
        return editor.replacePixelPreview(view);
    };
    const auto flatten =
        [](const auto &rendered, std::vector<std::uint32_t> pixels, DevicePixelRect clip)
    {
        for (const auto &tile : rendered.tiles)
            for (int y = 0; y < tile.bounds.height; ++y)
                for (int x = 0; x < tile.bounds.width; ++x)
                    pixels[std::size_t(tile.bounds.origin.y + y - clip.origin.y) * clip.width +
                           tile.bounds.origin.x + x - clip.origin.x] =
                        tile.pixels.pixels[std::size_t(y) * tile.bounds.width + x];
        return pixels;
    };
    if (!editor.undo() || !editor.beginPixelPreview("Whole gesture taper") ||
        !replacePreview(editor, *output))
        return 7;
    auto independentSource = makeRasterLayer(512, 256);
    independentSource.pixels = original;
    auto independentTarget = target;
    independentTarget.source.context = &independentSource;
    auto previewTarget = target;
    previewTarget.source.context = &editor;
    previewTarget.source.sampleArgb = [](const void *p, DevicePixelPoint q)
    { return static_cast<const BitmapEditor *>(p)->pixelPreviewSourceAt(q.x, q.y).value_or(0); };
    const auto shortened = std::span(samples).first(3);
    const auto expectedShort =
        ii::brush::renderGesture(shortened, ii::brush::Tool::Paint, controls, independentTarget);
    const auto liveShort =
        ii::brush::renderGesture(shortened, ii::brush::Tool::Paint, controls, previewTarget);
    if (!expectedShort || !liveShort || !replacePreview(editor, *liveShort))
        return 8;
    const auto expectedPixels = flatten(*expectedShort, original, target.clip);
    if (raster->pixels != expectedPixels || !editor.commitPixelPreview() ||
        editor.historyStatistics().undoEntries != 1 || !editor.undo() ||
        raster->pixels != original || !editor.redo() || raster->pixels != expectedPixels)
        return 9;
    // The whole-gesture tapered result can also be one entry in the document
    // coordinator, ordered with a layer edit. A live-preview case follows.
    std::vector<BitmapPatch> documentPatches;
    for (const auto &tile : output->tiles)
        documentPatches.push_back({tile.bounds, tile.pixels.pixels});
    DocumentHistory documentHistory(doc);
    if (!documentHistory.replaceBitmapPatches("paint",documentPatches,"Tapered stroke").changed ||
        !documentHistory.setLayerName("layer","Ink").changed ||
        !documentHistory.undo().changed || !documentHistory.undo().changed ||
        raster->pixels != expectedPixels || layerProperties(*findLayer(doc,"layer")).name != "Paint" ||
        !documentHistory.redo().changed || !documentHistory.redo().changed ||
        raster->pixels != final || layerProperties(*findLayer(doc,"layer")).name != "Ink")
        return 14;
    Document liveDoc;
    liveDoc.extent={512,256};
    liveDoc.assets.emplace_back(RasterAsset{"paint",makeRasterLayer(512,256,0x804488cc)});
    liveDoc.layers.emplace_back(StaticBitmapLayer{{"layer","Paint"},StaticSource{"paint"}});
    DocumentHistory liveHistory(liveDoc);
    if (!liveHistory.setLayerName("layer","Ink").changed || !liveHistory.undo().changed ||
        !liveHistory.beginPixelPreview("paint","Whole gesture taper").ok())
        return 15;
    auto liveTarget=target;
    liveTarget.source.context=&liveHistory;
    liveTarget.source.sampleArgb=[](const void *p,DevicePixelPoint q) {
        return static_cast<const DocumentHistory *>(p)->pixelPreviewSourceAt(q.x,q.y).value_or(0);
    };
    const auto globalFull=ii::brush::renderGesture(samples,ii::brush::Tool::Paint,controls,liveTarget);
    if (!globalFull) return 16;
    std::vector<BitmapPatch> livePatches;
    for (const auto &tile:globalFull->tiles) livePatches.push_back({tile.bounds,tile.pixels.pixels});
    if (!liveHistory.replacePixelPreview(livePatches).changed ||
        findRasterAsset(liveDoc,"paint")->pixels.pixels!=final)
        return 17;
    const auto globalShort=ii::brush::renderGesture(shortened,ii::brush::Tool::Paint,controls,liveTarget);
    if (!globalShort) return 18;
    livePatches.clear();
    for (const auto &tile:globalShort->tiles) livePatches.push_back({tile.bounds,tile.pixels.pixels});
    if (!liveHistory.replacePixelPreview(livePatches).changed ||
        findRasterAsset(liveDoc,"paint")->pixels.pixels!=expectedPixels ||
        liveHistory.commitPixelPreview().discardedRedoEntries!=1 ||
        !liveHistory.undo().changed || findRasterAsset(liveDoc,"paint")->pixels.pixels!=original ||
        !liveHistory.redo().changed || findRasterAsset(liveDoc,"paint")->pixels.pixels!=expectedPixels)
        return 19;
    Document chunks;
    chunks.extent = {512, 256};
    chunks.canvasMode = CanvasMode::Infinite;
    chunks.infiniteCanvas.origin = {-200, -100};
    chunks.infiniteCanvas.chunkSize = 128;
    chunks.assets.emplace_back(ChunkedRasterAsset{"paint", {}});
    chunks.layers.emplace_back(StaticBitmapLayer{{"layer", "Paint"}, StaticSource{"paint"}});
    for (auto &sample : samples)
    {
        sample.position.x -= 200;
        sample.position.y -= 100;
    }
    target.clip = {{-200, -100}, 512, 256};
    target.source = {};
    const auto world = ii::brush::renderGesture(samples, ii::brush::Tool::Paint, controls, target);
    if (!world)
        return 5;
    patches.clear();
    for (const auto &tile : world->tiles)
        patches.push_back({tile.bounds, tile.pixels.pixels});
    ChunkedBitmapEditor chunkEditor(chunks, "paint");
    if (!chunkEditor.replacePatches(patches) || chunkEditor.historyStatistics().undoEntries != 1 ||
        !chunkEditor.undo() || !findChunkedRasterAsset(chunks, "paint")->chunks.empty() ||
        !chunkEditor.redo())
        return 6;
    if (!chunkEditor.undo() || !chunkEditor.beginPixelPreview("World gesture taper") ||
        !replacePreview(chunkEditor, *world))
        return 10;
    const auto worldShort = ii::brush::renderGesture(std::span(samples).first(3),
                                                     ii::brush::Tool::Paint, controls, target);
    if (!worldShort || !replacePreview(chunkEditor, *worldShort))
        return 11;
    const auto expectedWorld =
        flatten(*worldShort, std::vector<std::uint32_t>(512 * 256), target.clip);
    for (int y = 0; y < 256; ++y)
        for (int x = 0; x < 512; ++x)
            if (chunkEditor.pixelAt(x - 200, y - 100) != expectedWorld[std::size_t(y) * 512 + x])
                return 12;
    if (!chunkEditor.commitPixelPreview() || chunkEditor.historyStatistics().undoEntries != 1 ||
        !chunkEditor.undo() || !findChunkedRasterAsset(chunks, "paint")->chunks.empty())
        return 13;
    DocumentHistory worldHistory(chunks);
    if (!worldHistory.beginPixelPreview("paint","World gesture taper").ok()) return 20;
    livePatches.clear();
    for (const auto &tile:world->tiles) livePatches.push_back({tile.bounds,tile.pixels.pixels});
    if (!worldHistory.replacePixelPreview(livePatches).changed) return 21;
    livePatches.clear();
    for (const auto &tile:worldShort->tiles) livePatches.push_back({tile.bounds,tile.pixels.pixels});
    if (!worldHistory.replacePixelPreview(livePatches).changed ||
        !worldHistory.commitPixelPreview().changed || !worldHistory.undo().changed ||
        !findChunkedRasterAsset(chunks,"paint")->chunks.empty() || !worldHistory.redo().changed)
        return 22;
    for (int y=0;y<256;++y)
        for (int x=0;x<512;++x)
            if (chunkEditor.pixelAt(x-200,y-100)!=expectedWorld[std::size_t(y)*512+x]) return 23;
    Document maskDoc;
    maskDoc.extent={64,64};
    maskDoc.assets.emplace_back(RasterAsset{"paint",makeRasterLayer(64,64)});
    maskDoc.assets.emplace_back(RasterAsset{"mask",makeRasterLayer(64,64)});
    maskDoc.layers.emplace_back(StaticBitmapLayer{{"layer","Paint"},StaticSource{"paint"}});
    layerProperties(maskDoc.layers.front()).appearance.rasterMask=LayerMask{.assetId="mask"};
    const std::array<std::uint32_t,1> paintValue{0xff112233},maskValue{0xff808080};
    const std::array paintPatch{BitmapPatch{{{2,2},1,1},paintValue}},
                     maskPatch{BitmapPatch{{{2,2},1,1},maskValue}};
    DocumentHistory maskHistory(maskDoc);
    if (!maskHistory.replaceBitmapPatches("paint",paintPatch).changed ||
        !maskHistory.beginPixelPreview("mask","Mask stroke").ok() ||
        !maskHistory.replacePixelPreview(maskPatch).changed || !maskHistory.commitPixelPreview().changed ||
        !maskHistory.undo().changed || findRasterAsset(maskDoc,"mask")->pixels.pixels[130]!=0 ||
        !maskHistory.undo().changed || findRasterAsset(maskDoc,"paint")->pixels.pixels[130]!=0 ||
        !maskHistory.redo().changed || !maskHistory.redo().changed ||
        findRasterAsset(maskDoc,"mask")->pixels.pixels[130]!=maskValue[0])
        return 24;
    std::cout << "authoring_tiles=" << output->tiles.size()
              << " world_tiles=" << world->tiles.size()
              << " one_command_undo=true whole_gesture_taper=true "
                 "complete_preview_replacement=true original_source_sampler=true "
                 "document_history_order=true document_live_preview=true mask_preview=true\n";
}
