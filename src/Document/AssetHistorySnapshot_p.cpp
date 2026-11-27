#include "Document/AssetHistorySnapshot_p.hpp"
#include "Document/DenseControlAsset_p.hpp"
#include "Serialization/IiscCodec.h"
#include "Vector/VectorEquality_p.hpp"

#include <algorithm>
#include <bit>
#include <limits>

namespace iiSharedCanvas::detail {
namespace {
bool add(std::size_t &total,std::size_t amount,std::size_t limit) noexcept {
    if (total>limit || amount>limit-total) return false;
    total+=amount; return true;
}
bool addArray(std::size_t &total,std::size_t count,std::size_t element,std::size_t limit) noexcept {
    return (!element || count<=std::numeric_limits<std::size_t>::max()/element) &&
        add(total,count*element,limit);
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
    } else if (const auto *video=std::get_if<VideoAsset>(&asset)) {
        if (!add(bytes,video->id.size(),budget) || !addArray(bytes,video->frames.size(),sizeof(Pixels),budget))
            return Code::BudgetExceeded;
        Video value; value.id=video->id; value.frameRate=video->frameRate; value.frames.reserve(video->frames.size());
        for (const auto &frame:video->frames) {
            Pixels copy;
            if (const auto result=capturePixels(frame,copy); result!=Code::None) return result;
            value.frames.push_back(std::move(copy));
        }
        candidate.value_=std::move(value);
    } else if (const auto *linked=std::get_if<LinkedCanvasAsset>(&asset)) {
        if (!add(bytes,linked->id.size(),budget) || !add(bytes,linked->document.size(),budget))
            return Code::BudgetExceeded;
        SerializationLimits limits;
        if (linked->document.size()>limits.maximumEmbeddedCanvasBytes || !limits.maximumEmbeddedCanvasDepth) return Code::Invalid;
        --limits.maximumEmbeddedCanvasDepth;
        const auto child=decodeIisc(linked->document,limits);
        if (!child.ok() || linked->frame>=child.document.timeline.frameCount) return Code::Invalid;
        candidate.value_=*linked;
    } else if (const auto *fill=std::get_if<FillLayerContent>(&asset)) {
        if (!add(bytes,fill->id.size(),budget) || !add(bytes,fill->patternAssetId.size(),budget) ||
            !addArray(bytes,fill->stops.size(),sizeof(FillGradientStop),budget)) return Code::BudgetExceeded;
        candidate.value_=*fill;
    } else if (const auto *adjustment=std::get_if<AdjustmentLayerContent>(&asset)) {
        if (!add(bytes,adjustment->id.size(),budget)) return Code::BudgetExceeded;
        candidate.value_=*adjustment;
    } else if (const auto *vector=std::get_if<VectorAsset>(&asset)) {
        const auto &[id,viewport,paths,shape]=*vector; (void)viewport;
        if (!add(bytes,id.size(),budget) || !addArray(bytes,paths.size(),sizeof(VectorPath),budget))
            return Code::BudgetExceeded;
        if (shape && !addArray(bytes,shape->nodes.size(),sizeof(ShapeExpressionNode),budget)) return Code::BudgetExceeded;
        for (const auto &path:paths) {
            const auto &[commands,fill,stroke]=path; (void)fill; (void)stroke;
            if (!addArray(bytes,commands.size(),sizeof(PathCommand),budget)) return Code::BudgetExceeded;
        }
        candidate.value_=*vector;
    } else {
        bool dense=false,tooLarge=false;
        std::size_t denseBytes=bytes;
        std::visit([&](const auto &value) {
            using T=std::decay_t<decltype(value)>;
            if constexpr (isDenseControlAsset<T>) {
                dense=true;
                const auto &samples=denseControlSamples(value);
                tooLarge=!add(denseBytes,value.id.size(),budget) ||
                    !addArray(denseBytes,samples.size(),sizeof(typename std::decay_t<decltype(samples)>::value_type),budget);
            }
        },asset);
        if (!dense) return Code::Unsupported;
        if (tooLarge) return Code::BudgetExceeded;
        bytes=denseBytes;
        candidate.value_=Dense{asset};
    }
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
    if (const auto *a=std::get_if<Video>(&value_)) {
        const auto *b=std::get_if<VideoAsset>(&asset);
        if (!b || a->id!=b->id || a->frameRate.numerator!=b->frameRate.numerator ||
            a->frameRate.denominator!=b->frameRate.denominator || a->frames.size()!=b->frames.size()) return false;
        for (std::size_t i=0;i<a->frames.size();++i) if (!samePixels(a->frames[i],b->frames[i])) return false;
        return true;
    }
    if (const auto *a=std::get_if<LinkedCanvasAsset>(&value_)) {
        const auto *b=std::get_if<LinkedCanvasAsset>(&asset); return b && *a==*b;
    }
    if (const auto *a=std::get_if<FillLayerContent>(&value_)) {
        const auto *b=std::get_if<FillLayerContent>(&asset); return b && *a==*b;
    }
    if (const auto *a=std::get_if<AdjustmentLayerContent>(&value_)) {
        const auto *b=std::get_if<AdjustmentLayerContent>(&asset); return b && *a==*b;
    }
    if (const auto *a=std::get_if<VectorAsset>(&value_)) {
        const auto *b=std::get_if<VectorAsset>(&asset); return b && vector_detail::same(*a,*b);
    }
    if (const auto *saved=std::get_if<Dense>(&value_)) {
        // Compare floating-point bits, including signed zero, so an edit
        // cannot be silently overwritten by an undo of an older snapshot.
        return std::visit([&](const auto &left) noexcept {
            using T=std::decay_t<decltype(left)>;
            if constexpr (isDenseControlAsset<T>) {
                const auto *right=std::get_if<T>(&asset);
                if (!right || left.id!=right->id || left.viewport!=right->viewport) return false;
                const auto &a=denseControlSamples(left),&b=denseControlSamples(*right);
                if (a.size()!=b.size()) return false;
                return std::equal(a.begin(),a.end(),b.begin(),[](const auto &x,const auto &y) noexcept {
                    using Sample=std::decay_t<decltype(x)>;
                    const auto sameDouble=[](double u,double v) noexcept {
                        return std::bit_cast<std::uint64_t>(u)==std::bit_cast<std::uint64_t>(v);
                    };
                    if constexpr (std::is_same_v<Sample,double>) return sameDouble(x,y);
                    else if constexpr (std::is_same_v<Sample,NormalMapSample>)
                        return sameDouble(x.x,y.x) && sameDouble(x.y,y.y) && sameDouble(x.z,y.z) && x.valid==y.valid;
                    else return x==y;
                });
            } else return false;
        },saved->asset);
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
    if (const auto *video=std::get_if<Video>(&value_)) {
        VideoAsset out; out.id=video->id; out.frameRate=video->frameRate; out.frames.reserve(video->frames.size());
        for (const auto &frame:video->frames) out.frames.push_back(restorePixels(frame));
        return out;
    }
    if (const auto *linked=std::get_if<LinkedCanvasAsset>(&value_)) return *linked;
    if (const auto *fill=std::get_if<FillLayerContent>(&value_)) return *fill;
    if (const auto *adjustment=std::get_if<AdjustmentLayerContent>(&value_)) return *adjustment;
    if (const auto *dense=std::get_if<Dense>(&value_)) return dense->asset;
    return std::get<VectorAsset>(value_);
}

std::string_view AssetHistorySnapshot::id() const noexcept {
    if (const auto *bitmap=std::get_if<Bitmap>(&value_)) return bitmap->id;
    if (const auto *chunks=std::get_if<Chunked>(&value_)) return chunks->id;
    if (const auto *video=std::get_if<Video>(&value_)) return video->id;
    if (const auto *vector=std::get_if<VectorAsset>(&value_)) return vector->id;
    if (const auto *linked=std::get_if<LinkedCanvasAsset>(&value_)) return linked->id;
    if (const auto *fill=std::get_if<FillLayerContent>(&value_)) return fill->id;
    if (const auto *adjustment=std::get_if<AdjustmentLayerContent>(&value_)) return adjustment->id;
    if (const auto *dense=std::get_if<Dense>(&value_)) return assetId(dense->asset);
    return {};
}
} // namespace iiSharedCanvas::detail
