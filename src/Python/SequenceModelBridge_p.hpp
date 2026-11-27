#pragma once

#include "SequenceModelTypes_p.hpp"
#include "Timeline/TimelineProjectCodec_p.hpp"
#include <span>
#include <string_view>
#include <tuple>
#include <type_traits>

// This private bridge exposes the same frozen field order as the document
// codec. It borrows the canvas for one synchronous call and returns detached
// buffers; callers serialize and publish the returned values externally.
namespace {
template<class T> struct ModelVector : std::false_type {};
template<class T, class A> struct ModelVector<std::vector<T,A>> : std::true_type { using Value=T; };
template<class T> struct ModelOptional : std::false_type {};
template<class T> struct ModelOptional<std::optional<T>> : std::true_type {};
template<class T> struct ModelVariant : std::false_type {};
template<class... T> struct ModelVariant<std::variant<T...>> : std::true_type {};
template<class T> concept ModelObject = requires(T &value) { timeline_codec::Fields<T>::tie(value); };

// Field names are derived directly from the frozen codec descriptor, so the
// Python projection cannot silently acquire a different wire field order.
std::vector<std::string_view> modelFieldNames(std::string_view source) {
    std::vector<std::string_view> names;
    while (!source.empty()) {
        const auto separator=source.find(',');
        auto field=source.substr(0,separator);
        while (!field.empty() && field.front()==' ') field.remove_prefix(1);
        while (!field.empty() && field.back()==' ') field.remove_suffix(1);
        if (!field.starts_with("x.")) throw std::logic_error("invalid timeline field descriptor");
        field.remove_prefix(2); names.push_back(field);
        if (separator==std::string_view::npos) break;
        source.remove_prefix(separator+1);
    }
    return names;
}

// The tree tags disambiguate optional, variant, and byte values from ordinary
// lists; floating-point output round-trips each stored binary double exactly.
template<class T> void modelJson(std::string &out,const T &value) {
    constexpr std::size_t maximumProjectionBytes=256ULL*1024*1024;
    if (out.size()>maximumProjectionBytes) throw std::length_error("timeline projection exceeds its budget");
    if constexpr (ModelObject<T>) {
        const auto names=modelFieldNames(timeline_codec::Fields<T>::fieldNames);
        const auto fields=timeline_codec::Fields<T>::tie(value);
        if (names.size()!=std::tuple_size_v<decltype(fields)>) throw std::logic_error("timeline field count mismatch");
        out+="{\"$type\":";jsonString(out,timeline_codec::Fields<T>::typeName);std::size_t index=0;
        std::apply([&](const auto &...field) { ((out+=',',jsonString(out,names[index++]),out+=':',modelJson(out,field)),...); },fields);
        out+='}';
    } else if constexpr (std::is_same_v<T,std::string>) {
        jsonString(out,value);
    } else if constexpr (std::is_same_v<T,std::monostate>) {
        out+="null";
    } else if constexpr (std::is_same_v<T,bool>) {
        out+=value?"true":"false";
    } else if constexpr (std::is_enum_v<T>) {
        out+=std::to_string(static_cast<std::underlying_type_t<T>>(value));
    } else if constexpr (std::is_integral_v<T>) {
        out+=std::to_string(value);
    } else if constexpr (std::is_floating_point_v<T>) {
        sequenceJsonDouble(out,value);
    } else if constexpr (ModelOptional<T>::value) {
        out+="{\"$optional\":";out+=value?"true":"false";
        if(value){out+=",\"value\":";modelJson(out,*value);}out+='}';
    } else if constexpr (ModelVariant<T>::value) {
        if(value.valueless_by_exception()) throw std::logic_error("timeline variant has no value");
        out+="{\"$variant\":"+std::to_string(value.index())+",\"value\":";
        std::visit([&](const auto &item){modelJson(out,item);},value);out+='}';
    } else if constexpr (ModelVector<T>::value) {
        if constexpr (std::is_same_v<typename ModelVector<T>::Value,std::uint8_t>) {
            constexpr char hex[]="0123456789abcdef";
            if(value.size()>(maximumProjectionBytes-out.size())/2) throw std::length_error("timeline byte projection exceeds its budget");
            out+="{\"$bytes\":\"";for(auto byte:value){out+=hex[byte>>4];out+=hex[byte&15];}out+="\"}";
        } else {
            out+='[';for(std::size_t i=0;i<value.size();++i){if(i)out+=',';modelJson(out,value[i]);}out+=']';
        }
    } else static_assert(!sizeof(T),"unsupported timeline model field");
    if (out.size()>maximumProjectionBytes) throw std::length_error("timeline projection exceeds its budget");
}

template<class... T> void modelSchema(std::string &out,std::tuple<T...> *) {
    out+='{';bool first=true;
    const auto add=[&]<class V>() {
        if(!first)out+=',';first=false;jsonString(out,timeline_codec::Fields<V>::typeName);out+=": [";
        const auto names=modelFieldNames(timeline_codec::Fields<V>::fieldNames);
        for(std::size_t i=0;i<names.size();++i){if(i)out+=',';jsonString(out,names[i]);}out+=']';
    };
    (add.template operator()<T>(),...);out+='}';
}
template<class... T> bool modelDefault(std::string &out,std::string_view name,std::tuple<T...> *) {
    return ((name==timeline_codec::Fields<T>::typeName ? (modelJson(out,T{}),true) : false) || ...);
}
} // namespace

