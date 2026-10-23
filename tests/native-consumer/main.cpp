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
    std::cout<<"RELOCATED_CANVAS_OK direct_file=true pixel_delta=true tiles=true qt=false\n";
}
