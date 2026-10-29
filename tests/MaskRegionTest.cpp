#include <Document/DocumentHistory.h>
#include <Render/FrameRenderer.h>
#include <Validation/Validation.h>

#include <algorithm>
#include <array>
#include <cstdint>
#include <cstdlib>
#include <iostream>
#include <new>
#include <string>
#include <string_view>
#include <vector>

#if defined(__has_feature)
#if __has_feature(address_sanitizer)
#define MASK_REGION_ASAN 1
#endif
#endif
#if defined(__SANITIZE_ADDRESS__) && !defined(MASK_REGION_ASAN)
#define MASK_REGION_ASAN 1
#endif
#ifndef MASK_REGION_ASAN
#define MASK_REGION_ASAN 0
#endif
namespace allocation {
thread_local bool observe=false;
thread_local std::size_t largest=0,total=0;
}
#if !MASK_REGION_ASAN
void *operator new(std::size_t size) {
    if (allocation::observe) { allocation::largest=std::max(allocation::largest,size); allocation::total+=size; }
    if (auto *p=std::malloc(size?size:1)) return p;
    throw std::bad_alloc();
}
void *operator new[](std::size_t n) { return ::operator new(n); }
void operator delete(void *p) noexcept { std::free(p); }
void operator delete[](void *p) noexcept { std::free(p); }
void operator delete(void *p,std::size_t) noexcept { std::free(p); }
void operator delete[](void *p,std::size_t) noexcept { std::free(p); }
#endif

