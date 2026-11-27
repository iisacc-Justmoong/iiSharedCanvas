#pragma once

#include "Document/Document.h"
#include <type_traits>

namespace iiSharedCanvas::detail {

// These assets own a rectangular array of native control samples. Their
// element types remain distinct; no intermediate color conversion is used.
template<class T>
inline constexpr bool isDenseControlAsset =
    std::is_same_v<T,DepthAsset> || std::is_same_v<T,LineArtAsset> ||
    std::is_same_v<T,CannyAsset> || std::is_same_v<T,ScribbleAsset> ||
    std::is_same_v<T,NormalMapAsset> || std::is_same_v<T,ShuffleAsset> ||
    std::is_same_v<T,TileAsset> || std::is_same_v<T,ReferenceAsset>;

// Returns the array owned by the supplied dense asset without extending its
// lifetime. The caller controls synchronization and write access.
template<class T>
decltype(auto) denseControlSamples(T &value) noexcept {
    static_assert(isDenseControlAsset<std::remove_cv_t<T>>);
    if constexpr (requires { value.values; }) return (value.values);
    else if constexpr (requires { value.coverage; }) return (value.coverage);
    else if constexpr (requires { value.mask; }) return (value.mask);
    else if constexpr (requires { value.samples; }) return (value.samples);
    else return (value.colors);
}
} // namespace iiSharedCanvas::detail
