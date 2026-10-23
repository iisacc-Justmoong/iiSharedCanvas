#include "Layered/OpenRasterParser_p.hpp"
#include "Layered/BlendModeMapping_p.hpp"
#include "Bitmap/BitmapCodec.h"
#include "Native/Media_p.hpp"
#include <iiSDKCore/Core/Text/Number.h>
#include <libxml/parser.h>
#include <zlib.h>
#include <algorithm>
#include <charconv>
#include <map>
#include <cstring>
#include <climits>
#include <memory>
#include <set>
#include <stdexcept>

namespace iiSharedCanvas::layered_detail {
namespace {
struct Failure final {MediaIoCode code;std::string message;};
[[noreturn]] void reject(std::string message,MediaIoCode code=MediaIoCode::InvalidData){throw Failure{code,std::move(message)};}
std::uint16_t u16(std::span<const std::uint8_t> bytes,std::size_t at){if(at>bytes.size()||bytes.size()-at<2)reject("Truncated ZIP field");return std::uint16_t(bytes[at])|(std::uint16_t(bytes[at+1])<<8);}
std::uint32_t u32(std::span<const std::uint8_t> bytes,std::size_t at){return std::uint32_t(u16(bytes,at))|(std::uint32_t(u16(bytes,at+2))<<16);}
bool safeName(std::string_view path){if(path.empty()||path.front()=='/'||path.find_first_of("\\:\0",0,3)!=path.npos||!media_detail::validText(path))return false;while(!path.empty()){auto end=path.find('/');auto part=path.substr(0,end);if(part==".."||part==".")return false;if(end==path.npos)break;path.remove_prefix(end+1);}return true;}
struct Entry final {std::size_t data=0,compressed=0,expanded=0;std::uint32_t crc=0;unsigned method=0;};
class Archive final {
public:
    Archive(std::span<const std::uint8_t> bytes,const LayeredDocumentImportOptions& options):bytes_(bytes){
        if(bytes.size()<22)reject("Truncated OpenRaster archive");std::size_t end=bytes.size()-22;const auto first=bytes.size()>65557?bytes.size()-65557:0;
        for(;;){if(u32(bytes,end)==0x06054b50&&u16(bytes,end+20)==bytes.size()-end-22)break;if(end==first)reject("ZIP end record is missing");--end;}
        if(u16(bytes,end+4)||u16(bytes,end+6)||u16(bytes,end+8)!=u16(bytes,end+10))reject("Multipart ZIP archives are unsupported",MediaIoCode::UnsupportedFeature);
        const auto count=u16(bytes,end+10);if(count==65535||count>options.maxArchiveEntries)reject("ZIP entry count exceeds the limit",MediaIoCode::LimitExceeded);
        const auto centralSize=u32(bytes,end+12),central=u32(bytes,end+16);if(central>end||centralSize>end-central||central+centralSize!=end)reject("Invalid ZIP central directory bounds");
        std::size_t at=central;std::uint64_t expanded=0;
        for(unsigned i=0;i<count;++i){if(at>central+centralSize||central+centralSize-at<46||u32(bytes,at)!=0x02014b50)reject("Invalid ZIP entry");
            const auto flags=u16(bytes,at+8),method=u16(bytes,at+10),nameSize=u16(bytes,at+28),extra=u16(bytes,at+30),comment=u16(bytes,at+32);const auto packed=u32(bytes,at+20),size=u32(bytes,at+24),local=u32(bytes,at+42);
            if(flags&~0x808U||(method!=0&&method!=8)||u16(bytes,at+34))reject("Encrypted, multipart or unsupported ZIP compression",MediaIoCode::UnsupportedFeature);
            if(std::uint64_t(nameSize)+extra+comment>central+centralSize-at-46)reject("ZIP metadata exceeds the central directory");
            std::string name(reinterpret_cast<const char*>(bytes.data()+at+46),nameSize);if(!safeName(name)||entries.contains(name))reject("ZIP entry path is unsafe or duplicated");
            const auto type=(u32(bytes,at+38)>>16)&0170000;if(type&&type!=0100000&&type!=0040000)reject("ZIP links and special files are unsupported",MediaIoCode::UnsupportedFeature);
            if(local>central||central-local<30||u32(bytes,local)!=0x04034b50||u16(bytes,local+6)!=flags||u16(bytes,local+8)!=method)reject("Invalid ZIP local entry");
            const auto localName=u16(bytes,local+26),localExtra=u16(bytes,local+28);const auto header=std::uint64_t(local)+30+localName+localExtra;
            if(header>central||packed>central-header||localName!=nameSize||std::memcmp(bytes.data()+local+30,name.data(),nameSize))reject("ZIP local data or name disagrees with its directory");
            if(size>options.limits.maxDecodedBytes-expanded)reject("Expanded archive exceeds the decoded byte budget",MediaIoCode::LimitExceeded);expanded+=size;
            entries.emplace(std::move(name),Entry{std::size_t(header),packed,size,u32(bytes,at+16),method});at+=46+nameSize+extra+comment;
        }
        if(at!=end)reject("ZIP central directory has trailing records");
    }
    std::vector<std::uint8_t> read(std::string_view name)const {
        auto found=entries.find(name);if(found==entries.end())reject("Missing OpenRaster entry: "+std::string(name));const auto& e=found->second;std::vector<std::uint8_t> output(e.expanded);
        if(e.method==0){if(e.compressed!=e.expanded)reject("Stored ZIP entry size disagrees");std::copy_n(bytes_.data()+e.data,e.expanded,output.data());}
        else{z_stream z{};if(inflateInit2(&z,-MAX_WBITS)!=Z_OK)reject("Cannot initialize ZIP inflater");std::uint8_t dummy=0;z.next_in=const_cast<Bytef*>(bytes_.data()+e.data);z.avail_in=uInt(e.compressed);z.next_out=output.empty()?&dummy:output.data();z.avail_out=uInt(std::max(std::size_t(1),e.expanded));const auto result=inflate(&z,Z_FINISH);const bool valid=result==Z_STREAM_END&&z.total_out==e.expanded&&z.total_in==e.compressed;inflateEnd(&z);if(!valid)reject("Deflated ZIP entry is truncated or exceeds its declared size");}
        if(crc32(0,output.data(),uInt(output.size()))!=e.crc)reject("ZIP entry CRC disagrees");return output;
    }
    std::map<std::string,Entry,std::less<>> entries;
private:std::span<const std::uint8_t> bytes_;
};
std::string attribute(xmlNode* node,const char* name){std::unique_ptr<xmlChar,decltype(xmlFree)> value(xmlGetProp(node,BAD_CAST name),xmlFree);return value?reinterpret_cast<const char*>(value.get()):"";}
bool has(xmlNode* node,const char* name){return xmlHasProp(node,BAD_CAST name)!=nullptr;}
void attributes(xmlNode* node,std::initializer_list<std::string_view> allowed){if(node->ns||node->nsDef)reject("Namespaced OpenRaster drawing is unsupported",MediaIoCode::UnsupportedFeature);for(auto* a=node->properties;a;a=a->next){if(a->ns||std::ranges::find(allowed,reinterpret_cast<const char*>(a->name))==allowed.end())reject("Unsupported OpenRaster attribute",MediaIoCode::UnsupportedFeature);}}
int integer(std::string_view value){int out=0;auto result=std::from_chars(value.data(),value.data()+value.size(),out);if(value.empty()||result.ec!=std::errc{}||result.ptr!=value.data()+value.size())reject("Invalid OpenRaster integer");return out;}
LayerProperties properties(xmlNode* node){LayerProperties p;p.name=attribute(node,"name");if(!media_detail::validText(p.name))reject("Invalid OpenRaster layer name");
    if(has(node,"opacity")){auto v=ii::core::parse_decimal(attribute(node,"opacity"));if(!v||*v<0||*v>1)reject("Invalid OpenRaster opacity");p.opacity=*v;}
    if(has(node,"visibility")){const auto v=attribute(node,"visibility");if(v!="visible"&&v!="hidden")reject("Invalid OpenRaster visibility");p.visible=v=="visible";}
    if(has(node,"composite-op")){const auto v=attribute(node,"composite-op");auto found=std::ranges::find_if(OraBlendModes,[&](const auto& pair){return pair.second==v;});if(found==OraBlendModes.end())reject("Unsupported OpenRaster blend mode",MediaIoCode::UnsupportedFeature);p.blendMode=found->first;}
    if(has(node,"x"))p.transform.translationX=integer(attribute(node,"x"));if(has(node,"y"))p.transform.translationY=integer(attribute(node,"y"));return p;
}
struct Layer final {std::string source;LayerProperties properties;};
void parseChildren(xmlNode* parent,std::vector<Layer>& layers,const LayeredDocumentImportOptions& options,unsigned version,unsigned depth,std::vector<std::string>& warnings){
    if(depth>options.limits.maxXmlDepth)reject("OpenRaster XML nesting exceeds the limit",MediaIoCode::LimitExceeded);
    for(auto* node=parent->children;node;node=node->next){
        if(node->type==XML_COMMENT_NODE)continue;if(node->type==XML_TEXT_NODE){if(!xmlIsBlankNode(node))reject("OpenRaster drawing cannot contain text");continue;}if(node->type!=XML_ELEMENT_NODE)reject("OpenRaster entities and processing instructions are unsupported");
        const std::string_view name(reinterpret_cast<const char*>(node->name));
        if(name=="stack"){
            attributes(node,{"name","opacity","visibility","composite-op","isolation","x","y"});auto p=properties(node);
            if(attribute(node,"isolation")!="auto"||p.opacity!=1||!p.visible||p.blendMode!=RasterBlendMode::SourceOver)reject("Non-neutral OpenRaster groups cannot be flattened without changing composition",MediaIoCode::UnsupportedFeature);
            if(version>=6&&(has(node,"x")||has(node,"y")))reject("OpenRaster group coordinates are unsupported",MediaIoCode::UnsupportedFeature);
            warnings.emplace_back("Neutral OpenRaster pass-through group hierarchy was flattened into independent layers");parseChildren(node,layers,options,version,depth+1,warnings);
        }else if(name=="layer"){
            attributes(node,{"src","name","x","y","opacity","visibility","composite-op"});if(layers.size()>=options.maxLayers)reject("OpenRaster layer count exceeds the limit",MediaIoCode::LimitExceeded);
            auto source=attribute(node,"src");if(!safeName(source)||!source.ends_with(".png"))reject("OpenRaster layer requires a safe PNG entry");
            for(auto* child=node->children;child;child=child->next)if(child->type!=XML_COMMENT_NODE&&(child->type!=XML_TEXT_NODE||!xmlIsBlankNode(child)))reject("OpenRaster layer children are unsupported",MediaIoCode::UnsupportedFeature);
            layers.push_back({std::move(source),properties(node)});
        }else reject("Unsupported OpenRaster drawing element",MediaIoCode::UnsupportedFeature);
    }
}
}
LayeredDocumentImportResult decodeOpenRaster(std::span<const std::uint8_t> bytes,const LayeredDocumentImportOptions& options){
    try{Archive archive(bytes,options);const auto mime=archive.read("mimetype");if(std::string_view(reinterpret_cast<const char*>(mime.data()),mime.size())!="image/openraster")reject("OpenRaster mimetype disagrees");auto stack=archive.read("stack.xml");
        if(stack.size()>INT_MAX||!media_detail::validText(std::string_view(reinterpret_cast<const char*>(stack.data()),stack.size())))reject("OpenRaster stack must be bounded canonical UTF-8");
        std::unique_ptr<xmlDoc,decltype(&xmlFreeDoc)> xml(xmlReadMemory(reinterpret_cast<const char*>(stack.data()),int(stack.size()),"stack.xml","UTF-8",XML_PARSE_NONET|XML_PARSE_NOERROR|XML_PARSE_NOWARNING),xmlFreeDoc);
        if(!xml||xml->intSubset||xml->extSubset)reject("OpenRaster XML is invalid or declares a DTD");auto* root=xmlDocGetRootElement(xml.get());if(!root||std::string_view(reinterpret_cast<const char*>(root->name))!="image")reject("OpenRaster XML requires an image root");attributes(root,{"version","w","h","name","xres","yres"});const auto version=attribute(root,"version");if(version.size()!=5||!version.starts_with("0.0.")||version.back()<'1'||version.back()>'6')reject("Unsupported OpenRaster version",MediaIoCode::UnsupportedFeature);
        LayeredDocumentImportResult result;result.format="ora";result.document.extent={integer(attribute(root,"w")),integer(attribute(root,"h"))};auto valid=media_detail::checkExtent(result.document.extent,options.limits);if(!valid.ok())throw Failure{valid.code,valid.message};
        xmlNode* drawing=nullptr;for(auto* node=root->children;node;node=node->next){if(node->type==XML_COMMENT_NODE||(node->type==XML_TEXT_NODE&&xmlIsBlankNode(node)))continue;if(node->type!=XML_ELEMENT_NODE||drawing||std::string_view(reinterpret_cast<const char*>(node->name))!="stack")reject("OpenRaster requires exactly one root stack");drawing=node;}if(!drawing)reject("OpenRaster root stack is missing");attributes(drawing,{"name","opacity","visibility","composite-op","isolation"});
        std::vector<Layer> layers;parseChildren(drawing,layers,options,unsigned(version.back()-'0'),1,result.result.warnings);std::uint64_t decodedBytes=stack.size();
        for(auto it=layers.rbegin();it!=layers.rend();++it){auto image=archive.read(it->source);BitmapImportOptions settings;const auto index=std::to_string(result.document.layers.size());settings.assetId=options.idPrefix+"-asset-"+index;settings.format="png";settings.limits=options.limits;settings.limits.maxDecodedBytes=options.limits.maxDecodedBytes-decodedBytes;auto decoded=decodeBitmap(image,settings);if(!decoded.ok())throw Failure{decoded.result.code,decoded.result.message};decodedBytes+=decoded.asset.pixels.pixels.size()*4;it->properties.id=options.idPrefix+"-layer-"+index;result.document.layers.emplace_back(StaticBitmapLayer{std::move(it->properties),StaticSource{decoded.asset.id}});result.document.assets.emplace_back(std::move(decoded.asset));}
        return result;
    }catch(const Failure& error){return {{},"ora",media_detail::error(error.code,error.message)};}
}
}