extern "C" {
int iisc_canvas_sequence_model(IiscCanvasControl *canvas,uint8_t *out,size_t capacity,size_t *required) {
    return invoke(canvas,[&](auto &owner){std::string value;modelJson(value,sequenceProject(owner));jsonOutput(value,out,capacity,required);});
}
int iisc_canvas_sequence_model_schema(IiscCanvasControl *canvas,uint8_t *out,size_t capacity,size_t *required) {
    return invoke(canvas,[&](auto &){std::string value;modelSchema(value,static_cast<timeline_codec::SequenceModelTypes *>(nullptr));jsonOutput(value,out,capacity,required);});
}
int iisc_canvas_sequence_model_default(IiscCanvasControl *canvas,const char *type,uint8_t *out,size_t capacity,size_t *required) {
    return invoke(canvas,[&](auto &){std::string value;if(!modelDefault(value,requiredString(type),static_cast<timeline_codec::SequenceModelTypes *>(nullptr)))
        throw std::invalid_argument("unknown timeline model type");jsonOutput(value,out,capacity,required);});
}
int iisc_canvas_sequence_media_list(IiscCanvasControl *canvas,uint8_t *out,size_t capacity,size_t *required) {
    return invoke(canvas,[&](auto &owner){std::string value="[";for(size_t i=0;i<owner.document.sequenceMedia.size();++i){
        const auto &media=owner.document.sequenceMedia[i];if(i)value+=',';value+="{\"id\":";jsonString(value,media.id);
        value+=",\"media_type\":";jsonString(value,media.mediaType);value+='}';}value+=']';jsonOutput(value,out,capacity,required);});
}
int iisc_canvas_sequence_media_data(IiscCanvasControl *canvas,const char *id,uint8_t *out,size_t capacity,size_t *required) {
    return invoke(canvas,[&](auto &owner){if(!required)throw std::invalid_argument("missing output size");
        const std::string_view name=requiredString(id);
        const auto found=std::find_if(owner.document.sequenceMedia.begin(),owner.document.sequenceMedia.end(),
            [&](const auto &media){return media.id==name;});
        if(found==owner.document.sequenceMedia.end() || !found->bytes)throw std::invalid_argument("unknown media identity");
        *required=found->bytes->size();if(out){if(capacity<*required)throw std::invalid_argument("output buffer too small");
            std::copy(found->bytes->begin(),found->bytes->end(),out);}});
}
int iisc_canvas_sequence_model_replace(IiscCanvasControl *canvas,const uint8_t *data,size_t size,
                                       const IiscSequenceOwnedMedia *input,size_t count) {
    return invoke(canvas,[&](auto &owner){
        const SerializationLimits limits;
        if((!data && size)||(!input && count)||size>limits.maximumTimelineProjectBytes||count>limits.maximumTimelineMediaAssets)
            throw std::invalid_argument("invalid or excessive sequence model input");
        std::optional<TimelineProject> project;
        if(size){auto decoded=detail::decodeTimelineProject(std::span<const std::uint8_t>(data,size),limits);
            if(decoded.error.code!=IiscErrorCode::None)throw std::invalid_argument(decoded.error.message);project=std::move(decoded.project);}
        std::vector<TimelineOwnedMedia> media;media.reserve(count);std::uint64_t total=0;
        for(size_t i=0;i<count;++i){const auto &source=input[i];
            if((!source.data && source.size)||source.size>limits.maximumTimelineMediaBytes-total)
                throw std::invalid_argument("invalid or excessive sequence media");
            total+=source.size;auto bytes=std::make_shared<std::vector<std::uint8_t>>();
            if(source.size)bytes->assign(source.data,source.data+source.size);
            media.push_back({requiredString(source.id),requiredString(source.media_type),std::move(bytes)});
        }
        accepted(owner.history.setSequenceProject(std::move(project),std::move(media),"Replace sequence project"));
    });
}
} // extern "C"
