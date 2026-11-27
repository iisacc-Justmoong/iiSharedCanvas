#pragma once

#include "Document/Document.h"

#include <bit>
#include <cstdint>
#include <type_traits>

namespace iiSharedCanvas::vector_detail {
// Compare the persisted binary values rather than approximate geometry. This
// shared predicate prevents history and record publication from disagreeing
// about whether a vector asset changed. It borrows immutable input only.
inline bool scalar(double a,double b) noexcept {
    static_assert(sizeof(double)==sizeof(std::uint64_t));
    return std::bit_cast<std::uint64_t>(a)==std::bit_cast<std::uint64_t>(b);
}
inline bool point(const Point &a,const Point &b) noexcept {
    return scalar(a.x,b.x) && scalar(a.y,b.y);
}
template <class> inline constexpr bool unsupportedPathCommand=false;
inline bool command(const PathCommand &a,const PathCommand &b) noexcept {
    if (a.index()!=b.index() || a.valueless_by_exception() || b.valueless_by_exception()) return false;
    return std::visit([&](const auto &x) {
        using T=std::decay_t<decltype(x)>; const auto &y=std::get<T>(b);
        if constexpr (std::is_same_v<T,MoveTo> || std::is_same_v<T,LineTo>) return point(x.point,y.point);
        else if constexpr (std::is_same_v<T,QuadraticTo>) return point(x.control,y.control) && point(x.end,y.end);
        else if constexpr (std::is_same_v<T,CubicTo>)
            return point(x.control1,y.control1) && point(x.control2,y.control2) && point(x.end,y.end);
        else if constexpr (std::is_same_v<T,ClosePath>) {
            static_assert(std::is_empty_v<T>,"Compare new ClosePath data before storing it in history");
            return true;
        } else {
            static_assert(unsupportedPathCommand<T>,"Compare new path commands before storing them in history");
            return false;
        }
    },a);
}
inline bool same(const VectorAsset &a,const VectorAsset &b) noexcept {
    if (a.id!=b.id || a.viewport!=b.viewport || a.paths.size()!=b.paths.size() || bool(a.shape)!=bool(b.shape)) return false;
    if (a.shape) {
        if (a.shape->root!=b.shape->root || a.shape->color!=b.shape->color || a.shape->nodes.size()!=b.shape->nodes.size()) return false;
        for (std::size_t i=0;i<a.shape->nodes.size();++i) {
            const auto &x=a.shape->nodes[i], &y=b.shape->nodes[i];
            if (x.kind!=y.kind || x.first!=y.first || x.second!=y.second ||
                !scalar(x.transform.m11,y.transform.m11) || !scalar(x.transform.m12,y.transform.m12) ||
                !scalar(x.transform.m21,y.transform.m21) || !scalar(x.transform.m22,y.transform.m22) ||
                !scalar(x.transform.translationX,y.transform.translationX) ||
                !scalar(x.transform.translationY,y.transform.translationY)) return false;
        }
    }
    for (std::size_t i=0;i<a.paths.size();++i) {
        const auto &x=a.paths[i], &y=b.paths[i];
        if (bool(x.fill)!=bool(y.fill) || bool(x.stroke)!=bool(y.stroke) || x.commands.size()!=y.commands.size()) return false;
        if (x.fill && x.fill->argb!=y.fill->argb) return false;
        if (x.stroke && (x.stroke->paint.argb!=y.stroke->paint.argb || !scalar(x.stroke->width,y.stroke->width))) return false;
        for (std::size_t j=0;j<x.commands.size();++j) if (!command(x.commands[j],y.commands[j])) return false;
    }
    return true;
}
} // namespace iiSharedCanvas::vector_detail
