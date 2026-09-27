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
    expect(editor.insertDepthAsset({"depth",{2,2},{0,0.5,1,0.000001}}).changed,"insert depth samples");
    DepthLayer layer; layer.properties={"layer","Depth"}; layer.source=StaticSource{"depth"};
    layer.control.modelId="test/depth"; layer.control.conditioningScale=0.75;
    expect(editor.insertDepthLayer(layer).changed,"insert depth layer");
    return doc;
}
void verify(const Document &doc) {
    expect(validate(doc).ok(),"valid depth document");
    expect(layerRole(doc.layers[0])==LayerRole::ControlNet && controlNetKind(doc.layers[0])==ControlNetKind::Depth,"dedicated depth role");
    const auto map=renderDepthControlMap(doc,"layer",0);
    expect(map.ok(),"depth map renders");
    expect(map.pixels.pixels==std::vector<std::uint32_t>({0xff000000,0xff808080,0xffffffff,0xff000000}),"linear black/gray/white, no normalization");
    expect(map.values==std::vector<double>({0,0.5,1,0.000001}),"unquantized values survive");
    expect(map.occupiedPixels==std::vector<std::uint8_t>({0,1,1,1}),"tiny positive depth is not empty space");
    const auto artwork=renderFrame(doc,0);
    expect(artwork.ok() && artwork.pixels.pixels==std::vector<std::uint32_t>(4,0),"depth excluded from artwork");
    const auto preview=renderFrameLayerTiles(doc,0,0,{{canvasRegion(doc),doc.extent}});
    expect(preview.ok() && preview.role==LayerRole::ControlNet && !preview.tiles.empty(),"explicit preview available");
}
}
int main() {
    auto doc=fixture(); verify(doc); expect(layerKind(doc.layers[0])==LayerKind::StaticBitmap,"static bitmap identity");
    DocumentEditor editor(doc); const auto encoded=encodeIisc(doc); expect(encoded.ok(),"encode depth");
    const auto decoded=decodeIisc(encoded.bytes); expect(decoded.ok(),"decode depth"); if(decoded.ok()) verify(decoded.document);
    if(decoded.ok()) expect(findDepthLayer(decoded.document,"layer")->control==findDepthLayer(doc,"layer")->control,"control settings survive");
    auto display=doc; auto *displayLayer=findDepthLayer(display,"layer");
    displayLayer->properties.visible=false; displayLayer->properties.opacity=0;
    displayLayer->properties.transform.translationX=100;
    expect(renderDepthControlMap(display,"layer",0).values==renderDepthControlMap(doc,"layer",0).values,"display properties cannot alter control values");
    displayLayer->properties.frameRange=LayerFrameRange{1,1};
    expect(!renderDepthControlMap(display,"layer",0).ok() && renderDepthControlMap(display,"layer",1).ok(),"explicit frame range");
    auto wrong=doc; findDepthLayer(wrong,"layer")->source=StaticSource{"missing"};
    expect(!validate(wrong).ok(),"missing source rejected");
    wrong=doc; wrong.assets.push_back(RasterAsset{"color",makeRasterLayer(2,2)});
    findDepthLayer(wrong,"layer")->source=StaticSource{"color"};
    expect(!validate(wrong).ok(),"raster cannot substitute depth data");
    Document upgrade; upgrade.extent={1,1}; upgrade.formatVersion.minor=8; DocumentEditor upgradeEditor(upgrade);
    expect(upgradeEditor.insertDepthAsset({"new",{1,1},{0.5}}).changed && upgrade.formatVersion.minor==CurrentFormatMinor,"legacy document upgrade");
    // Corrupt independent wire fields and repair CRC to exercise the decoder itself.
    const std::vector<std::uint8_t> marker{5,5,0,0,0,'d','e','p','t','h'};
    const auto found=std::search(encoded.bytes.begin(),encoded.bytes.end(),marker.begin(),marker.end());
    expect(found!=encoded.bytes.end(),"depth asset wire record exists");
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
            expect(!decodeIisc(bytes).ok(),"corrupt depth payload fails closed with valid checksum");
        };
        rejects(offset,255); // Unknown asset tag.
        rejects(offset+18,5); // Sample count no longer matches 2x2 dimensions.
        rejects(offset+26+7,0x40); // First f64 becomes 2.0, outside [0,1].
    }
    const auto revision=editor.revision();
    for(double bad:{-0.1,1.1,std::numeric_limits<double>::quiet_NaN(),std::numeric_limits<double>::infinity()}) {
        expect(!editor.setDepthSample("depth",0,0,bad).ok(),"reject invalid depth");
        expect(editor.revision()==revision && encodeIisc(doc).bytes==encoded.bytes,"invalid edits rollback");
    }
    expect(!editor.setDepthSample("depth",2,0,0.2).ok(),"coordinate bounds");
    auto bad=*findDepthAsset(doc,"depth"); bad.values.pop_back();
    expect(!editor.replaceDepthAsset("depth",bad).ok(),"reject wrong sample count");
    auto old=doc; old.formatVersion.minor=8; expect(!encodeIisc(old).ok(),"older format rejects depth");
    SerializationLimits limits; limits.maximumDepthSamples=3;
    expect(!encodeIisc(doc,limits).ok() && !decodeIisc(encoded.bytes,limits).ok(),"sample budgets encode/decode");
    expect(!renderDepthControlMap(doc,"layer",0,3).ok(),"output allocation budget");
    BitmapEditor brush; expect(!brush.bind(doc,"depth"),"color brush cannot corrupt scalar depth");
    expect(editor.insertDepthAsset({"next",{2,2},{1,0.25,0,0}}).changed,"second frame");
    expect(editor.setKeyframedSource("layer",{{0,"depth"},{1,"next"}}).changed,"dynamic depth");
    expect(layerKind(doc.layers[0])==LayerKind::DynamicBitmap,"dynamic bitmap identity");
    expect(renderDepthControlMap(doc,"layer",1).pixels.pixels[0]==0xffffffff,"current frame selects depth");
    expect(!renderDepthControlMap(doc,"layer",2).ok(),"out of timeline");
    auto settings=findDepthLayer(doc,"layer")->control; settings.enabled=false;
    expect(editor.setControlNetSettings("layer",settings).changed && !renderDepthControlMap(doc,"layer",0).ok(),"disabled control");
    settings.enabled=true; expect(editor.setControlNetSettings("layer",settings).changed,"reenable");
    settings.guidanceEnd=0; expect(!editor.setControlNetSettings("layer",settings).ok(),"invalid settings");
    QTemporaryDir dir(QStringLiteral(IISHAREDCANVAS_TEST_OUTPUT_DIR "/depth-XXXXXX"));
    expect(dir.isValid(),"test directory");
    if(dir.isValid()) {
        DocumentFile file; const auto path=dir.filePath("depth.iisc").toStdString();
        expect(file.create(path,doc).ok(),"working file create"); DocumentEditor bound(file);
        expect(bound.setDepthSample("depth",1,0,0.123456789012345).changed,"persist exact edit");
        auto updatedSettings=findDepthLayer(*file.document(),"layer")->control; updatedSettings.conditioningScale=0.6;
        expect(bound.setControlNetSettings("layer",updatedSettings).changed && file.lastWriteStatistics().recordsWritten<=2,"control-only edit reuses depth records");
        DocumentFile reopened; expect(reopened.open(path).ok(),"reopen");
        if(reopened.document()) {
            expect(findDepthAsset(*reopened.document(),"depth")->values[1]==0.123456789012345,"f64 lossless round trip");
            expect(renderDepthControlMap(*reopened.document(),"layer",1).values==std::vector<double>({1,0.25,0,0}),"dynamic file source survives");
        }
        expect(exportPsd(doc,dir.filePath("depth.psd").toStdString()).code==MediaIoCode::UnsupportedFeature,"PSD rejects depth loss");
        auto orphan=doc; orphan.layers.clear(); orphan.frames.clear();
        expect(exportTimelineInterchange(orphan,dir.filePath("timeline").toStdString()).code==MediaIoCode::UnsupportedFeature,"orphan depth interchange fails closed");
    }
    return failures ? 1 : 0;
}
