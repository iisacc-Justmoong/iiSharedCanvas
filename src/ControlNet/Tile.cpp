#include "ControlNet/Tile.h"
#include "Document/Document.h"
#include "Validation/Validation.h"
#include <algorithm>
#include <optional>
#include <stdexcept>

namespace iiSharedCanvas {
TileAsset *findTileAsset(Document &doc, const std::string &id) noexcept {
    auto *asset=findAsset(doc,id); return asset ? std::get_if<TileAsset>(asset) : nullptr;
}
const TileAsset *findTileAsset(const Document &doc, const std::string &id) noexcept {
    const auto *asset=findAsset(doc,id); return asset ? std::get_if<TileAsset>(asset) : nullptr;
}
TileLayer *findTileLayer(Document &doc, const std::string &id) noexcept {
    auto *layer=findLayer(doc,id); return layer ? std::get_if<TileLayer>(layer) : nullptr;
}
const TileLayer *findTileLayer(const Document &doc, const std::string &id) noexcept {
    const auto *layer=findLayer(doc,id); return layer ? std::get_if<TileLayer>(layer) : nullptr;
}
std::string validateTileAsset(const TileAsset &asset) {
    if(asset.viewport.width<=0 || asset.viewport.height<=0
        || std::uint64_t(asset.viewport.width)*asset.viewport.height!=asset.colors.size())
        return "tile dimensions must be positive and match the dense sample count";
    return {};
}
RasterLayer tileRasterPreview(const TileAsset &asset, std::uint64_t maxPixels) {
    const auto error=validateTileAsset(asset);
    if(!error.empty()) throw std::invalid_argument(error);
    if(asset.colors.size()>maxPixels) throw std::length_error("tile output exceeds maxPixels");
    auto result=makeRasterLayer(asset.viewport.width,asset.viewport.height,0xff000000);
    for(std::size_t i=0;i<asset.colors.size();++i) {
        const auto &color=asset.colors[i];
        result.pixels[i]=0xff000000U | (std::uint32_t(color.red)<<16) | (std::uint32_t(color.green)<<8) | color.blue;
    }
    return result;
}
namespace {
TileControlMapResult renderRegion(const Document &doc, const std::string &id,
    std::uint32_t frame, std::optional<TileRegion> requested, std::uint64_t maxPixels) {
    const auto fail=[](std::string message){TileControlMapResult r; r.message=std::move(message); return r;};
    const auto validation=validate(doc);
    if (!validation.ok()) return fail(validation.issues.front().path+": "+validation.issues.front().message);
    const auto *base=findLayer(doc,id); const auto *layer=base ? std::get_if<TileLayer>(base) : nullptr;
    if (!layer) return fail("tile layer not found");
    if (!layer->control.enabled) return fail("ControlNet layer is disabled");
    const auto *source=resolveAssetAt(doc,*base,frame);
    const auto *asset=source ? std::get_if<TileAsset>(source) : nullptr;
    if (!asset) return fail("tile source is outside its frame range or timeline");
    const auto region=requested.value_or(TileRegion{0,0,asset->viewport.width,asset->viewport.height});
    if (region.x<0 || region.y<0 || region.width<=0 || region.height<=0
        || std::int64_t(region.x)+region.width>asset->viewport.width
        || std::int64_t(region.y)+region.height>asset->viewport.height)
        return fail("tile region must be positive and entirely within the source viewport");
    const auto count=std::uint64_t(region.width)*region.height;
    if (count>maxPixels) return fail("tile output exceeds maxPixels");
    TileControlMapResult result; result.region=region;
    result.pixels=makeRasterLayer(region.width,region.height,0xff000000);
    result.colors.reserve(static_cast<std::size_t>(count));
    for (std::int32_t y=0;y<region.height;++y) for (std::int32_t x=0;x<region.width;++x) {
        const auto &c=asset->colors[std::size_t(region.y+y)*asset->viewport.width+region.x+x];
        result.colors.push_back(c);
        result.pixels.pixels[std::size_t(y)*region.width+x]=0xff000000U
            | (std::uint32_t(c.red)<<16) | (std::uint32_t(c.green)<<8) | c.blue;
    }
    return result;
}
} // namespace
TileControlMapResult renderTileControlMap(const Document &doc, const std::string &id,
    std::uint32_t frame, std::uint64_t maxPixels) {
    return renderRegion(doc,id,frame,std::nullopt,maxPixels);
}
TileControlMapResult renderTileControlRegion(const Document &doc, const std::string &id,
    std::uint32_t frame, TileRegion region, std::uint64_t maxPixels) {
    return renderRegion(doc,id,frame,region,maxPixels);
}
TileAsset makeTileAsset(std::string id, const RasterLayer &source, std::uint64_t maxPixels) {
    if (id.empty()) throw std::invalid_argument("tile asset id must not be empty");
    if (source.width<=0 || source.height<=0
        || std::uint64_t(source.width)*source.height!=source.pixels.size())
        throw std::invalid_argument("tile source dimensions must match positive pixel count");
    if (source.pixels.size()>maxPixels) throw std::length_error("tile output exceeds maxPixels");
    if (std::any_of(source.pixels.begin(),source.pixels.end(),[](std::uint32_t p){return (p>>24)!=255;}))
        throw std::invalid_argument("tile source must be opaque; composite transparency explicitly");
    TileAsset result; result.id=std::move(id); result.viewport={source.width,source.height};
    result.colors.reserve(source.pixels.size());
    for (auto pixel:source.pixels) result.colors.push_back({static_cast<std::uint8_t>((pixel>>16)&255),
        static_cast<std::uint8_t>((pixel>>8)&255),static_cast<std::uint8_t>(pixel&255)});
    return result;
}
} // namespace iiSharedCanvas
