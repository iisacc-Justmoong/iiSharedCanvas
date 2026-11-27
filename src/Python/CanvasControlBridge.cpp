#include "CanvasControlBridge.h"
#include <iiSharedCanvasNative.h>

#include <algorithm>
#include <array>
#include <charconv>
#include <cmath>
#include <cstdio>
#include <exception>
#include <limits>
#include <memory>
#include <stdexcept>
#include <string_view>
#include <unordered_set>
#include <type_traits>

using namespace iiSharedCanvas;

// The declaration order makes the owned document outlive its history and
// all session helpers, including when Python explicitly closes the handle.
struct IiscCanvasControl {
    Document document;
    DocumentHistory history;
    std::unique_ptr<LastEditToggle> toggle;
    std::unique_ptr<ReeditableRasterChange> reedit;
    std::array<char, 256> error{};
    explicit IiscCanvasControl(Document value) : document(std::move(value)), history(document) {}
};
// Opaque borrowed-by-call plan; Python or another C consumer owns its lifetime.
struct IiscSequenceFlatten { SequenceRasterFlattenPreparation value; };

namespace {
template<class Function> int invoke(IiscCanvasControl *canvas, Function &&function) noexcept
{
    if (!canvas) return 0;
    try { function(*canvas); canvas->error[0]='\0'; return 1; }
    catch (const std::exception &e) { std::snprintf(canvas->error.data(),canvas->error.size(),"%s",e.what()); }
    catch (...) { std::snprintf(canvas->error.data(),canvas->error.size(),"unknown native error"); }
    return 0;
}
void accepted(const DocumentHistoryResult &result)
{
    if (!result.ok()) throw std::runtime_error(std::string(result.message()));
}
int integer(double value)
{
    if (!std::isfinite(value) || value!=std::trunc(value) || value<std::numeric_limits<int>::min() || value>std::numeric_limits<int>::max())
        throw std::invalid_argument("expected a finite integer setting");
    return static_cast<int>(value);
}
bool boolean(double value)
{
    if (value!=0 && value!=1) throw std::invalid_argument("expected a boolean setting");
    return value!=0;
}
void adjustmentField(ColorAdjustment &a, std::string_view name, double value)
{
#define FIELD(x) if (name == #x) { a.x=value; return; }
    FIELD(exposure) FIELD(offset) FIELD(contrast) FIELD(pivot) FIELD(highlights) FIELD(highlightRange)
    FIELD(rolloff) FIELD(shadows) FIELD(shadowRange) FIELD(blackProtection) FIELD(whites) FIELD(whiteClip)
    FIELD(blacks) FIELD(blackClip) FIELD(temperature) FIELD(tint) FIELD(vibrance) FIELD(lowSaturationBias)
    FIELD(saturation) FIELD(channelSaturation) FIELD(skinProtection) FIELD(gradeHue) FIELD(gradeBalance) FIELD(curve)
#undef FIELD
#define FIELD(x) if (name == #x) { a.x=integer(value); return; }
    FIELD(saturationChannel) FIELD(gradeRange) FIELD(curveChannel)
#undef FIELD
#define FIELD(x) if (name == #x) { a.x=boolean(value); return; }
    FIELD(protectHighlights) FIELD(preserveColor) FIELD(linearLight) FIELD(adaptive) FIELD(liftBlacks)
    FIELD(gamutLimit) FIELD(colorize) FIELD(smoothCurve)
#undef FIELD
    throw std::invalid_argument("unknown color adjustment setting");
}
void effectField(RasterEffect &e, std::string_view name, double value)
{
#define FIELD(x) if (name == #x) { e.x=value; return; }
    FIELD(amount) FIELD(radius) FIELD(angle) FIELD(scale) FIELD(detail) FIELD(threshold)
    FIELD(midpoint) FIELD(roundness) FIELD(feather)
#undef FIELD
#define FIELD(x) if (name == #x) { e.x=boolean(value); return; }
    FIELD(monochromatic) FIELD(edgeAware) FIELD(natural)
#undef FIELD
    if (name=="seed") {
        if (!std::isfinite(value) || value<0 || value>std::numeric_limits<std::uint32_t>::max() || value!=std::trunc(value))
            throw std::invalid_argument("invalid effect seed");
        e.seed=static_cast<std::uint32_t>(value); return;
    }
    throw std::invalid_argument("unknown raster effect setting");
}
template<class Function> void fields(const IiscCanvasSetting *settings, size_t count, Function &&function)
{
    if ((!settings && count) || count>128) throw std::invalid_argument("invalid settings array");
    for (size_t i=0;i<count;++i) {
        if (!settings[i].name) throw std::invalid_argument("missing setting name");
        function(std::string_view(settings[i].name),settings[i].value);
    }
}
// JSON output is built locally from validated UTF-8 document fields. Control
// bytes are escaped so identifiers never change the result's structure.
void jsonString(std::string &out,std::string_view value)
{
    constexpr char hex[]="0123456789abcdef";
    out+='"';
    for (const unsigned char c:value) {
        if (c=='"' || c=='\\') { out+='\\'; out+=char(c); }
        else if (c<0x20) { out+="\\u00"; out+=hex[c>>4]; out+=hex[c&15]; }
        else out+=char(c);
    }
    out+='"';
}
void jsonOutput(const std::string &value,uint8_t *out,size_t capacity,size_t *required)
{
    if (!required) throw std::invalid_argument("missing output size");
    *required=value.size();
    if (out) {
        if (capacity<value.size()) throw std::invalid_argument("output buffer too small");
        std::copy(value.begin(),value.end(),out);
    }
}
const char *requiredString(const char *value)
{
    if (!value) throw std::invalid_argument("missing string argument");
    return value;
}
// Convert a borrowed C value to an owned native path command. Geometry is
// validated by the document before any candidate is committed.
PathCommand vectorCommand(const IiscVectorCommand &command)
{
    switch (command.kind) {
    case 0: return MoveTo{{command.x1,command.y1}};
    case 1: return LineTo{{command.x1,command.y1}};
    case 2: return QuadraticTo{{command.x1,command.y1},{command.x2,command.y2}};
    case 3: return CubicTo{{command.x1,command.y1},{command.x2,command.y2},{command.x3,command.y3}};
    case 4: return ClosePath{};
    default: throw std::invalid_argument("unknown vector path command");
    }
}
std::vector<std::string> layerIds(const char *const *ids,std::size_t count)
{
    if (!ids || !count || count>65536) throw std::invalid_argument("invalid layer ID array");
    std::vector<std::string> result; result.reserve(count);
    for (std::size_t i=0;i<count;++i) result.emplace_back(requiredString(ids[i]));
    return result;
}
std::vector<LayerGeometry> geometry(const IiscLayerGeometry *values,std::size_t count)
{
    if (!values || !count || count>65536) throw std::invalid_argument("invalid layer geometry array");
    std::vector<LayerGeometry> result; result.reserve(count);
    for (std::size_t i=0;i<count;++i)
        result.push_back({requiredString(values[i].id),{values[i].x,values[i].y,values[i].width,values[i].height}});
    return result;
}
// Bridge arrays are deliberately capped before constructing any owned copy.
RasterLayer borrowedRaster(const uint32_t *data,size_t count,int32_t width,int32_t height,size_t maximumPixels=16777216)
{
    if (!data || width<=0 || height<=0 || std::uint64_t(width)*height!=count || count>maximumPixels)
        throw std::invalid_argument("invalid or excessive raster array");
    return {width,height,{data,data+count}};
}
std::optional<RasterMask> borrowedMask(const uint8_t *data,size_t count,CanvasExtent extent)
{
    if (!data && !count) return std::nullopt;
    if (!data || extent.width<=0 || extent.height<=0 || std::uint64_t(extent.width)*extent.height!=count || count>16777216)
        throw std::invalid_argument("invalid or excessive selection array");
    return RasterMask{extent,{data,data+count}};
}
std::vector<std::string_view> idViews(const std::vector<std::string> &ids)
{
    std::vector<std::string_view> views; views.reserve(ids.size());
    for (const auto &id:ids) views.push_back(id);
    return views;
}
FrameTileRenderResult selectedComposition(const Document &document,FrameIndex frame,
    const FrameRenderTileRequest &request,const std::vector<std::string> &ids,
    const std::optional<RasterMask> &mask)
{
    const auto batch=renderFrameLayers(document,frame,{request});
    const std::span<const RasterMask> masks=mask ? std::span<const RasterMask>(&*mask,1) : std::span<const RasterMask>();
    if (ids.empty()) return MergedFrameComposite{}(batch,masks);
    const auto views=idViews(ids);
    return SelectedFrameComposite{}(batch,views,masks);
}
} // namespace

