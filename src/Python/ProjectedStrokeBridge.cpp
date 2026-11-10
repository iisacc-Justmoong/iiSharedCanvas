#include "ProjectedStrokeBridge.h"

#include <iiSharedCanvasNative.h>

#include <array>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <exception>
#include <limits>
#include <memory>
#include <stdexcept>
#include <vector>

using namespace iiSharedCanvas;

struct IiscProjectedRaster {
    Document document;
    BitmapEditor editor;
    std::array<char, 256> error{};

    IiscProjectedRaster(int width, int height, std::uint32_t background, bool erase) {
        document.extent = {width, height};
        document.assets.emplace_back(RasterAsset{"paint", makeRasterLayer(width, height, background)});
        BitmapBrush brush;
        brush.eraser = erase;
        if (!editor.bind(document, "paint") || !editor.setBrush(brush) ||
            !editor.setDeferredStrokeHistory(true))
            throw std::runtime_error(editor.lastError());
    }
    void set_error(const char *message) noexcept {
        std::snprintf(error.data(), error.size(), "%s", message ? message : "Unknown error");
    }
};

namespace {
template <class Function> int invoke(IiscProjectedRaster *raster, Function function) noexcept {
    if (!raster) return 0;
    try {
        if (!function(*raster)) {
            raster->set_error(raster->editor.lastError().c_str());
            return 0;
        }
        raster->error[0] = '\0';
        return 1;
    } catch (const std::exception &error) {
        raster->set_error(error.what());
    } catch (...) {
        raster->set_error("Unknown native error");
    }
    return 0;
}

std::vector<RasterSample> convert(const IiscProjectedSample *samples, std::size_t count,
                                  std::size_t maximum) {
    if ((count && !samples) || count > maximum)
        throw std::invalid_argument("Invalid projected sample batch");
    std::vector<RasterSample> result;
    result.reserve(count);
    for (std::size_t index = 0; index < count; ++index)
        result.push_back({{samples[index].x, samples[index].y}, samples[index].argb,
                          samples[index].opacity_cap, RasterBlendMode::SourceOver});
    return result;
}
}

extern "C" {
IiscProjectedRaster *iisc_projected_create(int32_t width, int32_t height,
                                            uint32_t background, int erase) {
    const auto maximum_pixels = RasterHistoryLimits{}.maximumInputBytes / sizeof(std::uint32_t);
    if (width <= 0 || height <= 0 || (erase != 0 && erase != 1) ||
        std::uint64_t(width) * std::uint64_t(height) >
            std::numeric_limits<std::size_t>::max() / sizeof(std::uint32_t) ||
        std::uint64_t(width) * std::uint64_t(height) > maximum_pixels) return nullptr;
    try { return new IiscProjectedRaster(width, height, background, erase != 0); }
    catch (...) { return nullptr; }
}
void iisc_projected_destroy(IiscProjectedRaster *raster) { delete raster; }
const char *iisc_projected_error(const IiscProjectedRaster *raster) {
    return raster ? raster->error.data() : "No raster is open";
}
int iisc_projected_begin(IiscProjectedRaster *raster) {
    return invoke(raster, [](auto &value) { return value.editor.beginProjectedStroke(); });
}
int iisc_projected_update(IiscProjectedRaster *raster,
                          const IiscProjectedSample *stable, size_t stable_count,
                          const IiscProjectedSample *transient, size_t transient_count) {
    return invoke(raster, [&](auto &value) {
        const auto maximum = value.editor.historyLimits().maximumInputBytes / sizeof(RasterSample);
        auto fixed = convert(stable, stable_count, maximum);
        auto tip = convert(transient, transient_count, maximum);
        return value.editor.updateProjectedStroke(fixed, tip);
    });
}
int iisc_projected_end(IiscProjectedRaster *raster) {
    return invoke(raster, [](auto &value) { return value.editor.endProjectedStroke(); });
}
int iisc_projected_cancel(IiscProjectedRaster *raster) {
    return invoke(raster, [](auto &value) {
        if (!value.editor.strokeActive()) throw std::logic_error("No projected stroke is active");
        value.editor.cancelStroke();
        return !value.editor.strokeActive();
    });
}
int iisc_projected_undo(IiscProjectedRaster *raster) {
    return invoke(raster, [](auto &value) { return value.editor.undo(); });
}
int iisc_projected_redo(IiscProjectedRaster *raster) {
    return invoke(raster, [](auto &value) { return value.editor.redo(); });
}
int iisc_projected_pixel(IiscProjectedRaster *raster, int32_t x, int32_t y, uint32_t *pixel) {
    return invoke(raster, [&](auto &value) {
        const auto result = value.editor.pixelAt(x, y);
        if (!pixel || !result) throw std::invalid_argument("Pixel coordinate is outside the raster");
        *pixel = *result;
        return true;
    });
}
int iisc_projected_source_pixel(IiscProjectedRaster *raster, int32_t x, int32_t y, uint32_t *pixel) {
    return invoke(raster, [&](auto &value) {
        const auto result = value.editor.strokeSourceAt(x, y);
        if (!pixel || !result) throw std::invalid_argument("No stroke source exists at the pixel");
        *pixel = *result;
        return true;
    });
}
}
