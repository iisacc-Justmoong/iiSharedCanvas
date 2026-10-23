#include "Media_p.hpp"
#include "SrgbProfile_p.hpp"
#include <iiFileProviderNative.h>
#include <iiFileProviderCompatibility.h>
#include <algorithm>
#include <cstring>
#include <limits>

namespace iiSharedCanvas::media_detail {
MediaIoResult checkExtent(CanvasExtent extent,const MediaLimits& limits) {
    if(extent.width<=0||extent.height<=0)return error(MediaIoCode::InvalidArgument,"Raster extent must be positive");
    const auto pixels=std::uint64_t(extent.width)*extent.height;
    if(pixels>limits.maxPixelsPerFrame||pixels>limits.maxDecodedBytes/4)return error(MediaIoCode::LimitExceeded,"Raster exceeds the decoded pixel budget");
    return {};
}
MediaIoResult checkRaster(const RasterLayer& raster,const MediaLimits& limits) {
    auto result=checkExtent({raster.width,raster.height},limits);if(!result.ok())return result;
    if(raster.pixels.size()!=std::uint64_t(raster.width)*raster.height)return error(MediaIoCode::InvalidArgument,"Raster pixel count disagrees with its extent");
    return {};
}
namespace {
bool localPath(std::string_view path){return !path.empty()&&validText(path)&&path.find("://")==path.npos;}
MediaIoResult ioError(const std::exception& error){return media_detail::error(MediaIoCode::IoError,error.what());}
}
MediaIoResult checkDestination(const std::string& path,bool overwrite,std::filesystem::path& absolute){
    if(!localPath(path))return error(MediaIoCode::InvalidArgument,"A local UTF-8 destination path is required");
    std::error_code ec;absolute=std::filesystem::absolute(std::filesystem::u8path(path),ec);
    if(ec)return error(MediaIoCode::IoError,ec.message());
    const auto status=std::filesystem::symlink_status(absolute,ec);
    if(ec&&ec!=std::errc::no_such_file_or_directory)return error(MediaIoCode::IoError,ec.message());
    if(std::filesystem::exists(status)&&(!overwrite||!std::filesystem::is_regular_file(status)))return error(MediaIoCode::AlreadyExists,"Destination already exists or is not a regular file");
    return {};
}
MediaBytesResult readFile(const std::string& path,const MediaLimits& limits){
    if(!localPath(path))return {{},error(MediaIoCode::InvalidArgument,"A local UTF-8 input path is required")};
    try{auto source=iiFileProvider::native::File::read(std::filesystem::u8path(path),limits.maxInputBytes);return {{source.begin(),source.end()},{}};}
    catch(const iiFileProvider::native::FileError& e){return {{},error(e.code()==iiFileProvider::native::FileCode::LimitExceeded?MediaIoCode::LimitExceeded:MediaIoCode::IoError,e.what())};}
    catch(const std::exception& e){return {{},ioError(e)};}
}
MediaIoResult writeFile(const std::string& path,std::span<const std::uint8_t> bytes,bool overwrite,const MediaLimits& limits){
    if(bytes.size()>limits.maxOutputBytes)return error(MediaIoCode::LimitExceeded,"Encoded media exceeds the output byte budget");
    std::filesystem::path destination;auto result=checkDestination(path,overwrite,destination);if(!result.ok())return result;
    try{std::string_view content(reinterpret_cast<const char*>(bytes.data()),bytes.size());
        if(overwrite)iiFileProvider::native::writeCompatible(destination,content);else iiFileProvider::native::File::create(destination,content);
        return {};
    }catch(const iiFileProvider::native::FileError& e){return error(e.code()==iiFileProvider::native::FileCode::AlreadyExists?MediaIoCode::AlreadyExists:MediaIoCode::IoError,e.what());}
    catch(const std::exception& e){return ioError(e);}
}
std::u16string utf16(std::string_view source){
    if(!ii::core::valid_utf8(source))throw std::invalid_argument("Text is not canonical UTF-8");
    std::u16string out;out.reserve(source.size());
    for(std::size_t i=0;i<source.size();){auto cp=std::uint32_t(std::uint8_t(source[i++]));
        if(cp>=0x80){const auto n=cp<0xe0?1:cp<0xf0?2:3;cp&=(1U<<(6-n))-1;for(int j=0;j<n;++j)cp=(cp<<6)|(std::uint8_t(source[i++])&63);}
        if(cp<=0xffff)out.push_back(char16_t(cp));else{cp-=0x10000;out.push_back(char16_t(0xd800+(cp>>10)));out.push_back(char16_t(0xdc00+(cp&1023)));}}
    return out;
}
std::span<const std::uint8_t> srgbProfile(){return NativeSrgbProfile;}
bool isSrgbProfile(std::span<const std::uint8_t> input){
    auto u32=[](std::span<const std::uint8_t> v,std::size_t i){return (std::uint32_t(v[i])<<24)|(std::uint32_t(v[i+1])<<16)|(std::uint32_t(v[i+2])<<8)|v[i+3];};
    if(input.size()<132||u32(input,0)!=input.size()||std::memcmp(input.data()+36,"acsp",4)||std::memcmp(input.data()+16,"RGB ",4))return false;
    const auto reference=srgbProfile();const auto count=u32(input,128);if(count>(input.size()-132)/12)return false;
    // Compare actual primaries/transfer functions; dates, profile ids and human
    // descriptions do not determine color semantics.
    for(const auto* name:{"rXYZ","gXYZ","bXYZ","rTRC","gTRC","bTRC","wtpt"}){
        std::span<const std::uint8_t> a,b;
        for(auto pair:{std::pair{input,&a},std::pair{reference,&b}}){const auto n=u32(pair.first,128);for(std::size_t i=0;i<n;++i){const auto at=132+12*i;if(at+12>pair.first.size())return false;if(std::memcmp(pair.first.data()+at,name,4))continue;const auto offset=u32(pair.first,at+4),size=u32(pair.first,at+8);if(offset>pair.first.size()||size>pair.first.size()-offset)return false;*pair.second=pair.first.subspan(offset,size);break;}}
        if(a.empty()||!std::ranges::equal(a,b))return false;
    }
    return true;
}
}
