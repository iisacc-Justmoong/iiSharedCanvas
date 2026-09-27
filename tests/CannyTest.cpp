#include <iiSharedCanvas.h>
#include <QTemporaryDir>
#include <algorithm>
#include <iostream>
#include <limits>
using namespace iiSharedCanvas;
namespace {
int failures = 0;
void expect(bool ok, const char *message) { if (!ok) { std::cerr << message << '\n'; ++failures; } }
Document fixture() {
    Document doc; doc.extent={2,2}; doc.timeline.frameCount=2;
    DocumentEditor editor(doc);
    expect(editor.insertCannyAsset({"canny",{2,2},{0,1,1,0}}).changed,"insert canny samples");
    CannyLayer layer; layer.properties={"layer","Canny"}; layer.source=StaticSource{"canny"};
    layer.control.modelId="test/canny"; layer.control.conditioningScale=0.75;
    expect(editor.insertCannyLayer(layer).changed,"insert canny layer");
    return doc;
}
void verify(const Document &doc) {
    expect(validate(doc).ok(),"valid canny document");
    expect(layerRole(doc.layers[0])==LayerRole::ControlNet && controlNetKind(doc.layers[0])==ControlNetKind::Canny,"dedicated canny role");
    const auto map=renderCannyControlMap(doc,"layer",0);
    expect(map.ok(),"canny map renders");
    expect(map.pixels.pixels==std::vector<std::uint32_t>({0xff000000,0xffffffff,0xffffffff,0xff000000}),"binary white lines on opaque black");
    expect(map.mask==std::vector<std::uint8_t>({0,1,1,0}),"binary mask survives");
    const auto artwork=renderFrame(doc,0);
    expect(artwork.ok() && artwork.pixels.pixels==std::vector<std::uint32_t>(4,0),"canny excluded from artwork");
    const auto preview=renderFrameLayerTiles(doc,0,0,{{canvasRegion(doc),doc.extent}});
    expect(preview.ok() && preview.role==LayerRole::ControlNet && !preview.tiles.empty(),"explicit preview available");
}
}
int main() {
    auto doc=fixture(); verify(doc); expect(isLineControlNetLayer(doc.layers[0]),"line control hierarchy"); expect(layerKind(doc.layers[0])==LayerKind::StaticBitmap,"static bitmap identity");
    DocumentEditor editor(doc); const auto encoded=encodeIisc(doc); expect(encoded.ok(),"encode canny");
    const auto decoded=decodeIisc(encoded.bytes); expect(decoded.ok(),"decode canny"); if(decoded.ok()) verify(decoded.document);
    if(decoded.ok()) expect(findCannyLayer(decoded.document,"layer")->control==findCannyLayer(doc,"layer")->control,"control settings survive");
    auto display=doc; auto *displayLayer=findCannyLayer(display,"layer");
    displayLayer->properties.visible=false; displayLayer->properties.opacity=0;
    displayLayer->properties.transform.translationX=100;
    expect(renderCannyControlMap(display,"layer",0).mask==renderCannyControlMap(doc,"layer",0).mask,"display properties cannot alter control values");
    displayLayer->properties.frameRange=LayerFrameRange{1,1};
    expect(!renderCannyControlMap(display,"layer",0).ok() && renderCannyControlMap(display,"layer",1).ok(),"explicit frame range");
    auto wrong=doc; findCannyLayer(wrong,"layer")->source=StaticSource{"missing"};
    expect(!validate(wrong).ok(),"missing source rejected");
    wrong=doc; wrong.assets.push_back(RasterAsset{"color",makeRasterLayer(2,2)});
    findCannyLayer(wrong,"layer")->source=StaticSource{"color"};
    expect(!validate(wrong).ok(),"raster cannot substitute canny data");
    expect(isLineControlNetLayer(Layer{LineArtLayer{}}) && !isLineControlNetLayer(Layer{DepthLayer{}}),"line family includes Line Art, excludes Depth");
    wrong=doc; wrong.assets.push_back(ScribbleAsset{"other",{2,2},{0,1,1,0}});
    findCannyLayer(wrong,"layer")->source=StaticSource{"other"};
    expect(!validate(wrong).ok(),"distinct binary line types cannot substitute for each other");
    Document upgrade; upgrade.extent={1,1}; upgrade.formatVersion.minor=10; DocumentEditor upgradeEditor(upgrade);
    expect(upgradeEditor.insertCannyAsset({"new",{1,1},{1}}).changed && upgrade.formatVersion.minor==CurrentFormatMinor,"legacy document upgrade");
    // Corrupt independent wire fields and repair CRC to exercise the decoder itself.
    const std::vector<std::uint8_t> marker{7,5,0,0,0,'c','a','n','n','y'};
    const auto found=std::search(encoded.bytes.begin(),encoded.bytes.end(),marker.begin(),marker.end());
    expect(found!=encoded.bytes.end(),"canny asset wire record exists");
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
            expect(!decodeIisc(bytes).ok(),"corrupt canny payload fails closed with valid checksum");
        };
        rejects(offset,255); // Unknown asset tag.
        rejects(offset+18,5); // Sample count no longer matches 2x2 dimensions.
        rejects(offset+26,2); // Nonbinary mask value.
    }
    const auto revision=editor.revision();
    for(std::uint8_t bad:{std::uint8_t(2),std::uint8_t(255)}) {
        expect(!editor.setCannySample("canny",0,0,bad).ok(),"reject invalid canny");
        expect(editor.revision()==revision && encodeIisc(doc).bytes==encoded.bytes,"invalid edits rollback");
    }
    expect(!editor.setCannySample("canny",2,0,1).ok(),"coordinate bounds");
    auto bad=*findCannyAsset(doc,"canny"); bad.mask.pop_back();
    expect(!editor.replaceCannyAsset("canny",bad).ok(),"reject wrong sample count");
    auto old=doc; old.formatVersion.minor=10; expect(!encodeIisc(old).ok(),"older format rejects canny");
    SerializationLimits limits; limits.maximumCannySamples=3;
    expect(!encodeIisc(doc,limits).ok() && !decodeIisc(encoded.bytes,limits).ok(),"sample budgets encode/decode");
    expect(!renderCannyControlMap(doc,"layer",0,3).ok(),"output allocation budget");
    BitmapEditor brush; expect(!brush.bind(doc,"canny"),"color brush cannot corrupt line mask");
    expect(editor.insertCannyAsset({"next",{2,2},{1,0,0,0}}).changed,"second frame");
    expect(editor.setKeyframedSource("layer",{{0,"canny"},{1,"next"}}).changed,"dynamic canny");
    expect(layerKind(doc.layers[0])==LayerKind::DynamicBitmap,"dynamic bitmap identity");
    expect(renderCannyControlMap(doc,"layer",1).pixels.pixels[0]==0xffffffff,"current frame selects canny");
    expect(!renderCannyControlMap(doc,"layer",2).ok(),"out of timeline");
    auto settings=findCannyLayer(doc,"layer")->control; settings.enabled=false;
    expect(editor.setControlNetSettings("layer",settings).changed && !renderCannyControlMap(doc,"layer",0).ok(),"disabled control");
    settings.enabled=true; expect(editor.setControlNetSettings("layer",settings).changed,"reenable");
    settings.guidanceEnd=0; expect(!editor.setControlNetSettings("layer",settings).ok(),"invalid settings");
    QTemporaryDir dir(QStringLiteral(IISHAREDCANVAS_TEST_OUTPUT_DIR "/canny-XXXXXX"));
    expect(dir.isValid(),"test directory");
    if(dir.isValid()) {
        DocumentFile file; const auto path=dir.filePath("canny.iisc").toStdString();
        expect(file.create(path,doc).ok(),"working file create"); DocumentEditor bound(file);
        expect(bound.setCannySample("canny",1,0,0).changed,"persist exact edit");
        auto updatedSettings=findCannyLayer(*file.document(),"layer")->control; updatedSettings.conditioningScale=0.6;
        expect(bound.setControlNetSettings("layer",updatedSettings).changed && file.lastWriteStatistics().recordsWritten<=2,"control-only edit reuses canny records");
        DocumentFile reopened; expect(reopened.open(path).ok(),"reopen");
        if(reopened.document()) {
            expect(findCannyAsset(*reopened.document(),"canny")->mask[1]==0,"exact binary mask round trip");
            expect(renderCannyControlMap(*reopened.document(),"layer",1).mask==std::vector<std::uint8_t>({1,0,0,0}),"dynamic file source survives");
        }
        expect(exportPsd(doc,dir.filePath("canny.psd").toStdString()).code==MediaIoCode::UnsupportedFeature,"PSD rejects canny loss");
        auto orphan=doc; orphan.layers.clear(); orphan.frames.clear();
        expect(exportTimelineInterchange(orphan,dir.filePath("timeline").toStdString()).code==MediaIoCode::UnsupportedFeature,"orphan canny interchange fails closed");
    }
    return failures ? 1 : 0;
}
