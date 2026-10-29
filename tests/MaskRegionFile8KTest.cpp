#include <Document/DocumentHistory.h>
#include <File/DocumentFile.h>
#include <Render/FrameRenderer.h>
#include <Validation/Validation.h>
#include <iiFileProviderNative.h>

#include <array>
#include <cstdint>
#include <filesystem>
#include <iostream>

using namespace iiSharedCanvas;
namespace {
constexpr int Side=8192, MarkX=4100, MarkY=4100;
int failures=0;
void check(bool value,const char *message) {
    if (!value) { ++failures; std::cerr << message << '\n'; }
}
std::uint64_t checksum(const RasterLayer &r) {
    std::uint64_t hash=1469598103934665603ULL;
    for (auto p:r.pixels) { hash^=p; hash*=1099511628211ULL; }
    return hash;
}
FrameRenderResult view(const Document &d) {
    return renderFrameRegion(d,0,{{4096,4096},{32,32}},{32,32});
}
}
int main() {
    Document source; source.extent={Side,Side};
    source.assets.emplace_back(RasterAsset{"paint",makeRasterLayer(64,64,0xff7293b1)});
    auto pixels=makeRasterLayer(Side,Side);
    std::uint32_t random=1;
    std::uint64_t originalHash=1469598103934665603ULL, changedHash=originalHash;
    const auto marked=std::size_t(MarkY)*Side+MarkX;
    const std::uint32_t value=0xffffffffU;
    for (std::size_t i=0;i<pixels.pixels.size();++i) {
        random^=random<<13; random^=random>>17; random^=random<<5;
        pixels.pixels[i]=random;
        originalHash^=random; originalHash*=1099511628211ULL;
        changedHash^=(i==marked?value:random); changedHash*=1099511628211ULL;
    }
    source.assets.emplace_back(RasterAsset{"mask",std::move(pixels)});
    source.layers.emplace_back(StaticBitmapLayer{{"layer","Paint"},StaticSource{"paint"}});
    auto &properties=layerProperties(source.layers[0]);
    properties.transform.translationX=4096; properties.transform.translationY=4096;
    LayerMask mask; mask.assetId="mask"; mask.linkedToLayer=false;
    mask.featherRadius=3.25; mask.outsideCoverage=0;
    properties.appearance.rasterMask=mask;
    if (!validate(source).ok()) { check(false,"8K file: high-entropy raster mask must validate"); return 1; }

    const std::filesystem::path parent(IISHAREDCANVAS_TEST_OUTPUT_DIR);
    std::filesystem::create_directories(parent); iiFileProvider::native::TemporaryDirectory tmp(parent);
    if (!tmp.isValid()) { check(false,"8K file: temporary directory"); return 1; }
    const auto path=(tmp.path()/"dense-mask.iisc").string();
    DocumentFile owner; const auto created=owner.create(path,source);
    if (!created.ok()) { std::cerr << "8K file: create: " << created.message << '\n'; return 1; }
    source={};
    const auto bytes=std::filesystem::file_size(path);
    check(bytes>200ULL*1024*1024,"8K file: actual dense mask occupies the working file");
    check(checksum(findRasterAsset(*owner.document(),"mask")->pixels)==originalHash,
          "8K file: every source mask pixel survived creation");
    const auto original=view(*owner.document());
    check(original.ok(),"8K file: original view");
    {
        DocumentHistory history(owner);
        const std::array<std::uint32_t,1> newPixel{value};
        const std::array patch{BitmapPatch{{{MarkX,MarkY},1,1},newPixel}};
        check(history.isBound() && history.beginPixelPreview("mask","Mask stroke").ok() &&
              history.replacePixelPreview(patch).changed,"8K file: preview visible result");
        const auto preview=view(*owner.document());
        check(preview.ok() && original.ok() && preview.pixels.pixels!=original.pixels.pixels,
              "8K file: mask preview changes the rendered view");
        check(history.cancelPixelPreview().changed && owner.revision()==2,
              "8K file: cancel durably restores the source");
        const auto cancelled=view(*owner.document());
        check(cancelled.ok() && original.ok() && cancelled.pixels.pixels==original.pixels.pixels,
              "8K file: cancelled view restores exactly");
        check(history.beginPixelPreview("mask","Mask stroke").ok() &&
              history.replacePixelPreview(patch).changed && history.commitPixelPreview().changed &&
              owner.revision()==3,"8K file: final preview publishes one command");
        check(history.undo().changed && owner.revision()==4 &&
              checksum(findRasterAsset(*owner.document(),"mask")->pixels)==originalHash,
              "8K file: undo restores all 8K source pixels");
        check(history.redo().changed && owner.revision()==5 &&
              checksum(findRasterAsset(*owner.document(),"mask")->pixels)==changedHash,
              "8K file: redo changes exactly the selected mask pixel");
    }
    owner.close();
    DocumentFile reopened; const auto opened=reopened.open(path);
    if (!opened.ok()) { std::cerr << "8K file: reopen: " << opened.message << '\n'; return 1; }
    check(reopened.revision()==5 && checksum(findRasterAsset(*reopened.document(),"mask")->pixels)==changedHash,
          "8K file: reopened document preserves all pixels and revision");
    const auto reopenedView=view(*reopened.document());
    check(reopenedView.ok() && original.ok() && reopenedView.pixels.pixels!=original.pixels.pixels,
          "8K file: reopened composition uses the committed mask");
    reopened.close();
    std::cout << "MASK_FILE_8K dense_mask_bytes=" << 4ULL*Side*Side << " file_bytes=" << bytes
              << " checked_pixels=" << std::uint64_t(Side)*Side
              << " cancel_undo_redo_reopen=" << (failures==0) << '\n';
    return failures?1:0;
}
