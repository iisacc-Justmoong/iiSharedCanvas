#include <iiSharedCanvas.h>
#include <iiFileProviderNative.h>
#include <filesystem>
#include <iostream>
#include <stdexcept>
using namespace iiSharedCanvas;
void check(bool value,const char* message){if(!value)throw std::runtime_error(message);}
int main() try {
 Document d;d.extent={16,12};d.timeline.frameCount=2;
 auto raster=makeRasterLayer(16,12,0);auto mask=makeRasterLayer(16,12,0xff000000U);
 for(int y=0;y<12;++y)for(int x=0;x<16;++x){raster.pixels[std::size_t(y)*16+x]=0xff000000U|std::uint32_t(x*13)<<16|std::uint32_t(y*17)<<8;mask.pixels[std::size_t(y)*16+x]=0xff000000U|std::uint32_t(x*16)*0x010101U;}
 d.assets.emplace_back(RasterAsset{"source",raster});d.assets.emplace_back(RasterAsset{"mask",mask});d.assets.emplace_back(RasterAsset{"second",makeRasterLayer(16,12,0x800000ffU)});
 LayerProperties base{"base","Base"};base.transform.translationX=1;base.transform.translationY=1;LayerMask coverage;coverage.assetId="mask";coverage.linkedToLayer=false;coverage.transform.translationX=2;coverage.outsideCoverage=0;base.appearance.rasterMask=coverage;
 d.layers.emplace_back(StaticBitmapLayer{base,StaticSource{"source"}});
 LayerProperties animated{"animated","Animated"};animated.opacity=.6;animated.motion={MotionKeyframe{.frame=0,.value={.position={1,0}}},MotionKeyframe{.frame=1,.value={.position={2,1}}}};
 d.layers.emplace_back(DynamicBitmapLayer{animated,KeyframedSource{{0,1}}});d.frames={{0,{{"animated","source"}}},{1,{{"animated","second"}}}};
 d.artboards={{"board","Board",{{4,2},{6,6}},0x80336699U}};LayerProperties grouped{"grouped","Grouped"};grouped.artboardId="board";grouped.opacity=.5;grouped.appearance.rasterMask=coverage;d.layers.emplace_back(StaticBitmapLayer{grouped,StaticSource{"source"}});
 check(validate(d).ok(),"fixture invalid");const auto initial=d;const CanvasRegion crop{{3,2},{8,7}};auto frame0=renderFrameRegion(d,0,crop,crop.extent);auto frame1=renderFrameRegion(d,1,crop,crop.extent);check(frame0.ok()&&frame1.ok(),"before render failed");
 const auto originalPixels=findRasterAsset(d,"source")->pixels.pixels;DocumentEditor editor(d);const auto rev=editor.revision();check(editor.reframeCanvas(crop).changed,"reframe did not change document");
 check(d.extent==crop.extent && findRasterAsset(d,"source")->pixels.pixels==originalPixels,"reframe deleted source pixels");
 auto after0=renderFrame(d,0),after1=renderFrame(d,1);check(after0.ok()&&after1.ok()&&after0.pixels.pixels==frame0.pixels.pixels&&after1.pixels.pixels==frame1.pixels.pixels,"reframe changed frame or mask pixels");
 auto encoded=encodeIisc(d);auto decoded=decodeIisc(encoded.bytes);check(encoded.ok()&&decoded.ok()&&renderFrame(decoded.document,1).pixels.pixels==after1.pixels.pixels,"reframe roundtrip failed");
 auto before=encodeIisc(d);const auto now=editor.revision();check(!editor.reframeCanvas({{-1,0},{2,2}}).ok() && !editor.reframeCanvas({{0,0},{0,1}}).ok() && !editor.reframeCanvas({{0,0},{20,2}}).ok(),"invalid crop accepted");check(editor.revision()==now && encodeIisc(d).bytes==before.bytes,"invalid crop mutated document");
 check(editor.reframeCanvas({{0,0},d.extent}).ok() && editor.revision()==now,"full canvas reframe was not a no-op");
 layerProperties(d.layers[0]).blendMode=RasterBlendMode::Dissolve;before=encodeIisc(d);const auto beforeDissolve=editor.revision();check(!editor.reframeCanvas({{1,1},{6,5}}).ok() && editor.revision()==beforeDissolve && encodeIisc(d).bytes==before.bytes,"dissolve crop was not rejected atomically");
 iiFileProvider::native::TemporaryDirectory temporary(std::filesystem::temp_directory_path());check(temporary.isValid(),"temp unavailable");DocumentFile file;const auto filePath=temporary.path()/"crop.iisc";check(file.create(filePath.string(),initial).ok(),"working file create failed");DocumentEditor persistent(file);check(persistent.reframeCanvas(crop).changed,"file reframe failed");file.close();check(file.open(filePath.string()).ok(),"file reopen failed");check(file.document() && renderFrame(*file.document(),1).pixels.pixels==frame1.pixels.pixels,"reopened crop changed pixels");file.close();
 Document infinite;infinite.extent={16,12};infinite.canvasMode=CanvasMode::Infinite;DocumentEditor other(infinite);check(!other.reframeCanvas({{0,0},{4,4}}).ok(),"infinite canvas accepted");
 check(rev<now,"successful reframe did not increment revision");std::cout<<"CANVAS_REFRAME_OK\n";return 0;
}catch(const std::exception& e){std::cerr<<e.what()<<"\n";return 1;}