extern "C" {
int iisc_canvas_sequence_flatten_prepare(IiscCanvasControl *c,const char *sequence,const char *clip,
    uint64_t maximumFrames,uint64_t maximumSnapshotBytes,IiscSequenceFlatten **output,IiscSequenceFlattenInfo *info)
{
    return invoke(c,[&](auto &canvas){
        if(!output || !info)throw std::invalid_argument("flatten preparation needs output storage");
        auto plan=std::make_unique<IiscSequenceFlatten>();
        plan->value=SequenceRasterFlattenPrepare{}(canvas.document,requiredString(sequence),requiredString(clip),maximumFrames,maximumSnapshotBytes);
        if(!plan->value.ok())throw std::runtime_error(plan->value.error);
        *info={plan->value.firstSequenceFrame,plan->value.frameCount,plan->value.extent.width,plan->value.extent.height,
               plan->value.sourceSequenceId.c_str(),plan->value.sourceClipId.c_str()};
        *output=plan.release();
    });
}
int iisc_canvas_sequence_flatten_write(IiscCanvasControl *c,const IiscSequenceFlatten *plan,const char *snapshot,const char *note)
{
    return invoke(c,[&](auto &){
        if(!plan)throw std::invalid_argument("missing flatten preparation");
        const auto result=SequenceRasterFlattenTemporaryWriter{}(plan->value,
            std::filesystem::u8path(requiredString(snapshot)),std::filesystem::u8path(requiredString(note)));
        if(!result.ok())throw std::runtime_error(result.error+(result.noteWritten ? "; temporary note was created" : ""));
    });
}
int iisc_canvas_sequence_flatten_commit(IiscCanvasControl *c,const IiscSequenceFlatten *plan,const uint32_t *pixels,size_t count,
    const char *source,const char *representation,const char *stream,const char *owned,const char *original,uint64_t maximumBytes)
{
    return invoke(c,[&](auto &canvas){
        if(!plan || !pixels || plan->value.extent.width<=0 || plan->value.extent.height<=0)
            throw std::invalid_argument("invalid flatten frame input");
        const auto size=std::uint64_t(plan->value.extent.width)*plan->value.extent.height;
        if(size>std::numeric_limits<size_t>::max() || plan->value.frameCount>std::numeric_limits<size_t>::max()/size ||
           count!=size*plan->value.frameCount || maximumBytes<24 || count>(maximumBytes-24)/4)
            throw std::invalid_argument("flatten frame array exceeds its count or byte budget");
        std::vector<RasterLayer> frames;frames.reserve(std::size_t(plan->value.frameCount));
        for(std::uint64_t i=0;i<plan->value.frameCount;++i){
            const auto *first=pixels+std::size_t(i*size);
            frames.push_back({plan->value.extent.width,plan->value.extent.height,{first,first+size}});
        }
        auto result=SequenceRasterFlattenCommit{}(canvas.document,plan->value,frames,
            {requiredString(source),requiredString(representation),requiredString(stream),requiredString(owned),requiredString(original)},maximumBytes);
        if(!result.ok())throw std::runtime_error(result.error);
        accepted(canvas.history.setSequenceProject(std::move(*result.project),std::move(result.media),"Flatten nested sequence"));
    });
}
void iisc_sequence_flatten_destroy(IiscSequenceFlatten *plan){delete plan;}
int iisc_canvas_sequence_flatten_replace_original(IiscCanvasControl *c,const char *sequence,const char *clip,
    const IiscCanvasControl *edited,uint64_t maximumSnapshotBytes)
{
    return invoke(c,[&](auto &canvas){
        if(!edited)throw std::invalid_argument("an edited source document is required");
        auto result=SequenceRasterFlattenReplaceOriginal{}(canvas.document,requiredString(sequence),requiredString(clip),
                                                            edited->document,maximumSnapshotBytes);
        if(!result.ok())throw std::runtime_error(result.error);
        accepted(canvas.history.setSequenceProject(std::move(*result.project),std::move(result.media),"Edit flattened sequence source"));
    });
}
int iisc_raw_sequence_frame(const uint8_t *bytes,size_t size,uint64_t frame,size_t maximumPixels,
    uint32_t *output,size_t capacity,int32_t *width,int32_t *height)
{
    try{
        if(!bytes || !output || !width || !height)return 0;
        const auto result=RawSequenceFrameReader{}({bytes,size},frame,maximumPixels);
        if(!result.ok() || result.pixels.pixels.size()!=capacity)return 0;
        std::copy(result.pixels.pixels.begin(),result.pixels.pixels.end(),output);
        *width=result.pixels.width;*height=result.pixels.height;return 1;
    }catch(...){return 0;}
}
int iisc_estimate_optical_flow(const uint32_t *first,const uint32_t *second,size_t count,int32_t width,int32_t height,
    uint32_t search,uint32_t patch,uint64_t maximumComparisons,size_t maximumPixels,double *forward,double *backward)
{
    try {
        if(!forward || !backward || count>std::numeric_limits<size_t>::max()/2)return 0;
        const auto a=borrowedRaster(first,count,width,height,maximumPixels),b=borrowedRaster(second,count,width,height,maximumPixels);
        OpticalFlowOptions options;options.searchRadius=search;options.patchRadius=patch;options.maximumComparisons=maximumComparisons;
        options.maximumPixels=maximumPixels;
        const auto result=OpticalFlowEstimator{}(a,b,options);
        if(!result.ok())return 0;
        for(size_t i=0;i<count;++i){forward[2*i]=result.flow.forward.vectors[i].x;forward[2*i+1]=result.flow.forward.vectors[i].y;
            backward[2*i]=result.flow.backward.vectors[i].x;backward[2*i+1]=result.flow.backward.vectors[i].y;}
        return 1;
    } catch(...) {return 0;}
}
int iisc_interpolate_optical_flow(const uint32_t *first,const uint32_t *second,size_t count,int32_t width,int32_t height,
    const double *forward,const double *backward,double phase,size_t maximumPixels,uint32_t *output)
{
    try {
        if(!forward || !backward || !output || count>std::numeric_limits<size_t>::max()/2)return 0;
        const auto a=borrowedRaster(first,count,width,height,maximumPixels),b=borrowedRaster(second,count,width,height,maximumPixels);
        OpticalFlowPair pair;pair.forward.width=pair.backward.width=width;pair.forward.height=pair.backward.height=height;
        pair.forward.vectors.reserve(count);pair.backward.vectors.reserve(count);
        for(size_t i=0;i<count;++i){pair.forward.vectors.push_back({forward[2*i],forward[2*i+1]});
            pair.backward.vectors.push_back({backward[2*i],backward[2*i+1]});}
        const auto result=OpticalFlowInterpolator{}(a,b,pair,phase,maximumPixels);
        if(!result.ok())return 0;
        std::copy(result.pixels.pixels.begin(),result.pixels.pixels.end(),output);
        return 1;
    } catch(...) {return 0;}
}
int iisc_calculate_channels(const uint32_t *first,const uint32_t *second,size_t count,int32_t width,int32_t height,
    int firstChannel,int secondChannel,int invertFirst,int invertSecond,int invertResult,int blend,double opacity,
    const uint8_t *selection,size_t selectionSize,uint8_t *output)
{
    try {
        if (!output) return 0;
        const auto a=borrowedRaster(first,count,width,height),b=borrowedRaster(second,count,width,height);
        const auto mask=borrowedMask(selection,selectionSize,{width,height});
        const ChannelSource sourceA{&a,static_cast<RasterChannel>(firstChannel),boolean(invertFirst)};
        const ChannelSource sourceB{&b,static_cast<RasterChannel>(secondChannel),boolean(invertSecond)};
        ChannelCompositeOptions options; options.blendMode=static_cast<RasterBlendMode>(blend);
        options.opacity=opacity; options.selection=mask ? &*mask : nullptr;
        const auto result=ChannelCalculations{}(sourceA,sourceB,boolean(invertResult),options);
        if (!result.ok()) return 0;
        std::copy(result.mask.alpha.begin(),result.mask.alpha.end(),output);
        return 1;
    } catch (...) { return 0; }
}
int iisc_canvas_apply_image(IiscCanvasControl *c,const char *layer,uint32_t frame,const uint32_t *source,size_t count,
    int32_t width,int32_t height,int sourceChannel,int targetChannel,int invert,int blend,double opacity,
    const uint8_t *selection,size_t selectionSize)
{
    return invoke(c,[&](auto &v) {
        const auto *targetLayer=findLayer(v.document,requiredString(layer));
        const auto *targetAsset=targetLayer ? resolveAssetAt(v.document,*targetLayer,frame) : nullptr;
        const auto *target=targetAsset ? std::get_if<RasterAsset>(targetAsset) : nullptr;
        if (!target) throw std::invalid_argument("target layer or frame has no finite raster asset");
        const auto revision=v.document.authorship.revision();
        const auto image=borrowedRaster(source,count,width,height);
        const auto mask=borrowedMask(selection,selectionSize,{width,height});
        ChannelCompositeOptions options; options.blendMode=static_cast<RasterBlendMode>(blend);
        options.opacity=opacity; options.selection=mask ? &*mask : nullptr;
        auto result=ApplyImageComposite{}({&image,static_cast<RasterChannel>(sourceChannel),boolean(invert)},
            target->pixels,static_cast<RasterChannel>(targetChannel),options);
        if (!result.ok()) throw std::invalid_argument(result.error);
        std::string fresh;
        for (std::size_t serial=1;serial<=v.document.assets.size()+v.document.audioAssets.size()+1;++serial) {
            fresh=std::string(layer)+"-apply-"+std::to_string(serial);
            if (!findAsset(v.document,fresh) && !findAudioAsset(v.document,fresh)) break;
            fresh.clear();
        }
        if (fresh.empty()) throw std::invalid_argument("cannot allocate a fresh asset identifier");
        accepted(v.history.replaceLayerFrameRaster(layer,frame,target->pixels,
            {std::move(fresh),std::move(result.pixels)},revision,"Apply image"));
    });
}
int iisc_canvas_set_frame_count(IiscCanvasControl *c,uint32_t count)
{
    return invoke(c,[&](auto &v) {
        auto timeline=v.document.timeline; timeline.frameCount=count;
        accepted(v.history.setTimeline(timeline,"Set frame count"));
    });
}
int iisc_canvas_composite(IiscCanvasControl *c,const char *const *ids,size_t idCount,uint32_t frame,
    int32_t x,int32_t y,int32_t width,int32_t height,int32_t outWidth,int32_t outHeight,int sampling,
    const uint8_t *selection,size_t selectionSize,uint32_t *output,size_t count)
{
    return invoke(c,[&](auto &v) {
        if (!output || outWidth<=0 || outHeight<=0 || std::uint64_t(outWidth)*outHeight!=count ||
            count>16777216 || (sampling!=0 && sampling!=1) || (!ids && idCount))
            throw std::invalid_argument("invalid composite output");
        const auto selected=idCount ? layerIds(ids,idCount) : std::vector<std::string>{};
        const auto mask=borrowedMask(selection,selectionSize,{outWidth,outHeight});
        const FrameRenderTileRequest request{{{x,y},{width,height}},{outWidth,outHeight},static_cast<RasterSampling>(sampling)};
        const auto result=selectedComposition(v.document,frame,request,selected,mask);
        if (!result.ok() || result.tiles.size()!=1) throw std::invalid_argument(result.message);
        std::copy(result.tiles.front().pixels.pixels.begin(),result.tiles.front().pixels.pixels.end(),output);
    });
}
int iisc_canvas_layer_css(IiscCanvasControl *c,const char *layer,uint32_t frame,uint8_t *output,size_t capacity,size_t *required)
{
    return invoke(c,[&](auto &v) {
        const auto index=layerIndex(v.document,requiredString(layer));
        if (!index || frame>=v.document.timeline.frameCount) throw std::invalid_argument("layer or frame was not found");
        const auto &selected=v.document.layers[*index];
        const auto *asset=resolveAssetAt(v.document,selected,frame);
        if (!asset) throw std::invalid_argument("layer has no content at the selected frame");
        CanvasExtent extent; const FillLayerContent *fill=nullptr;
        if (const auto *r=std::get_if<RasterAsset>(asset)) extent={r->pixels.width,r->pixels.height};
        else if (const auto *vector=std::get_if<VectorAsset>(asset)) extent=vector->viewport;
        else if ((fill=std::get_if<FillLayerContent>(asset))) extent=fill->extent;
        else if (const auto *adjustment=std::get_if<AdjustmentLayerContent>(asset)) extent=adjustment->extent;
        else throw std::invalid_argument("layer content does not have a finite CSS extent");
        const auto result=LayerCssEncoder{}(layerProperties(selected),extent,fill,&v.document.relations.groups);
        if (!result.ok()) throw std::invalid_argument(result.error);
        std::string json="{\"declarations\":"; jsonString(json,result.declarations); json+=",\"losses\":[";
        for (std::size_t i=0;i<result.losses.size();++i) { if (i) json+=','; jsonString(json,result.losses[i]); }
        json+="]}"; jsonOutput(json,output,capacity,required);
    });
}
int iisc_canvas_selected_svg(IiscCanvasControl *c,const char *const *ids,size_t idCount,uint32_t frame,
    int32_t x,int32_t y,int32_t width,int32_t height,uint8_t *output,size_t capacity,size_t *required)
{
    return invoke(c,[&](auto &v) {
        const auto selected=layerIds(ids,idCount); const auto views=idViews(selected);
        const FrameRenderTileRequest request{{{x,y},{width,height}},{width,height},RasterSampling::Nearest};
        const auto batch=renderFrameLayers(v.document,frame,{request});
        const auto rendered=SelectedFrameComposite{}(batch,views);
        if (!rendered.ok() || rendered.tiles.size()!=1) throw std::invalid_argument(rendered.message);
        std::vector<RasterLayer> layerPixels; layerPixels.reserve(selected.size());
        std::vector<SvgLayerRasterFallback> layerFallbacks; layerFallbacks.reserve(selected.size());
        for (const auto &id:selected) {
            const std::string_view view=id;
            auto single=SelectedFrameComposite{}(batch,std::span<const std::string_view>(&view,1));
            if (!single.ok() || single.tiles.size()!=1) continue;
            layerPixels.push_back(std::move(single.tiles.front().pixels));
            layerFallbacks.push_back({id,&layerPixels.back()});
        }
        const auto result=SelectedLayerSvgProjection{}(v.document,frame,views,request.region,
            &rendered.tiles.front().pixels,{},layerFallbacks);
        if (!result.ok()) throw std::invalid_argument(result.error);
        std::string json="{\"svg\":"; jsonString(json,result.svg); json+=",\"rasterized\":";
        json+=result.rasterized ? "true" : "false"; json+=",\"rasterized_layers\":[";
        for (std::size_t i=0;i<result.rasterizedLayers.size();++i) { if (i) json+=','; jsonString(json,result.rasterizedLayers[i]); }
        json+="],\"warnings\":[";
        for (std::size_t i=0;i<result.warnings.size();++i) { if (i) json+=','; jsonString(json,result.warnings[i]); }
        json+="]}"; jsonOutput(json,output,capacity,required);
    });
}
int iisc_canvas_put_fill(IiscCanvasControl *c,const char *id,const char *layer,const IiscFillSpec *spec)
{
    return invoke(c,[&](auto &v) {
        if (!spec || (!spec->stops && spec->stop_count) || spec->stop_count>256 || spec->kind<0 || spec->kind>3)
            throw std::invalid_argument("invalid fill specification");
        FillLayerContent fill; fill.id=requiredString(id);
        fill.extent={spec->width,spec->height}; fill.kind=static_cast<FillLayerKind>(spec->kind);
        fill.spread=boolean(spec->repeat) ? FillGradientSpread::Repeat : FillGradientSpread::Pad;
        fill.color=spec->color; fill.start={spec->start_x,spec->start_y}; fill.end={spec->end_x,spec->end_y};
        fill.patternOrigin={spec->pattern_x,spec->pattern_y}; fill.patternAssetId=spec->pattern_asset ? spec->pattern_asset : "";
        for (size_t i=0;i<spec->stop_count;++i) fill.stops.push_back({spec->stops[i].position,spec->stops[i].color});
        if (layer) accepted(v.history.insertLayerWithAsset(fill,StaticBitmapLayer{{layer,layer},StaticSource{fill.id}}));
        else accepted(v.history.replaceProceduralContent(std::move(fill)));
    });
}
int iisc_canvas_put_adjustment(IiscCanvasControl *c,const char *id,const char *layer,int32_t width,int32_t height,
    const IiscCanvasSetting *settings,size_t count)
{
    return invoke(c,[&](auto &v) {
        AdjustmentLayerContent content; content.id=requiredString(id); content.extent={width,height};
        fields(settings,count,[&](auto name,double value) { adjustmentField(content.adjustment,name,value); });
        if (layer) accepted(v.history.insertLayerWithAsset(content,StaticBitmapLayer{{layer,layer},StaticSource{content.id}}));
        else accepted(v.history.replaceProceduralContent(std::move(content)));
    });
}
int iisc_canvas_styles(IiscCanvasControl *c,const char *id,const IiscLayerStyle *styles,size_t count)
{
    return invoke(c,[&](auto &v) {
        if ((!styles && count) || count>64) throw std::invalid_argument("invalid style array");
        const auto *layer=findLayer(v.document,requiredString(id));
        if (!layer) throw std::invalid_argument("layer was not found");
        auto appearance=layerProperties(*layer).appearance; appearance.styles.effects.clear();
        for (size_t i=0;i<count;++i) {
            const auto &s=styles[i]; LayerStyle effect;
            if (s.kind<0 || s.kind>static_cast<int>(LayerStyleKind::Bevel))
                throw std::invalid_argument("unknown layer style kind");
            effect.kind=static_cast<LayerStyleKind>(s.kind); effect.enabled=boolean(s.enabled); effect.color=s.color;
            effect.opacity=s.opacity; effect.radius=s.radius; effect.offsetX=s.offset_x; effect.offsetY=s.offset_y;
            appearance.styles.effects.push_back(effect);
        }
        accepted(v.history.setLayerAppearance(id,std::move(appearance)));
    });
}
int iisc_canvas_mask(IiscCanvasControl *c,const char *id,int vector,const IiscMaskSpec *spec)
{
    return invoke(c,[&](auto &v) {
        const bool vectorMask=boolean(vector);
        std::optional<LayerMask> mask;
        if (spec) {
            if (spec->channel<0 || spec->channel>static_cast<int>(MaskChannel::Alpha))
                throw std::invalid_argument("unknown mask channel");
            mask.emplace(); mask->assetId=requiredString(spec->asset);
            mask->enabled=boolean(spec->enabled); mask->inverted=boolean(spec->inverted);
            mask->linkedToLayer=boolean(spec->linked); mask->channel=static_cast<MaskChannel>(spec->channel);
            mask->density=spec->density; mask->featherRadius=spec->feather; mask->outsideCoverage=spec->outside;
            mask->transform.m11=spec->m11; mask->transform.m12=spec->m12; mask->transform.m21=spec->m21;
            mask->transform.m22=spec->m22; mask->transform.translationX=spec->x; mask->transform.translationY=spec->y;
        }
        const auto candidate=vectorMask ? VectorMaskOperations{}(v.document,requiredString(id),std::move(mask))
                                        : LayerMaskOperations{}(v.document,requiredString(id),std::move(mask));
        if (!candidate.ok()) throw std::invalid_argument(candidate.error);
        accepted(v.history.setLayerAppearance(id,candidate.appearance));
    });
}
int iisc_canvas_slice(IiscCanvasControl *c,const char *layer,uint32_t frame,int32_t x,int32_t y,
    int32_t width,int32_t height,int32_t outputWidth,int32_t outputHeight,int sampling,uint32_t *output,size_t count)
{
    return invoke(c,[&](auto &v) {
        if (!output || outputWidth<=0 || outputHeight<=0 ||
            std::uint64_t(outputWidth)*outputHeight!=count || (sampling!=0 && sampling!=1))
            throw std::invalid_argument("invalid slice output");
        LayerSliceDescriptor descriptor;
        descriptor.source=layer ? LayerSliceSource::Layer : LayerSliceSource::Composite;
        if (layer) descriptor.layerId=layer;
        descriptor.frame=frame; descriptor.region={{x,y},{width,height}};
        descriptor.outputExtent={outputWidth,outputHeight}; descriptor.sampling=static_cast<RasterSampling>(sampling);
        // This bridge is the external controller: it selects a source, renders
        // it, resolves ancestor visibility and passes only detached data onward.
        FrameRenderResult rendered;
        if (descriptor.source==LayerSliceSource::Composite) {
            rendered=renderFrameRegion(v.document,frame,descriptor.region,descriptor.outputExtent,descriptor.sampling);
        } else {
            const auto index=layerIndex(v.document,descriptor.layerId);
            if (!index) throw std::invalid_argument("layer was not found");
            const FrameRenderTileRequest request{descriptor.region,descriptor.outputExtent,descriptor.sampling};
            const auto tile=renderFrameLayerTiles(v.document,frame,*index,{request});
            if (!tile.ok()) throw std::runtime_error(tile.message);
            const auto *asset=resolveAssetAt(v.document,v.document.layers[*index],frame);
            if (asset && std::holds_alternative<AdjustmentLayerContent>(*asset))
                throw std::invalid_argument("adjustment layers require their backdrop");
            bool visible=true; auto group=v.document.relations.groups.groupOf(descriptor.layerId);
            std::size_t visited=0;
            while (!group.empty()) {
                if (++visited>v.document.relations.groups.groups().size())
                    throw std::invalid_argument("invalid layer group cycle");
                const auto *parent=v.document.relations.groups.find(group);
                if (!parent) throw std::invalid_argument("layer group was not found");
                visible=visible && parent->visible; group=parent->parentId;
            }
            rendered=LayerRasterizationPass{}(tile,request,visible);
        }
        auto result=LayerSliceBuilder{}(rendered,descriptor.region,descriptor);
        if (!result.ok()) throw std::runtime_error(result.message);
        std::copy(result.pixels.pixels.begin(),result.pixels.pixels.end(),output);
    });
}
int iisc_canvas_arrange(IiscCanvasControl *c,const char *const *ids,size_t count,int action)
{
    return invoke(c,[&](auto &v) {
        if (action<0 || action>3) throw std::invalid_argument("unknown arrangement action");
        const auto input=layerIds(ids,count);
        const auto plan=LayerArrangementPlanner{}(v.document,input,static_cast<LayerArrangement>(action));
        if (!plan.ok()) throw std::invalid_argument(plan.error);
        accepted(v.history.applyLayerOrder(plan.order));
    });
}
int iisc_canvas_align(IiscCanvasControl *c,const IiscLayerGeometry *items,size_t count,
    double x,double y,double width,double height,int alignment)
{
    return invoke(c,[&](auto &v) {
        if (alignment<0 || alignment>5) throw std::invalid_argument("unknown alignment");
        const auto input=geometry(items,count);
        const auto plan=LayerAlignmentSolver{}(input,{x,y,width,height},static_cast<LayerAlignment>(alignment));
        if (!plan.ok()) throw std::invalid_argument(plan.error);
        accepted(v.history.translateLayers(plan.offsets));
    });
}
int iisc_canvas_distribute(IiscCanvasControl *c,const IiscLayerGeometry *items,size_t count,int axis,int mode)
{
    return invoke(c,[&](auto &v) {
        if (axis<0 || axis>1 || mode<0 || mode>1) throw std::invalid_argument("unknown distribution");
        const auto input=geometry(items,count);
        const auto plan=LayerDistributionSolver{}(input,static_cast<LayerAxis>(axis),static_cast<LayerDistribution>(mode));
        if (!plan.ok()) throw std::invalid_argument(plan.error);
        accepted(v.history.translateLayers(plan.offsets,"Distribute layers"));
    });
}
int iisc_canvas_put_vector(IiscCanvasControl *c,const char *asset,const char *layer,
    const IiscVectorCommand *commands,size_t count,uint32_t color)
{
    return invoke(c,[&](auto &v) {
        if (!commands || !count || count>1048576) throw std::invalid_argument("invalid vector command array");
        VectorPath path; path.fill=SolidPaint{color}; path.commands.reserve(count);
        for (std::size_t i=0;i<count;++i) path.commands.push_back(vectorCommand(commands[i]));
        VectorAsset value{requiredString(asset),v.document.extent,{std::move(path)}};
        accepted(v.history.insertLayerWithAsset(value,StaticVectorLayer{{requiredString(layer),layer},StaticSource{value.id}}));
    });
}
int iisc_canvas_combine_shapes(IiscCanvasControl *c,const IiscShapeOperand *items,size_t count,int operation,
    const char *asset,const char *layer,uint32_t color)
{
    return invoke(c,[&](auto &v) {
        if (!items || count<2 || count>4096 || operation<0 || operation>3)
            throw std::invalid_argument("invalid shape operands or operation");
        std::vector<ShapeOperand> operands; operands.reserve(count);
        for (std::size_t i=0;i<count;++i) {
            const auto *source=findVectorAsset(v.document,requiredString(items[i].asset));
            if (!source) throw std::invalid_argument("vector asset was not found");
            AffineTransform transform; transform.m11=items[i].m11; transform.m12=items[i].m12;
            transform.m21=items[i].m21; transform.m22=items[i].m22;
            transform.translationX=items[i].x; transform.translationY=items[i].y;
            operands.push_back({source,transform});
        }
        auto result=ShapeCombinationPlan{}(operands,static_cast<ShapeBooleanOperation>(operation),
                                           requiredString(asset),v.document.extent,color);
        if (!result.ok()) throw std::invalid_argument(result.error);
        auto id=result.asset.id;
        accepted(v.history.insertLayerWithAsset(std::move(result.asset),
            StaticVectorLayer{{requiredString(layer),layer},StaticSource{std::move(id)}}));
    });
}
int iisc_canvas_vector_command(IiscCanvasControl *c,const char *asset,size_t path,size_t index,IiscVectorCommand *output)
{
    return invoke(c,[&](auto &v) {
        const auto *vector=findVectorAsset(v.document,requiredString(asset));
        if (!output || !vector || path>=vector->paths.size() || index>=vector->paths[path].commands.size())
            throw std::invalid_argument("vector command was not found");
        *output={};
        std::visit([&](const auto &value) {
            using T=std::decay_t<decltype(value)>;
            if constexpr (std::is_same_v<T,MoveTo> || std::is_same_v<T,LineTo>) {
                output->kind=std::is_same_v<T,MoveTo> ? 0 : 1; output->x1=value.point.x; output->y1=value.point.y;
            } else if constexpr (std::is_same_v<T,QuadraticTo>) {
                output->kind=2; output->x1=value.control.x; output->y1=value.control.y;
                output->x2=value.end.x; output->y2=value.end.y;
            } else if constexpr (std::is_same_v<T,CubicTo>) {
                output->kind=3; output->x1=value.control1.x; output->y1=value.control1.y;
                output->x2=value.control2.x; output->y2=value.control2.y; output->x3=value.end.x; output->y3=value.end.y;
            } else output->kind=4;
        },vector->paths[path].commands[index]);
    });
}
int iisc_canvas_edit_vector_command(IiscCanvasControl *c,const char *asset,size_t path,size_t index,const IiscVectorCommand *command)
{
    return invoke(c,[&](auto &v) {
        const auto *vector=findVectorAsset(v.document,requiredString(asset));
        if (!command || !vector || path>=vector->paths.size() || index>=vector->paths[path].commands.size())
            throw std::invalid_argument("vector command was not found");
        auto replacement=vector->paths[path]; replacement.commands[index]=vectorCommand(*command);
        accepted(v.history.replaceVectorPath(asset,path,std::move(replacement)));
    });
}
int iisc_scan_transparent_bounds(const uint32_t *data,size_t count,int32_t width,int32_t height,
    int32_t x,int32_t y,uint8_t threshold,IiscCanvasBounds *output)
{
    if (!output || !data || width<=0 || height<=0 || std::uint64_t(width)*height!=count || count>256ULL*1024*1024)
        return 0;
    try {
        auto raster=makeRasterLayer(width,height);
        std::copy_n(data,count,raster.pixels.begin());
        const auto bounds=TransparentBoundsScanner{}(raster,{x,y},threshold);
        if (!bounds.ok()) return 0;
        *output={};
        if (bounds.bounds) *output={1,bounds.bounds->origin.x,bounds.bounds->origin.y,
                                   bounds.bounds->extent.width,bounds.bounds->extent.height};
        return 1;
    } catch (...) { return 0; }
}
int iisc_canvas_content_bounds(IiscCanvasControl *c,int scope,int hasRegion,int32_t x,int32_t y,
    int32_t width,int32_t height,const uint32_t *frames,size_t count,uint8_t threshold,size_t maximumPixels,
    IiscCanvasBounds *output)
{
    return invoke(c,[&](auto &v) {
        if (!output || (!frames && count) || count>v.document.timeline.frameCount || scope<0 || scope>1 ||
            (hasRegion!=0 && hasRegion!=1)) throw std::invalid_argument("invalid content bounds request");
        CanvasContentBoundsRequest request; request.scope=static_cast<CanvasContentBoundsScope>(scope);
        if (hasRegion) request.region=CanvasRegion{{x,y},{width,height}};
        if (count) request.frames.assign(frames,frames+count);
        request.threshold=threshold; request.maximumPixels=maximumPixels;
        auto bounds=queryCanvasContentBounds(v.document,request);
        if (!bounds.ok()) throw std::invalid_argument(bounds.error);
        *output={};
        if (bounds.bounds) *output={1,bounds.bounds->origin.x,bounds.bounds->origin.y,
                                   bounds.bounds->extent.width,bounds.bounds->extent.height};
    });
}
int iisc_canvas_pixel_aspect(IiscCanvasControl *c,uint32_t *numerator,uint32_t *denominator)
{
    return invoke(c,[&](auto &v) {
        if (!numerator || !denominator) throw std::invalid_argument("missing pixel aspect output");
        *numerator=v.document.pixelAspectRatio.numerator; *denominator=v.document.pixelAspectRatio.denominator;
    });
}
int iisc_canvas_set_pixel_aspect(IiscCanvasControl *c,uint32_t numerator,uint32_t denominator)
{
    return invoke(c,[&](auto &v) { accepted(v.history.setPixelAspectRatio({numerator,denominator})); });
}
int iisc_canvas_display(IiscCanvasControl *c,uint32_t frame,int sampling,uint32_t *output,size_t capacity,
    int32_t *width,int32_t *height)
{
    return invoke(c,[&](auto &v) {
        if (!width || !height || sampling<0 || sampling>1) throw std::invalid_argument("invalid display request");
        auto image=renderDisplayFrame(v.document,frame,static_cast<RasterSampling>(sampling));
        if (!image.ok()) throw std::runtime_error(image.message);
        *width=image.pixels.width; *height=image.pixels.height;
        if (output) {
            if (capacity<image.pixels.pixels.size()) throw std::invalid_argument("display output is too small");
            std::copy(image.pixels.pixels.begin(),image.pixels.pixels.end(),output);
        }
    });
}
int iisc_canvas_resize(IiscCanvasControl *c,int32_t width,int32_t height,int anchor)
{
    return invoke(c,[&](auto &v) {
        auto plan=CanvasResizePlan{}(v.document,{width,height},static_cast<CanvasAnchor>(anchor));
        if (!plan.ok()) throw std::invalid_argument(plan.error);
        accepted(v.history.applyCanvasGeometry(plan.plan,"Resize canvas"));
    });
}
int iisc_canvas_crop(IiscCanvasControl *c,int32_t x,int32_t y,int32_t width,int32_t height)
{
    return invoke(c,[&](auto &v) {
        auto plan=CanvasCropTransaction{}(v.document,{{x,y},{width,height}});
        if (!plan.ok()) throw std::invalid_argument(plan.error);
        accepted(v.history.applyCanvasGeometry(plan.plan,"Crop canvas"));
    });
}
int iisc_canvas_resample(IiscCanvasControl *c,int32_t width,int32_t height,int mode)
{
    return invoke(c,[&](auto &v) {
        auto plan=DocumentResamplingPlan{}(v.document,{width,height},static_cast<RasterResamplingMode>(mode));
        if (!plan.ok()) throw std::invalid_argument(plan.error);
        accepted(v.history.applyCanvasAffine(plan.plan,"Resample document"));
    });
}
int iisc_canvas_rotate(IiscCanvasControl *c,double degrees)
{
    return invoke(c,[&](auto &v) {
        auto plan=ImageRotationPlan{}(v.document,degrees);
        if (!plan.ok()) throw std::invalid_argument(plan.error);
        accepted(v.history.applyCanvasAffine(plan.plan,"Rotate image"));
    });
}
int iisc_canvas_flatten(IiscCanvasControl *c,const char *layer,const char *prefix)
{
    return invoke(c,[&](auto &v) {
        std::vector<FrameRenderResult> frames; frames.reserve(v.document.timeline.frameCount);
        for (FrameIndex frame=0;frame<v.document.timeline.frameCount;++frame) frames.push_back(renderFrame(v.document,frame));
        auto plan=DocumentFlattenTransaction{}(v.document,frames,requiredString(layer),requiredString(prefix));
        if (!plan.ok()) throw std::invalid_argument(plan.error);
        accepted(v.history.applyLayerTransaction(std::move(plan.plan),"Flatten document"));
    });
}
int iisc_canvas_merge(IiscCanvasControl *c,const char *const *ids,size_t count,const char *layer,const char *prefix)
{
    return invoke(c,[&](auto &v) {
        const auto selected=layerIds(ids,count);
        auto plan=prepareVerifiedLayerMerge(v.document,selected,requiredString(layer),requiredString(prefix));
        if (!plan.ok()) throw std::invalid_argument(plan.error);
        accepted(v.history.applyLayerTransaction(std::move(plan.plan),"Merge layers"));
    });
}
int iisc_canvas_selection_set(IiscCanvasControl *c,const char *id,const char *name,
    int32_t x,int32_t y,int32_t width,int32_t height,const uint8_t *data,size_t size)
{
    return invoke(c,[&](auto &v) {
        requiredString(id); requiredString(name);
        if (width<=0 || height<=0 || std::uint64_t(width)*height!=size || (!data && size))
            throw std::invalid_argument("mask dimensions and alpha bytes do not match");
        RasterMask raster{{width,height},{data,data+size}};
        auto mask=TiledMask::fromRaster({{x,y},{width,height}},{x,y},raster);
        if (!mask.ok()) throw std::invalid_argument(mask.error);
        auto next=v.document.relations;
        NamedSelectionChannel channel{id,name,std::make_shared<TiledMask>(std::move(mask.mask))};
        const bool ok=next.selections.find(id) ? next.selections.replace(id,std::move(channel))
                                                : next.selections.insert(std::move(channel));
        if (!ok) throw std::invalid_argument("invalid selection channel");
        accepted(v.history.setSelectionLayerRelations(std::move(next)));
    });
}
int iisc_canvas_selection_rename(IiscCanvasControl *c,const char *id,const char *name)
{
    return invoke(c,[&](auto &v) { auto next=v.document.relations;
        if (!next.selections.rename(requiredString(id),requiredString(name))) throw std::invalid_argument("selection channel was not found");
        accepted(v.history.setSelectionLayerRelations(std::move(next))); });
}
int iisc_canvas_selection_delete(IiscCanvasControl *c,const char *id)
{
    return invoke(c,[&](auto &v) { auto next=v.document.relations;
        if (!next.selections.erase(requiredString(id))) throw std::invalid_argument("selection channel was not found");
        accepted(v.history.setSelectionLayerRelations(std::move(next))); });
}
int iisc_canvas_selection_coverage(IiscCanvasControl *c,const char *id,int32_t x,int32_t y,uint8_t *out)
{
    return invoke(c,[&](auto &v) { const auto *channel=v.document.relations.selections.find(requiredString(id));
        if (!channel || !out) throw std::invalid_argument("selection channel or output was not found");
        *out=channel->mask->coverage(x,y); });
}
int iisc_canvas_group_set(IiscCanvasControl *c,const char *id,const char *name,const char *parent,int mode,int visible)
{
    return invoke(c,[&](auto &v) {
        requiredString(id); requiredString(name); requiredString(parent);
        if ((mode!=0 && mode!=1) || (visible!=0 && visible!=1)) throw std::invalid_argument("invalid group mode or visibility");
        auto next=v.document.relations;
        if (next.groups.find(id)) {
            if (!next.groups.rename(id,name) || !next.groups.setParent(id,parent) ||
                !next.groups.setMode(id,static_cast<LayerGroupMode>(mode)) || !next.groups.setVisible(id,visible))
                throw std::invalid_argument("invalid group update");
        } else if (!next.groups.insert(LayerGroup{id,name,parent,static_cast<LayerGroupMode>(mode),bool(visible)}))
            throw std::invalid_argument("invalid group or parent");
        accepted(v.history.setSelectionLayerRelations(std::move(next)));
    });
}
int iisc_canvas_group_delete(IiscCanvasControl *c,const char *id)
{
    return invoke(c,[&](auto &v) { auto next=v.document.relations; requiredString(id);
        if (!next.groups.erase(id)) throw std::invalid_argument("group is missing or still has children");
        next.locks.setGroup(id,LayerLock::None);
        accepted(v.history.setSelectionLayerRelations(std::move(next))); });
}
int iisc_canvas_group_assign(IiscCanvasControl *c,const char *layer,const char *group)
{
    return invoke(c,[&](auto &v) { auto next=v.document.relations;
        if (!next.groups.assign(requiredString(layer),requiredString(group))) throw std::invalid_argument("invalid group membership");
        accepted(v.history.setSelectionLayerRelations(std::move(next))); });
}
int iisc_canvas_link(IiscCanvasControl *c,const char *first,const char *second,int connect)
{
    return invoke(c,[&](auto &v) { auto next=v.document.relations; requiredString(first); requiredString(second);
        if ((connect!=0 && connect!=1) || !(connect ? next.links.connect(first,second) : next.links.disconnect(first,second)))
            throw std::invalid_argument("invalid or duplicate link");
        accepted(v.history.setSelectionLayerRelations(std::move(next))); });
}
int iisc_canvas_lock(IiscCanvasControl *c,const char *id,int group,uint8_t flags)
{
    return invoke(c,[&](auto &v) { auto next=v.document.relations; requiredString(id);
        if ((group!=0 && group!=1) || !(group ? next.locks.setGroup(id,static_cast<LayerLock>(flags)) :
                                               next.locks.setLayer(id,static_cast<LayerLock>(flags))))
            throw std::invalid_argument("invalid lock");
        accepted(v.history.setSelectionLayerRelations(std::move(next))); });
}
int iisc_canvas_relations(IiscCanvasControl *c,uint8_t *output,size_t capacity,size_t *required)
{
    return invoke(c,[&](auto &v) {
        const auto &state=v.document.relations; std::string out="{\"selections\":[";
        bool first=true;
        const auto comma=[&] { if (!first) out+=','; first=false; };
        for (const auto &channel:state.selections.channels()) {
            comma(); out+="{\"id\":"; jsonString(out,channel.id); out+=",\"name\":"; jsonString(out,channel.name);
            const auto domain=channel.mask->domain();
            out+=",\"origin\":["+std::to_string(domain.origin.x)+","+std::to_string(domain.origin.y)+"],\"extent\":["+
                std::to_string(domain.extent.width)+","+std::to_string(domain.extent.height)+"]}";
        }
        out+="],\"groups\":["; first=true;
        for (const auto &group:state.groups.groups()) {
            comma(); out+="{\"id\":"; jsonString(out,group.id); out+=",\"name\":"; jsonString(out,group.name);
            out+=",\"parent\":"; jsonString(out,group.parentId); out+=",\"mode\":";
            jsonString(out,group.mode==LayerGroupMode::PassThrough ? "pass_through" : "isolated");
            out+=group.visible ? ",\"visible\":true}" : ",\"visible\":false}";
        }
        out+="],\"memberships\":["; first=true;
        for (const auto &item:state.groups.memberships()) {
            comma(); out+='['; jsonString(out,item.layerId); out+=','; jsonString(out,item.groupId); out+=']';
        }
        out+="],\"links\":["; first=true;
        for (const auto &edge:state.links.edges()) {
            comma(); out+='['; jsonString(out,edge.first); out+=','; jsonString(out,edge.second); out+=']';
        }
        for (const auto &[name,entries]:{std::pair{"layer_locks",&state.locks.layers()},std::pair{"group_locks",&state.locks.groups()}}) {
            out+="],\""; out+=name; out+="\":["; first=true;
            for (const auto &entry:*entries) {
                comma(); out+='['; jsonString(out,entry.id); out+=','; out+=std::to_string(static_cast<unsigned>(entry.flags)); out+=']';
            }
        }
        out+="]}"; jsonOutput(out,output,capacity,required);
    });
}
int iisc_canvas_query(IiscCanvasControl *c,const char *name,int visible,int64_t frame,int role,
                      const char *group,uint8_t *output,size_t capacity,size_t *required)
{
    return invoke(c,[&](auto &v) {
        if (visible < -1 || visible > 1 || frame < -1 || frame>UINT32_MAX || role < -1 || role>1)
            throw std::invalid_argument("invalid layer query criterion");
        LayerQuery query; if (name) query.nameContains=name;
        if (visible>=0) query.visible=bool(visible);
        if (frame>=0) query.frame=static_cast<FrameIndex>(frame);
        if (role>=0) query.role=static_cast<LayerRole>(role);
        if (group) query.groupId=group;
        const LayerQueryIndex index(v.document); const auto matches=index.query(v.document,query);
        if (!matches.ok()) throw std::invalid_argument(matches.error);
        std::string out="["; bool first=true;
        for (const auto &id:matches.layerIds) { if (!first) out+=','; first=false; jsonString(out,id); }
        out+=']'; jsonOutput(out,output,capacity,required);
    });
}
IiscCanvasControl *iisc_canvas_clone(IiscCanvasControl *canvas)
{
    IiscCanvasControl *result=nullptr;
    invoke(canvas,[&](auto &source) {
        auto copy=DocumentCloneTransaction{}(source.document);
        if (!copy.ok()) throw std::runtime_error(copy.error.message);
        result=new IiscCanvasControl(std::move(copy.document));
    });
    return result;
}
IiscCanvasControl *iisc_canvas_raster(int32_t width,int32_t height,uint32_t argb)
{
    if (width<=0 || height<=0 || std::uint64_t(width)*height>SerializationLimits{}.maximumCanvasPixels) return nullptr;
    try {
        Document d; d.extent={width,height};
        d.assets.emplace_back(RasterAsset{"pixels",makeRasterLayer(width,height,argb)});
        d.layers.emplace_back(StaticBitmapLayer{{"layer","Layer"},StaticSource{"pixels"}});
        return new IiscCanvasControl(std::move(d));
    } catch (...) { return nullptr; }
}
IiscCanvasControl *iisc_canvas_from_channel(int32_t width,int32_t height,const uint8_t *coverage,size_t count)
{
    if (!coverage || width<=0 || height<=0 || std::uint64_t(width)*height!=count || count>16777216) return nullptr;
    try {
        auto pixels=makeRasterLayer(width,height,0xff000000U);
        for (std::size_t i=0;i<count;++i) pixels.pixels[i]=0xff000000U|std::uint32_t(coverage[i])*0x010101U;
        Document document; document.extent={width,height};
        document.assets.emplace_back(RasterAsset{"pixels",std::move(pixels)});
        document.layers.emplace_back(StaticBitmapLayer{{"layer","Layer"},StaticSource{"pixels"}});
        return new IiscCanvasControl(std::move(document));
    } catch (...) { return nullptr; }
}
IiscCanvasControl *iisc_canvas_decode(const uint8_t *data,size_t size)
{
    if (!data && size) return nullptr;
    try { auto decoded=decodeIisc(std::span{data,size}); return decoded.ok()?new IiscCanvasControl(std::move(decoded.document)):nullptr; }
    catch (...) { return nullptr; }
}
void iisc_canvas_destroy(IiscCanvasControl *canvas) { delete canvas; }
const char *iisc_canvas_error(const IiscCanvasControl *canvas) { return canvas?canvas->error.data():"canvas is closed"; }
int iisc_canvas_info(IiscCanvasControl *canvas,IiscCanvasInfo *out)
{
    return invoke(canvas,[&](auto &c) {
        if (!out) throw std::invalid_argument("missing output");
        const auto s=DocumentInfoSnapshotBuilder::build(c.document,&c.history);
        *out={s.extent.width,s.extent.height,s.timeline.frameCount,s.assetCount,s.layerCount,s.authorshipRevision,
              s.history->undoEntries,s.history->redoEntries};
    });
}
int iisc_canvas_encode(IiscCanvasControl *canvas,uint8_t *out,size_t capacity,size_t *required)
{
    return invoke(canvas,[&](auto &c) {
        if (!required) throw std::invalid_argument("missing output size");
        auto result=encodeIisc(c.document);
        if (!result.ok()) throw std::runtime_error(result.error.message);
        *required=result.bytes.size();
        if (out) { if (capacity<*required) throw std::invalid_argument("output buffer too small");
                   std::copy(result.bytes.begin(),result.bytes.end(),out); }
    });
}
int iisc_canvas_pixel(IiscCanvasControl *canvas,uint32_t frame,int32_t x,int32_t y,uint32_t *out)
{
    return invoke(canvas,[&](auto &c) {
        if (!out) throw std::invalid_argument("missing pixel output");
        auto rendered=renderFrame(c.document,frame);
        if (!rendered.ok()) throw std::runtime_error(rendered.message);
        const auto localX=std::int64_t(x)-rendered.origin.x, localY=std::int64_t(y)-rendered.origin.y;
        if (localX<0 || localY<0 || localX>=rendered.pixels.width || localY>=rendered.pixels.height)
            throw std::out_of_range("pixel is outside the rendered canvas");
        *out=rendered.pixels.pixels[std::size_t(localY)*rendered.pixels.width+std::size_t(localX)];
    });
}
int iisc_canvas_embed(IiscCanvasControl *canvas,const IiscCanvasControl *source,const char *id,const char *layer,uint32_t frame)
{
    return invoke(canvas,[&](auto &c) {
        if (!source || !id || !layer) throw std::invalid_argument("missing document or identifier");
        auto copy=LinkedAssetResolver::copy(id,source->document,frame);
        if (!copy.ok()) throw std::runtime_error(copy.error.message);
        Layer entry=StaticBitmapLayer{{layer,layer},StaticSource{id}};
        accepted(c.history.insertLayerWithAsset(std::move(copy.asset),std::move(entry)));
    });
}
int iisc_canvas_replace(IiscCanvasControl *canvas,const IiscCanvasControl *source,const char *id,uint32_t frame)
{
    return invoke(canvas,[&](auto &c) {
        if (!source || !id) throw std::invalid_argument("missing document or identifier");
        auto copy=LinkedAssetResolver::copy(id,source->document,frame);
        if (!copy.ok()) throw std::runtime_error(copy.error.message);
        accepted(c.history.replaceLinkedCanvasAsset(std::move(copy.asset)));
    });
}
int iisc_canvas_undo(IiscCanvasControl *c) { return invoke(c,[](auto &v){accepted(v.history.undo());}); }
int iisc_canvas_redo(IiscCanvasControl *c) { return invoke(c,[](auto &v){accepted(v.history.redo());}); }
int iisc_canvas_capture_last(IiscCanvasControl *c) { return invoke(c,[](auto &v){
    auto toggle=std::make_unique<LastEditToggle>(v.history);
    if (!toggle->hasEntry()) throw std::runtime_error("there is no edit to toggle");
    v.toggle=std::move(toggle);
}); }
int iisc_canvas_toggle_last(IiscCanvasControl *c) { return invoke(c,[](auto &v){
    if (!v.toggle) throw std::runtime_error("no edit was captured"); accepted(v.toggle->toggle());
}); }
int iisc_canvas_begin_reedit(IiscCanvasControl *c,const char *id,size_t budget) { return invoke(c,[&](auto &v){
    if (!id) throw std::invalid_argument("missing asset id");
    auto reedit=std::make_unique<ReeditableRasterChange>(v.history,id,budget);
    if (!reedit->ready()) throw std::runtime_error("raster is unavailable or exceeds the memory budget");
    v.reedit=std::move(reedit);
}); }
int iisc_canvas_adjust(IiscCanvasControl *c,const IiscCanvasSetting *values,size_t count) { return invoke(c,[&](auto &v){
    if (!v.reedit) throw std::runtime_error("no reeditable raster is active");
    ColorAdjustment adjustment; fields(values,count,[&](auto name,auto value){adjustmentField(adjustment,name,value);});
    accepted(v.reedit->apply(adjustment));
}); }
int iisc_canvas_effect(IiscCanvasControl *c,int32_t kind,const IiscCanvasSetting *values,size_t count) { return invoke(c,[&](auto &v){
    if (!v.reedit) throw std::runtime_error("no reeditable raster is active");
    if (kind<0 || kind>static_cast<int>(RasterEffectKind::Grayscale)) throw std::invalid_argument("invalid effect kind");
    RasterEffect effect; effect.kind=static_cast<RasterEffectKind>(kind);
    fields(values,count,[&](auto name,auto value){effectField(effect,name,value);}); accepted(v.reedit->apply(effect));
}); }
} // extern "C"

#include "SequenceBridge_p.hpp"
#include "SequenceModelBridge_p.hpp"
