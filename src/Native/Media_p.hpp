#pragma once
#include "Media/MediaIo.h"
#include <iiSDKCore/Core/Text/Utf8.h>
#include <span>
#include <filesystem>

namespace iiSharedCanvas::media_detail {
inline MediaIoResult error(MediaIoCode code, std::string message) { return {code,std::move(message),{}}; }
inline bool validText(std::string_view value) { return value.find('\0')==value.npos && ii::core::valid_utf8(value); }
MediaIoResult checkExtent(CanvasExtent,const MediaLimits&);
MediaIoResult checkRaster(const RasterLayer&,const MediaLimits&);
MediaIoResult checkDestination(const std::string&,bool,std::filesystem::path&);
MediaBytesResult readFile(const std::string&,const MediaLimits&);
MediaIoResult writeFile(const std::string&,std::span<const std::uint8_t>,bool,const MediaLimits&);
std::u16string utf16(std::string_view);
// A canonical sRGB profile exported from the system's named sRGB color space.
std::span<const std::uint8_t> srgbProfile();
bool isSrgbProfile(std::span<const std::uint8_t>);
struct PdfBounds final {
    double x=0,y=0,w=0,h=0;
    double left()const{return x;} double top()const{return y;}
    double right()const{return x+w;} double bottom()const{return y+h;}
    double width()const{return w;} double height()const{return h;}
};
PdfBounds vectorBounds(const VectorAsset&);
std::vector<std::uint8_t> vectorPdf(const VectorAsset&,PdfBounds,std::uint64_t);
}
