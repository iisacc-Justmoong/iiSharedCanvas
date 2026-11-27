#pragma once

#include "iiSharedCanvas/Export.h"
#include <Layer/RasterLayer.h>
#include <cstddef>
#include <cstdint>
#include <string>
#include <vector>

namespace iiSharedCanvas {

// Motion from a pixel centre in one input frame to the corresponding centre
// in the other. Coordinates are in pixels, x rightwards and y downwards.
struct OpticalFlowVector { double x = 0.0, y = 0.0; };

// Dense, row-major vectors owned by this value. The forward field is indexed
// in the first input frame and the backward field in the second input frame.
struct OpticalFlowField {
    std::int32_t width = 0, height = 0;
    std::vector<OpticalFlowVector> vectors;
};
struct OpticalFlowPair { OpticalFlowField forward, backward; };

// The bounded block matcher searches integer displacements and compares
// premultiplied RGBA patches. The budgets are controlled by the caller.
struct OpticalFlowOptions {
    std::uint32_t searchRadius = 4, patchRadius = 2;
    std::uint64_t maximumComparisons = 100000000;
    std::size_t maximumPixels = 16777216;
};
struct OpticalFlowEstimateResult {
    OpticalFlowPair flow;
    std::string error;
    [[nodiscard]] bool ok() const noexcept { return error.empty(); }
};
struct OpticalFlowRasterResult {
    RasterLayer pixels;
    std::string error;
    [[nodiscard]] bool ok() const noexcept { return error.empty(); }
};

// A stateless, deterministic reference motion estimator. The caller supplies
// both decoded frames, selects the work budget, owns the returned fields, and
// may replace this estimator with another engine. No external resources or
// shared state are accessed. Invalid dimensions or exhausted budgets fail
// without returning partial fields. Concurrent calls do not share state.
class IISHAREDCANVAS_EXPORT OpticalFlowEstimator final {
public:
    [[nodiscard]] OpticalFlowEstimateResult operator()(const RasterLayer &first, const RasterLayer &second,
                                                        const OpticalFlowOptions &options = {}) const;
};

// A stateless motion-compensated rasterizer. All inputs, including the two
// motion fields, are borrowed only for the call; the result owns its pixels.
// phase must be finite and in [0, 1]. Alpha-aware bilinear interpolation is
// numeric rendering; exact timeline positions remain in FrameMatching's
// rational or Bezier metadata. Invalid inputs return an error and no pixels.
// Concurrent calls are independent and no external resource is accessed.
class IISHAREDCANVAS_EXPORT OpticalFlowInterpolator final {
public:
    [[nodiscard]] OpticalFlowRasterResult operator()(const RasterLayer &first, const RasterLayer &second,
                                                      const OpticalFlowPair &flow, double phase,
                                                      std::size_t maximumPixels = 16777216) const;
};

} // namespace iiSharedCanvas
