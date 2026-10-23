#include <iiSharedCanvasNative.h>
#include <iiFileProviderNative.h>
#include <cstdlib>
#include <iostream>

#define CHECK(x) do { if(!(x)) {std::cerr<<__LINE__<<": " #x "\n";std::exit(1);} } while(false)
using namespace iiSharedCanvas;
int main(int argc,char** argv) {
    CHECK(argc==2);
    iiFileProvider::native::TemporaryDirectory temporary{std::filesystem::path(argv[1])};
    Document document; document.extent={32,24};
    CHECK(validate(document).ok());
    Document chunked=document;chunked.canvasMode=CanvasMode::Infinite;
    DocumentEditor draft_editor(chunked);
    CHECK(draft_editor.insertChunkedRasterAsset({"chunks",{}}).ok());
    const auto asset_count=chunked.assets.size();
    CHECK(!draft_editor.insertChunkedRasterAsset({"chunks",{}}).ok());
    CHECK(!draft_editor.insertChunkedRasterAsset({"invalid",{{0,0,makeRasterLayer(1,1)}}}).ok());
    CHECK(chunked.assets.size()==asset_count);
    CHECK(draft_editor.insertLayer(StaticBitmapLayer{{"chunk-layer","Chunks"},StaticSource{"chunks"}}).ok());
    const auto side=chunked.infiniteCanvas.chunkSize;auto negative_pixels=makeRasterLayer(side,side,0);
    negative_pixels.pixels[std::size_t(side-10)*side+side-10]=0xff1133ff;
    std::get<ChunkedRasterAsset>(chunked.assets.front()).chunks.push_back({-1,-1,std::move(negative_pixels)});
    const auto persisted=encodeIisc(chunked);CHECK(persisted.ok());
    const auto outside=renderFrameRegion(chunked,0,{{-16,-16},{32,32}},{32,32});
    CHECK(outside.ok()&&outside.pixels.pixels[6*32+6]==0xff1133ff);
    CHECK(encodeIisc(chunked).bytes==persisted.bytes&&chunked.extent==document.extent);
    CHECK(!renderFrameRegion(document,0,{{-16,-16},{32,32}},{32,32}).ok());
    CHECK(!renderFrameRegion(chunked,0,{{INT32_MAX-4,0},{16,16}},{16,16}).ok());
    auto encoded=encodeIisc(document); CHECK(encoded.ok());
    auto decoded=decodeIisc(encoded.bytes); CHECK(decoded.ok());
    CHECK(decoded.document.extent==document.extent);
    auto bad=encoded.bytes; bad.resize(bad.size()/2); CHECK(!decodeIisc(bad).ok());
    auto rendered=renderFrame(document,0); CHECK(rendered.ok());
    const auto path_bytes=(temporary.path()/std::filesystem::path(u8"네이티브.iisc")).u8string();
    const std::string path(path_bytes.begin(),path_bytes.end());
    DocumentFile file; CHECK(file.create(path,document).ok());
    DocumentEditor editor(file); const auto revision=file.revision();
    CHECK(editor.setCanvasExtent({64,48}).ok()); CHECK(file.revision()==revision+1);
    const auto unchanged=file.revision(); CHECK(editor.setCanvasExtent({64,48}).ok()); CHECK(file.revision()==unchanged);
    CHECK(!file.edit([](Document& value){value.extent.width=-1;return true;}).ok());
    CHECK(file.revision()==unchanged && file.document()->extent.width==64);
    CHECK(!file.edit([](Document&){throw std::runtime_error("discard");return true;}).ok());
    Document snapshot; const auto read_snapshot=DocumentFile::readSnapshot(path,snapshot);
    if(!read_snapshot.ok())std::cerr<<"snapshot error "<<int(read_snapshot.code)<<": "<<read_snapshot.message<<'\n';
    CHECK(read_snapshot.ok());
    CHECK(snapshot.extent==file.document()->extent); CHECK(file.revision()==unchanged);
    std::uint64_t read_revision=0; CHECK(DocumentFile::readRevision(path,read_revision).ok()); CHECK(read_revision==unchanged);
    file.close(); CHECK(file.open(path).ok()); CHECK(file.document()->extent.width==64);
    auto author=iiFileProvider::native::FileAuthor::fromIisaccAccount({{"sub","native-author"},{"email","native@example.com"},{"displayName","Native"}},"https://iisacc.com");
    CHECK(author); DocumentEditor reopened(file); CHECK(reopened.setFileAuthor(*author).ok());
    file.close(); CHECK(file.open(path).ok()); CHECK(file.document()->authorship.firstEditor().has_value());
    std::cout<<"NATIVE_CANVAS_OK serialization=true direct_file=true rollback=true readonly=true authorship=true\n";
}
