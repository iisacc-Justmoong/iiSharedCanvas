#include "../src/Bitmap/StrokeComposite_p.hpp"

#include <cstdlib>
#include <array>
#include <chrono>
#include <iostream>
#include <new>
#include <stdexcept>
#include <vector>

namespace {
thread_local bool measuring = false;
thread_local std::size_t requested = 0;

void checkMultipleTiles(RasterBlendMode mode) {
    constexpr int side = 192, origin = -64;
    auto original = makeRasterLayer(side, side, 0x80402010U);
    auto expected = original;
    auto pending = makeStrokeCompositeBuffer(side, side);
    iiSharedCanvas::detail::BitmapStrokeComposite composite;
    const auto read = [&](DevicePixelPoint point) {
        return original.pixels[std::size_t(point.y - origin) * side + point.x - origin];
    };
    constexpr std::array positions{-64, -1, 0, 63, 64, 127};
    for (int batch = 0; batch < 2; ++batch) {
        std::vector<RasterSample> world, reference;
        for (int i = 0; i < 120; ++i) {
            const int x = positions[std::size_t((i + batch) % positions.size())];
            const int y = positions[std::size_t((i * 5 + batch) % positions.size())];
            RasterSample sample{{x, y}, std::uint32_t(0x40336600U + (i % 128)), 220,
                                RasterBlendMode::SourceOver};
            world.push_back(sample);
            sample.position.x -= origin;
            sample.position.y -= origin;
            reference.push_back(sample);
        }
        accumulateStrokeSamples(pending, reference);
        if (mode == RasterBlendMode::SourceOver || mode == RasterBlendMode::DestinationOut)
            copyStrokePreviewSamplePixels(expected, original, pending, reference,
                                          mode == RasterBlendMode::DestinationOut);
        else for (const auto &sample : reference) {
            const auto index = std::size_t(sample.position.y) * side + sample.position.x;
            const auto source = strokeCompositePixelAt(pending, sample.position);
            expected.pixels[index] = source >> 24
                ? compositeRasterPixel(source, original.pixels[index], mode) : original.pixels[index];
        }
        auto prepared = composite.preparePaint(world, mode, read);
        std::size_t visited = 0;
        prepared.visit([&](auto bounds, const auto &, const auto &preview) {
            ++visited;
            for (int y = 0; y < 64; ++y) for (int x = 0; x < 64; ++x) {
                const auto expectedIndex = std::size_t(bounds.origin.y + y - origin) * side +
                                           bounds.origin.x + x - origin;
                if (preview.pixels[std::size_t(y) * 64 + x] != expected.pixels[expectedIndex])
                    throw std::runtime_error("multi-tile stroke changed pixel order or blend result");
            }
        });
        if (visited < 2) throw std::runtime_error("multi-tile stroke omitted tiles");
        composite.commitPaint(std::move(prepared));
    }
}

void checkInterleavedTileMemory() {
    constexpr int side = 128;
    constexpr std::size_t count = 32768;
    std::vector<RasterSample> samples;
    samples.reserve(count);
    for (std::size_t i = 0; i < count; ++i) {
        const int column = int((i / 64) % 2), row = int((i / 128) % 2);
        const int x = column * 64 + int(i % 64), y = row * 64 + int((i / 256) % 64);
        const auto alpha = std::uint32_t(32 + i % 128);
        samples.push_back({{x, y}, (alpha << 24) | 0x003366ffU,
                           std::uint8_t(160 + i % 96), RasterBlendMode::SourceOver});
    }
    const auto original = makeRasterLayer(side, side, 0x80402010U);
    auto expected = original;
    auto pending = makeStrokeCompositeBuffer(side, side);
    accumulateStrokeSamples(pending, samples);
    copyStrokePreviewSamplePixels(expected, original, pending, samples, false);
    iiSharedCanvas::detail::BitmapStrokeComposite composite;
    requested = 0; measuring = true;
    const auto start = std::chrono::steady_clock::now();
    auto prepared = composite.preparePaint(samples, RasterBlendMode::SourceOver,
        [&](DevicePixelPoint point) { return original.pixels[std::size_t(point.y) * side + point.x]; });
    const auto elapsed = std::chrono::duration_cast<std::chrono::microseconds>(
        std::chrono::steady_clock::now() - start).count();
    measuring = false;
    const auto bytes = requested;
    std::size_t visited = 0;
    prepared.visit([&](auto bounds, const auto &, const auto &preview) {
        ++visited;
        for (int y = 0; y < 64; ++y) for (int x = 0; x < 64; ++x) {
            const auto expectedIndex = std::size_t(bounds.origin.y + y) * side + bounds.origin.x + x;
            if (preview.pixels[std::size_t(y) * 64 + x] != expected.pixels[expectedIndex])
                throw std::runtime_error("interleaved tile input changed its reference pixels");
        }
    });
    if (visited != 4) throw std::runtime_error("interleaved input did not visit all four tiles");
    const auto budget = std::size_t(4 * 64 * 64) *
        (2 * sizeof(std::uint32_t) + sizeof(PremultipliedPixel)) +
        count * sizeof(RasterSample) + 8192;
    std::cout << "STROKE_INTERLEAVED requested=" << bytes << " budget=" << budget
              << " prepare_us=" << elapsed << '\n';
    if (bytes > budget) throw std::runtime_error("interleaved tile input repeatedly allocated grouped samples");
}

void checkSingleTileBatches(RasterBlendMode mode) {
    constexpr int side = 64, origin = -64;
    const auto original = makeRasterLayer(side, side, 0x80402010U);
    auto expected = original;
    auto pending = makeStrokeCompositeBuffer(side, side);
    iiSharedCanvas::detail::BitmapStrokeComposite composite;
    const auto read = [&](DevicePixelPoint point) {
        return original.pixels[std::size_t(point.y - origin) * side + point.x - origin];
    };
    for (int batch = 0; batch < 2; ++batch) {
        std::vector<RasterSample> world, reference;
        world.reserve(4097); reference.reserve(4097);
        for (int i = 0; i < 4097; ++i) {
            const int x = (i * 13 + batch) % side, y = (i * 7 + i / side + batch) % side;
            RasterSample sample{{origin + x, origin + y},
                (std::uint32_t(32 + i % 128) << 24) | 0x003366ffU,
                std::uint8_t(160 + i % 96),
                i % 2 ? RasterBlendMode::Multiply : RasterBlendMode::SourceOver};
            world.push_back(sample);
            sample.position = {x, y};
            sample.blendMode = RasterBlendMode::SourceOver;
            reference.push_back(sample);
        }
        const auto previous = expected;
        accumulateStrokeSamples(pending, reference);
        if (mode == RasterBlendMode::SourceOver || mode == RasterBlendMode::DestinationOut)
            copyStrokePreviewSamplePixels(expected, original, pending, reference,
                                          mode == RasterBlendMode::DestinationOut);
        else for (const auto &sample : reference) {
            const auto index = std::size_t(sample.position.y) * side + sample.position.x;
            const auto source = strokeCompositePixelAt(pending, sample.position);
            expected.pixels[index] = source >> 24
                ? compositeRasterPixel(source, original.pixels[index], mode) : original.pixels[index];
        }
        auto prepared = composite.preparePaint(world, mode, read);
        prepared.visit([&](auto bounds, const auto &before, const auto &preview) {
            if (bounds.origin.x != origin || bounds.origin.y != origin ||
                before.pixels != original.pixels || preview.pixels != expected.pixels)
                throw std::runtime_error("single-tile batches changed ordered blend pixels");
        });
        for (int y = 0; y < side; ++y) for (int x = 0; x < side; ++x)
            if (composite.previewPixel({origin + x, origin + y}, read) !=
                previous.pixels[std::size_t(y) * side + x])
                throw std::runtime_error("uncommitted single-tile preparation changed the stroke");
        composite.commitPaint(std::move(prepared));
    }
}
}
void *operator new(std::size_t size) {
    if (measuring) requested += size;
    if (auto *p = std::malloc(size ? size : 1)) return p;
    throw std::bad_alloc();
}
void *operator new[](std::size_t size) { return ::operator new(size); }
void operator delete(void *p) noexcept { std::free(p); }
void operator delete[](void *p) noexcept { std::free(p); }
void operator delete(void *p, std::size_t) noexcept { std::free(p); }
void operator delete[](void *p, std::size_t) noexcept { std::free(p); }

