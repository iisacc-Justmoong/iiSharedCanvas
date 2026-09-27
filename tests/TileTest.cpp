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
    expect(editor.insertTileAsset({"tile",{2,2},{{255,0,0},{0,255,0},{0,0,255},{12,34,56}}}).changed,"insert tile samples");
    TileLayer layer; layer.properties={"layer","Tile"}; layer.source=StaticSource{"tile"};
    layer.control.modelId="test/tile"; layer.control.conditioningScale=0.75;
    expect(editor.insertTileLayer(layer).changed,"insert tile layer");
    return doc;
}
void verify(const Document &doc) {
    expect(validate(doc).ok(),"valid tile document");
    expect(layerRole(doc.layers[0])==LayerRole::ControlNet && controlNetKind(doc.layers[0])==ControlNetKind::Tile,"dedicated tile role");
    const auto map=renderTileControlMap(doc,"layer",0);
    expect(map.ok(),"tile map renders");
    expect(map.pixels.pixels==std::vector<std::uint32_t>({0xffff0000,0xff00ff00,0xff0000ff,0xff0c2238}),"opaque exact RGB colors");
    expect(map.colors==std::vector<TileColor>({{255,0,0},{0,255,0},{0,0,255},{12,34,56}}),"exact prepared RGB survives");
    expect(map.region==TileRegion{0,0,2,2},"full map carries native source region");
    const auto crop=renderTileControlRegion(doc,"layer",0,{1,0,1,2},2);
    expect(crop.ok() && crop.region==TileRegion{1,0,1,2}
        && crop.pixels.pixels==std::vector<std::uint32_t>({0xff00ff00,0xff0c2238}),"bounded crop preserves exact source pixels");
    expect(crop.colors==std::vector<TileColor>({{0,255,0},{12,34,56}}),"crop colors match crop dimensions");
    const auto overlap=renderTileControlRegion(doc,"layer",0,{0,1,2,1});
    expect(overlap.ok() && overlap.pixels.pixels[1]==crop.pixels.pixels[1],"overlapping regions preserve identical conditioning");
    expect(!renderTileControlRegion(doc,"layer",0,{1,0,1,2},1).ok(),"crop output budget checked before allocation");
    for (auto region:std::vector<TileRegion>{{-1,0,1,1},{0,0,0,1},{1,1,2,1},{2147483647,0,2147483647,1}})
        expect(!renderTileControlRegion(doc,"layer",0,region).ok(),"invalid or overflow-sized crop fails closed");
    expect(!isLineControlNetLayer(doc.layers[0]),"tile is not a line control");
    const auto artwork=renderFrame(doc,0);
    expect(artwork.ok() && artwork.pixels.pixels==std::vector<std::uint32_t>(4,0),"tile excluded from artwork");
    const auto preview=renderFrameLayerTiles(doc,0,0,{{canvasRegion(doc),doc.extent}});
    expect(preview.ok() && preview.role==LayerRole::ControlNet && !preview.tiles.empty(),"explicit preview available");
}
}
int main() {
    const auto source=RasterLayer{2,2,{0xffff0000,0xff00ff00,0xff0000ff,0xff123456}};
    const auto prepared=makeTileAsset("prepared",source);
    expect(tileRasterPreview(prepared).pixels==source.pixels,"tile preparation preserves spatial detail and RGB");
    auto transparent=source; transparent.pixels[0]=0;
    bool alphaRejected=false, limited=false;
    try { (void)makeTileAsset("bad",transparent); } catch(const std::invalid_argument &) { alphaRejected=true; }
    try { (void)makeTileAsset("bad",source,3); } catch(const std::length_error &) { limited=true; }
    expect(alphaRejected && limited,"preparation rejects implicit alpha loss and oversized output");
    auto doc=fixture(); verify(doc); expect(layerKind(doc.layers[0])==LayerKind::StaticBitmap,"static bitmap identity");
    DocumentEditor editor(doc); const auto encoded=encodeIisc(doc); expect(encoded.ok(),"encode tile");
    const auto decoded=decodeIisc(encoded.bytes); expect(decoded.ok(),"decode tile"); if(decoded.ok()) verify(decoded.document);
    if(decoded.ok()) expect(findTileLayer(decoded.document,"layer")->control==findTileLayer(doc,"layer")->control,"control settings survive");
    auto display=doc; auto *displayLayer=findTileLayer(display,"layer");
    displayLayer->properties.visible=false; displayLayer->properties.opacity=0;
    displayLayer->properties.transform.translationX=100;
    expect(renderTileControlMap(display,"layer",0).colors==renderTileControlMap(doc,"layer",0).colors,"display properties cannot alter control values");
    displayLayer->properties.frameRange=LayerFrameRange{1,1};
    expect(!renderTileControlMap(display,"layer",0).ok() && renderTileControlMap(display,"layer",1).ok(),"explicit frame range");
    auto wrong=doc; findTileLayer(wrong,"layer")->source=StaticSource{"missing"};
    expect(!validate(wrong).ok(),"missing source rejected");
    wrong=doc; wrong.assets.push_back(RasterAsset{"color",makeRasterLayer(2,2)});
    findTileLayer(wrong,"layer")->source=StaticSource{"color"};
    expect(!validate(wrong).ok(),"raster cannot substitute tile data");
    Document upgrade; upgrade.extent={1,1}; upgrade.formatVersion.minor=14; DocumentEditor upgradeEditor(upgrade);
    expect(upgradeEditor.insertTileAsset({"new",{1,1},{{1,2,3}}}).changed && upgrade.formatVersion.minor==CurrentFormatMinor,"legacy document upgrade");
    // Corrupt independent wire fields and repair CRC to exercise the decoder itself.
    const std::vector<std::uint8_t> marker{12,4,0,0,0,'t','i','l','e'};
    const auto found=std::search(encoded.bytes.begin(),encoded.bytes.end(),marker.begin(),marker.end());
    expect(found!=encoded.bytes.end(),"tile asset wire record exists");
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
            expect(!decodeIisc(bytes).ok(),"corrupt tile payload fails closed with valid checksum");
        };
        rejects(offset,255); // Unknown asset tag.
        rejects(offset+17,5); // Sample count no longer matches 2x2 dimensions.
    }
    const auto revision=editor.revision();
    expect(!editor.setTileSample("tile",2,0,{1,2,3}).ok(),"coordinate bounds");
    expect(editor.revision()==revision && encodeIisc(doc).bytes==encoded.bytes,"invalid edits rollback");
    auto bad=*findTileAsset(doc,"tile"); bad.colors.pop_back();
    expect(!editor.replaceTileAsset("tile",bad).ok(),"reject wrong sample count");
    auto downgraded=encoded.bytes; downgraded[10]=14;
    expect(!decodeIisc(downgraded).ok(),"new tags rejected under old format header");
    auto old=doc; old.formatVersion.minor=14; expect(!encodeIisc(old).ok(),"older format rejects tile");
    SerializationLimits limits; limits.maximumTileSamples=3;
    expect(!encodeIisc(doc,limits).ok() && !decodeIisc(encoded.bytes,limits).ok(),"sample budgets encode/decode");
    expect(!renderTileControlMap(doc,"layer",0,3).ok(),"output allocation budget");
    BitmapEditor brush; expect(!brush.bind(doc,"tile"),"color brush cannot corrupt dedicated tile data");
    expect(editor.insertTileAsset({"next",{2,2},{{255,255,255},{1,2,3},{0,0,0},{4,5,6}}}).changed,"second frame");
    limits.maximumTileSamples=7; const auto multi=encodeIisc(doc);
    expect(!encodeIisc(doc,limits).ok() && !decodeIisc(multi.bytes,limits).ok(),"sample budget sums all assets");
    expect(editor.setKeyframedSource("layer",{{0,"tile"},{1,"next"}}).changed,"dynamic tile");
    expect(layerKind(doc.layers[0])==LayerKind::DynamicBitmap,"dynamic bitmap identity");
    expect(renderTileControlMap(doc,"layer",1).pixels.pixels[0]==0xffffffff,"current frame selects tile");
    expect(renderTileControlRegion(doc,"layer",1,{0,0,1,1}).pixels.pixels==std::vector<std::uint32_t>({0xffffffff}),"region export selects current frame");
    expect(!renderTileControlMap(doc,"layer",2).ok(),"out of timeline");
    auto settings=findTileLayer(doc,"layer")->control; settings.enabled=false;
    expect(editor.setControlNetSettings("layer",settings).changed && !renderTileControlMap(doc,"layer",0).ok(),"disabled control");
    expect(!renderTileControlRegion(doc,"layer",0,{0,0,1,1}).ok(),"disabled control also rejects region export");
    settings.enabled=true; expect(editor.setControlNetSettings("layer",settings).changed,"reenable");
    settings.guidanceEnd=0; expect(!editor.setControlNetSettings("layer",settings).ok(),"invalid settings");
    QTemporaryDir dir(QStringLiteral(IISHAREDCANVAS_TEST_OUTPUT_DIR "/tile-XXXXXX"));
    expect(dir.isValid(),"test directory");
    if(dir.isValid()) {
        DocumentFile file; const auto path=dir.filePath("tile.iisc").toStdString();
        expect(file.create(path,doc).ok(),"working file create"); DocumentEditor bound(file);
        expect(bound.setTileSample("tile",1,0,{123,45,67}).changed,"persist exact edit");
        const auto committed=encodeIisc(*file.document()).bytes; const auto committedRevision=bound.revision();
        expect(!bound.setTileSample("tile",-1,0,{0,0,0}).ok()
            && bound.revision()==committedRevision && encodeIisc(*file.document()).bytes==committed,"file-bound rejection preserves state");
        auto updatedSettings=findTileLayer(*file.document(),"layer")->control; updatedSettings.conditioningScale=0.6;
        expect(bound.setControlNetSettings("layer",updatedSettings).changed && file.lastWriteStatistics().recordsWritten<=2,"control-only edit reuses tile records");
        DocumentFile reopened; expect(reopened.open(path).ok(),"reopen");
        if(reopened.document()) {
            expect(findTileAsset(*reopened.document(),"tile")->colors[1]==TileColor{123,45,67},"RGB8 lossless round trip");
            expect(renderTileControlMap(*reopened.document(),"layer",1).colors==std::vector<TileColor>({{255,255,255},{1,2,3},{0,0,0},{4,5,6}}),"dynamic file source survives");
        }
        expect(exportPsd(doc,dir.filePath("tile.psd").toStdString()).code==MediaIoCode::UnsupportedFeature,"PSD rejects tile loss");
        auto orphan=doc; orphan.layers.clear(); orphan.frames.clear();
        expect(exportTimelineInterchange(orphan,dir.filePath("timeline").toStdString()).code==MediaIoCode::UnsupportedFeature,"orphan tile interchange fails closed");
    }
    return failures ? 1 : 0;
}
