#include <iiSharedCanvas.h>
#include <QTemporaryDir>
#include <algorithm>
#include <iostream>
#include <limits>
using namespace iiSharedCanvas;
namespace {
int failures=0;
void expect(bool ok,const char *message){if(!ok){std::cerr<<message<<'\n';++failures;}}
Document fixture(){
    Document doc;doc.extent={5,5};doc.timeline.frameCount=2;DocumentEditor editor(doc);
    expect(editor.insertMlsdAsset({"lines",{5,5},{{"horizontal",0,0,1,0,1,true},{"vertical",0.5,0,0.5,1,0.8,true}}}).changed,"insert MLSD geometry");
    MlsdLayer layer;layer.properties={"layer","MLSD"};layer.source=StaticSource{"lines"};layer.control.modelId="test/mlsd";
    expect(editor.insertMlsdLayer(layer).changed,"insert MLSD layer");return doc;
}
void verify(const Document &doc){
    expect(validate(doc).ok(),"valid MLSD document");
    expect(isLineControlNetLayer(doc.layers[0]) && controlNetKind(doc.layers[0])==ControlNetKind::Mlsd,"line control identity");
    auto map=renderMlsdControlMap(doc,"layer",0);expect(map.ok(),"render MLSD");
    auto expected=std::vector<std::uint32_t>(25,0xff000000);
    for(std::size_t i=0;i<5;++i){expected[i]=0xffffffff;expected[i*5+2]=0xffffffff;}
    expect(map.pixels.pixels==expected && map.segments.size()==2,"exact white straight segments on opaque black");
    const auto artwork=renderFrame(doc,0);expect(artwork.ok() && artwork.pixels.pixels==std::vector<std::uint32_t>(25,0),"excluded from artwork");
    auto preview=renderFrameLayerTiles(doc,0,0,{{canvasRegion(doc),doc.extent}});
    expect(preview.ok() && preview.role==LayerRole::ControlNet && !preview.tiles.empty(),"explicit vector preview");
}
}
int main(){
    auto doc=fixture();verify(doc);DocumentEditor editor(doc);
    expect(layerKind(doc.layers[0])==LayerKind::StaticVector,"MLSD is native vector content");
    auto encoded=encodeIisc(doc);expect(encoded.ok(),"snapshot encode");auto decoded=decodeIisc(encoded.bytes);
    expect(decoded.ok(),"snapshot decode");if(decoded.ok()){verify(decoded.document);expect(findMlsdAsset(decoded.document,"lines")->segments==findMlsdAsset(doc,"lines")->segments,"all segment fields round trip");}
    const std::vector<std::uint8_t> marker{9,5,0,0,0,'l','i','n','e','s'};
    const auto located=std::search(encoded.bytes.begin(),encoded.bytes.end(),marker.begin(),marker.end());
    expect(located!=encoded.bytes.end(),"MLSD native asset record");
    if(located!=encoded.bytes.end()){
        const auto offset=static_cast<std::size_t>(located-encoded.bytes.begin());
        const auto corrupt=[&](std::size_t at,std::uint8_t value){
            auto bytes=encoded.bytes;bytes.at(at)=value;std::uint32_t crc=0xffffffffU;
            for(std::size_t i=IiscHeaderSize;i<bytes.size();++i){crc^=bytes[i];for(int bit=0;bit<8;++bit)crc=(crc>>1U)^(0xedb88320U&(0U-(crc&1U)));}
            crc^=0xffffffffU;for(unsigned i=0;i<4;++i)bytes[24+i]=static_cast<std::uint8_t>(crc>>(8*i));
            expect(!decodeIisc(bytes).ok(),"invalid MLSD payload rejected despite valid checksum");
        };
        corrupt(offset,255);
        corrupt(offset+21,0xff); // Huge declared segment count; reject before allocating.
        corrupt(offset+22+4+10+7,0x40); // First x1 becomes 2.0, outside the normalized domain.
        corrupt(offset+22+4+10+40,2); // Invalid enabled byte.
    }
    auto reverse=*findMlsdAsset(doc,"lines");
    for(auto &line:reverse.segments){std::swap(line.x1,line.x2);std::swap(line.y1,line.y2);}
    expect(mlsdRasterPreview(reverse).pixels==renderMlsdControlMap(doc,"layer",0).pixels.pixels,"reversed horizontal/vertical endpoints rasterize correctly");
    reverse.segments[1].enabled=false;expect(mlsdRasterPreview(reverse).pixels[22]==0xff000000,"disabled segment omitted");
    auto disabledDoc=doc;findMlsdAsset(disabledDoc,"lines")->segments[1].enabled=false;
    auto disabledRestored=decodeIisc(encodeIisc(disabledDoc).bytes);
    expect(disabledRestored.ok() && !findMlsdAsset(disabledRestored.document,"lines")->segments[1].enabled,"disabled segment remains disabled after saving");
    auto descending=mlsdRasterPreview({"descending",{5,5},{{"line",0,1,1,0,1,true}}});
    auto descendingExpected=std::vector<std::uint32_t>(25,0xff000000);
    for(std::size_t i=0;i<5;++i)descendingExpected[(4-i)*5+i]=0xffffffff;
    expect(descending.pixels==descendingExpected,"negative slope line covers expected pixels");
    expect(mlsdRasterPreview({"tiny",{1,1},{{"line",0,0,1,1,1,true}}}).pixels==std::vector<std::uint32_t>({0xffffffff}),"subpixel segment collapses safely at one-pixel output");
    MlsdRenderOptions options;options.minimumConfidence=0.9;
    auto filtered=renderMlsdControlMap(doc,"layer",0,options);
    expect(filtered.ok() && filtered.segments.size()==1 && filtered.pixels.pixels[22]==0xff000000,"confidence filters geometry");
    options={};options.maximumPixels=24;expect(!renderMlsdControlMap(doc,"layer",0,options).ok(),"pixel budget");
    options={};options.maximumRasterSteps=9;expect(!renderMlsdControlMap(doc,"layer",0,options).ok(),"raster work budget counts overlapping lines too");
    options={};options.minimumConfidence=std::numeric_limits<double>::quiet_NaN();expect(!renderMlsdControlMap(doc,"layer",0,options).ok(),"invalid render option");
    auto display=doc;auto *layer=findMlsdLayer(display,"layer");layer->properties.visible=false;layer->properties.opacity=0;layer->properties.transform.translationX=99;
    expect(renderMlsdControlMap(display,"layer",0).pixels.pixels==renderMlsdControlMap(doc,"layer",0).pixels.pixels,"control ignores display properties");
    layer->properties.frameRange=LayerFrameRange{1,1};expect(!renderMlsdControlMap(display,"layer",0).ok(),"frame range enforced");
    const auto revision=editor.revision();
    auto segment=findMlsdAsset(doc,"lines")->segments[0];
    for(double bad:{-0.1,1.1,std::numeric_limits<double>::quiet_NaN(),std::numeric_limits<double>::infinity()}){
        auto changed=segment;changed.x1=bad;expect(!editor.setMlsdSegment("lines","horizontal",changed).ok(),"reject invalid coordinates");
        expect(editor.revision()==revision && encodeIisc(doc).bytes==encoded.bytes,"rejected edits atomic");
    }
    auto bad=segment;bad.x2=bad.x1;bad.y2=bad.y1;expect(!editor.setMlsdSegment("lines","horizontal",bad).ok(),"zero length rejected");
    bad=segment;bad.confidence=2;expect(!editor.setMlsdSegment("lines","horizontal",bad).ok(),"invalid confidence");
    bad=segment;bad.id="renamed";expect(!editor.setMlsdSegment("lines","horizontal",bad).ok(),"identity preserved");
    auto asset=*findMlsdAsset(doc,"lines");asset.segments.push_back(segment);expect(!editor.replaceMlsdAsset("lines",asset).ok(),"duplicate segment ids rejected");
    asset.segments.clear();expect(editor.insertMlsdAsset({"empty",{5,5},{}}).changed,"empty scene is valid");
    expect(mlsdRasterPreview(*findMlsdAsset(doc,"empty")).pixels==std::vector<std::uint32_t>(25,0xff000000),"empty preview is black");
    Document legacy;legacy.extent={1,1};legacy.formatVersion.minor=11;DocumentEditor legacyEditor(legacy);
    expect(!legacyEditor.insertMlsdAsset({"bad",{1,1},{{"line",-1,0,1,1,1,true}}}).ok() && legacy.formatVersion.minor==11 && legacy.assets.empty(),"invalid insertion restores legacy version and assets");
    expect(legacyEditor.insertMlsdAsset({"new",{1,1},{}}).changed && legacy.formatVersion.minor==CurrentFormatMinor,"valid insertion upgrades legacy documents");
    auto old=doc;old.formatVersion.minor=11;expect(!encodeIisc(old).ok(),"old format rejects MLSD");
    SerializationLimits limits;limits.maximumMlsdSegments=1;expect(!encodeIisc(doc,limits).ok() && !decodeIisc(encoded.bytes,limits).ok(),"segment budget encode/decode");
    auto wrong=doc;wrong.assets.push_back(CannyAsset{"edge",{5,5},std::vector<std::uint8_t>(25)});findMlsdLayer(wrong,"layer")->source=StaticSource{"edge"};expect(!validate(wrong).ok(),"binary edge masks cannot substitute MLSD geometry");
    expect(editor.insertMlsdAsset({"next",{5,5},{{"diagonal",0,0,1,1,1,true}}}).changed,"second frame");
    expect(editor.setKeyframedSource("layer",{{0,"lines"},{1,"next"}}).changed && layerKind(doc.layers[0])==LayerKind::DynamicVector,"dynamic vector identity");
    auto diagonal=renderMlsdControlMap(doc,"layer",1);expect(diagonal.ok() && diagonal.pixels.pixels[24]==0xffffffff && diagonal.pixels.pixels[4]==0xff000000,"diagonal raster and frame selection");
    expect(!renderMlsdControlMap(doc,"layer",2).ok(),"timeline bounds");
    auto settings=findMlsdLayer(doc,"layer")->control;settings.enabled=false;expect(editor.setControlNetSettings("layer",settings).changed && !renderMlsdControlMap(doc,"layer",0).ok(),"disabled control");
    settings.enabled=true;expect(editor.setControlNetSettings("layer",settings).changed,"reenable");
    QTemporaryDir directory(QStringLiteral(IISHAREDCANVAS_TEST_OUTPUT_DIR "/mlsd-XXXXXX"));expect(directory.isValid(),"test directory");
    if(directory.isValid()){
        const auto path=directory.filePath("work.iisc").toStdString();DocumentFile file;expect(file.create(path,doc).ok(),"working file create");DocumentEditor bound(file);
        segment.y1=0.25;segment.y2=0.25;segment.confidence=0.123456789012345;expect(bound.setMlsdSegment("lines","horizontal",segment).changed,"persist segment edit");
        auto editedMap=renderMlsdControlMap(*file.document(),"layer",0);expect(editedMap.ok() && editedMap.pixels.pixels[0]==0xff000000 && editedMap.pixels.pixels[5]==0xffffffff,"endpoint edit changes actual control pixels");
        settings.conditioningScale=0.6;expect(bound.setControlNetSettings("layer",settings).changed && file.lastWriteStatistics().recordsWritten<=2,"control edits reuse geometry records");
        DocumentFile reopened;expect(reopened.open(path).ok(),"working file open");if(reopened.document()){
            expect(findMlsdAsset(*reopened.document(),"lines")->segments[0]==segment,"precise segment geometry/metadata retained");
            expect(renderMlsdControlMap(*reopened.document(),"layer",1).pixels.pixels==diagonal.pixels.pixels,"dynamic geometry survives file round trip");
        }
        expect(exportPsd(doc,directory.filePath("mlsd.psd").toStdString()).code==MediaIoCode::UnsupportedFeature,"PSD rejects semantic loss");
        auto orphan=doc;orphan.layers.clear();orphan.frames.clear();expect(exportTimelineInterchange(orphan,directory.filePath("timeline").toStdString()).code==MediaIoCode::UnsupportedFeature,"orphan MLSD interchange rejected");
    }
    return failures?1:0;
}