int main() try {
    constexpr std::size_t count = 32768;
    std::vector<RasterSample> samples;
    samples.reserve(count);
    for (std::size_t i = 0; i < count; ++i) {
        const int x = int((i * 13) % 64), y = int((i * 7 + i / 64) % 64);
        samples.push_back({{x, y}, 0x803366ffU, 220, RasterBlendMode::SourceOver});
    }
    const auto original = makeRasterLayer(64, 64, 0x80402010U);
    auto expected = original;
    auto pending = makeStrokeCompositeBuffer(64, 64);
    accumulateStrokeSamples(pending, samples);
    copyStrokePreviewSamplePixels(expected, original, pending, samples, false);

    iiSharedCanvas::detail::BitmapStrokeComposite composite;
    requested = 0;
    measuring = true;
    auto prepared = composite.preparePaint(samples, RasterBlendMode::SourceOver,
        [&](DevicePixelPoint point) { return original.pixels[std::size_t(point.y) * 64 + point.x]; });
    measuring = false;
    const auto bytes = requested;
    bool visited = false;
    prepared.visit([&](auto bounds, const auto &before, const auto &preview) {
        visited = true;
        if (bounds.origin.x != 0 || bounds.origin.y != 0 || before.pixels != original.pixels ||
            preview.pixels != expected.pixels)
            throw std::runtime_error("prepared stroke differs from ordered reference pixels");
    });
    if (!visited) throw std::runtime_error("prepared stroke omitted the touched tile");
    composite.commitPaint(std::move(prepared));
    if (!composite.changed()) throw std::runtime_error("committed stroke lost its change");
    const auto budget = std::size_t(64 * 64) *
        (2 * sizeof(std::uint32_t) + sizeof(PremultipliedPixel)) + 1024 * sizeof(RasterSample) + 8192;
    std::cout << "STROKE_BATCH requested=" << bytes << " budget=" << budget << '\n';
    if (bytes > budget) throw std::runtime_error("single-tile stroke copied its complete sample buffer");
    const std::vector<RasterSample> next(samples.begin(), samples.begin() + 64);
    accumulateStrokeSamples(pending, next);
    copyStrokePreviewSamplePixels(expected, original, pending, next, false);
    requested = 0; measuring = true;
    auto continued = composite.preparePaint(next, RasterBlendMode::SourceOver,
        [&](DevicePixelPoint point) { return original.pixels[std::size_t(point.y) * 64 + point.x]; });
    measuring = false;
    const auto continuationBytes = requested;
    continued.visit([&](auto, const auto &before, const auto &preview) {
        if (before.pixels != original.pixels || preview.pixels != expected.pixels)
            throw std::runtime_error("continued stroke changed its original or preview pixels");
    });
    const auto continuationBudget = next.size() * sizeof(RasterSample) + 152 * 1024;
    std::cout << "STROKE_CONTINUATION requested=" << continuationBytes
              << " budget=" << continuationBudget << '\n';
    if (continuationBytes > continuationBudget)
        throw std::runtime_error("continued stroke copied its immutable original tile");
    for (const auto mode : {RasterBlendMode::SourceOver, RasterBlendMode::DestinationOut,
                            RasterBlendMode::Multiply}) {
        checkMultipleTiles(mode);
        checkSingleTileBatches(mode);
    }
    checkInterleavedTileMemory();
    return 0;
} catch (const std::exception &error) {
    measuring = false;
    std::cerr << error.what() << '\n';
    return 1;
}
