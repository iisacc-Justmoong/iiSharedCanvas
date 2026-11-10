#include <iiSharedCanvas.h>
#include <cmath>
#include <iostream>
#include <limits>
#include <stdexcept>
using namespace iiSharedCanvas;
void check(bool v,const char* m){if(!v)throw std::runtime_error(m);}
int main() try {
 GradientDefinition g;g.start={.5,.5};g.end={3.5,.5};g.stops={{0,0xffff0000U},{1,0xff0000ffU}};
 auto linear=gradientRaster({4,1},g);check(linear.ok(),"linear gradient failed");
 check(linear.pixels.pixels==std::vector<std::uint32_t>({0xffff0000U,0xffaa0055U,0xff5500aaU,0xff0000ffU}),"linear stops and endpoints failed");
 g.stops={{0,0xffff0000U},{1,0x000000ffU}};auto alpha=gradientRaster({4,1},g);
 check(alpha.ok() && alpha.pixels.pixels[1]==0xaaff0000U && alpha.pixels.pixels[2]==0x55ff0000U && alpha.pixels.pixels[3]==0,"transparent gradient must interpolate premultiplied colors");
 g.stops={{0,0xff000000U},{.5,0xff00ff00U},{1,0xffffffffU}};g.end={2.5,.5};g.spread=GradientSpread::Repeat;
 auto repeat=gradientRaster({4,1},g);check(repeat.ok() && repeat.pixels.pixels[0]==0xff000000U && repeat.pixels.pixels[1]==0xff00ff00U && repeat.pixels.pixels[2]==0xff000000U && repeat.pixels.pixels[3]==0xff00ff00U,"repeat or multiple stops failed");
 g.spread=GradientSpread::Pad;g.kind=GradientKind::Radial;g.start={.5,.5};g.end={2.5,.5};auto radial=gradientRaster({3,1},g);
 check(radial.ok() && radial.pixels.pixels[0]==0xff000000U && radial.pixels.pixels[1]==0xff00ff00U && radial.pixels.pixels[2]==0xffffffffU,"radial gradient failed");
 g.end=g.start;check(!gradientRaster({2,2},g).ok(),"zero length gradient accepted");g.end={1,1};g.stops={{.7,0xff000000U},{.3,0xffffffffU}};check(!gradientRaster({2,2},g).ok(),"unordered stops accepted");g.stops={{0,0},{1,0xffffffffU}};g.opacity=std::numeric_limits<double>::quiet_NaN();check(!gradientRaster({2,2},g).ok(),"invalid opacity accepted");
 std::cout<<"GRADIENT_RASTER_OK\n";return 0;
}catch(const std::exception& e){std::cerr<<e.what()<<"\n";return 1;}
