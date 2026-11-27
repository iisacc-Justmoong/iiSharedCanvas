#include <iiSharedCanvas>

#include <array>
#include <string_view>

int main()
{
    using namespace iiSharedCanvas;
    using namespace std::literals;
    auto source=makeRasterLayer(1,1,0xff801020U);
    auto target=makeRasterLayer(1,1,0xff102030U);
    const auto result=ApplyImageComposite{}({&source,RasterChannel::Red,false},target,RasterChannel::Green);
    if (!result.ok() || result.pixels.pixels[0]!=0xff108030U) return 1;
    const auto calculated=ChannelCalculations{}({&source,RasterChannel::Red,false},{&target,RasterChannel::Alpha,false});
    if (!calculated.ok() || calculated.mask.alpha[0]!=128) return 2;
    Document document; document.extent={1,1};
    document.assets.emplace_back(RasterAsset{"pixels",source});
    document.layers.emplace_back(StaticBitmapLayer{{"layer","Layer"},StaticSource{"pixels"}});
    const FrameRenderTileRequest request{{{0,0},{1,1}},{1,1},RasterSampling::Nearest};
    const auto batch=renderFrameLayers(document,0,{request});
    const std::array ids{"layer"sv};
    const auto selected=SelectedFrameComposite{}(batch,ids);
    const auto merged=MergedFrameComposite{}(batch);
    if (!selected.ok() || !merged.ok() || selected.tiles[0].pixels.pixels!=merged.tiles[0].pixels.pixels) return 3;
    const auto css=LayerCssEncoder{}(layerProperties(document.layers[0]),{1,1});
    const auto svg=SelectedLayerSvgProjection{}(document,0,ids,request.region,&selected.tiles[0].pixels);
    if (!css.ok() || css.losses.empty() || !svg.ok() || !svg.rasterized) return 4;
    document.timeline.frameCount=3;
    document.layers.emplace_back(StaticBitmapLayer{{"other","Other"},StaticSource{"pixels"}});
    DocumentHistory history(document);
    if (!history.replaceLayerFrameRaster("layer",1,source,{"isolated",target},document.authorship.revision()).changed) return 5;
    const auto pixel=[&](std::string_view id,FrameIndex frame) {
        const auto *layer=findLayer(document,std::string(id));
        const auto *asset=layer ? resolveAssetAt(document,*layer,frame) : nullptr;
        const auto *raster=asset ? std::get_if<RasterAsset>(asset) : nullptr;
        return raster ? raster->pixels.pixels[0] : 0U;
    };
    if (pixel("layer",0)!=source.pixels[0] || pixel("layer",1)!=target.pixels[0] ||
        pixel("layer",2)!=source.pixels[0] || pixel("other",1)!=source.pixels[0] ||
        !history.undo().changed || pixel("layer",1)!=source.pixels[0]) return 6;

    Document hybrid; hybrid.extent={1,1};
    VectorPath path; path.commands={MoveTo{{0,0}},LineTo{{1,0}},LineTo{{1,1}},ClosePath{}};
    path.fill=SolidPaint{0xff00ff00U};
    hybrid.assets.emplace_back(VectorAsset{"vector",{1,1},{path}});
    hybrid.assets.emplace_back(RasterAsset{"bitmap",source});
    hybrid.layers.emplace_back(StaticVectorLayer{{"vector-layer","Vector"},StaticSource{"vector"}});
    hybrid.layers.emplace_back(StaticBitmapLayer{{"bitmap-layer","Bitmap"},StaticSource{"bitmap"}});
    const std::array hybridIds{"vector-layer"sv,"bitmap-layer"sv};
    const std::array perLayer{SvgLayerRasterFallback{"bitmap-layer",&source}};
    const auto mixed=SelectedLayerSvgProjection{}(hybrid,0,hybridIds,request.region,&source,{},perLayer);
    if (!mixed.ok() || mixed.rasterizedLayers!=std::vector<std::string>{"bitmap-layer"} ||
        mixed.svg.find("<path ")==std::string::npos || mixed.svg.find("<image ")==std::string::npos) return 7;
    return 0;
}
