#pragma once

#include "Document/Document.h"

#include <algorithm>
#include <array>
#include <cmath>
#include <cstdint>
#include <limits>
#include <optional>

namespace iiSharedCanvas::canvas_affine_detail {
inline AffineTransform compose(const AffineTransform &a,const AffineTransform &b) noexcept {
    return {a.m11*b.m11+a.m21*b.m12,a.m12*b.m11+a.m22*b.m12,
            a.m11*b.m21+a.m21*b.m22,a.m12*b.m21+a.m22*b.m22,
            a.m11*b.translationX+a.m21*b.translationY+a.translationX,
            a.m12*b.translationX+a.m22*b.translationY+a.translationY};
}
inline bool invertible(const AffineTransform &t) noexcept {
    const auto d=t.m11*t.m22-t.m12*t.m21;
    return std::isfinite(t.m11) && std::isfinite(t.m12) && std::isfinite(t.m21) &&
        std::isfinite(t.m22) && std::isfinite(t.translationX) && std::isfinite(t.translationY) &&
        std::isfinite(d) && std::abs(d)>1e-12;
}
inline std::optional<CanvasRegion> bounds(CanvasRegion region,const AffineTransform &t) noexcept {
    if (region.extent.width<=0 || region.extent.height<=0 || !invertible(t)) return {};
    double left=std::numeric_limits<double>::infinity(),top=left,right=-left,bottom=-left;
    for (const double x : {double(region.origin.x),double(region.origin.x)+region.extent.width})
        for (const double y : {double(region.origin.y),double(region.origin.y)+region.extent.height}) {
            const double px=t.m11*x+t.m21*y+t.translationX,py=t.m12*x+t.m22*y+t.translationY;
            left=std::min(left,px); top=std::min(top,py); right=std::max(right,px); bottom=std::max(bottom,py);
        }
    if (!std::isfinite(left) || !std::isfinite(top) || !std::isfinite(right) || !std::isfinite(bottom)) return {};
    left=std::floor(left); top=std::floor(top); right=std::ceil(right); bottom=std::ceil(bottom);
    constexpr double lo=std::numeric_limits<std::int32_t>::min(),hi=std::numeric_limits<std::int32_t>::max();
    if (left<lo || top<lo || right>hi || bottom>hi || right-left>hi || bottom-top>hi || right<=left || bottom<=top) return {};
    return CanvasRegion{{std::int32_t(left),std::int32_t(top)},{std::int32_t(right-left),std::int32_t(bottom-top)}};
}
} // namespace iiSharedCanvas::canvas_affine_detail
