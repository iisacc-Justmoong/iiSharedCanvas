#include <iiSharedCanvas.h>
#include <filesystem>
#include <iostream>
#include <limits>
using namespace iiSharedCanvas;
int failures = 0;
void check(bool ok, const char *m) { if (!ok) { ++failures; std::cerr << m << '\n'; } }
Document fixture() {
    Document d; d.extent = {4,1};
    d.assets.emplace_back(RasterAsset{"source", makeRasterLayer(4,1,0xff123456)});
    d.assets.emplace_back(RasterAsset{"a", {4,1,{0xffffffff,0xff000000,0xff808080,0xff000000}}});
    d.assets.emplace_back(RasterAsset{"b", {4,1,{0xff000000,0xffffffff,0xff808080,0xff000000}}});
    d.layers.emplace_back(StaticBitmapLayer{{"layer","Layer"}, StaticSource{"source"}});
    return d;
}
LayerAppearance &a(Document &d) { return layerProperties(d.layers[0]).appearance; }
int alpha(Document &d,int x) { auto r=renderFrame(d,0); check(r.ok(),"stack renders"); return r.ok() ? int(r.pixels.pixels[x]>>24) : -1; }
int main() {
    auto d=fixture(); a(d).maskStack={{"one","Painted",LayerMask{.assetId="a",.outsideCoverage=0}}, {"two","Shape",LayerMask{.assetId="b",.outsideCoverage=0}}};
    check(alpha(d,0)==255 && alpha(d,1)==255 && alpha(d,2)==128,"union is maximum coverage");
    a(d).maskStack[1].combine=MaskCombineMode::Subtract;
    check(alpha(d,0)==255 && alpha(d,1)==0 && alpha(d,2)==64,"subtract preserves soft edge");
    a(d).maskStack[1].combine=MaskCombineMode::Intersect;
    check(alpha(d,0)==0 && alpha(d,1)==0 && alpha(d,2)==64,"intersect multiplies coverage");
    a(d).maskStack[1].combine=MaskCombineMode::Replace;
    check(alpha(d,0)==0 && alpha(d,1)==255,"replace takes operand");
    a(d).maskStack[1].mask.enabled=false;
    check(alpha(d,0)==255 && alpha(d,1)==0,"hidden entry is skipped");
    a(d).maskStack[0].mask.density=0;
    check(alpha(d,0)==255 && alpha(d,1)==255,"density zero reveals original");
    a(d).maskStack[0].mask.density=1; a(d).maskStack[0].mask.inverted=true;
    check(alpha(d,0)==0 && alpha(d,1)==255,"inversion is nondestructive");
    a(d).maskStackEnabled=false;
    check(alpha(d,0)==255 && alpha(d,1)==255,"master bypass reveals original");
    check(std::get<RasterAsset>(d.assets[0]).pixels.pixels[0]==0xff123456,"source pixels remain unchanged");
    const auto encoded=encodeIisc(d); auto decoded=decodeIisc(encoded.bytes);
    check(encoded.ok() && decoded.ok() && a(decoded.document)==a(d),"all stack properties round trip");
    auto legacy=fixture(); legacy.formatVersion.minor=20;
    check(decodeIisc(encodeIisc(legacy).bytes).ok(),"legacy 1.20 stays readable");
    d.formatVersion.minor=20; check(!validate(d).ok(),"stack cannot silently downgrade"); d.formatVersion.minor=CurrentFormatMinor;
    SerializationLimits limits; limits.maximumMaskStackEntriesPerLayer=1;
    check(!encodeIisc(d,limits).ok() && !decodeIisc(encoded.bytes,limits).ok(),"encode and decode budgets enforced");
    const auto refs=assetReferences(d,"a"); check(refs.size()==1,"mask stack assets participate in reference accounting");
    DocumentEditor editor(d); check(editor.renameAsset("a","renamed").ok() && a(d).maskStack[0].mask.assetId=="renamed","asset rename updates stack");
    auto invalid=a(d); invalid.maskStack[1].id="one"; const auto revision=editor.revision();
    check(!editor.setLayerAppearance("layer",invalid).ok() && editor.revision()==revision,"duplicate IDs rejected atomically");
    invalid=a(d); invalid.maskStack[0].combine=static_cast<MaskCombineMode>(99);
    check(!editor.setLayerAppearance("layer",invalid).ok(),"unknown combine rejected");
    const auto path=std::filesystem::path(IISHAREDCANVAS_TEST_OUTPUT_DIR)/"mask-stack.iisc";
    std::filesystem::create_directories(path.parent_path()); std::filesystem::remove(path);
    DocumentFile file; check(file.create(path.string(),d).ok(),"working file created");
    DocumentEditor bound(file); auto changed=a(d); changed.maskStack[0].name="Renamed"; changed.maskStack[0].mask.featherRadius=0.4;
    check(bound.setLayerAppearance("layer",changed).ok(),"file bound stack commit");
    const auto committed=file.revision(); invalid=changed; invalid.maskStack[0].mask.assetId="missing";
    check(!bound.setLayerAppearance("layer",invalid).ok() && file.revision()==committed,"invalid edit preserves durable revision");
    file.close(); check(file.open(path.string()).ok(),"working file reopened");
    if(file.document()) { check(layerProperties(file.document()->layers[0]).appearance==changed,"stack file persistence"); check(renderFrame(*file.document(),0).ok(),"reopened file renders"); }
    file.close(); std::filesystem::remove(path);
    return failures ? 1 : 0;
}
