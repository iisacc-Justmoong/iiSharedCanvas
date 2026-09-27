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
    expect(editor.insertReferenceAsset({"reference",{2,2},{{255,0,0},{0,255,0},{0,0,255},{12,34,56}}}).changed,"insert reference samples");
    ReferenceLayer layer; layer.properties={"layer","Reference"}; layer.source=StaticSource{"reference"};
    layer.control.modelId=""; layer.reference={ReferenceMode::AttentionAdaIN,0.35}; layer.control.conditioningScale=0.75;
    expect(editor.insertReferenceLayer(layer).changed,"insert reference layer");
    return doc;
}
void verify(const Document &doc) {
    expect(validate(doc).ok(),"valid reference document");
    expect(layerRole(doc.layers[0])==LayerRole::ControlNet && controlNetKind(doc.layers[0])==ControlNetKind::Reference,"dedicated reference role");
    const auto map=renderReferenceControlMap(doc,"layer",0);
    expect(map.ok(),"reference map renders");
    expect(map.pixels.pixels==std::vector<std::uint32_t>({0xffff0000,0xff00ff00,0xff0000ff,0xff0c2238}),"opaque exact RGB colors");
    expect(map.colors==std::vector<ReferenceColor>({{255,0,0},{0,255,0},{0,0,255},{12,34,56}}),"exact prepared RGB survives");
    expect(map.reference==findReferenceLayer(doc,"layer")->reference && map.control==findReferenceLayer(doc,"layer")->control,"output carries reference and control contracts");
    expect(!isLineControlNetLayer(doc.layers[0]),"reference is not a line control");
    const auto artwork=renderFrame(doc,0);
    expect(artwork.ok() && artwork.pixels.pixels==std::vector<std::uint32_t>(4,0),"reference excluded from artwork");
    const auto preview=renderFrameLayerTiles(doc,0,0,{{canvasRegion(doc),doc.extent}});
    expect(preview.ok() && preview.role==LayerRole::ControlNet && !preview.tiles.empty(),"explicit preview available");
}
}
int main() {
    const auto source=RasterLayer{2,2,{0xffff0000,0xff00ff00,0xff0000ff,0xff123456}};
    const auto prepared=makeReferenceAsset("prepared",source);
    expect(referenceRasterPreview(prepared).pixels==source.pixels,"reference preparation preserves spatial detail and RGB");
    auto transparent=source; transparent.pixels[0]=0;
    bool alphaRejected=false, limited=false;
    try { (void)makeReferenceAsset("bad",transparent); } catch(const std::invalid_argument &) { alphaRejected=true; }
    try { (void)makeReferenceAsset("bad",source,3); } catch(const std::length_error &) { limited=true; }
    expect(alphaRejected && limited,"preparation rejects implicit alpha loss and oversized output");
    auto doc=fixture(); verify(doc); expect(layerKind(doc.layers[0])==LayerKind::StaticBitmap,"static bitmap identity");
    DocumentEditor editor(doc); const auto encoded=encodeIisc(doc); expect(encoded.ok(),"encode reference");
    const auto decoded=decodeIisc(encoded.bytes); expect(decoded.ok(),"decode reference"); if(decoded.ok()) verify(decoded.document);
    if(decoded.ok()) expect(findReferenceLayer(decoded.document,"layer")->control==findReferenceLayer(doc,"layer")->control,"control settings survive");
    auto display=doc; auto *displayLayer=findReferenceLayer(display,"layer");
    displayLayer->properties.visible=false; displayLayer->properties.opacity=0;
    displayLayer->properties.transform.translationX=100;
    expect(renderReferenceControlMap(display,"layer",0).colors==renderReferenceControlMap(doc,"layer",0).colors,"display properties cannot alter control values");
    displayLayer->properties.frameRange=LayerFrameRange{1,1};
    expect(!renderReferenceControlMap(display,"layer",0).ok() && renderReferenceControlMap(display,"layer",1).ok(),"explicit frame range");
    auto wrong=doc; findReferenceLayer(wrong,"layer")->source=StaticSource{"missing"};
    expect(!validate(wrong).ok(),"missing source rejected");
    wrong=doc; wrong.assets.push_back(RasterAsset{"color",makeRasterLayer(2,2)});
    findReferenceLayer(wrong,"layer")->source=StaticSource{"color"};
    expect(!validate(wrong).ok(),"raster cannot substitute reference data");
    Document upgrade; upgrade.extent={1,1}; upgrade.formatVersion.minor=15; DocumentEditor upgradeEditor(upgrade);
    expect(upgradeEditor.insertReferenceAsset({"new",{1,1},{{1,2,3}}}).changed && upgrade.formatVersion.minor==CurrentFormatMinor,"legacy document upgrade");
    // Corrupt independent wire fields and repair CRC to exercise the decoder itself.
    const std::vector<std::uint8_t> marker{13,9,0,0,0,'r','e','f','e','r','e','n','c','e'};
    const auto found=std::search(encoded.bytes.begin(),encoded.bytes.end(),marker.begin(),marker.end());
    expect(found!=encoded.bytes.end(),"reference asset wire record exists");
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
            expect(!decodeIisc(bytes).ok(),"corrupt reference payload fails closed with valid checksum");
        };
        rejects(offset,255); // Unknown asset tag.
        rejects(offset+22,5); // Sample count no longer matches 2x2 dimensions.
        const std::vector<std::uint8_t> referenceMarker{2,102,102,102,102,102,102,214,63}; // Mode and binary64 0.35.
        const auto setting=std::search(encoded.bytes.begin(),encoded.bytes.end(),referenceMarker.begin(),referenceMarker.end());
        expect(setting!=encoded.bytes.end(),"reference settings wire record exists");
        if (setting!=encoded.bytes.end()) {
            const auto at=static_cast<std::size_t>(setting-encoded.bytes.begin());
            rejects(at,255); rejects(at+8,0x40); // Unknown mode and out-of-range fidelity.
        }
    }
    for (auto mode:{ReferenceMode::Attention,ReferenceMode::AdaIN,ReferenceMode::AttentionAdaIN}) {
        auto variant=doc; DocumentEditor modeEditor(variant);
        expect(modeEditor.setReferenceSettings("layer",{mode,0.75}).ok(),"set reference mode");
        expect(renderReferenceControlMap(variant,"layer",0).pixels.pixels==renderReferenceControlMap(doc,"layer",0).pixels.pixels,"reference settings never blend or modify RGB");
        auto saved=encodeIisc(variant); auto loaded=decodeIisc(saved.bytes);
        expect(saved.ok() && loaded.ok() && findReferenceLayer(loaded.document,"layer")->reference==ReferenceSettings{mode,0.75},"all reference modes survive snapshot roundtrip");
    }
    const auto revision=editor.revision();
    for (auto bad:std::vector<ReferenceSettings>{{static_cast<ReferenceMode>(255),0.5},{ReferenceMode::Attention,-0.1},{ReferenceMode::AdaIN,1.1},{ReferenceMode::Attention,std::numeric_limits<double>::quiet_NaN()}}) {
        expect(!editor.setReferenceSettings("layer",bad).ok() && editor.revision()==revision
            && encodeIisc(doc).bytes==encoded.bytes,"invalid reference settings roll back");
    }
    expect(!editor.setReferenceSample("reference",2,0,{1,2,3}).ok(),"coordinate bounds");
    expect(editor.revision()==revision && encodeIisc(doc).bytes==encoded.bytes,"invalid edits rollback");
    auto bad=*findReferenceAsset(doc,"reference"); bad.colors.pop_back();
    expect(!editor.replaceReferenceAsset("reference",bad).ok(),"reject wrong sample count");
    auto downgraded=encoded.bytes; downgraded[10]=15;
    expect(!decodeIisc(downgraded).ok(),"new tags rejected under old format header");
    auto old=doc; old.formatVersion.minor=15; expect(!encodeIisc(old).ok(),"older format rejects reference");
    SerializationLimits limits; limits.maximumReferenceSamples=3;
    expect(!encodeIisc(doc,limits).ok() && !decodeIisc(encoded.bytes,limits).ok(),"sample budgets encode/decode");
    expect(!renderReferenceControlMap(doc,"layer",0,3).ok(),"output allocation budget");
    BitmapEditor brush; expect(!brush.bind(doc,"reference"),"color brush cannot corrupt dedicated reference data");
    expect(editor.insertReferenceAsset({"next",{2,2},{{255,255,255},{1,2,3},{0,0,0},{4,5,6}}}).changed,"second frame");
    limits.maximumReferenceSamples=7; const auto multi=encodeIisc(doc);
    expect(!encodeIisc(doc,limits).ok() && !decodeIisc(multi.bytes,limits).ok(),"sample budget sums all assets");
    expect(editor.setKeyframedSource("layer",{{0,"reference"},{1,"next"}}).changed,"dynamic reference");
    expect(layerKind(doc.layers[0])==LayerKind::DynamicBitmap,"dynamic bitmap identity");
    expect(renderReferenceControlMap(doc,"layer",1).pixels.pixels[0]==0xffffffff,"current frame selects reference");
    expect(!renderReferenceControlMap(doc,"layer",2).ok(),"out of timeline");
    auto settings=findReferenceLayer(doc,"layer")->control; settings.enabled=false;
    expect(editor.setControlNetSettings("layer",settings).changed && !renderReferenceControlMap(doc,"layer",0).ok(),"disabled control");
    settings.enabled=true; expect(editor.setControlNetSettings("layer",settings).changed,"reenable");
    settings.guidanceEnd=0; expect(!editor.setControlNetSettings("layer",settings).ok(),"invalid settings");
    QTemporaryDir dir(QStringLiteral(IISHAREDCANVAS_TEST_OUTPUT_DIR "/reference-XXXXXX"));
    expect(dir.isValid(),"test directory");
    if(dir.isValid()) {
        DocumentFile file; const auto path=dir.filePath("reference.iisc").toStdString();
        expect(file.create(path,doc).ok(),"working file create"); DocumentEditor bound(file);
        expect(bound.setReferenceSample("reference",1,0,{123,45,67}).changed,"persist exact edit");
        const auto committed=encodeIisc(*file.document()).bytes; const auto committedRevision=bound.revision();
        expect(!bound.setReferenceSample("reference",-1,0,{0,0,0}).ok()
            && bound.revision()==committedRevision && encodeIisc(*file.document()).bytes==committed,"file-bound rejection preserves state");
        expect(!bound.setReferenceSettings("layer",{ReferenceMode::Attention,2.0}).ok()
            && bound.revision()==committedRevision && encodeIisc(*file.document()).bytes==committed,"file-bound invalid fidelity is atomic");
        expect(bound.setReferenceSettings("layer",{ReferenceMode::AdaIN,1.0}).changed && file.lastWriteStatistics().recordsWritten<=2,"reference-only edit reuses image records");
        auto updatedSettings=findReferenceLayer(*file.document(),"layer")->control; updatedSettings.conditioningScale=0.6;
        expect(bound.setControlNetSettings("layer",updatedSettings).changed && file.lastWriteStatistics().recordsWritten<=2,"control-only edit reuses reference records");
        DocumentFile reopened; expect(reopened.open(path).ok(),"reopen");
        if(reopened.document()) {
            expect(findReferenceLayer(*reopened.document(),"layer")->reference==ReferenceSettings{ReferenceMode::AdaIN,1.0},"reference settings persist in working file");
            expect(findReferenceAsset(*reopened.document(),"reference")->colors[1]==ReferenceColor{123,45,67},"RGB8 lossless round trip");
            expect(renderReferenceControlMap(*reopened.document(),"layer",1).colors==std::vector<ReferenceColor>({{255,255,255},{1,2,3},{0,0,0},{4,5,6}}),"dynamic file source survives");
        }
        expect(exportPsd(doc,dir.filePath("reference.psd").toStdString()).code==MediaIoCode::UnsupportedFeature,"PSD rejects reference loss");
        auto orphan=doc; orphan.layers.clear(); orphan.frames.clear();
        expect(exportTimelineInterchange(orphan,dir.filePath("timeline").toStdString()).code==MediaIoCode::UnsupportedFeature,"orphan reference interchange fails closed");
    }
    return failures ? 1 : 0;
}
