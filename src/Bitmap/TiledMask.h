#pragma once
#include "Bitmap/BitmapProcessing.h"
#include "iiSharedCanvas/Export.h"
#include <cstddef>
#include <cstdint>
#include <expected>
#include <memory>
#include <optional>
#include <span>
#include <string>
#include <vector>

namespace iiSharedCanvas {
// Limits apply to represented tiles and their dense 64x64 coverage arrays;
// allocator overhead and transient output buffers are not included.
struct TiledMaskLimits {
    std::size_t maximumTiles = 65536;
    std::size_t maximumDenseBytes = 32 * 1024 * 1024;
};
struct TiledMaskResult;
struct TiledMaskPatch { CanvasOrigin origin; RasterMask pixels; };
// Immutable, copyable mask in document coordinates. A tile can be implicit,
// uniformly covered, or hold 64x64 alpha samples; copies share immutable data.
class IISHAREDCANVAS_EXPORT TiledMask final {
public:
    struct State;
    TiledMask() = default;
    static TiledMaskResult rectangle(CanvasRegion domain, Point first, Point second,
                                     bool antialias = true, TiledMaskLimits limits = {});
    static TiledMaskResult ellipse(CanvasRegion domain, Point first, Point second,
                                   bool antialias = true, TiledMaskLimits limits = {});
    static TiledMaskResult polygon(CanvasRegion domain, std::span<const Point> points,
                                   bool antialias = true, TiledMaskLimits limits = {});
    // Import a bounded coverage image at document coordinates; other domain pixels use outsideCoverage.
    static TiledMaskResult fromRaster(CanvasRegion domain, CanvasOrigin origin, const RasterMask&,
                                      std::uint8_t outsideCoverage = 0, TiledMaskLimits limits = {});
    [[nodiscard]] TiledMaskResult combine(const TiledMask&, MaskOperation,
                                           TiledMaskLimits limits = {}) const;
    [[nodiscard]] TiledMaskResult inverted(TiledMaskLimits limits = {}) const;
    // Crops a source region and maps its top-left to the destination origin.
    [[nodiscard]] TiledMaskResult reframed(CanvasRegion, CanvasOrigin destination = {}, TiledMaskLimits limits = {}) const;
    // Atomically replaces bounded coverage regions; patches are applied in order.
    [[nodiscard]] TiledMaskResult patched(std::span<const TiledMaskPatch>, TiledMaskLimits limits = {}) const;
    // Applies the same bounded box feather as featherMask; large nonuniform regions may be rejected.
    [[nodiscard]] TiledMaskResult feathered(double radius, TiledMaskLimits limits = {}) const;
    [[nodiscard]] std::uint8_t coverage(std::int32_t x, std::int32_t y) const noexcept;
    // Returns zero outside the domain. Requests are bounded to 64 megapixels.
    [[nodiscard]] std::expected<RasterMask, std::string> materialize(CanvasRegion region) const;
    [[nodiscard]] CanvasRegion domain() const noexcept;
    // Conservative document-space bounds of nonzero coverage, or no bounds for an empty mask.
    // Nonzero implicit background returns the entire domain.
    [[nodiscard]] std::optional<CanvasRegion> nonzeroBounds() const noexcept;
    // Conservative, non-overlapping 64px regions covering all nonzero samples.
    // The result is in document coordinates, clipped to the domain and limited
    // by maxRegions; implicit nonzero background may require the whole domain.
    [[nodiscard]] std::expected<std::vector<CanvasRegion>, std::string> nonzeroRegions(std::size_t maxRegions) const;
    [[nodiscard]] std::size_t tile_count() const noexcept;
    [[nodiscard]] std::size_t dense_bytes() const noexcept;
    // Compares coverage, not the internal background/tile representation.
    [[nodiscard]] bool equivalent(const TiledMask&) const noexcept;
    // Conservative owned payload estimate; excludes allocator overhead.
    [[nodiscard]] std::size_t storage_bytes() const noexcept;
    explicit operator bool() const noexcept { return bool(state_); }
private:
    explicit TiledMask(std::shared_ptr<const State> state) : state_(std::move(state)) {}
    std::shared_ptr<const State> state_;
};
struct TiledMaskResult {
    TiledMask mask;
    std::string error;
    [[nodiscard]] bool ok() const noexcept { return error.empty() && bool(mask); }
    explicit operator bool() const noexcept { return ok(); }
    TiledMask* operator->() noexcept { return &mask; }
    const TiledMask* operator->() const noexcept { return &mask; }
    TiledMask& operator*() noexcept { return mask; }
    const TiledMask& operator*() const noexcept { return mask; }
};
} // namespace iiSharedCanvas
