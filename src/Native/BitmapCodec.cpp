#include "Bitmap/BitmapCodec.h"
#include "Native/Media_p.hpp"
#include "Native/PngText_p.hpp"
#include "Native/Qoi_p.hpp"
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
std::vector<MediaFormatCapability> bitmapFormats(const MediaBackendOptions&,bool extended){std::vector<MediaFormatCapability> result{{"png",true,true},{"jpeg",true,true},{"bmp",true,true},{"gif",true,false},{"tga",true,true},{"psd",true,false},{"webp",true,true},{"tiff",true,true}};if(extended)result.push_back({"qoi",true,true});return result;}
BitmapImportResult decodeBitmap(std::span<const std::uint8_t> bytes,const BitmapImportOptions& options){
    BitmapImportResult result;
    if(options.assetId.empty()||!media_detail::validText(options.assetId)){result.result=media_detail::error(MediaIoCode::InvalidArgument,"A canonical UTF-8 asset id is required");return result;}
    if(options.imageIndex){result.result=media_detail::error(MediaIoCode::UnsupportedFeature,"Native bitmap decoding exposes the first image; use document/timeline import for animation");return result;}
    const bool qoi=bytes.size()>=4&&bytes[0]=='q'&&bytes[1]=='o'&&bytes[2]=='i'&&bytes[3]=='f';
    if(qoi||normalized(options.format)=="qoi"){
        if(!options.extendedCodecs||(!options.format.empty()&&normalized(options.format)!="qoi")){result.result=media_detail::error(MediaIoCode::UnsupportedFormat,"QOI extended codec is disabled or conflicts with the requested format");return result;}
        return media_detail::decodeQoi(bytes,options);
    }
    auto info=ii::image::inspect(bytes,bounds(options.limits));if(!info){result.result=failure(info.error());return result;}
    if(!options.applyOrientation&&info->format!=ii::image::Format::png){result.result=media_detail::error(MediaIoCode::UnsupportedFeature,"Unoriented native bitmap import is supported for PNG layer data only");return result;}
    result.format=name(info->format);if(!options.format.empty()&&format(options.format)!=info->format){result.result=media_detail::error(MediaIoCode::UnsupportedFormat,"Explicit bitmap format disagrees with the content");return result;}
    if(info->format==ii::image::Format::png){auto text=media_detail::readPngText(bytes,options.limits.maxDecodedBytes);if(!text.result.ok()){result.result=std::move(text.result);return result;}result.text=std::move(text.text);}
    auto decoded=ii::image::decode(bytes,bounds(options.limits));if(!decoded){result.text.clear();result.result=failure(decoded.error());return result;}
    if(!decoded->metadata.icc_profile.empty()&&!media_detail::isSrgbProfile(decoded->metadata.icc_profile)){decoded=ii::image::to_srgb(std::move(*decoded),bounds(options.limits));if(!decoded){result.text.clear();result.result=failure(decoded.error());return result;}}
    result.asset.id=options.assetId;auto& raster=result.asset.pixels;raster.width=int(decoded->width);raster.height=int(decoded->height);raster.pixels.resize(std::size_t(raster.width)*raster.height);
    for(std::size_t i=0;i<raster.pixels.size();++i){auto* p=decoded->rgba.data()+4*i;raster.pixels[i]=(std::uint32_t(p[3])<<24)|(std::uint32_t(p[0])<<16)|(std::uint32_t(p[1])<<8)|p[2];}
    if(decoded->metadata.dots_per_meter_x||decoded->metadata.dots_per_meter_y)result.result.warnings.emplace_back("Print resolution is not retained by the canvas pixel model");
    return result;
}
BitmapImportResult importBitmap(const std::string& path,const BitmapImportOptions& options){auto bytes=media_detail::readFile(path,options.limits);if(!bytes.ok()){BitmapImportResult result;result.result=std::move(bytes.result);return result;}return decodeBitmap(bytes.bytes,options);}
MediaBytesResult encodeBitmap(const RasterLayer& raster,const BitmapExportOptions& options){
    auto valid=media_detail::checkRaster(raster,options.limits);if(!valid.ok())return {{},std::move(valid)};
    if(options.quality < -1 || options.quality > 100)return {{},media_detail::error(MediaIoCode::InvalidArgument,"Bitmap quality must be -1 or between 0 and 100")};
    const auto requestedFormat=format(options.format);
    if(auto text=media_detail::validateTextEntries(options.text,requestedFormat==ii::image::Format::png);!text.ok())return {{},std::move(text)};
    if(normalized(options.format)=="qoi"){
        if(!options.extendedCodecs)return {{},media_detail::error(MediaIoCode::UnsupportedFormat,"QOI extended codec is disabled")};
        return media_detail::encodeQoi(raster,options);
    }
    ii::image::Image image;image.width=std::uint32_t(raster.width);image.height=std::uint32_t(raster.height);image.rgba.resize(raster.pixels.size()*4);image.metadata.icc_profile.assign(media_detail::srgbProfile().begin(),media_detail::srgbProfile().end());
    for(std::size_t i=0;i<raster.pixels.size();++i){auto* p=image.rgba.data()+4*i;const auto c=raster.pixels[i];p[0]=std::uint8_t(c>>16);p[1]=std::uint8_t(c>>8);p[2]=std::uint8_t(c);p[3]=std::uint8_t(c>>24);}
    auto encoded=ii::image::encode(image.view(),requestedFormat,{.quality=options.quality<0?90:options.quality,.matte_rgb=options.matteArgb&0xffffff},bounds(options.limits));if(!encoded)return {{},failure(encoded.error())};
    if(requestedFormat==ii::image::Format::png)return media_detail::writePngText(std::move(*encoded),options.text,options.limits.maxOutputBytes);
    MediaBytesResult result{std::move(*encoded),{}};
    if(!options.text.empty())result.result.warnings.emplace_back("text metadata retention is only guaranteed for PNG");
    if(requestedFormat==ii::image::Format::jpeg&&std::any_of(raster.pixels.begin(),raster.pixels.end(),[](auto pixel){return (pixel>>24)!=255;}))result.result.warnings.emplace_back("alpha composited onto matte for the selected bitmap format");
    return result;
}
MediaIoResult exportBitmap(const RasterLayer& raster,const std::string& path,const BitmapExportOptions& options){auto encoded=encodeBitmap(raster,options);if(!encoded.ok())return std::move(encoded.result);auto result=media_detail::writeFile(path,encoded.bytes,options.overwrite,options.limits);result.warnings=std::move(encoded.result.warnings);return result;}
MediaIoResult exportBitmapFrame(const Document& document,FrameIndex frame,const std::string& path,const BitmapExportOptions& options){
    const auto budget=std::size_t(std::min({options.limits.maxPixelsPerFrame,options.limits.maxDecodedBytes/4,std::uint64_t(SIZE_MAX)}));
    if (!displayPixelExtent(documentViewRegion(document).extent,document.pixelAspectRatio,budget))
        return media_detail::error(MediaIoCode::LimitExceeded,"display extent exceeds export limits");
    auto rendered=document.pixelAspectRatio.numerator==document.pixelAspectRatio.denominator
        ? renderFrame(document,frame) : renderDisplayFrame(document,frame,RasterSampling::Smooth,budget);
    if(!rendered.ok())return media_detail::error(MediaIoCode::InvalidData,rendered.message);
    return exportBitmap(rendered.pixels,path,options);
}
}
