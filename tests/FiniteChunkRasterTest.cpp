#include <Bitmap/BitmapEditor.h>
#include <Bitmap/ChunkedBitmapEditor.h>
#include <Document/DocumentHistory.h>
#include <File/DocumentFile.h>
#include <Serialization/IiscCodec.h>
#include <Render/FrameRenderer.h>
#include <Validation/Validation.h>
#include <iiFileProviderNative.h>
#include <filesystem>
#include <iostream>
#include <stdexcept>
using namespace iiSharedCanvas;
void check(bool v,const char* s){if(!v)throw std::runtime_error(s);}
Document fixture(){Document d;d.extent={512,512};d.assets.emplace_back(ChunkedRasterAsset{"paint",{}});d.layers.emplace_back(StaticBitmapLayer{{"layer","Paint"},StaticSource{"paint"}});return d;}
StrokePoint point(double x,double y){StrokePoint p;p.position={x,y};p.pressure=1;return p;}
int main()try{
 auto d=fixture();check(validate(d).ok(),"finite chunk document rejected");auto invalid=d;invalid.infiniteCanvas.chunkSize=128;check(!validate(invalid).ok(),"noncanonical finite chunk size accepted");auto oldFormat=d;oldFormat.formatVersion.minor=0;check(!validate(oldFormat).ok()&&!encodeIisc(oldFormat).ok(),"legacy format accepted finite chunk asset");
 ChunkedBitmapEditor direct(d,"paint");check(direct.isBound()&&!direct.setStrokeCanvasGrowth(true)&&!direct.pixelAt(-1,0).has_value(),"finite chunk editor allowed growth or outside sampling");
 const auto encoded=encodeIisc(d);check(encoded.ok(),"finite chunk encode failed");const auto decoded=decodeIisc(encoded.bytes);check(decoded.ok()&&decoded.document.canvasMode==CanvasMode::Finite&&decoded.document.infiniteCanvas.chunkSize==256,"finite chunk roundtrip failed");
 BitmapBrush brush;brush.argb=0xff3399cc;brush.size=8;brush.opacity=1;brush.flow=1;DocumentHistory history(d);check(history.beginStroke("paint",brush,point(20,20)).changed&&history.endStroke(point(24,20)).changed,"finite chunk stroke failed");const auto* paintedAsset=findChunkedRasterAsset(d,"paint");check(paintedAsset&&paintedAsset->chunks.size()==1,"finite stroke failed to allocate chunk");const auto paintedPixels=paintedAsset->chunks[0].pixels.pixels;const auto undo=history.undo();check(undo.changed&&findChunkedRasterAsset(d,"paint")->chunks.empty(),"finite chunk undo failed");check(history.beginStroke("paint",brush,point(600,600)).ok()&&!history.endStroke(point(601,600)).changed&&history.canRedo(),"outside clipped stroke changed redo");const auto redo=history.redo();const auto* restored=findChunkedRasterAsset(d,"paint");check(redo.changed&&restored&&restored->chunks.size()==1&&restored->chunks[0].pixels.pixels==paintedPixels,"finite chunk redo changed pixels");
 check(history.beginStroke("paint",brush,point(511,100)).changed&&history.endStroke(point(520,100)).ok(),"finite edge stroke failed");const auto* edgeAsset=findChunkedRasterAsset(d,"paint");check(edgeAsset&&findRasterChunk(*edgeAsset,1,0)&&!findRasterChunk(*edgeAsset,2,0)&&d.extent==CanvasExtent{512,512},"finite edge stroke allocated outside the canvas");check(history.undo().changed&&history.canRedo(),"finite edge undo failed");
 const auto before=encodeIisc(d).bytes;check(!history.beginStroke("paint",brush,point(530,20),StrokeCanvasMode::Grow).ok()&&encodeIisc(d).bytes==before&&history.canRedo()&&d.extent==CanvasExtent{512,512},"finite canvas growth accepted or redo changed");
 iiFileProvider::native::TemporaryDirectory tmp(std::filesystem::path(IISHAREDCANVAS_TEST_OUTPUT_DIR));check(tmp.isValid(),"temporary directory failed");auto path=(tmp.path()/"finite-chunks.iisc").string();DocumentFile file;check(file.create(path,fixture()).ok(),"finite chunk working file failed");DocumentHistory fileHistory(file);check(fileHistory.beginStroke("paint",brush,point(20,20)).changed&&fileHistory.endStroke(point(24,20)).changed,"finite chunk file stroke failed");check(fileHistory.undo().changed&&fileHistory.redo().changed,"finite chunk file history failed");Document reopened;check(DocumentFile::readSnapshot(path,reopened).ok(),"finite chunk reopen failed");const auto rendered=renderFrameRegion(reopened,0,{{22,20},{1,1}},{1,1});check(rendered.ok()&&(rendered.pixels.pixels[0]>>24)>0,"finite chunk reopen changed painted pixels");
 std::cout<<"FINITE_CHUNK_RASTER_OK\n";return 0;
}catch(const std::exception& e){std::cerr<<e.what()<<"\n";return 1;}
