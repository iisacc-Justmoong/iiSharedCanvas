#include <iiSharedCanvasNative.h>
#include <iiFileProviderNative.h>
#include <zlib.h>
#include <algorithm>
#include <iostream>
#include <stdexcept>

using namespace iiSharedCanvas;
namespace {
void check(bool result,const char* message){if(!result)throw std::runtime_error(message);}
std::string text(const std::vector<std::uint8_t>& bytes){return {reinterpret_cast<const char*>(bytes.data()),bytes.size()};}
std::vector<std::uint8_t> zip(const std::vector<std::pair<std::string,std::string>>& entries){
    std::vector<std::uint8_t> bytes;const auto u16=[&](unsigned v){bytes.push_back(std::uint8_t(v));bytes.push_back(std::uint8_t(v>>8));};const auto u32=[&](std::uint32_t v){u16(v);u16(v>>16);};const auto append=[&](std::string_view s){bytes.insert(bytes.end(),s.begin(),s.end());};std::vector<std::uint32_t> offsets;
    for(const auto& [name,value]:entries){offsets.push_back(std::uint32_t(bytes.size()));u32(0x04034b50);u16(20);u16(0x800);u16(0);u16(0);u16(0);u32(crc32(0,reinterpret_cast<const Bytef*>(value.data()),uInt(value.size())));u32(std::uint32_t(value.size()));u32(std::uint32_t(value.size()));u16(unsigned(name.size()));u16(0);append(name);append(value);}
    const auto central=std::uint32_t(bytes.size());std::size_t index=0;
    for(const auto& [name,value]:entries){u32(0x02014b50);u16(20);u16(20);u16(0x800);u16(0);u16(0);u16(0);u32(crc32(0,reinterpret_cast<const Bytef*>(value.data()),uInt(value.size())));u32(std::uint32_t(value.size()));u32(std::uint32_t(value.size()));u16(unsigned(name.size()));u16(0);u16(0);u16(0);u16(0);u32(0);u32(offsets[index++]);append(name);}
    const auto size=std::uint32_t(bytes.size())-central;u32(0x06054b50);u16(0);u16(0);u16(unsigned(entries.size()));u16(unsigned(entries.size()));u32(size);u32(central);u16(0);return bytes;
}
}
int main(int argc,char** argv){try{check(argc==2,"A workspace test directory is required");iiFileProvider::native::TemporaryDirectory temporary{std::filesystem::path(argv[1])};
    Document document;document.extent={4,3};auto pixels=makeRasterLayer(4,3,0);pixels.pixels[0]=0x7f123456;pixels.pixels[1]=0xffabcdef;
    document.assets.emplace_back(RasterAsset{"paint",pixels});document.layers.emplace_back(StaticBitmapLayer{{"layer","한글 🙂"},StaticSource{"paint"}});
    for(auto format:{"png","bmp","tga","webp","tiff"}){BitmapExportOptions options;options.format=format;auto encoded=encodeBitmap(pixels,options);check(encoded.ok(),encoded.result.message.c_str());auto decoded=decodeBitmap(encoded.bytes);check(decoded.ok(),decoded.result.message.c_str());check(decoded.asset.pixels.pixels==pixels.pixels,"Native bitmap changed straight alpha or channel order");}
    auto psd=encodePsd(document);check(psd.ok(),psd.result.message.c_str());auto imported=decodeLayeredDocument(psd.bytes);check(imported.ok(),imported.result.message.c_str());check(imported.document.layers.size()==1&&layerProperties(imported.document.layers[0]).name=="한글 🙂","PSD lost Unicode or layer count");check(renderFrame(imported.document,0).pixels.pixels==pixels.pixels,"PSD composite differs from native pixels");
    for(std::size_t cut:{std::size_t(0),std::size_t(4),psd.bytes.size()/2,psd.bytes.size()-1})check(!decodeLayeredDocument(std::span(psd.bytes).first(cut)).ok(),"Truncated PSD accepted");
    const auto output=temporary.path()/"layered.psd";check(exportPsd(document,output.string()).ok(),"PSD exclusive publication failed");check(exportPsd(document,output.string()).code==MediaIoCode::AlreadyExists,"PSD replaced an existing file without overwrite");
    check(importLayeredDocument(output.string()).ok(),"Provider-owned native PSD read failed");
    VectorPath path;path.fill=SolidPaint{0x80ff0000};path.stroke=StrokeStyle{{0xff0022ff},.5};path.commands={MoveTo{{.1,.2}},QuadraticTo{{1,3},{3,1}},CubicTo{{3,0},{1,0},{.1,.2}},ClosePath{}};
    document.assets.emplace_back(VectorAsset{"vector",{4,3},{path}});document.layers.emplace_back(StaticVectorLayer{{"vector-layer","Vector"},StaticSource{"vector"}});
    auto vector=encodePsd(document);check(vector.ok(),vector.result.message.c_str());auto payload=text(vector.bytes);check(payload.find("SoLd")!=payload.npos&&payload.find("lnk2")!=payload.npos&&payload.find("%PDF-1.4")!=payload.npos&&payload.find("/ExtGState")!=payload.npos&&payload.find(" c\n")!=payload.npos,"Vector Smart Object or native PDF path data missing");
    iiFileProvider::native::File::create(temporary.path()/"vector.psd",payload);
    auto png=encodeBitmap(pixels);check(png.ok(),"Cannot build an independent ORA fixture");const std::string xml="<image version=\"0.0.6\" w=\"4\" h=\"3\"><stack><layer name=\"한글\" src=\"data/layer.png\"/></stack></image>";
    auto ora=zip({{"mimetype","image/openraster"},{"stack.xml",xml},{"data/layer.png",text(png.bytes)}});auto decoded=decodeLayeredDocument(ora);check(decoded.ok(),decoded.result.message.c_str());check(renderFrame(decoded.document,0).pixels.pixels==pixels.pixels,"ORA layer pixels changed");
    auto unsafe=zip({{"mimetype","image/openraster"},{"stack.xml",xml},{"../escape.png",text(png.bytes)}});check(!decodeLayeredDocument(unsafe).ok(),"Archive path traversal accepted");
    ora[45]^=1;check(!decodeLayeredDocument(ora).ok(),"Corrupted archive passed its CRC");
    auto dtd=zip({{"mimetype","image/openraster"},{"stack.xml","<!DOCTYPE image [<!ENTITY x SYSTEM 'file:///etc/passwd'>]>"+xml},{"data/layer.png",text(png.bytes)}});check(!decodeLayeredDocument(dtd).ok(),"DTD accepted");
    std::cout<<"NATIVE_MEDIA_OK rgba=true layered_psd=true unicode=true vector_pdf=true ora=true bounds=true\n";
    return 0;
}catch(const std::exception& e){std::cerr<<e.what()<<'\n';return 1;}}
