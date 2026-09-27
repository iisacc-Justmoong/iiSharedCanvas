#include <iiSharedCanvas.h>
#include <QTemporaryDir>
#include <algorithm>
#include <bit>
#include <iostream>
#include <limits>
using namespace iiSharedCanvas;
namespace {
int failures=0;
void expect(bool ok,const char *message) { if(!ok) { std::cerr<<message<<'\n'; ++failures; } }
IpAdapterAsset embedding(std::string id="embedding") {
    IpAdapterAsset a; a.id=std::move(id);
    a.descriptor={IpAdapterEmbeddingStage::ProjectedTokens,"clip-vit-h","encoder-sha","ip-adapter-plus","adapter-sha","sdxl-1.0","resize224-center-crop-normalize-v1"};
    a.conditional={2,3,{-2.5F,0.0F,1.25F,2.0F,-0.5F,0.125F}};
    a.unconditional=IpAdapterTensor{2,3,{0.25F,-0.25F,0.5F,0.75F,1.5F,-1.0F}};
    return a;
}
Document fixture() {
    Document d; d.extent={2,2}; d.timeline.frameCount=3; DocumentEditor e(d);
    expect(e.insertIpAdapterAsset(embedding()).changed,"insert embeddings");
    IpAdapterLayer l; l.properties={"layer","IP-Adapter"}; l.source=StaticSource{"embedding"};
    l.control.modelId="ip-adapter-plus"; l.control.modelRevision="adapter-sha"; l.control.conditioningScale=0.6;
    expect(e.insertIpAdapterLayer(l).changed,"insert embedding layer"); return d;
}
void verify(const Document &d) {
    expect(validate(d).ok(),"valid embedding document");
    expect(layerRole(d.layers[0])==LayerRole::ControlNet && controlNetKind(d.layers[0])==ControlNetKind::IpAdapter,"conditioning role");
    expect(layerRepresentation(d.layers[0])==LayerRepresentation::Embedding && !isLineControlNetLayer(d.layers[0]),"embeddings are nonspatial");
    const auto r=exportIpAdapterEmbeddings(d,"layer",0);
    expect(r.ok() && r.conditional==embedding().conditional && r.unconditional==embedding().unconditional,"exact signed float values and independent CFG branch");
    expect(r.descriptor==embedding().descriptor && r.control==findIpAdapterLayer(d,"layer")->control,"export provenance and settings");
    const auto preview=renderFrameLayerTiles(d,0,0,{{canvasRegion(d),d.extent}});
    expect(preview.ok() && !preview.spatial && preview.tiles.empty(),"explicit nonspatial preview metadata without fabricated pixels");
    const auto art=renderFrame(d,0); expect(art.ok() && art.pixels.pixels==std::vector<std::uint32_t>(4,0),"embeddings excluded from composition");
}
void repairCrc(std::vector<std::uint8_t> &bytes) {
    std::uint32_t crc=0xffffffffU;
    for(std::size_t i=IiscHeaderSize;i<bytes.size();++i) { crc^=bytes[i]; for(int j=0;j<8;++j) crc=(crc>>1U)^(0xedb88320U & (0U-(crc&1U))); }
    crc^=0xffffffffU; for(unsigned i=0;i<4;++i) bytes[24+i]=static_cast<std::uint8_t>(crc>>(8*i));
}
}
int main() {
    auto d=fixture(); verify(d); DocumentEditor e(d);
    expect(layerKind(d.layers[0])==LayerKind::StaticEmbedding,"static embedding kind");
    const auto original=encodeIisc(d); expect(original.ok(),"encode embeddings");
    auto decoded=decodeIisc(original.bytes); expect(decoded.ok(),"decode embeddings"); if(decoded.ok()) verify(decoded.document);
    const auto revision=e.revision();
    for(int i=0;i<10;++i) {
        auto bad=embedding();
        switch(i) {
        case 0: bad.conditional.values.pop_back(); break;
        case 1: bad.conditional.tokenCount=0; break;
        case 2: bad.conditional.values[0]=std::numeric_limits<float>::infinity(); break;
        case 3: bad.unconditional->values[0]=std::numeric_limits<float>::quiet_NaN(); break;
        case 4: bad.unconditional->tokenCount=3; bad.unconditional->channelCount=2; break;
        case 5: bad.descriptor.encoderRevision.clear(); break;
        case 6: bad.descriptor.stage=static_cast<IpAdapterEmbeddingStage>(255); break;
        case 7: bad.descriptor.stage=IpAdapterEmbeddingStage::EncoderPooled; break;
        case 8: bad.descriptor.adapterRevision="other"; break;
        case 9: bad.conditional.tokenCount=std::numeric_limits<std::uint32_t>::max(); bad.conditional.channelCount=std::numeric_limits<std::uint32_t>::max(); break;
        }
        expect(!e.replaceIpAdapterAsset("embedding",bad).ok() && e.revision()==revision && encodeIisc(d).bytes==original.bytes,"invalid tensor or provenance rolls back");
    }
    expect(!e.setIpAdapterValue("embedding",IpAdapterBranch::Conditional,2,0,1).ok(),"token bounds");
    expect(!e.setIpAdapterValue("embedding",static_cast<IpAdapterBranch>(255),0,0,1).ok(),"unknown branch fails closed");
    expect(!e.insertStaticLayer({"wrong"},LayerRepresentation::Embedding,"embedding").ok(),"artwork factory cannot create embedding without adapter contract");
    auto signedZero=d; DocumentEditor ze(signedZero);
    expect(ze.setIpAdapterValue("embedding",IpAdapterBranch::Conditional,0,1,-0.0F).changed,"signed zero edit changes stored float bits");
    auto zeroLoaded=decodeIisc(encodeIisc(signedZero).bytes);
    expect(zeroLoaded.ok() && std::bit_cast<std::uint32_t>(findIpAdapterAsset(zeroLoaded.document,"embedding")->conditional.values[1])==0x80000000U,"signed zero bits survive persistence");
    auto mixed=d; DocumentEditor me(mixed);
    expect(me.insertRasterAsset("art",makeRasterLayer(2,2,0xff123456)).changed && me.insertStaticLayer({"art-layer"},LayerRepresentation::Bitmap,"art").changed,"mixed artwork fixture");
    const auto composed=renderFrame(mixed,0); expect(composed.ok() && composed.pixels.pixels==std::vector<std::uint32_t>(4,0xff123456),"nonspatial metadata preserves ordinary artwork composition");
    auto wrong=d; findIpAdapterLayer(wrong,"layer")->control.modelId="different"; expect(!validate(wrong).ok(),"reject model mismatch");
    wrong=d; wrong.assets.push_back(RasterAsset{"image",makeRasterLayer(2,2)}); findIpAdapterLayer(wrong,"layer")->source=StaticSource{"image"}; expect(!validate(wrong).ok(),"RGB cannot substitute embedding tensor");
    auto noCfg=d; findIpAdapterAsset(noCfg,"embedding")->unconditional.reset();
    expect(validate(noCfg).ok() && !exportIpAdapterEmbeddings(noCfg,"layer",0).ok(),"CFG export requires explicit negative branch");
    expect(exportIpAdapterEmbeddings(noCfg,"layer",0,{false,6}).ok(),"explicit non-CFG export supported");
    const auto noCfgLoaded=decodeIisc(encodeIisc(noCfg).bytes);
    expect(noCfgLoaded.ok() && !findIpAdapterAsset(noCfgLoaded.document,"embedding")->unconditional,"absent branch survives native persistence");
    DocumentEditor noCfgEditor(noCfg); expect(!noCfgEditor.setIpAdapterValue("embedding",IpAdapterBranch::Unconditional,0,0,1).ok(),"absent branch is not synthesized");
    for(auto stage:{IpAdapterEmbeddingStage::EncoderPooled,IpAdapterEmbeddingStage::EncoderHiddenStates,IpAdapterEmbeddingStage::ProjectedTokens}) {
        auto a=embedding(); a.descriptor.stage=stage;
        if(stage==IpAdapterEmbeddingStage::EncoderPooled) { a.conditional.tokenCount=1; a.conditional.values.resize(3); a.unconditional->tokenCount=1; a.unconditional->values.resize(3); }
        auto v=d; DocumentEditor ve(v); expect(ve.replaceIpAdapterAsset("embedding",a).ok(),"all explicit embedding stages supported");
        auto loaded=decodeIisc(encodeIisc(v).bytes); expect(loaded.ok() && *findIpAdapterAsset(loaded.document,"embedding")==a,"all stages persist exactly");
    }
    auto hidden=d; auto *l=findIpAdapterLayer(hidden,"layer"); l->properties.visible=false; l->properties.opacity=0; l->properties.transform.translationX=100;
    expect(exportIpAdapterEmbeddings(hidden,"layer",0).conditional==embedding().conditional,"display state cannot alter embeddings");
    l->properties.frameRange=LayerFrameRange{1,2}; expect(!exportIpAdapterEmbeddings(hidden,"layer",0).ok() && exportIpAdapterEmbeddings(hidden,"layer",1).ok(),"frame range honored");
    SerializationLimits limits; limits.maximumIpAdapterValues=11;
    expect(!encodeIisc(d,limits).ok() && !decodeIisc(original.bytes,limits).ok(),"both branches count against serialization budget");
    expect(!exportIpAdapterEmbeddings(d,"layer",0,{true,11}).ok(),"output budget covers both branches");
    limits={}; limits.maximumStringBytes=20; expect(!encodeIisc(d,limits).ok() && !decodeIisc(original.bytes,limits).ok(),"descriptor string budgets");
    auto old=d; old.formatVersion.minor=16; expect(!encodeIisc(old).ok(),"old format rejects embeddings");
    auto bytes=original.bytes; bytes[10]=16; expect(!decodeIisc(bytes).ok(),"new tags rejected under old header");
    const std::vector<std::uint8_t> marker{14,9,0,0,0,'e','m','b','e','d','d','i','n','g'};
    const auto found=std::search(original.bytes.begin(),original.bytes.end(),marker.begin(),marker.end());
    expect(found!=original.bytes.end(),"wire asset marker");
    if(found!=original.bytes.end()) {
        auto offset=static_cast<std::size_t>(found-original.bytes.begin()); bytes=original.bytes; bytes[offset+marker.size()]=255; repairCrc(bytes); expect(!decodeIisc(bytes).ok(),"unknown stage fails closed with correct CRC");
    }
    const std::vector<std::uint8_t> tensor{2,0,0,0,3,0,0,0,6,0,0,0,0,0,0,0,0,0,32,192};
    const auto tf=std::search(original.bytes.begin(),original.bytes.end(),tensor.begin(),tensor.end()); expect(tf!=original.bytes.end(),"float32 tensor wire record");
    if(tf!=original.bytes.end()) {
        const auto at=static_cast<std::size_t>(tf-original.bytes.begin());
        bytes=original.bytes; bytes[at+16+6*4]=2; repairCrc(bytes); expect(!decodeIisc(bytes).ok(),"unknown branch presence flag rejected");
        bytes=original.bytes; bytes[at+8]=7; repairCrc(bytes); expect(!decodeIisc(bytes).ok(),"tensor count mismatch rejected before allocation");
        bytes=original.bytes; bytes[at+16]=0; bytes[at+17]=0; bytes[at+18]=128; bytes[at+19]=127; repairCrc(bytes); expect(!decodeIisc(bytes).ok(),"wire infinity rejected");
    }
    BitmapEditor brush; expect(!brush.bind(d,"embedding"),"bitmap editing cannot corrupt embeddings");
    auto next=embedding("next"); next.conditional.values[0]=9.0F;
    expect(e.insertIpAdapterAsset(next).changed,"next embedding state");
    expect(e.setKeyframedSource("layer",{{0,"embedding"},{2,"next"}}).changed,"dynamic source");
    expect(layerKind(d.layers[0])==LayerKind::DynamicEmbedding && exportIpAdapterEmbeddings(d,"layer",1).conditional.values[0]==-2.5F && exportIpAdapterEmbeddings(d,"layer",2).conditional.values[0]==9,"hold-only timeline sampling");
    auto incompatible=next; incompatible.descriptor.encoderId="other"; expect(!e.replaceIpAdapterAsset("next",incompatible).ok(),"dynamic frames cannot mix embedding contracts");
    limits={}; limits.maximumIpAdapterValues=23; const auto multi=encodeIisc(d); expect(!encodeIisc(d,limits).ok() && !decodeIisc(multi.bytes,limits).ok(),"aggregate asset budget");
    expect(!exportIpAdapterEmbeddings(d,"layer",3).ok(),"timeline bounds");
    auto c=findIpAdapterLayer(d,"layer")->control; c.enabled=false; expect(e.setControlNetSettings("layer",c).changed && !exportIpAdapterEmbeddings(d,"layer",0).ok(),"disabled conditioning");
    c.enabled=true; expect(e.setControlNetSettings("layer",c).changed,"reenable"); c.guidanceEnd=0; expect(!e.setControlNetSettings("layer",c).ok(),"invalid schedule");
    Document legacy; legacy.extent={1,1}; legacy.formatVersion.minor=16; DocumentEditor le(legacy); expect(le.insertIpAdapterAsset(embedding()).changed && legacy.formatVersion.minor==17,"legacy upgrade");
    QTemporaryDir dir(QStringLiteral(IISHAREDCANVAS_TEST_OUTPUT_DIR "/ip-adapter-XXXXXX")); expect(dir.isValid(),"test directory");
    if(dir.isValid()) {
        auto path=dir.filePath("embedding.iisc").toStdString(); DocumentFile file; expect(file.create(path,d).ok(),"working file create"); DocumentEditor bound(file);
        expect(bound.setIpAdapterValue("embedding",IpAdapterBranch::Conditional,0,1,0.375F).changed,"persist conditional edit");
        expect(bound.setIpAdapterValue("embedding",IpAdapterBranch::Unconditional,1,2,-1.125F).changed,"persist independent negative branch");
        auto saved=encodeIisc(*file.document()).bytes; const auto rev=bound.revision(); expect(!bound.setIpAdapterValue("embedding",IpAdapterBranch::Conditional,0,0,std::numeric_limits<float>::quiet_NaN()).ok() && bound.revision()==rev && encodeIisc(*file.document()).bytes==saved,"file-bound invalid edit atomic");
        c=findIpAdapterLayer(*file.document(),"layer")->control; c.conditioningScale=0.25; expect(bound.setControlNetSettings("layer",c).changed && file.lastWriteStatistics().recordsWritten<=2,"settings edit reuses tensors");
        DocumentFile reopened; expect(reopened.open(path).ok(),"working file reopen");
        if(reopened.document()) { const auto *a=findIpAdapterAsset(*reopened.document(),"embedding"); expect(a && a->conditional.values[1]==0.375F && a->unconditional->values[5]==-1.125F,"float32 file roundtrip"); expect(exportIpAdapterEmbeddings(*reopened.document(),"layer",2).conditional.values[0]==9,"dynamic file roundtrip"); }
        expect(exportPsd(d,dir.filePath("embedding.psd").toStdString()).code==MediaIoCode::UnsupportedFeature,"PSD rejects embedding loss");
        auto orphan=d; orphan.layers.clear(); orphan.frames.clear(); expect(exportTimelineInterchange(orphan,dir.filePath("timeline").toStdString()).code==MediaIoCode::UnsupportedFeature,"orphan embedding interchange rejects data loss");
    }
    return failures?1:0;
}
