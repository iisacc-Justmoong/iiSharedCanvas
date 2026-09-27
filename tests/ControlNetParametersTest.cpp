#include <iiSharedCanvas.h>
#include <QTemporaryDir>
#include <iostream>
#include <limits>
#include <utility>
#include <type_traits>
using namespace iiSharedCanvas;
namespace {
int failures=0;
void expect(bool ok,const char *message) { if(!ok) { std::cerr<<message<<'\n'; ++failures; } }
Document fixture() {
    Document d; d.extent={2,2}; d.timeline.frameCount=3;
    auto add=[&](Asset a,Layer l) { const auto id=assetId(a); layerProperties(l)={id,id}; layerSource(l)=StaticSource{id}; d.assets.push_back(std::move(a)); d.layers.push_back(std::move(l)); };
    SemanticSegmentLayer semantic; semantic.segmentation.taxonomy.id="classes";
    SemanticClass c; c.id=1; c.key="person"; c.name="Person"; c.controlColor=0xff123456;
    semantic.segmentation.taxonomy.classes={c}; SemanticRegion region; region.id=1; region.classId=1; region.maskColor=0xffff0000; semantic.segmentation.regions={region};
    add(RasterAsset{"semantic",makeRasterLayer(2,2,0xffff0000)},semantic);
    add(PoseAsset{"pose",{2,2},{makeNeutralPosePerson("person")}},PoseLayer{});
    add(DepthAsset{"depth",{2,2},{0,0.2,0.5,1}},DepthLayer{});
    add(LineArtAsset{"line",{2,2},{0,0.2,0.5,1}},LineArtLayer{});
    add(CannyAsset{"canny",{2,2},{0,1,0,1}},CannyLayer{});
    add(ScribbleAsset{"scribble",{2,2},{1,0,1,0}},ScribbleLayer{});
    add(MlsdAsset{"mlsd",{2,2},{{"segment",0,0,1,1,1,true}}},MlsdLayer{});
    add(NormalMapAsset{"normal",{2,2},std::vector<NormalMapSample>(4)},NormalMapLayer{});
    add(ShuffleAsset{"shuffle",{2,2},std::vector<ShuffleColor>(4)},ShuffleLayer{});
    add(TileAsset{"tile",{2,2},std::vector<TileColor>(4)},TileLayer{});
    add(ReferenceAsset{"reference",{2,2},std::vector<ReferenceColor>(4)},ReferenceLayer{});
    IpAdapterAsset ip; ip.id="ip"; ip.descriptor={IpAdapterEmbeddingStage::ProjectedTokens,"encoder","e1","adapter","a1","sdxl","crop224"}; ip.conditional={1,2,{1,-1}}; ip.unconditional=IpAdapterTensor{1,2,{0.25F,0.5F}};
    IpAdapterLayer il; il.control.modelId="adapter"; il.control.modelRevision="a1"; add(ip,il);
    expect(validate(d).ok(),"all 12 fixture types valid"); return d;
}
void modify(ControlNetParameters &p) {
    auto *c=controlNetSettings(p.layer); expect(c!=nullptr,"mutable common settings");
    c->conditioningScale=0.75; c->guidanceStart=0.2; c->guidanceEnd=0.8;
    layerProperties(p.layer).name="Edited";
    std::visit([&](auto &a) {
        using T=std::decay_t<decltype(a)>;
        if constexpr(std::is_same_v<T,RasterAsset>) {
            auto &s=std::get<SemanticSegmentLayer>(p.layer).segmentation;
            s.taxonomy.version="v2"; s.taxonomy.classes[0].description="Detailed object"; s.regions[0].maskColor=0xff00ff00; s.regions[0].attributes={{"material","cotton"}};
            a.pixels.pixels.assign(4,0xff00ff00);
        } else if constexpr(std::is_same_v<T,PoseAsset>) {
            a.people[0].name="Actor"; a.people[0].face.leftEye.upperLid[0].confidence=0.25;
            a.people[0].expressions.push_back({"smile","Smile",0.5,{{PoseGroup::MouthOuterUpper,0,0.01,0,0}}});
        } else if constexpr(std::is_same_v<T,DepthAsset>) a.values[1]=0.8;
        else if constexpr(std::is_same_v<T,LineArtAsset>) a.coverage[1]=0.8;
        else if constexpr(std::is_same_v<T,CannyAsset> || std::is_same_v<T,ScribbleAsset>) a.mask[0]=1-a.mask[0];
        else if constexpr(std::is_same_v<T,MlsdAsset>) { a.segments[0].confidence=0.5; a.segments[0].x2=0.75; }
        else if constexpr(std::is_same_v<T,NormalMapAsset>) a.samples[0]={1,0,0,true};
        else if constexpr(std::is_same_v<T,ShuffleAsset> || std::is_same_v<T,TileAsset> || std::is_same_v<T,ReferenceAsset>) {
            a.colors[0]={12,34,56};
            if constexpr(std::is_same_v<T,ReferenceAsset>) std::get<ReferenceLayer>(p.layer).reference={ReferenceMode::AdaIN,0.7};
        } else if constexpr(std::is_same_v<T,IpAdapterAsset>) {
            a.descriptor.adapterRevision="a2"; c->modelRevision="a2"; a.conditional.values[0]=2.5F; a.unconditional->values[1]=-0.5F;
        }
    },p.assets[0]);
}
void verifyDetails(const Document &d) {
    expect(findSemanticSegmentLayer(d,"semantic")->segmentation.regions[0].attributes[0].value=="cotton"
        && findRasterAsset(d,"semantic")->pixels.pixels[0]==0xff00ff00,"semantic metadata and mask both changed");
    expect(findPoseAsset(d,"pose")->people[0].expressions[0].weight==0.5
        && findPoseAsset(d,"pose")->people[0].face.leftEye.upperLid[0].confidence==0.25,"pose expression and facial anchor details changed");
    expect(findDepthAsset(d,"depth")->values[1]==0.8 && findLineArtAsset(d,"line")->coverage[1]==0.8,"depth and coverage details changed");
    expect(findCannyAsset(d,"canny")->mask[0]==1 && findScribbleAsset(d,"scribble")->mask[0]==0,"binary masks changed");
    expect(findMlsdAsset(d,"mlsd")->segments[0].x2==0.75 && findMlsdAsset(d,"mlsd")->segments[0].confidence==0.5,"MLSD endpoints and confidence changed");
    expect(findNormalMapAsset(d,"normal")->samples[0]==NormalMapSample{1,0,0,true},"normal vector details changed");
    expect(findShuffleAsset(d,"shuffle")->colors[0]==ShuffleColor{12,34,56} && findTileAsset(d,"tile")->colors[0]==TileColor{12,34,56},"RGB samples changed");
    expect(findReferenceAsset(d,"reference")->colors[0]==ReferenceColor{12,34,56} && findReferenceLayer(d,"reference")->reference==ReferenceSettings{ReferenceMode::AdaIN,0.7},"reference image and application settings changed");
    expect(findIpAdapterAsset(d,"ip")->conditional.values[0]==2.5F && findIpAdapterAsset(d,"ip")->unconditional->values[1]==-0.5F,"both embedding branches changed");
}

}
int main() {
    auto d=fixture(); DocumentEditor e(d); std::vector<std::string> ids;
    for(const auto &l:d.layers) ids.push_back(layerProperties(l).id);
    for(const auto &id:ids) {
        auto read=e.controlNetParameters(id); expect(read.ok() && read.parameters->assets.size()==1,"read typed layer and object"); if(!read.ok()) continue;
        const auto before=encodeIisc(d).bytes; auto parameters=*read.parameters; modify(parameters);
        expect(encodeIisc(d).bytes==before,"query returns detached editable values");
        const auto revision=e.revision(); expect(e.setControlNetParameters(id,std::move(parameters)).changed && e.revision()==revision+1,"each detailed edit is one transaction");
        const auto *settings=controlNetSettings(*findLayer(std::as_const(d),id)); expect(settings && settings->conditioningScale==0.75 && settings->guidanceStart==0.2,"common settings applied for every kind");
        expect(layerProperties(*findLayer(d,id)).name=="Edited" && encodeIisc(d).bytes!=before,"detail edit persisted in model");
        const auto modelId=settings->modelId;
        ControlNetSettingsPatch patch; patch.enabled=false; patch.conditioningScale=0;
        expect(e.patchControlNetSettings(id,patch).changed,"false and zero patch values applied");
        expect(controlNetSettings(*findLayer(d,id))->modelId==modelId,"patch retains unspecified model id");
        const auto stable=encodeIisc(d).bytes; const auto rev=e.revision(); ControlNetSettingsPatch bad; bad.guidanceEnd=0;
        expect(!e.patchControlNetSettings(id,bad).ok() && e.revision()==rev && encodeIisc(d).bytes==stable,"invalid patches roll back all 12 types");
        expect(e.patchControlNetSettings(id,{}).ok() && e.revision()==rev,"empty patch is unchanged");
        auto invalid=*e.controlNetParameters(id).parameters; controlNetSettings(invalid.layer)->conditioningScale=std::numeric_limits<double>::quiet_NaN();
        expect(!e.setControlNetParameters(id,std::move(invalid)).ok() && encodeIisc(d).bytes==stable && e.revision()==rev,"atomic rollback for all types");
    }
    verifyDetails(d);
    auto saved=encodeIisc(d); auto loaded=decodeIisc(saved.bytes); expect(saved.ok() && loaded.ok() && encodeIisc(loaded.document).bytes==saved.bytes,"all detailed fields snapshot roundtrip");
    expect(renderSemanticControlMap(fixture(),"semantic",0).ok(),"original semantic fixture remains usable");
    Layer artwork=BitmapLayer{}; expect(controlNetSettings(artwork)==nullptr && controlNetSettings(std::as_const(artwork))==nullptr,"artwork has no conditioning settings");
    expect(!getControlNetParameters(d,"missing").ok(),"missing layer rejected");
    DocumentEditor detached; expect(!detached.controlNetParameters("depth").ok() && !detached.patchControlNetSettings("depth",{}).ok(),"unbound API rejects");
    auto structural=*e.controlNetParameters("depth").parameters; const auto stable=encodeIisc(d).bytes;
    auto reject=[&](ControlNetParameters p){const auto rev=e.revision(); expect(!e.setControlNetParameters("depth",std::move(p)).ok() && e.revision()==rev && encodeIisc(d).bytes==stable,"structural changes rejected atomically");};
    auto bad=structural; bad.assets.clear(); reject(bad);
    bad=structural; bad.assets.push_back(bad.assets[0]); reject(bad);
    bad=structural; std::get<DepthAsset>(bad.assets[0]).id="foreign"; reject(bad);
    bad=structural; bad.assets[0]=LineArtAsset{"depth",{2,2},{0,0,0,0}}; reject(bad);
    bad=structural; layerProperties(bad.layer).id="renamed"; reject(bad);
    bad=structural; layerSource(bad.layer)=StaticSource{"other"}; reject(bad);
    bad=structural; bad.layer=BitmapLayer{{"depth"},StaticSource{"depth"}}; reject(bad);
    bad=structural; std::get<DepthAsset>(bad.assets[0]).values[0]=2; reject(bad);
    // Dynamic edits cover every unique referenced state, even disabled/outside display range.
    auto next=*findIpAdapterAsset(d,"ip"); next.id="ip-next"; expect(e.insertIpAdapterAsset(next).changed,"second IP state");
    expect(e.setKeyframedSource("ip",{{0,"ip"},{1,"ip-next"},{2,"ip"}}).changed,"dynamic IP source");
    expect(e.setLayerFrameRange("ip",LayerFrameRange{2,2}).changed,"limited display range");
    auto dynamic=e.controlNetParameters("ip"); expect(dynamic.ok() && dynamic.parameters->assets.size()==2,"all unique frame states regardless of range or enable");
    auto incomplete=*dynamic.parameters; std::get<IpAdapterAsset>(incomplete.assets[0]).descriptor.adapterRevision="a3"; controlNetSettings(incomplete.layer)->modelRevision="a3";
    const auto before=encodeIisc(d).bytes; expect(!e.setControlNetParameters("ip",incomplete).ok() && encodeIisc(d).bytes==before,"mismatched dynamic contracts roll back");
    for(auto &a:incomplete.assets) std::get<IpAdapterAsset>(a).descriptor.adapterRevision="a3";
    expect(e.setControlNetParameters("ip",incomplete).changed,"all frame contracts and layer binding change atomically");
    expect(findIpAdapterAsset(d,"ip-next")->descriptor.adapterRevision=="a3","second frame contract committed");
    // Shared semantic assets must remain valid for every owner.
    auto shared=fixture(); auto duplicate=shared.layers.front(); layerProperties(duplicate).id="shared"; shared.layers.push_back(duplicate); DocumentEditor se(shared);
    auto sp=*se.controlNetParameters("semantic").parameters; modify(sp); const auto sharedBytes=encodeIisc(shared).bytes;
    expect(!se.setControlNetParameters("semantic",sp).ok() && encodeIisc(shared).bytes==sharedBytes,"shared asset validation prevents breaking another layer");
    QTemporaryDir dir(QStringLiteral(IISHAREDCANVAS_TEST_OUTPUT_DIR "/control-parameters-XXXXXX")); expect(dir.isValid(),"test directory");
    if(dir.isValid()) {
        auto original=fixture(); DocumentFile file; auto path=dir.filePath("parameters.iisc").toStdString(); expect(file.create(path,original).ok(),"create working file"); DocumentEditor bound(file);
        for(const auto &id:ids) { auto p=bound.controlNetParameters(id); expect(p.ok(),"file-bound query"); if(p.ok()) { modify(*p.parameters); expect(bound.setControlNetParameters(id,std::move(*p.parameters)).changed,"file-bound detailed edit"); } }
        auto committed=encodeIisc(*file.document()).bytes; const auto rev=bound.revision(); auto invalid=*bound.controlNetParameters("depth").parameters;
        std::get<DepthAsset>(invalid.assets[0]).values[0]=-1; expect(!bound.setControlNetParameters("depth",std::move(invalid)).ok() && bound.revision()==rev && encodeIisc(*file.document()).bytes==committed,"file-bound atomic failure");
        ControlNetSettingsPatch p; p.conditioningScale=0.4; expect(bound.patchControlNetSettings("pose",p).changed && file.lastWriteStatistics().recordsWritten<=2,"common patch reuses source asset records");
        DocumentFile reopened; expect(reopened.open(path).ok() && encodeIisc(*reopened.document()).bytes==encodeIisc(*file.document()).bytes,"all 12 kinds working-file roundtrip");
        if(reopened.document()) verifyDetails(*reopened.document());
        file.close(); expect(!bound.controlNetParameters("pose").ok(),"stale file binding rejected");
    }
    return failures?1:0;
}
