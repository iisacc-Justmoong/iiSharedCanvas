#include <iiSharedCanvas.h>
#include <cmath>
#include <iostream>
#include <limits>
#include <stdexcept>
using namespace iiSharedCanvas;
void check(bool v,const char* m){if(!v)throw std::runtime_error(m);}
int main() try {
 auto image=makeRasterLayer(64,64,0xff000000U);for(int y=0;y<64;++y)for(int x=32;x<64;++x)image.pixels[std::size_t(y)*64+x]=0xffffffffU;
 MagneticPathOptions opts;opts.radius=8;auto path=magneticSelectionPath(image,{28,4},{37,59},opts);check(path.ok() && path.attracted && path.points.size()>3,"edge path failed");
 for(auto p:path.points)check(std::abs(p.x-32)<=3,"path departed the vertical edge");
 auto repeat=magneticSelectionPath(image,{28,4},{37,59},opts);check(repeat.ok() && repeat.points==path.points,"path must be deterministic");
 auto corner=makeRasterLayer(64,64,0xff000000U);for(int y=32;y<64;++y)for(int x=32;x<64;++x)corner.pixels[std::size_t(y)*64+x]=0xffffffffU;
 auto bent=magneticSelectionPath(corner,{30,57},{58,30},opts);check(bent.ok()&&bent.attracted,"corner edge path failed");
 for(std::size_t i=0;i<bent.points.size();++i){auto p=bent.points[i];check(std::abs(p.x-32)<=3||std::abs(p.y-32)<=3,"corner path cut through a flat region");if(i){auto q=bent.points[i-1];check(std::abs(p.x-q.x)<=1 && std::abs(p.y-q.y)<=1,"edge path contains a discontinuity");}}
 auto flat=makeRasterLayer(64,64,0xff808080U);auto straight=magneticSelectionPath(flat,{1,3},{60,50},opts);check(straight.ok()&&!straight.attracted&&straight.points.size()==2,"flat image must use direct path");
 auto transparent=makeRasterLayer(64,64,0x00ff0000U);for(int y=0;y<64;++y)for(int x=32;x<64;++x)transparent.pixels[std::size_t(y)*64+x]=0x0000ff00U;
 auto hidden=magneticSelectionPath(transparent,{1,3},{60,50},opts);check(hidden.ok()&&!hidden.attracted,"transparent RGB must not create edges");
 check(!magneticSelectionPath(image,{std::numeric_limits<double>::quiet_NaN(),0},{1,1},opts).ok(),"invalid point accepted");
 opts.radius=std::numeric_limits<double>::quiet_NaN();check(!magneticSelectionPath(image,{2,2},{3,3},opts).ok(),"invalid radius accepted");opts.radius=8;opts.maxSearchPixels=4;check(!magneticSelectionPath(image,{1,1},{60,60},opts).ok(),"search budget ignored");
 std::cout<<"MAGNETIC_PATH_OK\n";return 0;
}catch(const std::exception& e){std::cerr<<e.what()<<"\n";return 1;}
