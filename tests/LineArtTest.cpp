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
    expect(editor.insertLineArtAsset({"lineart",{2,2},{0,0.5,1,0.000001}}).changed,"insert lineart samples");
    LineArtLayer layer; layer.properties={"layer","LineArt"}; layer.source=StaticSource{"lineart"};
    layer.control.modelId="test/lineart"; layer.control.conditioningScale=0.75;
    expect(editor.insertLineArtLayer(layer).changed,"insert lineart layer");
    return doc;
}
void verify(const Document &doc) {
    expect(validate(doc).ok(),"valid lineart document");
    expect(layerRole(doc.layers[0])==LayerRole::ControlNet && controlNetKind(doc.layers[0])==ControlNetKind::LineArt,"dedicated lineart role");
    const auto map=renderLineArtControlMap(doc,"layer",0);
    expect(map.ok(),"lineart map renders");
    expect(map.pixels.pixels==std::vector<std::uint32_t>({0xffffffff,0xff808080,0xff000000,0xffffffff}),"linear white/gray/black, no normalization");
    expect(map.coverage==std::vector<double>({0,0.5,1,0.000001}),"unquantized values survive");
    expect(map.inkPixels==std::vector<std::uint8_t>({0,1,1,1}),"tiny ink coverage survives preview quantization");
    const auto artwork=renderFrame(doc,0);
    expect(artwork.ok() && artwork.pixels.pixels==std::vector<std::uint32_t>(4,0),"lineart excluded from artwork");
    const auto preview=renderFrameLayerTiles(doc,0,0,{{canvasRegion(doc),doc.extent}});
    expect(preview.ok() && preview.role==LayerRole::ControlNet && !preview.tiles.empty(),"explicit preview available");
}
}
int main() {
    auto doc=fixture(); verify(doc); expect(layerKind(doc.layers[0])==LayerKind::StaticBitmap,"static bitmap identity");
    DocumentEditor editor(doc); const auto encoded=encodeIisc(doc); expect(encoded.ok(),"encode lineart");
    const auto decoded=decodeIisc(encoded.bytes); expect(decoded.ok(),"decode lineart"); if(decoded.ok()) verify(decoded.document);
    if(decoded.ok()) expect(findLineArtLayer(decoded.document,"layer")->control==findLineArtLayer(doc,"layer")->control,"control settings survive");
    auto display=doc; auto *displayLayer=findLineArtLayer(display,"layer");
    displayLayer->properties.visible=false; displayLayer->properties.opacity=0;
    displayLayer->properties.transform.translationX=100;
    expect(renderLineArtControlMap(display,"layer",0).coverage==renderLineArtControlMap(doc,"layer",0).coverage,"display properties cannot alter control values");
    displayLayer->properties.frameRange=LayerFrameRange{1,1};
    expect(!renderLineArtControlMap(display,"layer",0).ok() && renderLineArtControlMap(display,"layer",1).ok(),"explicit frame range");
    auto wrong=doc; findLineArtLayer(wrong,"layer")->source=StaticSource{"missing"};
    expect(!validate(wrong).ok(),"missing source rejected");
    wrong=doc; wrong.assets.push_back(RasterAsset{"color",makeRasterLayer(2,2)});
    findLineArtLayer(wrong,"layer")->source=StaticSource{"color"};
    expect(!validate(wrong).ok(),"raster cannot substitute lineart data");
    Document upgrade; upgrade.extent={1,1}; upgrade.formatVersion.minor=9; DocumentEditor upgradeEditor(upgrade);
    expect(upgradeEditor.insertLineArtAsset({"new",{1,1},{0.5}}).changed && upgrade.formatVersion.minor==CurrentFormatMinor,"legacy document upgrade");
    // Corrupt independent wire fields and repair CRC to exercise the decoder itself.
    const std::vector<std::uint8_t> marker{6,7,0,0,0,'l','i','n','e','a','r','t'};
    const auto found=std::search(encoded.bytes.begin(),encoded.bytes.end(),marker.begin(),marker.end());
    expect(found!=encoded.bytes.end(),"lineart asset wire record exists");
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
            expect(!decodeIisc(bytes).ok(),"corrupt lineart payload fails closed with valid checksum");
        };
        rejects(offset,255); // Unknown asset tag.
        rejects(offset+20,5); // Sample count no longer matches 2x2 dimensions.
        rejects(offset+28+7,0x40); // First f64 becomes 2.0, outside [0,1].
    }
    const auto revision=editor.revision();
    for(double bad:{-0.1,1.1,std::numeric_limits<double>::quiet_NaN(),std::numeric_limits<double>::infinity()}) {
        expect(!editor.setLineArtSample("lineart",0,0,bad).ok(),"reject invalid lineart");
        expect(editor.revision()==revision && encodeIisc(doc).bytes==encoded.bytes,"invalid edits rollback");
    }
    expect(!editor.setLineArtSample("lineart",2,0,0.2).ok(),"coordinate bounds");
    auto bad=*findLineArtAsset(doc,"lineart"); bad.coverage.pop_back();
    expect(!editor.replaceLineArtAsset("lineart",bad).ok(),"reject wrong sample count");
    auto old=doc; old.formatVersion.minor=9; expect(!encodeIisc(old).ok(),"older format rejects lineart");
    SerializationLimits limits; limits.maximumLineArtSamples=3;
    expect(!encodeIisc(doc,limits).ok() && !decodeIisc(encoded.bytes,limits).ok(),"sample budgets encode/decode");
    expect(!renderLineArtControlMap(doc,"layer",0,3).ok(),"output allocation budget");
    BitmapEditor brush; expect(!brush.bind(doc,"lineart"),"color brush cannot corrupt line coverage");
    expect(editor.insertLineArtAsset({"next",{2,2},{1,0.25,0,0}}).changed,"second frame");
    expect(editor.setKeyframedSource("layer",{{0,"lineart"},{1,"next"}}).changed,"dynamic lineart");
    expect(layerKind(doc.layers[0])==LayerKind::DynamicBitmap,"dynamic bitmap identity");
    expect(renderLineArtControlMap(doc,"layer",1).pixels.pixels[0]==0xff000000,"current frame selects lineart");
    expect(!renderLineArtControlMap(doc,"layer",2).ok(),"out of timeline");
    auto settings=findLineArtLayer(doc,"layer")->control; settings.enabled=false;
    expect(editor.setControlNetSettings("layer",settings).changed && !renderLineArtControlMap(doc,"layer",0).ok(),"disabled control");
    settings.enabled=true; expect(editor.setControlNetSettings("layer",settings).changed,"reenable");
    settings.guidanceEnd=0; expect(!editor.setControlNetSettings("layer",settings).ok(),"invalid settings");
    QTemporaryDir dir(QStringLiteral(IISHAREDCANVAS_TEST_OUTPUT_DIR "/lineart-XXXXXX"));
    expect(dir.isValid(),"test directory");
    if(dir.isValid()) {
        DocumentFile file; const auto path=dir.filePath("lineart.iisc").toStdString();
        expect(file.create(path,doc).ok(),"working file create"); DocumentEditor bound(file);
        expect(bound.setLineArtSample("lineart",1,0,0.123456789012345).changed,"persist exact edit");
        auto updatedSettings=findLineArtLayer(*file.document(),"layer")->control; updatedSettings.conditioningScale=0.6;
        expect(bound.setControlNetSettings("layer",updatedSettings).changed && file.lastWriteStatistics().recordsWritten<=2,"control-only edit reuses lineart records");
        DocumentFile reopened; expect(reopened.open(path).ok(),"reopen");
        if(reopened.document()) {
            expect(findLineArtAsset(*reopened.document(),"lineart")->coverage[1]==0.123456789012345,"f64 lossless round trip");
            expect(renderLineArtControlMap(*reopened.document(),"layer",1).coverage==std::vector<double>({1,0.25,0,0}),"dynamic file source survives");
        }
        expect(exportPsd(doc,dir.filePath("lineart.psd").toStdString()).code==MediaIoCode::UnsupportedFeature,"PSD rejects lineart loss");
        auto orphan=doc; orphan.layers.clear(); orphan.frames.clear();
        expect(exportTimelineInterchange(orphan,dir.filePath("timeline").toStdString()).code==MediaIoCode::UnsupportedFeature,"orphan lineart interchange fails closed");
    }
    return failures ? 1 : 0;
}
