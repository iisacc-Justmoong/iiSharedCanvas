#include "Bitmap/TiledMaskEditor.h"
#include "Bitmap/BrushEngineState_p.hpp"
#include "Bitmap/StrokeComposite_p.hpp"
#include "Bitmap/StrokeSample_p.hpp"
#include <algorithm>
#include <cmath>
#include <limits>
#include <new>
#include <stdexcept>
#include <utility>
#include <vector>
namespace iiSharedCanvas {
struct TiledMaskEditor::Impl {
    TiledMask initial,current;
    std::vector<TiledMask> undo,redo;
    std::size_t budget;
    BrushState brush;
    RasterDabStream stream;
    detail::BitmapStrokeComposite stroke;
    bool active=false;
    double time=0;
    std::uint32_t seed=1;
    std::string error;
    explicit Impl(TiledMask mask,std::size_t bytes):initial(mask),current(std::move(mask)),budget(bytes){}
};
namespace {
bool unit(double x){return std::isfinite(x)&&x>=0&&x<=1;}
bool validBrush(const BitmapBrush& b){return std::isfinite(b.size)&&b.size>0&&b.size<=4096&&unit(b.opacity)&&unit(b.flow)&&unit(b.hardness)&&std::isfinite(b.spacing)&&b.spacing>=0&&b.spacing<=4096&&std::isfinite(b.spacingRatio)&&b.spacingRatio>=0&&b.spacingRatio<=4;}
BrushState resolved(const BitmapBrush& b,std::uint32_t seed) {
    Rasterizer r=b.engineState?b.engineState->rasterizer:Rasterizer{};
    r.radius=static_cast<Types::Pixel>(std::max(1.,std::ceil(b.size*.5)));r.brushSize=b.size;
    r.argb=b.eraser?0xff000000U:b.argb;r.spacing=b.spacing;r.spacingRatio=b.spacingRatio;r.spacingEnabled=b.spacingEnabled;
    r.opacity=b.opacity;r.opacityEnabled=b.opacityEnabled;r.flow=b.flow;r.flowEnabled=b.flowEnabled;
    r.hardness=b.hardness;r.hardnessEnabled=b.hardnessEnabled;
    r.blendMode=b.eraser?RasterBlendMode::DestinationOut:b.engineState?b.engineState->rasterizer.blendMode:RasterBlendMode::SourceOver;
    BrushDynamics d;d.pressureToSize=1;d.pressureToFlow=1;d.pressureToOpacity=1;d.pressureToOpacityEnabled=b.pressureToOpacityEnabled;
    return {std::move(r),b.engineState?b.engineState->dynamics:std::move(d),b.engineState?b.engineState->material:BrushMaterial{},seed};
}
}
TiledMaskEditor::TiledMaskEditor(TiledMask mask,std::size_t budget):impl_(std::make_unique<Impl>(std::move(mask),budget)){}
TiledMaskEditor::~TiledMaskEditor()=default;
bool TiledMaskEditor::begin(const StrokePoint& sample,const BitmapBrush& brush) {
    auto& p=*impl_;if(p.active||!p.current){p.error="mask is invalid or a stroke is already active";return false;}
    try {
        if(!validBrush(brush)){p.error="brush values are outside the supported finite range";return false;}
        if(brush.engineState){const auto errors=detail::validateEngineState(*brush.engineState);if(!errors.empty()){p.error=errors.front();return false;}}
        p.brush=resolved(brush,p.seed);p.stream={};p.stroke={};p.active=true;p.time=sample.time;
        if(!append(sample,false)){cancel();return false;}p.error.clear();return true;
    } catch(const std::bad_alloc&) {cancel();p.error="insufficient memory for mask stroke";return false;}
}
bool TiledMaskEditor::extend(const StrokePoint& sample){return append(sample,false);}
bool TiledMaskEditor::append(const StrokePoint& sample,bool finish) {
    auto& p=*impl_;
    if(!p.active){p.error="no active mask stroke";return false;}
    if(!detail::validStrokeSample(sample)||!unit(sample.pressure)||sample.time<p.time||
       sample.position.x < -1000000000. || sample.position.x > 1000000000. ||
       sample.position.y < -1000000000. || sample.position.y > 1000000000.) {p.error="invalid or out-of-order mask stroke sample";return false;}
    try {
        auto stream=p.stream;auto candidate=p.stroke;
        auto dabs=appendRasterDabs(stream,sample,p.brush,finish);
        const auto domain=p.current.domain();
        struct Context {const TiledMaskEditor* editor;CanvasOrigin origin;};Context ctx{this,domain.origin};
        const RasterSourceSampler sampler{&ctx,[](const void* raw,DevicePixelPoint q)->std::uint32_t {
            const auto& c=*static_cast<const Context*>(raw);const auto a=c.editor->coverage(q.x+c.origin.x,q.y+c.origin.y);
            return a ? (std::uint32_t(a)<<24)|0xffffffU : 0U;
        },{domain.origin.x,domain.origin.y},domain.extent.width,domain.extent.height};
        auto samples=projectBrushDabs(dabs,p.brush.rasterizer,RasterProjection{},sampler,p.brush.material);
        if(samples.size()>16ULL*1024*1024){p.error="mask stroke exceeds the sample budget";return false;}
        std::erase_if(samples,[&](const auto& v){return v.position.x<domain.origin.x || v.position.y<domain.origin.y ||
            std::int64_t(v.position.x)>=std::int64_t(domain.origin.x)+domain.extent.width ||
            std::int64_t(v.position.y)>=std::int64_t(domain.origin.y)+domain.extent.height;});
        candidate.paint(samples,p.brush.rasterizer.blendMode,[&](DevicePixelPoint q){const auto a=p.current.coverage(q.x,q.y);return a?(std::uint32_t(a)<<24)|0xffffffU:0U;},[](auto,auto){});
        if(candidate.storageBytes()>p.budget){p.error="mask stroke exceeds the working budget";return false;}
        p.stroke=std::move(candidate);p.stream=std::move(stream);p.time=sample.time;p.error.clear();return true;
    } catch(const std::bad_alloc&) {p.error="insufficient memory for mask stroke";return false;}
      catch(const std::logic_error& e) {p.error=e.what();return false;}
}
bool TiledMaskEditor::end(const StrokePoint& sample) {
    auto& p=*impl_;if(!append(sample,true)){cancel();return false;}
    try {
        std::vector<TiledMaskPatch> patches;
        p.stroke.visit([&](DevicePixelRect r,const RasterLayer&,const RasterLayer& preview){
            RasterMask mask{{r.width,r.height},std::vector<std::uint8_t>(preview.pixels.size())};
            for(std::size_t i=0;i<mask.alpha.size();++i)mask.alpha[i]=std::uint8_t(preview.pixels[i]>>24);
            patches.push_back({{r.origin.x,r.origin.y},std::move(mask)});
        });
        auto next=p.current.patched(patches);
        if(!next){p.error=next.error;cancel();return false;}
        if(!next->equivalent(p.current)) {
            if(next->storage_bytes()>p.budget || p.current.storage_bytes()>p.budget-next->storage_bytes()) {p.error="mask history exceeds the budget";cancel();return false;}
            p.undo.push_back(p.current);p.current=std::move(*next);p.redo.clear();
            std::size_t used=p.current.storage_bytes();for(auto it=p.undo.rbegin();it!=p.undo.rend();++it)used+=it->storage_bytes();
            while((used>p.budget||p.undo.size()>100)&&!p.undo.empty()){used-=p.undo.front().storage_bytes();p.undo.erase(p.undo.begin());}
            ++p.seed;
        }
        cancel();p.error.clear();return true;
    } catch(const std::bad_alloc&) {p.error="insufficient memory for mask history";cancel();return false;}
}
void TiledMaskEditor::cancel() noexcept {impl_->active=false;impl_->stream={};impl_->stroke={};}
bool TiledMaskEditor::undo(){auto& p=*impl_;if(p.active||p.undo.empty())return false;try{p.redo.push_back(p.current);p.current=std::move(p.undo.back());p.undo.pop_back();p.error.clear();return true;}catch(const std::bad_alloc&){p.error="insufficient memory for mask undo";return false;}}
bool TiledMaskEditor::redo(){auto& p=*impl_;if(p.active||p.redo.empty())return false;try{p.undo.push_back(p.current);p.current=std::move(p.redo.back());p.redo.pop_back();p.error.clear();return true;}catch(const std::bad_alloc&){p.error="insufficient memory for mask redo";return false;}}
bool TiledMaskEditor::canUndo()const noexcept{return !impl_->undo.empty();}
bool TiledMaskEditor::canRedo()const noexcept{return !impl_->redo.empty();}
bool TiledMaskEditor::strokeActive()const noexcept{return impl_->active;}
bool TiledMaskEditor::changed()const noexcept{return !impl_->current.equivalent(impl_->initial);}
std::uint8_t TiledMaskEditor::coverage(std::int32_t x,std::int32_t y)const noexcept {
    const auto& p=*impl_;const auto base=[&](DevicePixelPoint q){const auto a=p.current.coverage(q.x,q.y);return a?(std::uint32_t(a)<<24)|0xffffffU:0U;};
    return p.active?std::uint8_t(p.stroke.previewPixel({x,y},base)>>24):p.current.coverage(x,y);
}
const TiledMask& TiledMaskEditor::mask()const noexcept{return impl_->current;}
const std::string& TiledMaskEditor::error()const noexcept{return impl_->error;}
}
