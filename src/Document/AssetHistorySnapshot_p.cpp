#include "Document/AssetHistorySnapshot_p.hpp"

#include <algorithm>
#include <bit>
#include <limits>
#include <type_traits>

namespace iiSharedCanvas::detail {
namespace {
template <class> inline constexpr bool unsupportedPathCommand=false;
bool add(std::size_t &total,std::size_t amount,std::size_t limit) noexcept {
    if (total>limit || amount>limit-total) return false;
    total+=amount; return true;
}
bool addArray(std::size_t &total,std::size_t count,std::size_t element,std::size_t limit) noexcept {
    return (!element || count<=std::numeric_limits<std::size_t>::max()/element) &&
        add(total,count*element,limit);
}
bool sameDouble(double a,double b) noexcept {
    static_assert(sizeof(double)==sizeof(std::uint64_t));
    return std::bit_cast<std::uint64_t>(a)==std::bit_cast<std::uint64_t>(b);
}
bool point(const Point &a,const Point &b) noexcept {
    const auto &[ax,ay]=a; const auto &[bx,by]=b;
    return sameDouble(ax,bx) && sameDouble(ay,by);
}
bool command(const PathCommand &a,const PathCommand &b) noexcept {
    if (a.index()!=b.index() || a.valueless_by_exception() || b.valueless_by_exception()) return false;
    return std::visit([&](const auto &x) {
        using T=std::decay_t<decltype(x)>; const auto &y=std::get<T>(b);
        if constexpr (std::is_same_v<T,MoveTo> || std::is_same_v<T,LineTo>) {
            const auto &[xp]=x; const auto &[yp]=y; return point(xp,yp);
        } else if constexpr (std::is_same_v<T,QuadraticTo>) {
            const auto &[xc,xe]=x; const auto &[yc,ye]=y;
            return point(xc,yc) && point(xe,ye);
        } else if constexpr (std::is_same_v<T,CubicTo>) {
            const auto &[xc1,xc2,xe]=x; const auto &[yc1,yc2,ye]=y;
            return point(xc1,yc1) && point(xc2,yc2) && point(xe,ye);
        } else if constexpr (std::is_same_v<T,ClosePath>) {
            static_assert(std::is_empty_v<T>,"Compare new ClosePath data before storing it in history");
            return true;
        }
        else {
            static_assert(unsupportedPathCommand<T>,"Compare new path commands before storing them in history");
            return false;
        }
    },a);
}
bool sameVector(const VectorAsset &a,const VectorAsset &b) noexcept {
    const auto &[aid,aview,apaths]=a; const auto &[bid,bview,bpaths]=b;
    const auto &[aw,ah]=aview; const auto &[bw,bh]=bview;
    if (aid!=bid || aw!=bw || ah!=bh || apaths.size()!=bpaths.size()) return false;
    for (std::size_t i=0;i<apaths.size();++i) {
        const auto &[xc,xf,xs]=apaths[i]; const auto &[yc,yf,ys]=bpaths[i];
        if (bool(xf)!=bool(yf) || bool(xs)!=bool(ys) || xc.size()!=yc.size()) return false;
        if (xf) {
            const auto &[xcolor]=*xf; const auto &[ycolor]=*yf;
            if (xcolor!=ycolor) return false;
        }
        if (xs) {
            const auto &[xp,xw]=*xs; const auto &[yp,yw]=*ys;
            const auto &[xcolor]=xp; const auto &[ycolor]=yp;
            if (xcolor!=ycolor || !sameDouble(xw,yw)) return false;
        }
        for (std::size_t j=0;j<xc.size();++j)
            if (!command(xc[j],yc[j])) return false;
    }
    return true;
}
}

AssetHistorySnapshot::Code AssetHistorySnapshot::capture(
    const Asset &asset,std::size_t budget,AssetHistorySnapshot &output) {
    AssetHistorySnapshot candidate; std::size_t bytes=sizeof(candidate);
    if (bytes>budget) return Code::BudgetExceeded;
    const auto capturePixels=[&](const RasterLayer &input,Pixels &out)->Code {
        const auto &[width,height,pixels]=input;
        if (width<=0 || height<=0 || std::uint64_t(width)*std::uint64_t(height)!=pixels.size()) return Code::Invalid;
        const auto n=pixels.size();
        if (n>std::numeric_limits<std::size_t>::max()/sizeof(std::uint32_t)) return Code::BudgetExceeded;
        const auto raw=n*sizeof(std::uint32_t); const auto remaining=budget-bytes;
        std::size_t runs=0; std::uint32_t last=0,count=0;
        bool rawPreferred=false;
        for (auto pixel:pixels) {
            if (!count || pixel!=last || count==std::numeric_limits<std::uint32_t>::max()) {
                ++runs; last=pixel; count=1;
                if (runs>remaining/(2*sizeof(std::uint32_t)) && raw>remaining) return Code::BudgetExceeded;
                if (runs>=n/2+n%2 && raw<=remaining) { rawPreferred=true; break; }
            } else ++count;
        }
        const bool useRuns=!rawPreferred && runs<n/2+n%2;
        const auto words=useRuns ? runs*2 : n;
        if (!addArray(bytes,words,sizeof(std::uint32_t),budget)) return Code::BudgetExceeded;
        out.width=width; out.height=height; out.runs=useRuns;
        if (!useRuns) { out.words=pixels; return Code::None; }
        out.words.reserve(words); last=0; count=0;
        for (auto pixel:pixels) {
            if (count && (pixel!=last || count==std::numeric_limits<std::uint32_t>::max())) {
                out.words.push_back(count); out.words.push_back(last); count=0;
            }
            last=pixel; ++count;
        }
        if (count) { out.words.push_back(count); out.words.push_back(last); }
        return Code::None;
    };
    if (const auto *bitmap=std::get_if<RasterAsset>(&asset)) {
        const auto &[id,pixels]=*bitmap;
        if (!add(bytes,id.size(),budget)) return Code::BudgetExceeded;
        Bitmap value; value.id=id;
        if (const auto result=capturePixels(pixels,value.pixels); result!=Code::None) return result;
        candidate.value_=std::move(value);
    } else if (const auto *chunks=std::get_if<ChunkedRasterAsset>(&asset)) {
        const auto &[id,inputChunks]=*chunks;
        if (!add(bytes,id.size(),budget) || !addArray(bytes,inputChunks.size(),sizeof(Chunk),budget))
            return Code::BudgetExceeded;
        Chunked value; value.id=id; value.chunks.reserve(inputChunks.size());
        for (const auto &chunk:inputChunks) {
            const auto &[column,row,pixels]=chunk;
            Chunk copy; copy.column=column; copy.row=row;
            if (const auto result=capturePixels(pixels,copy.pixels); result!=Code::None) return result;
            value.chunks.push_back(std::move(copy));
        }
        candidate.value_=std::move(value);
    } else if (const auto *vector=std::get_if<VectorAsset>(&asset)) {
        const auto &[id,viewport,paths]=*vector; (void)viewport;
        if (!add(bytes,id.size(),budget) || !addArray(bytes,paths.size(),sizeof(VectorPath),budget))
            return Code::BudgetExceeded;
        for (const auto &path:paths) {
            const auto &[commands,fill,stroke]=path; (void)fill; (void)stroke;
            if (!addArray(bytes,commands.size(),sizeof(PathCommand),budget)) return Code::BudgetExceeded;
        }
        candidate.value_=*vector;
    } else return Code::Unsupported;
    candidate.bytes_=bytes;
    output=std::move(candidate);
    return Code::None;
}

bool AssetHistorySnapshot::matches(const Asset &asset) const noexcept {
    const auto samePixels=[](const Pixels &a,const RasterLayer &b) noexcept {
        if (a.width!=b.width || a.height!=b.height || a.width<=0 || a.height<=0 ||
            std::uint64_t(a.width)*std::uint64_t(a.height)!=b.pixels.size()) return false;
        if (!a.runs) return a.words==b.pixels;
        std::size_t at=0;
        for (std::size_t i=0;i+1<a.words.size();i+=2) {
            const auto count=a.words[i], value=a.words[i+1];
            if (!count || count>b.pixels.size()-at) return false;
            if (!std::all_of(b.pixels.begin()+static_cast<std::ptrdiff_t>(at),
                             b.pixels.begin()+static_cast<std::ptrdiff_t>(at+count),
                             [value](auto pixel) { return pixel==value; })) return false;
            at+=count;
        }
        return !(a.words.size()%2) && at==b.pixels.size();
    };
    if (const auto *a=std::get_if<Bitmap>(&value_)) {
        const auto *b=std::get_if<RasterAsset>(&asset);
        return b && a->id==b->id && samePixels(a->pixels,b->pixels);
    }
    if (const auto *a=std::get_if<Chunked>(&value_)) {
        const auto *b=std::get_if<ChunkedRasterAsset>(&asset);
        if (!b || a->id!=b->id || a->chunks.size()!=b->chunks.size()) return false;
        for (std::size_t i=0;i<a->chunks.size();++i)
            if (a->chunks[i].column!=b->chunks[i].column || a->chunks[i].row!=b->chunks[i].row ||
                !samePixels(a->chunks[i].pixels,b->chunks[i].pixels)) return false;
        return true;
    }
    if (const auto *a=std::get_if<VectorAsset>(&value_)) {
        const auto *b=std::get_if<VectorAsset>(&asset); return b && sameVector(*a,*b);
    }
    return false;
}

Asset AssetHistorySnapshot::restore() const {
    const auto restorePixels=[](const Pixels &saved) {
        RasterLayer out; out.width=saved.width; out.height=saved.height;
        if (!saved.runs) { out.pixels=saved.words; return out; }
        out.pixels.reserve(std::size_t(saved.width)*std::size_t(saved.height));
        for (std::size_t i=0;i+1<saved.words.size();i+=2)
            out.pixels.insert(out.pixels.end(),saved.words[i],saved.words[i+1]);
        return out;
    };
    if (const auto *bitmap=std::get_if<Bitmap>(&value_)) return RasterAsset{bitmap->id,restorePixels(bitmap->pixels)};
    if (const auto *chunks=std::get_if<Chunked>(&value_)) {
        ChunkedRasterAsset out; out.id=chunks->id; out.chunks.reserve(chunks->chunks.size());
        for (const auto &chunk:chunks->chunks) out.chunks.push_back({chunk.column,chunk.row,restorePixels(chunk.pixels)});
        return out;
    }
    return std::get<VectorAsset>(value_);
}

std::string_view AssetHistorySnapshot::id() const noexcept {
    if (const auto *bitmap=std::get_if<Bitmap>(&value_)) return bitmap->id;
    if (const auto *chunks=std::get_if<Chunked>(&value_)) return chunks->id;
    if (const auto *vector=std::get_if<VectorAsset>(&value_)) return vector->id;
    return {};
}
} // namespace iiSharedCanvas::detail
