#include <Video/OpticalFlow.h>

#include <iostream>
#include <limits>

using namespace iiSharedCanvas;
namespace {
int failures = 0;
void expect(bool value, const char *message) {
    if (!value) { std::cerr << "FAIL: " << message << '\n'; ++failures; }
}
}

int main() {
    auto first = makeRasterLayer(7, 5);
    auto second = makeRasterLayer(7, 5);
    for (int y = 0; y < first.height; ++y) for (int x = 0; x < first.width; ++x) {
        const auto v = static_cast<std::uint32_t>((x * 61 + y * 37 + x * y * 19) & 255);
        first.pixels[std::size_t(y) * first.width + x] = 0xff000000U | (v << 16) | (v << 8) | v;
        if (x + 1 < first.width) second.pixels[std::size_t(y) * first.width + x + 1] = first.pixels[std::size_t(y) * first.width + x];
    }
    OpticalFlowOptions options;
    options.searchRadius = 2; options.patchRadius = 1; options.maximumComparisons = 1000000;
    const auto estimated = OpticalFlowEstimator{}(first, second, options);
    expect(estimated.ok() && estimated.flow.forward.vectors[2 * 7 + 3].x == 1.0 &&
           estimated.flow.forward.vectors[2 * 7 + 3].y == 0.0 &&
           estimated.flow.backward.vectors[2 * 7 + 4].x == -1.0,
           "motion estimation finds the forward and backward displacement of a textured translation");

    OpticalFlowPair known;
    known.forward.width = known.backward.width = first.width;
    known.forward.height = known.backward.height = first.height;
    known.forward.vectors.assign(first.pixels.size(), {2.0, 0.0});
    known.backward.vectors.assign(first.pixels.size(), {-2.0, 0.0});
    auto shifted = makeRasterLayer(7, 5);
    for (int y = 0; y < first.height; ++y) for (int x = 0; x + 2 < first.width; ++x)
        shifted.pixels[std::size_t(y) * first.width + x + 2] = first.pixels[std::size_t(y) * first.width + x];
    const auto midpoint = OpticalFlowInterpolator{}(first, shifted, known, 0.5);
    expect(midpoint.ok() && midpoint.pixels.pixels[2 * 7 + 3] == first.pixels[2 * 7 + 2],
           "motion-compensated interpolation follows the caller's forward and backward fields");
    expect(OpticalFlowInterpolator{}(first, shifted, known, 0.0).pixels.pixels == first.pixels &&
           OpticalFlowInterpolator{}(first, shifted, known, 1.0).pixels.pixels == shifted.pixels,
           "interpolation endpoints reproduce their input pixels exactly");
    known.forward.vectors[0].x = std::numeric_limits<double>::quiet_NaN();
    expect(!OpticalFlowInterpolator{}(first, shifted, known, 0.5).ok(),
           "non-finite motion vectors are rejected without returning partial pixels");
    options.maximumComparisons = 1;
    expect(!OpticalFlowEstimator{}(first, second, options).ok(),
           "estimation obeys the explicit caller work budget");
    return failures ? 1 : 0;
}
