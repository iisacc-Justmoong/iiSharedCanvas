#include "Media_p.hpp"
#include <iiSDKCore/Core/Text/Number.h>
#include <algorithm>
#include <cmath>
#include <limits>
#include <stdexcept>
#include <type_traits>

namespace iiSharedCanvas::media_detail {
PdfBounds vectorBounds(const VectorAsset& asset){
    double left=0,top=0,right=asset.viewport.width,bottom=asset.viewport.height;
    for(const auto& path:asset.paths){
        const auto half=path.stroke?path.stroke->width/2:0;
        const auto include=[&](Point p){left=std::min(left,p.x-half);top=std::min(top,p.y-half);right=std::max(right,p.x+half);bottom=std::max(bottom,p.y+half);};
        for(const auto& command:path.commands)std::visit([&](const auto& c){using T=std::decay_t<decltype(c)>;
            if constexpr(std::same_as<T,MoveTo>||std::same_as<T,LineTo>)include(c.point);
            else if constexpr(std::same_as<T,QuadraticTo>){include(c.control);include(c.end);}
            else if constexpr(std::same_as<T,CubicTo>){include(c.control1);include(c.control2);include(c.end);}
        },command);
    }
    left=std::floor(left);top=std::floor(top);right=std::ceil(right);bottom=std::ceil(bottom);
    if(!std::isfinite(left)||!std::isfinite(top)||!std::isfinite(right)||!std::isfinite(bottom)||right<=left||bottom<=top||right-left>14400||bottom-top>14400)throw std::invalid_argument("Vector PDF bounds must be finite and at most 14400 points per dimension");
    return {left,top,right-left,bottom-top};
}
std::vector<std::uint8_t> vectorPdf(const VectorAsset& asset,PdfBounds bounds,std::uint64_t limit){
    const auto number=[](double value){if(!std::isfinite(value))throw std::invalid_argument("Nonfinite PDF coordinate");auto text=ii::core::format_decimal(value);const auto exponent=text.find_first_of("eE");if(exponent==text.npos)return text;
        const auto power=std::stoi(text.substr(exponent+1));text.resize(exponent);const bool minus=text.starts_with('-');if(minus)text.erase(0,1);auto dot=text.find('.');if(dot==text.npos)dot=text.size();else text.erase(dot,1);const auto position=std::int64_t(dot)+power;
        if(position<=0)text="0."+std::string(std::size_t(-position),'0')+text;else if(std::size_t(position)>=text.size())text+=std::string(std::size_t(position)-text.size(),'0');else text.insert(std::size_t(position),1,'.');return (minus?"-":"")+text;};
    std::string stream;
    const auto append=[&](std::string_view value){if(stream.size()>limit||value.size()>limit-stream.size())throw std::length_error("Vector PDF exceeds its byte budget");stream+=value;};
    append("q\n1 0 0 -1 "+number(-bounds.x)+" "+number(bounds.h+bounds.y)+" cm\n1 J 1 j\n");
    std::vector<std::string> graphics;
    std::size_t index=0;
    for(const auto& path:asset.paths){
        if(!path.fill&&!path.stroke)continue;
        const auto alpha=[](std::uint32_t c){return double((c>>24)&255)/255;};
        const auto rgb=[&](std::uint32_t c){return number(double((c>>16)&255)/255)+" "+number(double((c>>8)&255)/255)+" "+number(double(c&255)/255);};
        graphics.push_back("<< /Type /ExtGState /ca "+number(path.fill?alpha(path.fill->argb):1)+" /CA "+number(path.stroke?alpha(path.stroke->paint.argb):1)+" >>");
        append("/G"+std::to_string(index++)+" gs\n");
        if(path.fill)append(rgb(path.fill->argb)+" rg\n");
        if(path.stroke)append(rgb(path.stroke->paint.argb)+" RG "+number(path.stroke->width)+" w\n");
        Point current{},start{};
        const auto point=[&](Point p){return number(p.x)+" "+number(p.y);};
        for(const auto& command:path.commands)std::visit([&](const auto& c){using T=std::decay_t<decltype(c)>;
            if constexpr(std::same_as<T,MoveTo>){append(point(c.point)+" m\n");current=start=c.point;}
            else if constexpr(std::same_as<T,LineTo>){append(point(c.point)+" l\n");current=c.point;}
            else if constexpr(std::same_as<T,QuadraticTo>){Point a{current.x+(c.control.x-current.x)*2/3,current.y+(c.control.y-current.y)*2/3},b{c.end.x+(c.control.x-c.end.x)*2/3,c.end.y+(c.control.y-c.end.y)*2/3};append(point(a)+" "+point(b)+" "+point(c.end)+" c\n");current=c.end;}
            else if constexpr(std::same_as<T,CubicTo>){append(point(c.control1)+" "+point(c.control2)+" "+point(c.end)+" c\n");current=c.end;}
            else{append("h\n");current=start;}
        },command);
        append(path.fill?(path.stroke?"B*\n":"f*\n"):"S\n");
    }
    append("Q\n");
    std::string resources="<< /ExtGState << ";for(std::size_t i=0;i<graphics.size();++i)resources+="/G"+std::to_string(i)+" "+std::to_string(5+i)+" 0 R ";resources+=">> >>";
    std::vector<std::string> objects{
        "<< /Type /Catalog /Pages 2 0 R >>",
        "<< /Type /Pages /Count 1 /Kids [3 0 R] >>",
        "<< /Type /Page /Parent 2 0 R /MediaBox [0 0 "+number(bounds.w)+" "+number(bounds.h)+"] /Resources "+resources+" /Contents 4 0 R >>",
        "<< /Length "+std::to_string(stream.size())+" >>\nstream\n"+stream+"endstream"};
    objects.insert(objects.end(),graphics.begin(),graphics.end());
    std::string output="%PDF-1.4\n%\xe2\xe3\xcf\xd3\n";std::vector<std::size_t> offsets{0};
    for(std::size_t i=0;i<objects.size();++i){offsets.push_back(output.size());output+=std::to_string(i+1)+" 0 obj\n"+objects[i]+"\nendobj\n";if(output.size()>limit)throw std::length_error("Vector PDF exceeds its byte budget");}
    const auto xref=output.size();output+="xref\n0 "+std::to_string(offsets.size())+"\n0000000000 65535 f \n";
    for(std::size_t i=1;i<offsets.size();++i){auto value=std::to_string(offsets[i]);if(value.size()>10)throw std::length_error("PDF offset exceeds its field");output+=std::string(10-value.size(),'0')+value+" 00000 n \n";}
    output+="trailer\n<< /Size "+std::to_string(offsets.size())+" /Root 1 0 R >>\nstartxref\n"+std::to_string(xref)+"\n%%EOF\n";
    if(output.size()>limit)throw std::length_error("Vector PDF exceeds its byte budget");
    return {output.begin(),output.end()};
}
}
