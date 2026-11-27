#include <iiSharedCanvas>

#include <algorithm>
#include <array>
#include <filesystem>
#include <iostream>
#include <limits>
#include <string_view>
#include <zlib.h>

using namespace iiSharedCanvas;

namespace {
int failures = 0;
void expect(bool condition, std::string_view description)
{
    if (!condition) { ++failures; std::cerr << description << '\n'; }
}

Document sample(std::uint32_t pixel)
{
    Document d;
    d.extent = {2, 2};
    d.assets.emplace_back(RasterAsset{"pixels", makeRasterLayer(2, 2, pixel)});
    d.layers.emplace_back(StaticBitmapLayer{{"layer", "Layer"}, StaticSource{"pixels"}});
    return d;
}

void embeddedCopy()
{
    auto child = sample(0xff224466);
    auto copied = LinkedAssetResolver::copy("linked", child, 0);
    expect(copied.ok(), "copy the child document");
    if (!copied.ok()) return;
    auto parent = sample(0);
    parent.assets.emplace_back(copied.asset);
    parent.layers.emplace_back(StaticBitmapLayer{{"linked-one", "First"}, StaticSource{"linked"}});
    parent.layers.emplace_back(StaticBitmapLayer{{"linked-two", "Second"}, StaticSource{"linked"}});
    findRasterAsset(child, "pixels")->pixels.pixels[0] = 0xffffffff;
    auto resolved = LinkedAssetResolver::resolve(copied.asset);
    expect(resolved.ok() && resolved.pixels.pixels[0] == 0xff224466,
           "the linked copy is independent from the original");
    auto encoded = encodeIisc(parent);
    expect(encoded.ok(), "encode an owned linked canvas");
    expect(encodePsd(parent).result.code == MediaIoCode::UnsupportedFeature,
           "PSD export explicitly rejects owned documents it cannot preserve");
    if (!encoded.ok()) return;
    auto decoded = decodeIisc(encoded.bytes);
    expect(decoded.ok(), "decode an owned linked canvas");
    if (!decoded.ok()) return;
    const auto info = DocumentInfoSnapshotBuilder::build(decoded.document);
    expect(info.assets.size() == 2 && info.assets[1].linkedFrame == 0 &&
           info.assets[1].linkedBytes == copied.asset.document.size(),
           "snapshot identifies linked assets without copying their documents");
    const auto *asset = std::get_if<LinkedCanvasAsset>(findAsset(decoded.document, "linked"));
    expect(asset && LinkedAssetResolver::resolve(*asset).ok(), "restore the owned document");
    auto rendered = renderFrame(decoded.document, 0);
    expect(rendered.ok() && rendered.pixels.pixels[0] == 0xff224466,
           "layers render the shared linked asset");
    if (asset) {
        auto corrupt = *asset;
        corrupt.document.back() ^= 1;
        expect(!LinkedAssetResolver::resolve(corrupt).ok(), "reject a corrupt embedded document");
        auto invalidParent = decoded.document;
        *std::get_if<LinkedCanvasAsset>(findAsset(invalidParent, "linked")) = corrupt;
        expect(!encodeIisc(invalidParent).ok(), "reject a corrupt embedded document during encoding");
        auto forged = encoded.bytes;
        const auto at = std::search(forged.begin() + IiscHeaderSize, forged.end(),
                                    asset->document.begin(), asset->document.end());
        expect(at != forged.end(), "embedded bytes exist in the parent container");
        if (at != forged.end()) {
            *(at + static_cast<std::ptrdiff_t>(asset->document.size() - 1)) ^= 1;
            const auto crc = crc32(0, forged.data() + IiscHeaderSize,
                                   static_cast<uInt>(forged.size() - IiscHeaderSize));
            for (unsigned i = 0; i != 4; ++i) forged[24 + i] = static_cast<std::uint8_t>(crc >> (i * 8));
            expect(!decodeIisc(forged).ok(), "reject a corrupt child even when the parent checksum is valid");
        }
    }
    SerializationLimits noNesting; noNesting.maximumEmbeddedCanvasDepth = 0;
    expect(!decodeIisc(encoded.bytes, noNesting).ok(), "enforce nested document depth limit");
    SerializationLimits noBytes; noBytes.maximumEmbeddedCanvasBytes = 1;
    expect(!decodeIisc(encoded.bytes, noBytes).ok(), "enforce embedded bytes limit");
    auto legacy = parent; legacy.formatVersion.minor = 22;
    expect(!encodeIisc(legacy).ok(), "old format cannot silently carry a linked asset");
}

void editsAndFile()
{
    auto original = sample(0xff112233);
    auto copy = LinkedAssetResolver::copy("linked", original, 0);
    auto parent = sample(0);
    DocumentHistory history(parent);
    Layer layer = StaticBitmapLayer{{"linked-layer", "Linked"}, StaticSource{"linked"}};
    expect(history.insertLayerWithAsset(copy.asset, layer).changed && history.undo().changed &&
           !findAsset(parent, "linked") && history.redo().changed && findAsset(parent, "linked"),
           "linked asset and layer participate in one history entry");
    auto second = sample(0xffabcdef);
    auto replacement = LinkedAssetResolver::copy("linked", second, 0);
    expect(history.replaceLinkedCanvasAsset(replacement.asset).changed &&
           renderFrame(parent, 0).pixels.pixels[0] == 0xffabcdef,
           "replace the one owned asset observed by layers");
    expect(history.undo().changed && renderFrame(parent, 0).pixels.pixels[0] == 0xff112233 &&
           history.redo().changed && renderFrame(parent, 0).pixels.pixels[0] == 0xffabcdef,
           "undo and redo a shared copy replacement");
    DocumentEditor editor(parent);
    auto invalid = replacement.asset; invalid.document.back() ^= 1;
    const auto before = encodeIisc(parent).bytes;
    expect(!editor.replaceLinkedCanvasAsset("linked", invalid).ok() && encodeIisc(parent).bytes == before,
           "invalid replacement preserves the owner");
    DocumentHistoryLimits limits; limits.maximumBytes = 1;
    DocumentHistory limited(parent, limits);
    expect(limited.replaceLinkedCanvasAsset(copy.asset).code == DocumentHistoryCode::BudgetExceeded &&
           encodeIisc(parent).bytes == before, "replacement budget failure preserves the owner");
    const auto root = std::filesystem::path(IISHAREDCANVAS_TEST_OUTPUT_DIR) / "document-asset-control";
    std::filesystem::create_directories(root);
    const auto from = root / "owned.iisc", to = root / "moved.iisc";
    std::filesystem::remove(from); std::filesystem::remove(to);
    DocumentFile file;
    auto created = file.create(from.string(), parent);
    expect(created.ok(), "create a working file with a linked asset");
    if (!created.ok()) return;
    auto snapshot = DocumentInfoSnapshotBuilder::build(file);
    expect(snapshot && snapshot->storageRevision == file.revision() && snapshot->filePath == from.string(),
           "snapshot reports file metadata and storage revision separately");
    DocumentHistory fileHistory(file);
    expect(fileHistory.replaceLinkedCanvasAsset(copy.asset).changed, "file-bound replacement persists");
    LastEditToggle fileToggle(fileHistory);
    expect(fileToggle.toggle().changed && fileToggle.toggle().changed,
           "file-bound replacement toggles through undo and redo");
    file.close();
    expect(fileToggle.toggle().code == DocumentHistoryCode::NotBound, "closed files cannot be toggled");
    std::filesystem::rename(from, to);
    auto opened = file.open(to.string());
    expect(opened.ok(), "open the moved parent file");
    if (opened.ok()) {
        auto frame = renderFrame(*file.document(), 0);
        expect(frame.ok() && frame.pixels.pixels[0] == 0xff112233,
               "moving a parent file retains its owned canvas");
    }
    file.close();
}

void nestingAndOrigin()
{
    Document infinite; infinite.extent = {2, 2}; infinite.canvasMode = CanvasMode::Infinite;
    infinite.infiniteCanvas.origin = {-1, -1}; infinite.infiniteCanvas.chunkSize = 32;
    auto chunk = makeRasterLayer(32, 32, 0);
    chunk.pixels[31 * 32 + 31] = 0xff667788;
    infinite.assets.emplace_back(ChunkedRasterAsset{"chunk", {{-1, -1, std::move(chunk)}}});
    infinite.layers.emplace_back(StaticBitmapLayer{{"chunk-layer", "Chunk"}, StaticSource{"chunk"}});
    auto child = LinkedAssetResolver::copy("embedded", infinite, 0);
    expect(child.ok(), "copy infinite child");
    if (!child.ok()) return;
    auto parent = sample(0);
    parent.assets.emplace_back(child.asset);
    Layer placed = StaticBitmapLayer{{"placed", "Placed"}, StaticSource{"embedded"}};
    layerProperties(placed).transform.translationX = 1;
    layerProperties(placed).transform.translationY = 1;
    parent.layers.push_back(placed);
    auto rendered = renderFrame(parent, 0);
    expect(rendered.ok() && rendered.pixels.pixels[0] == 0xff667788,
           "child origin is included in the parent layer transform");
    auto outerAsset = LinkedAssetResolver::copy("outer", parent, 0);
    expect(outerAsset.ok(), "copy a child that already owns another child");
    if (!outerAsset.ok()) return;
    auto outer = sample(0);
    outer.assets.emplace_back(outerAsset.asset);
    outer.layers.emplace_back(StaticBitmapLayer{{"outer-layer", "Outer"}, StaticSource{"outer"}});
    auto encoded = encodeIisc(outer);
    SerializationLimits one; one.maximumEmbeddedCanvasDepth = 1;
    SerializationLimits two; two.maximumEmbeddedCanvasDepth = 2;
    expect(encoded.ok() && !decodeIisc(encoded.bytes, one).ok() && decodeIisc(encoded.bytes, two).ok(),
           "nested copies obey the configured depth");
    expect(renderFrame(outer, 0).ok() && renderFrame(outer, 0).pixels.pixels[0] == 0xff667788,
           "nested copies render their selected frames");
    auto wide = sample(0xff123456);
    wide.artboards.push_back(Artboard{"far", "Far", {{100, 0}, {1, 1}}});
    auto nested = LinkedAssetResolver::copy("nested", wide, 0);
    auto middle = sample(0);
    middle.assets.emplace_back(nested.asset);
    middle.layers.emplace_back(StaticBitmapLayer{{"nested-layer", "Nested"}, StaticSource{"nested"}});
    auto root = LinkedAssetResolver::copy("root", middle, 0);
    SerializationLimits smallView; smallView.maximumCanvasPixels = 4;
    expect(root.ok() && !LinkedAssetResolver::resolve(root.asset, smallView).ok(),
           "nested child artboards cannot bypass the caller's render pixel limit");
}

void snapshotAndToggle()
{
    auto d = sample(0xff123456);
    DocumentHistory h(d);
    auto snapshot = DocumentInfoSnapshotBuilder::build(d, &h);
    expect(snapshot.assetCount == 1 && snapshot.layerCount == 1 && snapshot.assets[0].id == "pixels",
           "snapshot captures detached document information");
    expect(h.setLayerName("layer", "Changed").changed, "prepare toggle");
    LastEditToggle toggle(h);
    expect(toggle.toggle().changed && layerProperties(d.layers[0]).name == "Layer", "toggle undo");
    expect(toggle.toggle().changed && layerProperties(d.layers[0]).name == "Changed", "toggle redo");
    expect(snapshot.layers[0].name == "Layer", "snapshot is detached from edits");
    expect(h.setLayerVisible("layer", false).changed, "create a later edit");
    expect(toggle.toggle().code == DocumentHistoryCode::Conflict && !layerProperties(d.layers[0]).visible,
           "toggle must not undo a different command");
    LastEditToggle previewToggle(h);
    expect(h.beginPixelPreview("pixels").ok(), "start a preview");
    expect(previewToggle.toggle().code == DocumentHistoryCode::PreviewActive,
           "toggle cannot cross an active preview");
    expect(h.cancelPixelPreview().ok() && previewToggle.toggle().changed,
           "toggle resumes after the preview is cancelled");
    expect(h.reset().ok() && previewToggle.toggle().code == DocumentHistoryCode::Conflict,
           "reset invalidates the captured entry");
}

void reeditableRaster()
{
    auto d = sample(0xff406080);
    const RasterLayer original = findRasterAsset(d, "pixels")->pixels;
    DocumentHistory history(d);
    ReeditableRasterChange change(history, "pixels", 1024);
    expect(change.ready(), "capture the original raster");
    ColorAdjustment first; first.exposure = 1;
    ColorAdjustment second; second.exposure = 2;
    expect(change.apply(first).changed && change.apply(second).changed, "apply two settings");
    expect(change.settings() && std::get<ColorAdjustment>(*change.settings()) == second,
           "retain the current accepted settings for the editor");
    const auto expected = adjustRaster(original, second);
    expect(expected.ok() && findRasterAsset(d, "pixels")->pixels.pixels == expected.pixels.pixels,
           "recalculate from original pixels instead of accumulating");
    RasterEffect invert; invert.kind = RasterEffectKind::Invert;
    expect(change.apply(invert).changed && change.apply(second).changed &&
           findRasterAsset(d, "pixels")->pixels.pixels == expected.pixels.pixels,
           "switching effect types still uses the original pixels");
    const auto before = findRasterAsset(d, "pixels")->pixels.pixels;
    const auto count = history.statistics().undoEntries;
    second.exposure = std::numeric_limits<double>::quiet_NaN();
    expect(!change.apply(second).ok() && findRasterAsset(d, "pixels")->pixels.pixels == before &&
           history.statistics().undoEntries == count, "invalid setting preserves pixels and history");
    expect(history.undo().changed, "raster settings participate in history");
    expect(change.apply(first).code == DocumentHistoryCode::Conflict, "reject a stale raster session");
    ReeditableRasterChange tiny(history, "pixels", 1);
    expect(!tiny.ready(), "enforce original and result memory budget");
}
} // namespace

int main()
{
    embeddedCopy();
    editsAndFile();
    nestingAndOrigin();
    snapshotAndToggle();
    reeditableRaster();
    return failures ? 1 : 0;
}
