#include <iiSharedCanvasCore.h>
#include <iiFileProviderNative.h>
#include <filesystem>
#include <chrono>
#include <iostream>
#include <zlib.h>

using namespace iiSharedCanvas;
namespace {
int failures = 0;
void expect(bool value, const char *message) {
    if (!value) { ++failures; std::cerr << message << '\n'; }
}
Document fixture() {
    Document d; d.extent = {2,2}; d.timeline.frameCount = 2;
    d.artboards = {{"a","A",{{0,0},{2,2}}},{"b","B",{{3,0},{2,2}}}};
    d.assets.emplace_back(RasterAsset{"red",makeRasterLayer(2,2,0xffff0000U)});
    d.assets.emplace_back(RasterAsset{"blue",makeRasterLayer(2,2,0xff0000ffU)});
    LayerProperties p; p.id="static"; p.artboardId="a";
    d.layers.emplace_back(StaticBitmapLayer{p,StaticSource{"red"}});
    p.id="dynamic"; p.artboardId="b";
    d.layers.emplace_back(DynamicBitmapLayer{p,KeyframedSource{{0,1}}});
    d.frames={{0,{{"dynamic","red"}}},{1,{{"dynamic","blue"}}}};
    return d;
}
PublicationMetadata metadata() {
    PublicationMetadata m; m.title="제목 Ω"; m.description=std::string(80*1024,'x');
    m.coverArtboardId="b";
    m.canvases={{"b","캡션 B","대체 텍스트"},{"a","Caption A","Alt A"}};
    m.settings={{"license",std::string("All rights reserved")},{"comments",std::string("Everyone")},
                {"adult",false},{"reactionCounts",true},{"scheduledAt",std::string("2026-10-23T10:00:00+09:00")}};
    return m;
}
void checksum(std::vector<std::uint8_t> &bytes) {
    const auto value=::crc32(0,bytes.data()+IiscHeaderSize,uInt(bytes.size()-IiscHeaderSize));
    for (unsigned i=0;i<4;++i) bytes[24+i]=std::uint8_t(value>>(8*i));
}
}
int main() {
    auto d=fixture(); const auto original=encodeIisc(d).bytes;
    const auto m=metadata(); DocumentEditor editor(d);
    expect(editor.setPublicationMetadata(m).changed,"publication metadata must be a native document edit");
    expect(d.publicationMetadata==m && validate(d).ok(),"public metadata model validates");
    expect(!editor.setPublicationMetadata(m).changed,"identical metadata is a no-op");
    const auto encoded=encodeIisc(d); const auto decoded=decodeIisc(encoded.bytes);
    expect(encoded.ok() && decoded.ok() && decoded.document.publicationMetadata==m,
           "all text, boolean settings, cover and ordered captions round trip inside iisc");
    expect(decoded.ok() && encodeIisc(decoded.document).bytes==encoded.bytes,"metadata encoding is canonical");
    auto typed=fixture(); PublicationMetadata typedMetadata;
    typedMetadata.settings={{"a",false},{"b",true}}; typed.publicationMetadata=typedMetadata;
    const auto typedBytes=encodeIisc(typed).bytes;
    auto malformed=typedBytes; malformed[malformed.size()-2]=2; checksum(malformed);
    expect(decodeIisc(malformed).error.code==IiscErrorCode::InvalidData,"unknown setting tags reject after a valid checksum");
    malformed=typedBytes; malformed.back()=2; checksum(malformed);
    expect(decodeIisc(malformed).error.code==IiscErrorCode::InvalidData,"invalid publication booleans reject");
    malformed=typedBytes; malformed[malformed.size()-3]='a'; checksum(malformed);
    expect(decodeIisc(malformed).error.code==IiscErrorCode::InvalidData,"duplicate decoded setting names reject");
    expect(decoded.ok() && renderArtboard(decoded.document,1,"b").pixels.pixels.front()==0xff0000ffU,
           "publication metadata preserves dynamic canvas content");
    expect(decoded.ok() && renderArtboard(decoded.document,0,"a").pixels.pixels.front()==0xffff0000U,
           "publication metadata preserves static canvas content");
    SerializationLimits limits; limits.maximumMetadataStringBytes=1024;
    expect(encodeIisc(d,limits).error.code==IiscErrorCode::LimitExceeded
        && decodeIisc(encoded.bytes,limits).error.code==IiscErrorCode::LimitExceeded,"metadata string budgets apply on both paths");
    limits={}; limits.maximumMetadataEntries=1;
    expect(!encodeIisc(d,limits).ok() && !decodeIisc(encoded.bytes,limits).ok(),"metadata entry budgets apply on both paths");
    auto invalid=m; invalid.coverArtboardId="missing";
    const auto prior=encodeIisc(d).bytes; const auto revision=editor.revision();
    expect(!editor.setPublicationMetadata(invalid).ok() && editor.revision()==revision && encodeIisc(d).bytes==prior,
           "invalid reference edits reject atomically");
    invalid=m; invalid.canvases[1].artboardId="b";
    expect(!editor.setPublicationMetadata(invalid).ok(),"duplicate publication canvases reject");
    invalid=m; invalid.settings.emplace("",true);
    expect(!editor.setPublicationMetadata(invalid).ok(),"empty setting names reject");
    auto removed=d; DocumentEditor removeEditor(removed);
    expect(removeEditor.removeArtboard("b",ArtboardRemoval::DeleteLayers).changed
        && removed.publicationMetadata->coverArtboardId=="a"
        && removed.publicationMetadata->canvases.size()==1 && validate(removed).ok(),
        "removing an artboard reconciles publication references in the same edit");
    auto legacy=fixture(); legacy.formatVersion.minor=21;
    const auto old=encodeIisc(legacy); const auto oldRead=decodeIisc(old.bytes);
    expect(old.ok() && oldRead.ok() && !oldRead.document.publicationMetadata
        && encodeIisc(oldRead.document).bytes==old.bytes,"format 1.21 is readable without byte drift");
    legacy.publicationMetadata=m;
    expect(!encodeIisc(legacy).ok(),"older versions cannot silently omit metadata");
    legacy.publicationMetadata.reset(); DocumentEditor legacyEditor(legacy);
    expect(legacyEditor.setPublicationMetadata(m).changed && legacy.formatVersion.minor==CurrentFormatMinor,
           "metadata editor upgrades older document formats");
    auto single=fixture(); single.artboards.clear(); single.layers.clear(); single.frames.clear();
    auto root=m; root.coverArtboardId.clear(); root.canvases={{"","Root canvas","Root alt"}};
    DocumentEditor singleEditor(single);
    expect(singleEditor.setPublicationMetadata(root).changed && decodeIisc(encodeIisc(single).bytes).ok(),
           "documents without artboards can annotate the root canvas");
    expect(singleEditor.insertArtboard({"added","Added",{{0,0},{2,2}}}).changed
        && single.publicationMetadata->canvases.front().artboardId.empty(),
        "adding artboards preserves metadata for the document's root canvas");
    const auto directory=std::filesystem::path(IISHAREDCANVAS_TEST_OUTPUT_DIR)
        /("publication-metadata-"+std::to_string(std::chrono::steady_clock::now().time_since_epoch().count()));
    std::filesystem::create_directories(directory);
    struct Cleanup { std::filesystem::path path; ~Cleanup() { std::error_code e; std::filesystem::remove_all(path,e); } } cleanup{directory};
    const auto snapshot=directory/"source.iisc", copy=directory/"renamed.iisc", working=directory/"working.iisc";
    for (const auto &p:{snapshot,copy,working}) { std::error_code ec; std::filesystem::remove(p,ec); }
    const std::string bytes(encoded.bytes.begin(),encoded.bytes.end());
    iiFileProvider::native::File::create(snapshot,bytes);
    std::filesystem::copy_file(snapshot,copy);
    Document restored;
    expect(DocumentFile::readSnapshot(copy.string(),restored).ok() && restored.publicationMetadata==m,
           "copying only one iisc restores the complete publication");
    expect(!std::filesystem::exists(snapshot.string()+".iisacc.json"),"snapshot needs no metadata companion");
    DocumentFile owner; expect(owner.create(working.string(),fixture()).ok(),"working file opens");
    DocumentEditor bound(owner); expect(bound.setPublicationMetadata(m).changed,"file-bound metadata edit commits");
    expect(owner.lastWriteStatistics().recordsWritten<=3,"metadata edit does not rewrite media records");
    expect(DocumentFile::readSnapshot(working.string(),restored).ok() && restored.publicationMetadata==m,
           "committed working metadata is included in detached reads");
    owner.close();
    const auto workingCopy=directory/"working-copy.iisc"; std::filesystem::copy_file(working,workingCopy);
    expect(DocumentFile::readSnapshot(workingCopy.string(),restored).ok() && restored.publicationMetadata==m,
           "a closed working iisc can also travel without any companion");
    expect(owner.open(working.string()).ok() && owner.document()->publicationMetadata==m,
                          "metadata survives a working file close and reopen");
    DocumentEditor reopened(owner);
    expect(reopened.clearPublicationMetadata().changed && !owner.document()->publicationMetadata,"metadata can be cleared explicitly");
    owner.close();
    std::cout << "Publication metadata: " << (failures?"FAILED":"passed") << '\n';
    return failures?1:0;
}
