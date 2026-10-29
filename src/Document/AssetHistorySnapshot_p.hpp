#pragma once

#include "Document/Document.h"

#include <cstddef>
#include <cstdint>
#include <variant>
#include <vector>

namespace iiSharedCanvas::detail {

// Lossless, bounded history payload for the asset kinds used by static artwork
// layers. The input asset remains owned by its caller while this is prepared.
class AssetHistorySnapshot final {
    struct Pixels {
        int width=0,height=0;
        bool runs=false;
        // Raw ARGB32 pixels or alternating run length and ARGB32 value.
        std::vector<std::uint32_t> words;
    };
    struct Bitmap { std::string id; Pixels pixels; };
    struct Chunk { std::int32_t column=0,row=0; Pixels pixels; };
    struct Chunked { std::string id; std::vector<Chunk> chunks; };
    using Value=std::variant<std::monostate,Bitmap,Chunked,VectorAsset>;
    Value value_;
    std::size_t bytes_=0;

public:
    enum class Code { None, Invalid, Unsupported, BudgetExceeded };
    // Budget counts this value, populated arrays, owned strings and geometry;
    // it excludes allocator overhead and unused capacity.
    static Code capture(const Asset &asset,std::size_t budget,AssetHistorySnapshot &output);
    [[nodiscard]] bool matches(const Asset &asset) const noexcept;
    // May allocate the restored asset; the snapshot is unchanged on failure.
    [[nodiscard]] Asset restore() const;
    [[nodiscard]] std::size_t bytes() const noexcept { return bytes_; }
    [[nodiscard]] std::string_view id() const noexcept;
};
} // namespace iiSharedCanvas::detail
