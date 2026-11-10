#include <Bitmap/TiledMaskEditor.h>
#include <Bitmap/BitmapEditor.h>
#include <Document/Document.h>
#include <stdexcept>
#include <iostream>
using namespace iiSharedCanvas;
void check(bool value,const char* message){if(!value)throw std::runtime_error(message);}
StrokePoint point(double x,double y,double t){StrokePoint p;p.position={x,y};p.time=t;p.pressure=1;return p;}
int main() try {
 const CanvasRegion domain{{-19,23},{173,131}};
 auto full=TiledMask::rectangle(domain,{-19,23},{154,154},false);check(bool(full),"mask setup failed");
 TiledMaskEditor editor(*full);BitmapBrush brush;brush.size=12;brush.opacity=.65;brush.flow=.8;brush.argb=0xffffffffU;brush.eraser=true;
 Document dense;dense.extent=domain.extent;dense.assets.emplace_back(RasterAsset{"mask",makeRasterLayer(173,131,0xffffffffU)});dense.layers.emplace_back(StaticBitmapLayer{{"layer","Mask"},StaticSource{"mask"}});BitmapEditor reference(dense,"mask");check(reference.setBrush(brush),"dense brush setup failed");
 auto first=point(-5,40,1),last=point(95,100,2);auto localFirst=point(14,17,1),localLast=point(114,77,2);
 check(editor.begin(first,brush)&&reference.beginStrokeInput(localFirst),"begin failed");check(editor.coverage(-5,40)<255,"live preview missing");
 check(editor.end(last)&&reference.endStrokeInput(localLast),"end failed");const auto* expected=reference.pixels();check(expected,"dense pixels missing");
 for(int y=0;y<131;++y)for(int x=0;x<173;++x)check(editor.coverage(x-19,y+23)==std::uint8_t(expected->pixels[std::size_t(y)*173+x]>>24),"tiled stroke differs from dense brush");
 check(full->coverage(-5,40)==255&&editor.changed()&&editor.undo()&&editor.coverage(-5,40)==255&&editor.redo()&&editor.coverage(-5,40)<255,"copy or local history failed");
 check(editor.undo()&&editor.canRedo(),"redo fixture failed");check(editor.begin(point(5,50,3),brush),"cancel fixture failed");editor.cancel();check(editor.canRedo()&&editor.coverage(5,50)==255,"cancel changed mask or redo");check(editor.redo(),"redo after cancel failed");
 const CanvasRegion dynamicsDomain{{0,0},{128,128}};auto dynamicsFull=TiledMask::rectangle(dynamicsDomain,{0,0},{128,128},false);check(bool(dynamicsFull),"dynamics mask failed");TiledMaskEditor dynamicEditor(*dynamicsFull);Document dynamicDoc;dynamicDoc.extent=dynamicsDomain.extent;dynamicDoc.assets.emplace_back(RasterAsset{"dynamic",makeRasterLayer(128,128,0xffffffffU)});dynamicDoc.layers.emplace_back(StaticBitmapLayer{{"dynamic-layer","Mask"},StaticSource{"dynamic"}});BitmapEditor dynamicReference(dynamicDoc,"dynamic");BitmapBrush dynamicBrush=brush;dynamicBrush.engineState=BrushState{};dynamicBrush.engineState->material.scatter.enabled=true;dynamicBrush.engineState->material.scatter.radius=5;dynamicBrush.engineState->material.scatter.count=3;dynamicBrush.engineState->rasterizer.symmetry.mode=BrushSymmetryMode::Vertical;dynamicBrush.engineState->rasterizer.symmetry.center={64,64};check(dynamicReference.setBrush(dynamicBrush),"dynamics brush invalid");auto d0=point(30,30,1),d1=point(45,60,1.5),d2=point(60,90,2);check(dynamicEditor.begin(d0,dynamicBrush)&&dynamicReference.beginStrokeInput(d0)&&dynamicEditor.extend(d1)&&dynamicReference.continueStrokeInput(d1)&&dynamicEditor.end(d2)&&dynamicReference.endStrokeInput(d2),"scatter symmetry stroke failed");const auto* dynamicPixels=dynamicReference.pixels();for(int y=0;y<128;++y)for(int x=0;x<128;++x)check(dynamicEditor.coverage(x,y)==std::uint8_t(dynamicPixels->pixels[std::size_t(y)*128+x]>>24),"scatter symmetry differs from dense brush");
 const CanvasRegion huge{{-500000,-500000},{1000000,1000000}};auto sparse=TiledMask::rectangle(huge,{-500000,-500000},{500000,500000},false);check(bool(sparse),"large setup failed");TiledMaskEditor big(*sparse);check(big.begin(point(-2,-2,1),brush)&&big.end(point(4,4,2)),"large stroke failed");check(big.coverage(0,0)<255&&big.coverage(100000,100000)==255&&big.mask().dense_bytes()<128*1024,"large stroke lost sparse bounds");check(big.undo()&&big.coverage(0,0)==255&&big.redo()&&big.coverage(0,0)<255,"large stroke history failed");
 check(big.undo()&&big.canRedo(),"no-op redo fixture failed");BitmapBrush noOp=brush;noOp.opacity=0;check(big.begin(point(2,2,3),noOp)&&big.end(point(2,2,4))&&big.canRedo()&&big.coverage(0,0)==255,"no-op stroke lost redo");check(big.redo(),"redo after no-op failed");TiledMaskEditor tiny(*sparse,1024);check(!tiny.begin(point(0,0,1),brush)&&!tiny.strokeActive()&&!tiny.changed()&&tiny.coverage(0,0)==255,"working budget did not reject atomically");
 BitmapBrush invalid=brush;invalid.size=-1;check(!big.begin(point(1,1,4),invalid)&&big.canUndo(),"invalid brush changed history");auto nan=point(1,1,4);nan.pressure=2;check(!big.begin(nan,brush)&&!big.strokeActive()&&big.canUndo(),"invalid sample changed history");
 std::cout<<"TILED_MASK_EDITOR_OK\n";return 0;
}catch(const std::exception& e){std::cerr<<e.what()<<'\n';return 1;}
