#include <iiSharedCanvasNative.h>
#include <algorithm>
#include <cmath>
#include <iostream>
#include <limits>
#include <vector>
using namespace iiSharedCanvas;
int main() {
    int failed=0;
    auto check=[&](bool value,const char *message){if(!value){std::cerr<<message<<'\n';++failed;}};
    auto a=selectionGeometry({4,4},SelectionGeometry::Rectangle,{1,1},{3,3},false);
    check(a.ok() && a.mask.alpha==std::vector<std::uint8_t>({0,0,0,0,0,255,255,0,0,255,255,0,0,0,0,0}),"rectangle coverage");
    auto reverse=selectionGeometry({4,4},SelectionGeometry::Rectangle,{3,3},{1,1},false);
    check(reverse.ok() && reverse.mask.alpha==a.mask.alpha,"reverse drag");
    auto outside=selectionGeometry({4,4},SelectionGeometry::Rectangle,{-2,-2},{2,2},false);
    check(outside.ok() && outside.mask.alpha[0]==255 && outside.mask.alpha[2]==0,"canvas clipping");
    auto ellipse=selectionGeometry({4,4},SelectionGeometry::Ellipse,{0,0},{4,4},false);
    check(ellipse.ok() && ellipse.mask.alpha[0]==0 && ellipse.mask.alpha[5]==255,"ellipse shape");
    auto smooth=selectionGeometry({4,4},SelectionGeometry::Ellipse,{0,0},{4,4},true);
    check(smooth.ok() && smooth.mask.alpha[0]>0 && smooth.mask.alpha[0]<255,"ellipse antialiasing");
    auto empty=selectionGeometry({4,4},SelectionGeometry::Rectangle,{1,1},{1,2},false);
    check(empty.ok() && empty.mask.alpha==std::vector<std::uint8_t>(16,0),"zero area");
    auto combined=combineMasks(a.mask,outside.mask,MaskOperation::Intersect);
    check(combined.ok() && combined.mask.alpha[5]==255 && combined.mask.alpha[10]==0,"combine interoperates");
    check(!selectionGeometry({0,4},SelectionGeometry::Rectangle,{0,0},{1,1},false).ok(),"invalid extent");
    check(!selectionGeometry({4,4},SelectionGeometry::Rectangle,{std::numeric_limits<double>::quiet_NaN(),0},{1,1},false).ok(),"nonfinite point");
    check(!selectionGeometry({4,4},static_cast<SelectionGeometry>(99),{0,0},{1,1},false).ok(),"invalid geometry");
    const std::vector<Point> polygon{{1,1},{3,1},{3,3},{1,3}};
    auto poly=selectionPolygon({4,4},polygon,false);
    check(poly.ok() && poly.mask.alpha==a.mask.alpha,"polygon coverage");
    auto reversePoly=polygon; std::reverse(reversePoly.begin(),reversePoly.end());
    check(selectionPolygon({4,4},reversePoly,false).mask.alpha==a.mask.alpha,"polygon direction");
    const std::vector<Point> clipped{{-2,-2},{2,-2},{2,2},{-2,2}};
    check(selectionPolygon({4,4},clipped,false).mask.alpha==outside.mask.alpha,"polygon clipping");
    const std::vector<Point> fractional{{0.5,0},{2.5,0},{2.5,2},{0.5,2}};
    auto fractionalMask=selectionPolygon({4,4},fractional,true);
    check(fractionalMask.ok() && fractionalMask.mask.alpha[0]==128 && fractionalMask.mask.alpha[1]==255 && fractionalMask.mask.alpha[2]==128,"polygon antialiasing");
    const std::vector<Point> crossing{{0,0},{4,4},{0,4},{4,0}};
    auto crossed=selectionPolygon({4,4},crossing,false);
    check(crossed.ok() && crossed.mask.alpha[1]==255 && crossed.mask.alpha[13]==255 && crossed.mask.alpha[4]==0,"polygon even odd rule");
    check(!selectionPolygon({4,4},std::vector<Point>{{0,0},{1,1}},false).ok(),"polygon needs three points");
    check(!selectionPolygon({4,4},std::vector<Point>{{0,0},{1,1},{std::numeric_limits<double>::infinity(),0}},false).ok(),"polygon nonfinite point");
    check(!selectionPolygon({4,4},std::vector<Point>(65537,{0,0}),false).ok(),"polygon point budget");
    check(!selectionPolygon({4,4},std::vector<Point>{{0,0},{1,1},{1e10,0}},false).ok(),"polygon coordinate budget");
    std::vector<Point> expensive(65536);
    for (std::size_t i=0; i<expensive.size(); ++i) expensive[i] = {double(i%2), i%2 ? 256.0 : 0.0};
    check(!selectionPolygon({256,256},expensive,true).ok(),"polygon edge scan budget");
    return failed?1:0;
}
