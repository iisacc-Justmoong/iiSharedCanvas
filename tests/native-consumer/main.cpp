#include <iiSharedCanvasNative.h>
#include <iiFileProviderNative.h>
#include <cstdlib>
#include <iostream>
#define CHECK(x)do{if(!(x)){std::cerr<<__LINE__<<": " #x "\n";std::exit(1);}}while(false)
int main(int argc,char** argv){CHECK(argc==2);using namespace iiSharedCanvas;
    iiFileProvider::native::TemporaryDirectory temporary{std::filesystem::path(argv[1])};
    auto path=(temporary.path()/"relocated.iisc").u8string();std::string filename(path.begin(),path.end());
    Document document;document.extent={16,16};document.assets.emplace_back(RasterAsset{"pixels",makeRasterLayer(16,16,0xffff0000)});
    document.layers.emplace_back(StaticBitmapLayer{{"layer","Layer"},StaticSource{"pixels"}});
    // Exercise the exported timeline contract using the installed native package.
    CHECK((staticLayerTimelineRange(document, document.layers.front()) == LayerFrameRange{0, 0}));
    CHECK((defaultDynamicLayerTimelineRange({30000, 1001}) == LayerFrameRange{0, 149}));
    auto *layer=findLayer(document,"layer");DocumentEditor memory(document);
    LayerChange rename{"layer",LayerChangeField::Name,std::string("Layer"),std::string("Ink")};
    {LayerPropertyGuard rollback(document,*layer,rename.field);CHECK(rename.apply(memory,true).changed);}
    CHECK(layerProperties(*layer).name=="Layer");
    {LayerPropertyGuard commit(document,*layer,rename.field);CHECK(rename.apply(memory,true).changed);commit.commit();}
    CHECK(layerProperties(*layer).name=="Ink"&&rename.apply(memory,false).changed);
    DocumentFile file;CHECK(file.create(filename,document).ok());const auto before=file.revision();
    CHECK(file.edit([](Document& draft){std::get<RasterAsset>(draft.assets[0]).pixels.pixels[0]=0xff0000ff;return true;}).ok());CHECK(file.revision()==before+1);
    const auto encoded=encodeIisc(*file.document());CHECK(encoded.ok()&&decodeIisc(encoded.bytes).ok());file.close();
    CHECK(file.open(filename).ok());auto frame=renderFrameTiles(*file.document(),0,{{{{0,0},{8,8}},{8,8}}});
    CHECK(frame.ok()&&frame.tiles.size()==1&&frame.tiles[0].pixels.pixels[0]==0xff0000ff);
    CHECK(std::get<RasterAsset>(file.document()->assets[0]).pixels.pixels[1]==0xffff0000);
    const auto whole=renderFrame(*file.document(),0);CHECK(whole.ok());
    const auto png=encodeBitmap(whole.pixels);CHECK(png.ok());const auto bitmap=decodeBitmap(png.bytes);
    CHECK(bitmap.ok()&&bitmap.asset.pixels.pixels==whole.pixels.pixels);
    const auto psd_path=(temporary.path()/"relocated.psd").u8string();std::string psd_name(psd_path.begin(),psd_path.end());
    CHECK(exportPsd(*file.document(),psd_name).ok());CHECK(importLayeredDocument(psd_name).ok());
    Document checkpoint=*file.document();
    std::get<RasterAsset>(checkpoint.assets[0]).pixels.pixels[1]=0xff887766;
    const auto author=checkpoint.authorship.dump();const auto snapshot=encodeIisc(checkpoint).bytes;
    CHECK(file.replaceSnapshot(checkpoint,file.revision()).changed && file.document()->authorship.dump()==author &&
          encodeIisc(*file.document()).bytes==snapshot);
    auto edited = checkpoint;
    std::get<RasterAsset>(edited.assets[0]).pixels.pixels[2]=0xff223399;
    const auto edit = encodeIiscEdit(checkpoint, edited);
    IiscEditImpact impact;
    CHECK(edit.ok() && describeIiscEdit(edit.bytes, impact).code==IiscErrorCode::None &&
          impact.pixels.size()==1);
    CHECK(applyIiscEdit(checkpoint, edit.bytes).code==IiscErrorCode::None);
    CHECK(file.publishEdit(checkpoint,file.revision()).changed &&
          encodeIisc(*file.document()).bytes==encodeIisc(edited).bytes);
    std::cout<<"RELOCATED_CANVAS_OK direct_file=true pixel_delta=true tiles=true qt=false\n";
}
