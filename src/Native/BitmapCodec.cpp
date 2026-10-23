#include "Bitmap/BitmapCodec.h"
#include "Native/Media_p.hpp"
#include "Render/FrameRenderer.h"
#include <iiImageCodec/ImageCodec.h>
#include <algorithm>
#include <cctype>

namespace iiSharedCanvas {
namespace {
std::string normalized(std::string text){for(auto& c:text)c=char(std::tolower(static_cast<unsigned char>(c)));if(text=="jpg")text="jpeg";if(text=="tif")text="tiff";return text;}
ii::image::Format format(std::string text){text=normalized(std::move(text));using F=ii::image::Format;
    if(text=="png")return F::png;if(text=="jpeg")return F::jpeg;if(text=="bmp")return F::bmp;if(text=="gif")return F::gif;if(text=="tga")return F::tga;if(text=="psd")return F::psd;if(text=="webp")return F::webp;if(text=="tiff")return F::tiff;return F::unknown;}
std::string name(ii::image::Format f){using F=ii::image::Format;switch(f){case F::png:return "png";case F::jpeg:return "jpeg";case F::bmp:return "bmp";case F::gif:return "gif";case F::tga:return "tga";case F::psd:return "psd";case F::webp:return "webp";case F::tiff:return "tiff";default:return {};}}
MediaIoResult failure(const ii::image::Error& e){using C=ii::image::ErrorCode;return media_detail::error(e.code==C::unsupported_format?MediaIoCode::UnsupportedFormat:e.code==C::limit_exceeded?MediaIoCode::LimitExceeded:MediaIoCode::InvalidData,e.message);}
ii::image::Limits bounds(const MediaLimits& l){return {.input_bytes=std::size_t(std::min(l.maxInputBytes,std::uint64_t(SIZE_MAX))),.output_bytes=std::size_t(std::min(l.maxOutputBytes,std::uint64_t(SIZE_MAX))),.pixel_bytes=std::size_t(std::min({l.maxDecodedBytes,l.maxPixelsPerFrame>UINT64_MAX/4?UINT64_MAX:l.maxPixelsPerFrame*4,std::uint64_t(SIZE_MAX)}))};}
}
std::vector<MediaFormatCapability> bitmapFormats(const MediaBackendOptions&,bool){return {{"png",true,true},{"jpeg",true,true},{"bmp",true,true},{"gif",true,false},{"tga",true,true},{"psd",true,false},{"webp",true,true},{"tiff",true,true}};}
BitmapImportResult decodeBitmap(std::span<const std::uint8_t> bytes,const BitmapImportOptions& options){
    BitmapImportResult result;
    if(options.assetId.empty()||!media_detail::validText(options.assetId)){result.result=media_detail::error(MediaIoCode::InvalidArgument,"A canonical UTF-8 asset id is required");return result;}
    if(options.imageIndex){result.result=media_detail::error(MediaIoCode::UnsupportedFeature,"Native bitmap decoding exposes the first image; use document/timeline import for animation");return result;}
    auto info=ii::image::inspect(bytes,bounds(options.limits));if(!info){result.result=failure(info.error());return result;}
    if(!options.applyOrientation&&info->format!=ii::image::Format::png){result.result=media_detail::error(MediaIoCode::UnsupportedFeature,"Unoriented native bitmap import is supported for PNG layer data only");return result;}
    result.format=name(info->format);if(!options.format.empty()&&format(options.format)!=info->format){result.result=media_detail::error(MediaIoCode::UnsupportedFormat,"Explicit bitmap format disagrees with the content");return result;}
    auto decoded=ii::image::decode(bytes,bounds(options.limits));if(!decoded){result.result=failure(decoded.error());return result;}
    if(!decoded->metadata.icc_profile.empty()&&!media_detail::isSrgbProfile(decoded->metadata.icc_profile)){decoded=ii::image::to_srgb(std::move(*decoded),bounds(options.limits));if(!decoded){result.result=failure(decoded.error());return result;}}
    result.asset.id=options.assetId;auto& raster=result.asset.pixels;raster.width=int(decoded->width);raster.height=int(decoded->height);raster.pixels.resize(std::size_t(raster.width)*raster.height);
    for(std::size_t i=0;i<raster.pixels.size();++i){auto* p=decoded->rgba.data()+4*i;raster.pixels[i]=(std::uint32_t(p[3])<<24)|(std::uint32_t(p[0])<<16)|(std::uint32_t(p[1])<<8)|p[2];}
    if(decoded->metadata.dots_per_meter_x||decoded->metadata.dots_per_meter_y)result.result.warnings.emplace_back("Print resolution is not retained by the canvas pixel model");
    return result;
}
BitmapImportResult importBitmap(const std::string& path,const BitmapImportOptions& options){auto bytes=media_detail::readFile(path,options.limits);if(!bytes.ok()){BitmapImportResult result;result.result=std::move(bytes.result);return result;}return decodeBitmap(bytes.bytes,options);}
MediaBytesResult encodeBitmap(const RasterLayer& raster,const BitmapExportOptions& options){
    auto valid=media_detail::checkRaster(raster,options.limits);if(!valid.ok())return {{},std::move(valid)};
    if(!options.text.empty())return {{},media_detail::error(MediaIoCode::UnsupportedFeature,"Native bitmap text metadata requires an explicit metadata container")};
    ii::image::Image image;image.width=std::uint32_t(raster.width);image.height=std::uint32_t(raster.height);image.rgba.resize(raster.pixels.size()*4);image.metadata.icc_profile.assign(media_detail::srgbProfile().begin(),media_detail::srgbProfile().end());
    for(std::size_t i=0;i<raster.pixels.size();++i){auto* p=image.rgba.data()+4*i;const auto c=raster.pixels[i];p[0]=std::uint8_t(c>>16);p[1]=std::uint8_t(c>>8);p[2]=std::uint8_t(c);p[3]=std::uint8_t(c>>24);}
    auto encoded=ii::image::encode(image.view(),format(options.format),{.quality=options.quality<0?90:options.quality,.matte_rgb=options.matteArgb&0xffffff},bounds(options.limits));if(!encoded)return {{},failure(encoded.error())};return {std::move(*encoded),{}};
}
MediaIoResult exportBitmap(const RasterLayer& raster,const std::string& path,const BitmapExportOptions& options){auto encoded=encodeBitmap(raster,options);if(!encoded.ok())return std::move(encoded.result);return media_detail::writeFile(path,encoded.bytes,options.overwrite,options.limits);}
MediaIoResult exportBitmapFrame(const Document& document,FrameIndex frame,const std::string& path,const BitmapExportOptions& options){auto rendered=renderFrame(document,frame);if(!rendered.ok())return media_detail::error(MediaIoCode::InvalidData,rendered.message);return exportBitmap(rendered.pixels,path,options);}
}
