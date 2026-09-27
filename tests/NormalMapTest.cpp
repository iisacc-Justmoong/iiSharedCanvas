#include <iiSharedCanvas.h>
#include <QTemporaryDir>
#include <algorithm>
#include <iostream>
#include <limits>
#include <stdexcept>
using namespace iiSharedCanvas;
namespace {
int failures = 0;
void expect(bool ok, const char *message) { if (!ok) { std::cerr << message << '\n'; ++failures; } }
Document fixture() {
    Document doc; doc.extent={2,2}; doc.timeline.frameCount=2;
    DocumentEditor editor(doc);
    expect(editor.insertNormalMapAsset({"normalMap",{2,2},{{0,0,1},{1,0,0},{0,-1,0},{0,0,0,false}}}).changed,"insert normalMap samples");
    NormalMapLayer layer; layer.properties={"layer","NormalMap"}; layer.source=StaticSource{"normalMap"};
    layer.control.modelId="test/normalMap"; layer.control.conditioningScale=0.75;
    expect(editor.insertNormalMapLayer(layer).changed,"insert normalMap layer");
    return doc;
}
void verify(const Document &doc) {
    expect(validate(doc).ok(),"valid normalMap document");
    expect(layerRole(doc.layers[0])==LayerRole::ControlNet && controlNetKind(doc.layers[0])==ControlNetKind::NormalMap,"dedicated normalMap role");
    const auto map=renderNormalMapControlMap(doc,"layer",0);
    expect(map.ok(),"normalMap map renders");
    expect(map.pixels.pixels==std::vector<std::uint32_t>({0xff8080ff,0xffff8080,0xff800080,0xff000000}),"XYZ encodes signed unit components, missing is black");
    expect(map.samples==std::vector<NormalMapSample>({{0,0,1},{1,0,0},{0,-1,0},{0,0,0,false}}),"unquantized values survive");
    expect(map.validPixels==std::vector<std::uint8_t>({1,1,1,0}),"validity is distinct from RGB");
    expect(!isLineControlNetLayer(doc.layers[0]),"normal map is not a line control");
    const auto swapped=renderNormalMapControlMap(doc,"layer",0,{NormalMapChannelOrder::Zyx,true});
    expect(swapped.ok() && swapped.pixels.pixels==std::vector<std::uint32_t>({0xffff8080,0xff8080ff,0xff80ff80,0xff000000}),"explicit channel order and Y flip");
    expect(swapped.samples==map.samples,"output conversion never rewrites authored samples");
    expect(!renderNormalMapControlMap(doc,"layer",0,{static_cast<NormalMapChannelOrder>(255)}).ok(),"unknown output encoding rejected");
    const auto artwork=renderFrame(doc,0);
    expect(artwork.ok() && artwork.pixels.pixels==std::vector<std::uint32_t>(4,0),"normalMap excluded from artwork");
    const auto preview=renderFrameLayerTiles(doc,0,0,{{canvasRegion(doc),doc.extent}});
    expect(preview.ok() && preview.role==LayerRole::ControlNet && !preview.tiles.empty(),"explicit preview available");
}
}
int main() {
    auto doc=fixture(); verify(doc); expect(layerKind(doc.layers[0])==LayerKind::StaticBitmap,"static bitmap identity");
    DocumentEditor editor(doc); const auto encoded=encodeIisc(doc); expect(encoded.ok(),"encode normalMap");
    const auto decoded=decodeIisc(encoded.bytes); expect(decoded.ok(),"decode normalMap"); if(decoded.ok()) verify(decoded.document);
    if(decoded.ok()) expect(findNormalMapLayer(decoded.document,"layer")->control==findNormalMapLayer(doc,"layer")->control,"control settings survive");
    auto display=doc; auto *displayLayer=findNormalMapLayer(display,"layer");
    displayLayer->properties.visible=false; displayLayer->properties.opacity=0;
    displayLayer->properties.transform.translationX=100;
    expect(renderNormalMapControlMap(display,"layer",0).samples==renderNormalMapControlMap(doc,"layer",0).samples,"display properties cannot alter control values");
    displayLayer->properties.frameRange=LayerFrameRange{1,1};
    expect(!renderNormalMapControlMap(display,"layer",0).ok() && renderNormalMapControlMap(display,"layer",1).ok(),"explicit frame range");
    auto wrong=doc; findNormalMapLayer(wrong,"layer")->source=StaticSource{"missing"};
    expect(!validate(wrong).ok(),"missing source rejected");
    wrong=doc; wrong.assets.push_back(RasterAsset{"color",makeRasterLayer(2,2)});
    findNormalMapLayer(wrong,"layer")->source=StaticSource{"color"};
    expect(!validate(wrong).ok(),"raster cannot substitute normalMap data");
    Document upgrade; upgrade.extent={1,1}; upgrade.formatVersion.minor=12; DocumentEditor upgradeEditor(upgrade);
    expect(upgradeEditor.insertNormalMapAsset({"new",{1,1},{{0,0,1}}}).changed && upgrade.formatVersion.minor==CurrentFormatMinor,"legacy document upgrade");
    // Corrupt independent wire fields and repair CRC to exercise the decoder itself.
    const std::vector<std::uint8_t> marker{10,9,0,0,0,'n','o','r','m','a','l','M','a','p'};
    const auto found=std::search(encoded.bytes.begin(),encoded.bytes.end(),marker.begin(),marker.end());
    expect(found!=encoded.bytes.end(),"normalMap asset wire record exists");
    if(found!=encoded.bytes.end()) {
        const auto offset=static_cast<std::size_t>(found-encoded.bytes.begin());
        const auto rejects=[&](std::size_t at, std::uint8_t value) {
            auto bytes=encoded.bytes; bytes.at(at)=value;
            std::uint32_t crc=0xffffffffU;
            for(std::size_t i=IiscHeaderSize;i<bytes.size();++i) {
                crc^=bytes[i];
                for(int bit=0;bit<8;++bit) crc=(crc>>1U)^(0xedb88320U & (0U-(crc&1U)));
            }
            crc^=0xffffffffU;
            for(unsigned i=0;i<4;++i) bytes[24+i]=static_cast<std::uint8_t>(crc>>(8*i));
            expect(!decodeIisc(bytes).ok(),"corrupt normalMap payload fails closed with valid checksum");
        };
        rejects(offset,255); // Unknown asset tag.
        rejects(offset+22,5); // Sample count no longer matches 2x2 dimensions.
        rejects(offset+30+7,0x40); // First component becomes 2.0.
        rejects(offset+30+24,2); // Invalid boolean encoding.
    }
    const auto revision=editor.revision();
    for(NormalMapSample bad:std::vector<NormalMapSample>{{0,0,0},{0,0,2},{1,1,0},{0,0,1,false},{std::numeric_limits<double>::quiet_NaN(),0,1},{0,std::numeric_limits<double>::infinity(),1}}) {
        expect(!editor.setNormalMapSample("normalMap",0,0,bad).ok(),"reject invalid normalMap");
        expect(editor.revision()==revision && encodeIisc(doc).bytes==encoded.bytes,"invalid edits rollback");
    }
    expect(!editor.setNormalMapSample("normalMap",2,0,{0,0,1}).ok(),"coordinate bounds");
    auto bad=*findNormalMapAsset(doc,"normalMap"); bad.samples.pop_back();
    expect(!editor.replaceNormalMapAsset("normalMap",bad).ok(),"reject wrong sample count");
    auto badMissing=*findNormalMapAsset(doc,"normalMap"); badMissing.samples[3]={0,0,1,false};
    expect(!editor.replaceNormalMapAsset("normalMap",badMissing).ok(),"missing samples require canonical zero vector");
    auto old=doc; old.formatVersion.minor=12; expect(!encodeIisc(old).ok(),"older format rejects normalMap");
    SerializationLimits limits; limits.maximumNormalMapSamples=3;
    expect(!encodeIisc(doc,limits).ok() && !decodeIisc(encoded.bytes,limits).ok(),"sample budgets encode/decode");
    expect(!renderNormalMapControlMap(doc,"layer",0,{NormalMapChannelOrder::Xyz,false,3}).ok(),"output allocation budget");
    bool budgetThrows=false, invalidThrows=false;
    try { (void)normalMapRasterPreview(*findNormalMapAsset(doc,"normalMap"),{NormalMapChannelOrder::Xyz,false,3}); }
    catch(const std::length_error &) { budgetThrows=true; }
    try { (void)normalMapRasterPreview(badMissing); }
    catch(const std::invalid_argument &) { invalidThrows=true; }
    expect(budgetThrows && invalidThrows,"direct preview rejects budget and malformed vectors");
    expect(!renderNormalMapControlMap(doc,"missing",0).ok(),"missing layer fails");
    expect(!editor.setNormalMapSample("missing",0,0,{0,0,1}).ok(),"missing asset edit fails");
    const auto unchangedRevision=editor.revision();
    expect(!editor.setNormalMapSample("normalMap",0,0,{0,0,1}).changed && editor.revision()==unchangedRevision,"identical sample is a no-op");
    BitmapEditor brush; expect(!brush.bind(doc,"normalMap"),"color brush cannot corrupt normal vectors");
    expect(editor.insertNormalMapAsset({"next",{2,2},{{-1,0,0},{0,1,0},{0,0,-1},{0,0,1}}}).changed,"second frame");
    const auto twoAssets=encodeIisc(doc); limits.maximumNormalMapSamples=7;
    expect(!encodeIisc(doc,limits).ok() && !decodeIisc(twoAssets.bytes,limits).ok(),"sample budgets accumulate across assets");
    expect(editor.setKeyframedSource("layer",{{0,"normalMap"},{1,"next"}}).changed,"dynamic normalMap");
    expect(layerKind(doc.layers[0])==LayerKind::DynamicBitmap,"dynamic bitmap identity");
    expect(renderNormalMapControlMap(doc,"layer",1).pixels.pixels[0]==0xff008080,"current frame selects normalMap");
    expect(renderNormalMapControlMap(doc,"layer",1).pixels.pixels==std::vector<std::uint32_t>({0xff008080,0xff80ff80,0xff808000,0xff8080ff}),"all remaining signed axes encode exactly");
    auto mixed=doc; mixed.assets.push_back(DepthAsset{"scalar",{2,2},{0,0,0,0}});
    DocumentEditor mixedEditor(mixed); const auto beforeMixed=encodeIisc(mixed);
    expect(!mixedEditor.setKeyframedSource("layer",{{0,"normalMap"},{1,"scalar"}}).ok() && encodeIisc(mixed).bytes==beforeMixed.bytes,"dynamic sources cannot mix normal and depth");
    expect(!renderNormalMapControlMap(doc,"layer",2).ok(),"out of timeline");
    auto settings=findNormalMapLayer(doc,"layer")->control; settings.enabled=false;
    expect(editor.setControlNetSettings("layer",settings).changed && !renderNormalMapControlMap(doc,"layer",0).ok(),"disabled control");
    settings.enabled=true; expect(editor.setControlNetSettings("layer",settings).changed,"reenable");
    settings.guidanceEnd=0; expect(!editor.setControlNetSettings("layer",settings).ok(),"invalid settings");
    QTemporaryDir dir(QStringLiteral(IISHAREDCANVAS_TEST_OUTPUT_DIR "/normalMap-XXXXXX"));
    expect(dir.isValid(),"test directory");
    if(dir.isValid()) {
        DocumentFile file; const auto path=dir.filePath("normalMap.iisc").toStdString();
        expect(file.create(path,doc).ok(),"working file create"); DocumentEditor bound(file);
        expect(bound.setNormalMapSample("normalMap",1,0,{0.6,0,0.8}).changed,"persist exact edit");
        const auto committed=encodeIisc(*file.document()).bytes; const auto committedRevision=bound.revision();
        expect(!bound.setNormalMapSample("normalMap",0,0,{0,0,0}).ok()
            && bound.revision()==committedRevision && encodeIisc(*file.document()).bytes==committed,"file-bound invalid edit is atomic");
        auto updatedSettings=findNormalMapLayer(*file.document(),"layer")->control; updatedSettings.conditioningScale=0.6;
        expect(bound.setControlNetSettings("layer",updatedSettings).changed && file.lastWriteStatistics().recordsWritten<=2,"control-only edit reuses normalMap records");
        DocumentFile reopened; expect(reopened.open(path).ok(),"reopen");
        if(reopened.document()) {
            expect(findNormalMapAsset(*reopened.document(),"normalMap")->samples[1]==NormalMapSample{0.6,0,0.8},"f64 lossless round trip");
            expect(renderNormalMapControlMap(*reopened.document(),"layer",1).samples==std::vector<NormalMapSample>({{-1,0,0},{0,1,0},{0,0,-1},{0,0,1}}),"dynamic file source survives");
        }
        expect(exportPsd(doc,dir.filePath("normalMap.psd").toStdString()).code==MediaIoCode::UnsupportedFeature,"PSD rejects normalMap loss");
        auto orphan=doc; orphan.layers.clear(); orphan.frames.clear();
        expect(exportTimelineInterchange(orphan,dir.filePath("timeline").toStdString()).code==MediaIoCode::UnsupportedFeature,"orphan normalMap interchange fails closed");
    }
    return failures ? 1 : 0;
}
