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
    expect(editor.insertShuffleAsset({"shuffle",{2,2},{{255,0,0},{0,255,0},{0,0,255},{12,34,56}}}).changed,"insert shuffle samples");
    ShuffleLayer layer; layer.properties={"layer","Shuffle"}; layer.source=StaticSource{"shuffle"};
    layer.control.modelId="test/shuffle"; layer.control.conditioningScale=0.75;
    expect(editor.insertShuffleLayer(layer).changed,"insert shuffle layer");
    return doc;
}
void verify(const Document &doc) {
    expect(validate(doc).ok(),"valid shuffle document");
    expect(layerRole(doc.layers[0])==LayerRole::ControlNet && controlNetKind(doc.layers[0])==ControlNetKind::Shuffle,"dedicated shuffle role");
    const auto map=renderShuffleControlMap(doc,"layer",0);
    expect(map.ok(),"shuffle map renders");
    expect(map.pixels.pixels==std::vector<std::uint32_t>({0xffff0000,0xff00ff00,0xff0000ff,0xff0c2238}),"opaque exact RGB colors");
    expect(map.colors==std::vector<ShuffleColor>({{255,0,0},{0,255,0},{0,0,255},{12,34,56}}),"exact prepared RGB survives");
    expect(!isLineControlNetLayer(doc.layers[0]),"shuffle is not a line control");
    const auto artwork=renderFrame(doc,0);
    expect(artwork.ok() && artwork.pixels.pixels==std::vector<std::uint32_t>(4,0),"shuffle excluded from artwork");
    const auto preview=renderFrameLayerTiles(doc,0,0,{{canvasRegion(doc),doc.extent}});
    expect(preview.ok() && preview.role==LayerRole::ControlNet && !preview.tiles.empty(),"explicit preview available");
}
}
int main() {
    const auto source=RasterLayer{2,2,{0xffff0000,0xff00ff00,0xff0000ff,0xffffffff}};
    const auto remapped=makeShuffleAsset("remap",source,{2,1},{{1,1},{0.5,0.5}});
    expect(remapped.colors==std::vector<ShuffleColor>({{255,255,255},{128,128,128}}),"explicit normalized remap uses bilinear RGB sampling");
    const auto identity=makeShuffleAsset("identity",source,{2,2},{{0,0},{1,0},{0,1},{1,1}});
    expect(shuffleRasterPreview(identity).pixels==source.pixels,"identity field preserves source pixels");
    for (auto uv:std::vector<ShuffleCoordinate>{{-0.1,0},{1.1,0},{0,std::numeric_limits<double>::quiet_NaN()}}) {
        bool rejected=false;
        try { (void)makeShuffleAsset("bad",source,{1,1},{uv}); } catch(const std::invalid_argument &) { rejected=true; }
        expect(rejected,"invalid remap coordinate rejected");
    }
    bool limited=false;
    try { (void)makeShuffleAsset("bad",source,{2,2},{{0,0},{1,0},{0,1},{1,1}},3); } catch(const std::length_error &) { limited=true; }
    expect(limited,"remap output budget");
    auto transparent=source; transparent.pixels[0]=0;
    bool rejectedAlpha=false;
    try { (void)makeShuffleAsset("bad",transparent,{1,1},{{0,0}}); } catch(const std::invalid_argument &) { rejectedAlpha=true; }
    expect(rejectedAlpha,"transparent input requires explicit caller compositing");
    const auto single=makeShuffleAsset("single",RasterLayer{1,1,{0xff123456}},{2,1},{{0,0},{1,1}});
    expect(single.colors==std::vector<ShuffleColor>({{18,52,86},{18,52,86}}),"single source pixel and edge coordinates");
    bool badCount=false;
    try { (void)makeShuffleAsset("bad",source,{2,1},{{0,0}}); } catch(const std::invalid_argument &) { badCount=true; }
    expect(badCount,"remap count mismatch");
    auto doc=fixture(); verify(doc); expect(layerKind(doc.layers[0])==LayerKind::StaticBitmap,"static bitmap identity");
    DocumentEditor editor(doc); const auto encoded=encodeIisc(doc); expect(encoded.ok(),"encode shuffle");
    const auto decoded=decodeIisc(encoded.bytes); expect(decoded.ok(),"decode shuffle"); if(decoded.ok()) verify(decoded.document);
    if(decoded.ok()) expect(findShuffleLayer(decoded.document,"layer")->control==findShuffleLayer(doc,"layer")->control,"control settings survive");
    auto display=doc; auto *displayLayer=findShuffleLayer(display,"layer");
    displayLayer->properties.visible=false; displayLayer->properties.opacity=0;
    displayLayer->properties.transform.translationX=100;
    expect(renderShuffleControlMap(display,"layer",0).colors==renderShuffleControlMap(doc,"layer",0).colors,"display properties cannot alter control values");
    displayLayer->properties.frameRange=LayerFrameRange{1,1};
    expect(!renderShuffleControlMap(display,"layer",0).ok() && renderShuffleControlMap(display,"layer",1).ok(),"explicit frame range");
    auto wrong=doc; findShuffleLayer(wrong,"layer")->source=StaticSource{"missing"};
    expect(!validate(wrong).ok(),"missing source rejected");
    wrong=doc; wrong.assets.push_back(RasterAsset{"color",makeRasterLayer(2,2)});
    findShuffleLayer(wrong,"layer")->source=StaticSource{"color"};
    expect(!validate(wrong).ok(),"raster cannot substitute shuffle data");
    Document upgrade; upgrade.extent={1,1}; upgrade.formatVersion.minor=13; DocumentEditor upgradeEditor(upgrade);
    expect(upgradeEditor.insertShuffleAsset({"new",{1,1},{{1,2,3}}}).changed && upgrade.formatVersion.minor==CurrentFormatMinor,"legacy document upgrade");
    // Corrupt independent wire fields and repair CRC to exercise the decoder itself.
    const std::vector<std::uint8_t> marker{11,7,0,0,0,'s','h','u','f','f','l','e'};
    const auto found=std::search(encoded.bytes.begin(),encoded.bytes.end(),marker.begin(),marker.end());
    expect(found!=encoded.bytes.end(),"shuffle asset wire record exists");
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
            expect(!decodeIisc(bytes).ok(),"corrupt shuffle payload fails closed with valid checksum");
        };
        rejects(offset,255); // Unknown asset tag.
        rejects(offset+20,5); // Sample count no longer matches 2x2 dimensions.
    }
    const auto revision=editor.revision();
    expect(!editor.setShuffleSample("shuffle",2,0,{1,2,3}).ok(),"coordinate bounds");
    expect(editor.revision()==revision && encodeIisc(doc).bytes==encoded.bytes,"invalid edits rollback");
    auto bad=*findShuffleAsset(doc,"shuffle"); bad.colors.pop_back();
    expect(!editor.replaceShuffleAsset("shuffle",bad).ok(),"reject wrong sample count");
    auto old=doc; old.formatVersion.minor=13; expect(!encodeIisc(old).ok(),"older format rejects shuffle");
    SerializationLimits limits; limits.maximumShuffleSamples=3;
    expect(!encodeIisc(doc,limits).ok() && !decodeIisc(encoded.bytes,limits).ok(),"sample budgets encode/decode");
    expect(!renderShuffleControlMap(doc,"layer",0,3).ok(),"output allocation budget");
    BitmapEditor brush; expect(!brush.bind(doc,"shuffle"),"color brush cannot corrupt dedicated shuffle data");
    expect(editor.insertShuffleAsset({"next",{2,2},{{255,255,255},{1,2,3},{0,0,0},{4,5,6}}}).changed,"second frame");
    limits.maximumShuffleSamples=7; const auto multi=encodeIisc(doc);
    expect(!encodeIisc(doc,limits).ok() && !decodeIisc(multi.bytes,limits).ok(),"sample budget sums all assets");
    expect(editor.setKeyframedSource("layer",{{0,"shuffle"},{1,"next"}}).changed,"dynamic shuffle");
    expect(layerKind(doc.layers[0])==LayerKind::DynamicBitmap,"dynamic bitmap identity");
    expect(renderShuffleControlMap(doc,"layer",1).pixels.pixels[0]==0xffffffff,"current frame selects shuffle");
    expect(!renderShuffleControlMap(doc,"layer",2).ok(),"out of timeline");
    auto settings=findShuffleLayer(doc,"layer")->control; settings.enabled=false;
    expect(editor.setControlNetSettings("layer",settings).changed && !renderShuffleControlMap(doc,"layer",0).ok(),"disabled control");
    settings.enabled=true; expect(editor.setControlNetSettings("layer",settings).changed,"reenable");
    settings.guidanceEnd=0; expect(!editor.setControlNetSettings("layer",settings).ok(),"invalid settings");
    QTemporaryDir dir(QStringLiteral(IISHAREDCANVAS_TEST_OUTPUT_DIR "/shuffle-XXXXXX"));
    expect(dir.isValid(),"test directory");
    if(dir.isValid()) {
        DocumentFile file; const auto path=dir.filePath("shuffle.iisc").toStdString();
        expect(file.create(path,doc).ok(),"working file create"); DocumentEditor bound(file);
        expect(bound.setShuffleSample("shuffle",1,0,{123,45,67}).changed,"persist exact edit");
        const auto committed=encodeIisc(*file.document()).bytes; const auto committedRevision=bound.revision();
        expect(!bound.setShuffleSample("shuffle",-1,0,{0,0,0}).ok()
            && bound.revision()==committedRevision && encodeIisc(*file.document()).bytes==committed,"file-bound rejection preserves state");
        auto updatedSettings=findShuffleLayer(*file.document(),"layer")->control; updatedSettings.conditioningScale=0.6;
        expect(bound.setControlNetSettings("layer",updatedSettings).changed && file.lastWriteStatistics().recordsWritten<=2,"control-only edit reuses shuffle records");
        DocumentFile reopened; expect(reopened.open(path).ok(),"reopen");
        if(reopened.document()) {
            expect(findShuffleAsset(*reopened.document(),"shuffle")->colors[1]==ShuffleColor{123,45,67},"RGB8 lossless round trip");
            expect(renderShuffleControlMap(*reopened.document(),"layer",1).colors==std::vector<ShuffleColor>({{255,255,255},{1,2,3},{0,0,0},{4,5,6}}),"dynamic file source survives");
        }
        expect(exportPsd(doc,dir.filePath("shuffle.psd").toStdString()).code==MediaIoCode::UnsupportedFeature,"PSD rejects shuffle loss");
        auto orphan=doc; orphan.layers.clear(); orphan.frames.clear();
        expect(exportTimelineInterchange(orphan,dir.filePath("timeline").toStdString()).code==MediaIoCode::UnsupportedFeature,"orphan shuffle interchange fails closed");
    }
    return failures ? 1 : 0;
}
