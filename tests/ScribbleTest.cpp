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
    expect(editor.insertScribbleAsset({"scribble",{2,2},{0,1,1,0}}).changed,"insert scribble samples");
    ScribbleLayer layer; layer.properties={"layer","Scribble"}; layer.source=StaticSource{"scribble"};
    layer.control.modelId="test/scribble"; layer.control.conditioningScale=0.75;
    expect(editor.insertScribbleLayer(layer).changed,"insert scribble layer");
    return doc;
}
void verify(const Document &doc) {
    expect(validate(doc).ok(),"valid scribble document");
    expect(layerRole(doc.layers[0])==LayerRole::ControlNet && controlNetKind(doc.layers[0])==ControlNetKind::Scribble,"dedicated scribble role");
    const auto map=renderScribbleControlMap(doc,"layer",0);
    expect(map.ok(),"scribble map renders");
    expect(map.pixels.pixels==std::vector<std::uint32_t>({0xff000000,0xffffffff,0xffffffff,0xff000000}),"binary white lines on opaque black");
    expect(map.mask==std::vector<std::uint8_t>({0,1,1,0}),"binary mask survives");
    const auto artwork=renderFrame(doc,0);
    expect(artwork.ok() && artwork.pixels.pixels==std::vector<std::uint32_t>(4,0),"scribble excluded from artwork");
    const auto preview=renderFrameLayerTiles(doc,0,0,{{canvasRegion(doc),doc.extent}});
    expect(preview.ok() && preview.role==LayerRole::ControlNet && !preview.tiles.empty(),"explicit preview available");
}
}
int main() {
    auto doc=fixture(); verify(doc); expect(isLineControlNetLayer(doc.layers[0]),"line control hierarchy"); expect(layerKind(doc.layers[0])==LayerKind::StaticBitmap,"static bitmap identity");
    DocumentEditor editor(doc); const auto encoded=encodeIisc(doc); expect(encoded.ok(),"encode scribble");
    const auto decoded=decodeIisc(encoded.bytes); expect(decoded.ok(),"decode scribble"); if(decoded.ok()) verify(decoded.document);
    if(decoded.ok()) expect(findScribbleLayer(decoded.document,"layer")->control==findScribbleLayer(doc,"layer")->control,"control settings survive");
    auto display=doc; auto *displayLayer=findScribbleLayer(display,"layer");
    displayLayer->properties.visible=false; displayLayer->properties.opacity=0;
    displayLayer->properties.transform.translationX=100;
    expect(renderScribbleControlMap(display,"layer",0).mask==renderScribbleControlMap(doc,"layer",0).mask,"display properties cannot alter control values");
    displayLayer->properties.frameRange=LayerFrameRange{1,1};
    expect(!renderScribbleControlMap(display,"layer",0).ok() && renderScribbleControlMap(display,"layer",1).ok(),"explicit frame range");
    auto wrong=doc; findScribbleLayer(wrong,"layer")->source=StaticSource{"missing"};
    expect(!validate(wrong).ok(),"missing source rejected");
    wrong=doc; wrong.assets.push_back(RasterAsset{"color",makeRasterLayer(2,2)});
    findScribbleLayer(wrong,"layer")->source=StaticSource{"color"};
    expect(!validate(wrong).ok(),"raster cannot substitute scribble data");
    expect(isLineControlNetLayer(Layer{LineArtLayer{}}) && !isLineControlNetLayer(Layer{DepthLayer{}}),"line family includes Line Art, excludes Depth");
    wrong=doc; wrong.assets.push_back(CannyAsset{"other",{2,2},{0,1,1,0}});
    findScribbleLayer(wrong,"layer")->source=StaticSource{"other"};
    expect(!validate(wrong).ok(),"distinct binary line types cannot substitute for each other");
    Document upgrade; upgrade.extent={1,1}; upgrade.formatVersion.minor=10; DocumentEditor upgradeEditor(upgrade);
    expect(upgradeEditor.insertScribbleAsset({"new",{1,1},{1}}).changed && upgrade.formatVersion.minor==CurrentFormatMinor,"legacy document upgrade");
    // Corrupt independent wire fields and repair CRC to exercise the decoder itself.
    const std::vector<std::uint8_t> marker{8,8,0,0,0,'s','c','r','i','b','b','l','e'};
    const auto found=std::search(encoded.bytes.begin(),encoded.bytes.end(),marker.begin(),marker.end());
    expect(found!=encoded.bytes.end(),"scribble asset wire record exists");
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
            expect(!decodeIisc(bytes).ok(),"corrupt scribble payload fails closed with valid checksum");
        };
        rejects(offset,255); // Unknown asset tag.
        rejects(offset+21,5); // Sample count no longer matches 2x2 dimensions.
        rejects(offset+29,2); // Nonbinary mask value.
    }
    const auto revision=editor.revision();
    for(std::uint8_t bad:{std::uint8_t(2),std::uint8_t(255)}) {
        expect(!editor.setScribbleSample("scribble",0,0,bad).ok(),"reject invalid scribble");
        expect(editor.revision()==revision && encodeIisc(doc).bytes==encoded.bytes,"invalid edits rollback");
    }
    expect(!editor.setScribbleSample("scribble",2,0,1).ok(),"coordinate bounds");
    auto bad=*findScribbleAsset(doc,"scribble"); bad.mask.pop_back();
    expect(!editor.replaceScribbleAsset("scribble",bad).ok(),"reject wrong sample count");
    auto old=doc; old.formatVersion.minor=10; expect(!encodeIisc(old).ok(),"older format rejects scribble");
    SerializationLimits limits; limits.maximumScribbleSamples=3;
    expect(!encodeIisc(doc,limits).ok() && !decodeIisc(encoded.bytes,limits).ok(),"sample budgets encode/decode");
    expect(!renderScribbleControlMap(doc,"layer",0,3).ok(),"output allocation budget");
    BitmapEditor brush; expect(!brush.bind(doc,"scribble"),"color brush cannot corrupt line mask");
    expect(editor.insertScribbleAsset({"next",{2,2},{1,0,0,0}}).changed,"second frame");
    expect(editor.setKeyframedSource("layer",{{0,"scribble"},{1,"next"}}).changed,"dynamic scribble");
    expect(layerKind(doc.layers[0])==LayerKind::DynamicBitmap,"dynamic bitmap identity");
    expect(renderScribbleControlMap(doc,"layer",1).pixels.pixels[0]==0xffffffff,"current frame selects scribble");
    expect(!renderScribbleControlMap(doc,"layer",2).ok(),"out of timeline");
    auto settings=findScribbleLayer(doc,"layer")->control; settings.enabled=false;
    expect(editor.setControlNetSettings("layer",settings).changed && !renderScribbleControlMap(doc,"layer",0).ok(),"disabled control");
    settings.enabled=true; expect(editor.setControlNetSettings("layer",settings).changed,"reenable");
    settings.guidanceEnd=0; expect(!editor.setControlNetSettings("layer",settings).ok(),"invalid settings");
    QTemporaryDir dir(QStringLiteral(IISHAREDCANVAS_TEST_OUTPUT_DIR "/scribble-XXXXXX"));
    expect(dir.isValid(),"test directory");
    if(dir.isValid()) {
        DocumentFile file; const auto path=dir.filePath("scribble.iisc").toStdString();
        expect(file.create(path,doc).ok(),"working file create"); DocumentEditor bound(file);
        expect(bound.setScribbleSample("scribble",1,0,0).changed,"persist exact edit");
        auto updatedSettings=findScribbleLayer(*file.document(),"layer")->control; updatedSettings.conditioningScale=0.6;
        expect(bound.setControlNetSettings("layer",updatedSettings).changed && file.lastWriteStatistics().recordsWritten<=2,"control-only edit reuses scribble records");
        DocumentFile reopened; expect(reopened.open(path).ok(),"reopen");
        if(reopened.document()) {
            expect(findScribbleAsset(*reopened.document(),"scribble")->mask[1]==0,"exact binary mask round trip");
            expect(renderScribbleControlMap(*reopened.document(),"layer",1).mask==std::vector<std::uint8_t>({1,0,0,0}),"dynamic file source survives");
        }
        expect(exportPsd(doc,dir.filePath("scribble.psd").toStdString()).code==MediaIoCode::UnsupportedFeature,"PSD rejects scribble loss");
        auto orphan=doc; orphan.layers.clear(); orphan.frames.clear();
        expect(exportTimelineInterchange(orphan,dir.filePath("timeline").toStdString()).code==MediaIoCode::UnsupportedFeature,"orphan scribble interchange fails closed");
    }
    return failures ? 1 : 0;
}
