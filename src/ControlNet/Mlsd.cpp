#include "ControlNet/Mlsd.h"
#include "Document/Document.h"
#include "Validation/Validation.h"
#include <algorithm>
#include <cmath>
#include <cstdlib>
#include <stdexcept>
#include <unordered_set>

namespace iiSharedCanvas {
namespace {
std::int64_t pixel(double coordinate,std::int32_t extent){return std::llround(coordinate*(extent-1));}
bool selected(const MlsdSegment &line,const MlsdRenderOptions &options){return line.enabled && line.confidence>=options.minimumConfidence;}
}
MlsdAsset *findMlsdAsset(Document &doc,const std::string &id) noexcept {
    auto *asset=findAsset(doc,id);return asset?std::get_if<MlsdAsset>(asset):nullptr;
}
const MlsdAsset *findMlsdAsset(const Document &doc,const std::string &id) noexcept {
    const auto *asset=findAsset(doc,id);return asset?std::get_if<MlsdAsset>(asset):nullptr;
}
MlsdLayer *findMlsdLayer(Document &doc,const std::string &id) noexcept {
    auto *layer=findLayer(doc,id);return layer?std::get_if<MlsdLayer>(layer):nullptr;
}
const MlsdLayer *findMlsdLayer(const Document &doc,const std::string &id) noexcept {
    const auto *layer=findLayer(doc,id);return layer?std::get_if<MlsdLayer>(layer):nullptr;
}
std::string validateMlsdAsset(const MlsdAsset &asset){
    if(asset.viewport.width<=0 || asset.viewport.height<=0) return "MLSD viewport dimensions must be positive";
    if(asset.segments.size()>100000) return "MLSD assets allow at most 100000 segments";
    std::unordered_set<std::string> ids;
    for(const auto &line:asset.segments){
        if(line.id.empty() || !ids.insert(line.id).second) return "MLSD segment ids must be nonempty and unique";
        for(double value:{line.x1,line.y1,line.x2,line.y2,line.confidence})
            if(!std::isfinite(value) || value<0 || value>1) return "MLSD coordinates and confidence must be finite within [0,1]";
        if(line.x1==line.x2 && line.y1==line.y2) return "MLSD segments must have distinct endpoints";
    }
    return {};
}
RasterLayer mlsdRasterPreview(const MlsdAsset &asset,const MlsdRenderOptions &options){
    const auto message=validateMlsdAsset(asset);if(!message.empty()) throw std::invalid_argument(message);
    if(!std::isfinite(options.minimumConfidence) || options.minimumConfidence<0 || options.minimumConfidence>1)
        throw std::invalid_argument("MLSD confidence filter must be finite within [0,1]");
    if(std::uint64_t(asset.viewport.width)*asset.viewport.height>options.maximumPixels)
        throw std::length_error("MLSD output exceeds maximumPixels");
    auto remaining=options.maximumRasterSteps;
    for(const auto &line:asset.segments){
        if(!selected(line,options)) continue;
        const auto steps=static_cast<std::uint64_t>(std::max(std::abs(pixel(line.x2,asset.viewport.width)-pixel(line.x1,asset.viewport.width)),
            std::abs(pixel(line.y2,asset.viewport.height)-pixel(line.y1,asset.viewport.height))))+1;
        if(steps>remaining) throw std::length_error("MLSD output exceeds maximumRasterSteps");
        remaining-=steps;
    }
    auto output=makeRasterLayer(asset.viewport.width,asset.viewport.height,0xff000000);
    for(const auto &line:asset.segments){
        if(!selected(line,options)) continue;
        auto x=pixel(line.x1,asset.viewport.width),y=pixel(line.y1,asset.viewport.height);
        const auto endX=pixel(line.x2,asset.viewport.width),endY=pixel(line.y2,asset.viewport.height);
        const auto dx=std::abs(endX-x),dy=-std::abs(endY-y);
        const std::int64_t sx=x<endX?1:-1,sy=y<endY?1:-1;
        auto error=dx+dy;
        for(;;){
            output.pixels[static_cast<std::size_t>(y)*asset.viewport.width+static_cast<std::size_t>(x)]=0xffffffff;
            if(x==endX && y==endY) break;
            const auto twice=2*error;
            if(twice>=dy){error+=dy;x+=sx;}
            if(twice<=dx){error+=dx;y+=sy;}
        }
    }
    return output;
}
VectorAsset mlsdVectorPreview(const MlsdAsset &asset){
    const auto message=validateMlsdAsset(asset);if(!message.empty()) throw std::invalid_argument(message);
    VectorAsset output;output.id=asset.id+".preview";output.viewport=asset.viewport;
    const double width=asset.viewport.width,height=asset.viewport.height;
    VectorPath background;background.fill=SolidPaint{0xff000000};
    background.commands={MoveTo{{0,0}},LineTo{{width,0}},LineTo{{width,height}},LineTo{{0,height}},ClosePath{}};
    output.paths.push_back(std::move(background));
    for(const auto &line:asset.segments){
        if(!line.enabled) continue;
        VectorPath path;path.stroke=StrokeStyle{SolidPaint{0xffffffff},1};
        path.commands={MoveTo{{line.x1*(width-1)+0.5,line.y1*(height-1)+0.5}},LineTo{{line.x2*(width-1)+0.5,line.y2*(height-1)+0.5}}};
        output.paths.push_back(std::move(path));
    }
    return output;
}
MlsdControlMapResult renderMlsdControlMap(const Document &doc,const std::string &id,std::uint32_t frame,const MlsdRenderOptions &options){
    const auto fail=[](std::string message){MlsdControlMapResult result;result.message=std::move(message);return result;};
    const auto validation=validate(doc);if(!validation.ok())return fail(validation.issues.front().path+": "+validation.issues.front().message);
    const auto *base=findLayer(doc,id);const auto *layer=base?std::get_if<MlsdLayer>(base):nullptr;
    if(!layer)return fail("MLSD layer not found");
    if(!layer->control.enabled)return fail("ControlNet layer is disabled");
    const auto *source=resolveAssetAt(doc,*base,frame);const auto *asset=source?std::get_if<MlsdAsset>(source):nullptr;
    if(!asset)return fail("MLSD source is outside its frame range or timeline");
    MlsdControlMapResult result;
    try{result.pixels=mlsdRasterPreview(*asset,options);}
    catch(const std::invalid_argument &error){return fail(error.what());}
    catch(const std::length_error &error){return fail(error.what());}
    for(const auto &line:asset->segments)if(selected(line,options))result.segments.push_back(line);
    return result;
}
} // namespace iiSharedCanvas