using namespace iiSharedCanvas;
namespace {
int failures=0;
void check(bool value,std::string_view what) {
    if (!value) { ++failures; std::cerr << what << '\n'; }
}
std::uint32_t sample(int x,int y) {
    const auto a=std::uint32_t((x*13+y*7+41)&255);
    const auto r=std::uint32_t((x*3+y*11+17)&255);
    const auto g=std::uint32_t((x*5+y*19+83)&255);
    const auto b=std::uint32_t((x*23+y*2+29)&255);
    return (a<<24)|(r<<16)|(g<<8)|b;
}
Document fixture(int width=96,int height=72,int ox=0,int oy=0) {
    Document d; d.extent={width,height};
    d.assets.emplace_back(RasterAsset{"paint",makeRasterLayer(width,height,0xff7293b1)});
    auto image=makeRasterLayer(width,height);
    for (int y=0;y<height;++y) for (int x=0;x<width;++x)
        image.pixels[std::size_t(y)*width+x]=sample(x+ox,y+oy);
    d.assets.emplace_back(RasterAsset{"mask",std::move(image)});
    VectorPath path;
    path.commands={MoveTo{{8.,9.}},LineTo{{78.,12.}},CubicTo{{91.,29.},{70.,49.},{23.,61.}},ClosePath{}};
    path.fill=SolidPaint{0x20dd5500}; path.stroke=StrokeStyle{SolidPaint{0x400066aa},3.2};
    d.assets.emplace_back(VectorAsset{"vector",{width,height},{path}});
    d.layers.emplace_back(StaticBitmapLayer{{"layer","Paint"},StaticSource{"paint"}});
    return d;
}
void configure(Document &d,int mode,double radius,bool smooth) {
    auto &p=layerProperties(d.layers[0]);
    p.transform.translationX=1.25; p.transform.translationY=-0.75;
    LayerMask raster; raster.assetId="mask"; raster.featherRadius=radius;
    raster.density=0.68; raster.inverted=smooth; raster.outsideCoverage=0.27;
    raster.linkedToLayer=!smooth;
    raster.channel=smooth?MaskChannel::Alpha:MaskChannel::Luminance;
    raster.transform={0.93,0.17,-0.14,1.04,5.25,-2.5};
    LayerMask vector=raster; vector.assetId="vector"; vector.channel=MaskChannel::Alpha;
    vector.inverted=!raster.inverted; vector.transform={1.01,-0.11,0.08,0.96,-1.75,3.5};
    auto &appearance=p.appearance;
    if (mode==0) appearance.rasterMask=raster;
    if (mode==1) appearance.vectorMask=vector;
    if (mode==2) {
        appearance.maskStack={{"a","Raster",raster,MaskCombineMode::Add},
                              {"b","Vector",vector,MaskCombineMode::Subtract},
                              {"c","Raster again",raster,MaskCombineMode::Intersect}};
    }
    if (mode==3) {
        LayerFilter filter; filter.operation=RasterEffect{RasterEffectKind::Invert};
        appearance.filters={filter}; appearance.filterMask=raster;
    }
}
std::uint64_t fingerprint(const RasterLayer &r) {
    std::uint64_t hash=1469598103934665603ULL;
    for (auto p:r.pixels) for (int i=0;i<4;++i) { hash^=(p>>(8*i))&255; hash*=1099511628211ULL; }
    return hash;
}
// Generated with AppleClang on macOS arm64 against the installed 0.35.5
// implementation before region sampling was introduced. Floating-point
// implementation differences make these bit-exact hashes platform-specific.
#if defined(__APPLE__) && defined(__aarch64__) && defined(__clang__)
constexpr std::array<std::uint64_t,24> golden{
    3729685177158927669ULL,17668376497229480582ULL,11657527486311461974ULL,
    18273762839775628392ULL,3437840749850923737ULL,17871890841990389549ULL,
    5327159050775433855ULL,5368420047138042906ULL,1825606083435433498ULL,
    17218388157071925285ULL,17170075596458520353ULL,5213959216013175565ULL,
    2651146610684286185ULL,16554077362774169135ULL,12448013475148203359ULL,
    4300137222737335971ULL,14847574598300216250ULL,4279779323590952707ULL,
    7690551565425251057ULL,4173213617457876468ULL,12138083596454388541ULL,
    16788145302118213070ULL,5341916155629773705ULL,14560323294079843486ULL};
#endif
void small(bool capture) {
    const std::array<double,3> radii{0,0.75,2.5};
    int number=0;
    for (int mode=0;mode<4;++mode) for (double radius:radii) for (int smooth=0;smooth<2;++smooth) {
        auto d=fixture(); configure(d,mode,radius,bool(smooth));
        const auto sampling=smooth?RasterSampling::Smooth:RasterSampling::Nearest;
        const int width=smooth?48:96,height=smooth?36:72;
        const auto full=renderFrameRegion(d,0,{{0,0},{96,72}},{width,height},sampling);
        const auto tiles=renderFrameTiles(d,0,{
            {{{0,0},{48,36}},{width/2,height/2},sampling},
            {{{48,0},{48,36}},{width/2,height/2},sampling},
            {{{0,36},{48,36}},{width/2,height/2},sampling},
            {{{48,36},{48,36}},{width/2,height/2},sampling}});
        check(full.ok() && tiles.ok() && tiles.tiles.size()==4,"small: full and tiled render succeed");
        if (full.ok() && tiles.ok() && tiles.tiles.size()==4) {
            const auto hash=fingerprint(full.pixels);
            if (capture) std::cout << "MASK_GOLDEN case=" << number << " hash=" << hash << '\n';
#if defined(__APPLE__) && defined(__aarch64__) && defined(__clang__)
            else check(hash==golden[std::size_t(number)],"small: pixel output matches prior renderer");
#endif
            bool same=true;
            for (int y=0;y<height;++y) for (int x=0;x<width;++x) {
                const int tile=(y>=height/2?2:0)+(x>=width/2?1:0);
                const int tx=x%(width/2),ty=y%(height/2);
                same=same && full.pixels.pixels[std::size_t(y)*width+x]==
                    tiles.tiles[tile].pixels.pixels[std::size_t(ty)*(width/2)+tx];
            }
            check(same,"small: tiled output matches unsplit output without seams");
        }
        ++number;
    }
    auto extreme=fixture();
    auto &appearance=layerProperties(extreme.layers[0]).appearance;
    LayerMask mask; mask.assetId="mask"; mask.outsideCoverage=0.4; mask.channel=MaskChannel::Alpha;
    mask.transform.m11=1e-310; appearance.rasterMask=mask;
    for (auto sampling:{RasterSampling::Nearest,RasterSampling::Smooth}) {
        const auto output=renderFrameRegion(extreme,0,{{2,2},{1,1}},{1,1},sampling);
        check(output.ok() && (output.pixels.pixels[0]>>24)==102,
              "small: finite near-singular transforms use outside coverage when inverse overflows");
    }
    // Exercise cache eviction with more feathered masks than the 32-tile
    // cache can hold. Replace makes the last operand an independent oracle.
    auto one=fixture(), many=fixture();
    LayerMask stackMask; stackMask.assetId="mask"; stackMask.featherRadius=0.75;
    auto &oneStack=layerProperties(one.layers[0]).appearance.maskStack;
    auto &manyStack=layerProperties(many.layers[0]).appearance.maskStack;
    oneStack={{"last","Last",stackMask,MaskCombineMode::Replace}};
    for (int i=0;i<35;++i)
        manyStack.push_back({std::to_string(i),"Mask",stackMask,MaskCombineMode::Replace});
    check(validate(many).ok(),"small: 35-mask stack is valid");
    const auto single=renderFrameRegion(one,0,{{32,16},{16,16}},{16,16});
    allocation::largest=0; allocation::total=0; allocation::observe=true;
    const auto stacked=renderFrameRegion(many,0,{{32,16},{16,16}},{16,16});
    allocation::observe=false;
    check(single.ok() && stacked.ok() && single.pixels.pixels==stacked.pixels.pixels,
          "small: stack exceeding tile cache preserves Replace semantics");
#if !MASK_REGION_ASAN
    check(allocation::largest<4*1024*1024 && allocation::total<16*1024*1024,
          "small: long feathered stack does not thrash for every output pixel");
#endif
}
void large() {
    constexpr int side=8192, offset=4032; auto d=fixture(side,side);
    auto ref=fixture(128,128,offset,offset);
    auto &mask=layerProperties(d.layers[0]).appearance.rasterMask;
    auto &refMask=layerProperties(ref.layers[0]).appearance.rasterMask;
    LayerMask settings; settings.assetId="mask"; settings.outsideCoverage=0.25;
    settings.density=0.8; settings.channel=MaskChannel::Luminance;
    mask=settings; refMask=settings;
    check(validate(d).ok(),"8K: raster mask is a valid document");
    std::size_t maximum=0,total=0;
    for (double radius:{0.0,3.25,128.0}) for (auto sampling:{RasterSampling::Nearest,RasterSampling::Smooth}) {
        mask->featherRadius=radius; refMask->featherRadius=radius;
        const int extent=radius==128?1:40;
        allocation::largest=0; allocation::total=0; allocation::observe=true;
        const auto rendered=renderFrameRegion(d,0,{{4070,4070},{extent,extent}},{extent,extent},sampling);
        allocation::observe=false;
        maximum=std::max(maximum,allocation::largest); total=std::max(total,allocation::total);
        // Radius 128 reaches beyond the 128px reference; use the large source
        // only for memory and finite-output checks in that case.
        check(rendered.ok(),"8K: raster mask region renders");
        if (radius!=128) {
            const auto expected=renderFrameRegion(ref,0,{{38,38},{extent,extent}},{extent,extent},sampling);
            check(expected.ok() && rendered.ok() && rendered.pixels.pixels==expected.pixels.pixels,
                  "8K: local reference agrees for feathered and direct coverage");
        }
    }
#if !MASK_REGION_ASAN
    check(maximum<4*1024*1024 && total<16*1024*1024,
          "8K: small region does not allocate complete mask assets or coverage arrays");
#endif
    mask->featherRadius=3.25; refMask->featherRadius=3.25;
    bool undoOk=false;
    {
        const auto before=renderFrameRegion(d,0,{{4070,4070},{40,40}},{40,40});
        DocumentHistory history(d); const auto old=findRasterAsset(d,"mask")->pixels.pixels[std::size_t(4090)*side+4090];
        const std::array<std::uint32_t,1> changed{old^0x00ffffffU};
        const std::array patch{BitmapPatch{{{4090,4090},1,1},changed}};
        check(history.isBound() && history.beginPixelPreview("mask").ok() &&
              history.replacePixelPreview(patch).changed && history.commitPixelPreview().changed,
              "8K: mask preview and global undo command");
        const auto edited=renderFrameRegion(d,0,{{4070,4070},{40,40}},{40,40});
        check(edited.ok() && before.ok() && edited.pixels.pixels!=before.pixels.pixels,
              "8K: edited mask changes composition");
        undoOk=history.undo().changed;
        check(undoOk,"8K: mask undo succeeds");
        const auto undone=renderFrameRegion(d,0,{{4070,4070},{40,40}},{40,40});
        check(undone.ok() && before.ok() && undone.pixels.pixels==before.pixels.pixels,
              "8K: mask undo restores composed pixels");
    }
    bool allRestored=true;
    const auto &maskPixels=findRasterAsset(d,"mask")->pixels.pixels;
    const auto &sourcePixels=findRasterAsset(d,"paint")->pixels.pixels;
    for (int y=0;y<side;++y) for (int x=0;x<side;++x) {
        const auto i=std::size_t(y)*side+x;
        allRestored=(maskPixels[i]==sample(x,y) && sourcePixels[i]==0xff7293b1) && allRestored;
    }
    check(allRestored,"8K: all source and mask pixels remain unchanged after undo and rendering");
    auto &a=layerProperties(d.layers[0]).appearance;
    auto &ra=layerProperties(ref.layers[0]).appearance;
    a.rasterMask.reset(); ra.rasterMask.reset();
    LayerMask first=settings; first.featherRadius=3.25;
    LayerMask second=first; second.channel=MaskChannel::Alpha; second.inverted=true; second.density=0.57;
    a.maskStack={{"one","First",first,MaskCombineMode::Add},{"two","Second",second,MaskCombineMode::Subtract}};
    ra.maskStack=a.maskStack;
    allocation::largest=0; allocation::total=0; allocation::observe=true;
    const auto stackRegion=renderFrameRegion(d,0,{{4070,4070},{40,40}},{40,40},RasterSampling::Smooth);
    allocation::observe=false;
    const auto refStack=renderFrameRegion(ref,0,{{38,38},{40,40}},{40,40},RasterSampling::Smooth);
    check(stackRegion.ok() && refStack.ok() && stackRegion.pixels.pixels==refStack.pixels.pixels,
          "8K: mask stack matches local reference");
#if !MASK_REGION_ASAN
    check(allocation::largest<4*1024*1024 && allocation::total<16*1024*1024,
          "8K: mask stack uses bounded region storage");
#endif
    a.maskStack.clear(); ra.maskStack.clear();
    VectorPath path; path.commands={MoveTo{{4050,4050}},LineTo{{4120,4050}},LineTo{{4090,4120}},ClosePath{}};
    path.fill=SolidPaint{0xffffffff};
    d.assets[2]=VectorAsset{"vector",{side,side},{path}};
    VectorPath refPath; refPath.commands={MoveTo{{18,18}},LineTo{{88,18}},LineTo{{58,88}},ClosePath{}};
    refPath.fill=SolidPaint{0xffffffff}; ref.assets[2]=VectorAsset{"vector",{128,128},{refPath}};
    LayerMask vector; vector.assetId="vector"; vector.outsideCoverage=0; vector.featherRadius=3.25;
    a.vectorMask=vector; ra.vectorMask=vector;
    allocation::largest=0; allocation::total=0; allocation::observe=true;
    const auto vectorRegion=renderFrameRegion(d,0,{{4070,4070},{40,40}},{40,40});
    allocation::observe=false;
    check(vectorRegion.ok(),"8K: vector mask with 8K viewport renders");
    const auto vectorReference=renderFrameRegion(ref,0,{{38,38},{40,40}},{40,40});
    check(vectorRegion.ok() && vectorReference.ok() && vectorRegion.pixels.pixels==vectorReference.pixels.pixels,
          "8K: vector mask coverage matches local reference");
#if !MASK_REGION_ASAN
    check(allocation::largest<4*1024*1024 && allocation::total<16*1024*1024,
          "8K: vector mask region rasterizes bounded tiles");
#endif
    std::cout << "MASK_REGION_8K source_bytes=" << 2ULL*side*side*4
              << " max_cpp_allocation=" << maximum << " max_cpp_total=" << total
              << " checked_pixels=" << 2ULL*side*side << " restored=" << allRestored
              << " history_undo=" << undoOk << " vector_render=" << vectorRegion.ok() << '\n';
}
void cost() {
    auto d=fixture(2048,2048);
    LayerMask mask; mask.assetId="mask"; mask.featherRadius=3.25;
    layerProperties(d.layers[0]).appearance.rasterMask=mask;
    allocation::largest=0; allocation::total=0; allocation::observe=true;
    const auto rendered=renderFrameRegion(d,0,{{1000,1000},{32,32}},{32,32});
    allocation::observe=false;
    check(rendered.ok(),"cost: measured region renders");
    std::cout << "MASK_REGION_COST source_mask_bytes=" << 2048ULL*2048*4
              << " max_cpp_allocation=" << allocation::largest << " cpp_total=" << allocation::total
              << " pixel_hash=" << (rendered.ok()?fingerprint(rendered.pixels):0) << '\n';
}
}
int main(int argc,char **argv) {
    if (argc==2 && std::string_view(argv[1])=="--large") large();
    else if (argc==2 && std::string_view(argv[1])=="--cost") cost();
    else small(argc==2 && std::string_view(argv[1])=="--capture");
    return failures?1:0;
}
