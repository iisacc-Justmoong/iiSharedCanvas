#include <iiSharedCanvas.h>
#include <Stroke/StrokePoint.h>
#include <Brush/BrushResolve.h>
#include <iiFileProviderNative.h>

#include <algorithm>
#include <cmath>
#include <iostream>
#include <chrono>
#include <limits>
#include <numbers>
#include <filesystem>

using namespace iiSharedCanvas;
namespace {
int failures = 0;
void check(bool value, const char* message)
{
    if (!value) { std::cerr << message << '\n'; ++failures; }
}
template<class E>
constexpr bool richInput = requires(E& editor, const StrokePoint& point) {
    editor.beginStroke(point);
    editor.continueStroke(point);
    editor.endStroke(point);
};
Document canvas(bool chunked)
{
    Document result;
    result.extent = {64, 64};
    if (chunked) {
        result.canvasMode = CanvasMode::Infinite;
        result.infiniteCanvas = {{0, 0}, 64};
        result.assets.emplace_back(ChunkedRasterAsset{"paint", {}});
    } else result.assets.emplace_back(RasterAsset{"paint", makeRasterLayer(64, 64)});
    result.layers.emplace_back(StaticBitmapLayer{{"layer", "Paint"}, StaticSource{"paint"}});
    return result;
}
RasterLayer pixels(const Document& document)
{
    if (const auto* asset = findRasterAsset(document, "paint")) return asset->pixels;
    const auto* asset = findChunkedRasterAsset(document, "paint");
    if (const auto* chunk = asset ? findRasterChunk(*asset, 0, 0) : nullptr) return chunk->pixels;
    return makeRasterLayer(64, 64);
}
BitmapBrush brush()
{
    BitmapBrush value;
    value.size = 16;
    value.argb = 0xff3366ccU;
    BrushPreset preset;
    preset.size = value.size;
    const auto resolved = resolveBrushPreset(preset, value.argb);
    check(resolved.brush.has_value(), "input fixture uses a resolved brush preset");
    value.engineState = resolved.brush;
    return value;
}
std::pair<int, int> footprint(const RasterLayer& layer)
{
    int left = layer.width, right = -1, top = layer.height, bottom = -1;
    for (int y = 0; y < layer.height; ++y)
        for (int x = 0; x < layer.width; ++x)
            if (layer.pixels[std::size_t(y) * layer.width + x] >> 24) {
                left = std::min(left, x); right = std::max(right, x);
                top = std::min(top, y); bottom = std::max(bottom, y);
            }
    return {right - left + 1, bottom - top + 1};
}
template<class E>
RasterLayer stamp(bool chunked, BitmapBrush configuration, StrokePoint point, double duration = 0)
{
    auto document = canvas(chunked);
    E editor(document, "paint");
    check(editor.setBrush(configuration), "rich input brush configuration");
    check(editor.beginStroke(point), "rich stroke begin");
    point.time += duration;
    check(editor.endStroke(point), "rich stroke end");
    return pixels(document);
}
template<class E>
void exercise(bool chunked)
{
    if constexpr (!richInput<E>) {
        check(false, "bitmap editor lacks StrokePoint input boundary");
    } else {
        StrokePoint point;
        point.position = {32, 32}; point.time = 100;
        auto configuration = brush();
        configuration.engineState->rasterizer.shape.roundness = .25;
        configuration.engineState->rasterizer.shape.angleMode = BrushAngleMode::StylusRotation;
        auto horizontal = stamp<E>(chunked, configuration, point);
        point.rotationRadians = std::numbers::pi / 2;
        auto vertical = stamp<E>(chunked, configuration, point);
        const auto a = footprint(horizontal), b = footprint(vertical);
        check(a.first > a.second && b.second > b.first, "stylus rotation must rotate actual pixels");
        configuration = brush();
        configuration.engineState->dynamics.tiltToEllipse = 1;
        point.rotationRadians = 0;
        auto upright = stamp<E>(chunked, configuration, point);
        point.tiltX = .8;
        auto tilted = stamp<E>(chunked, configuration, point);
        check(footprint(tilted).first * footprint(upright).second
                  > footprint(upright).first * footprint(tilted).second,
              "tilt must increase the footprint aspect ratio with normalized tip size");
        check(footprint(tilted).second < footprint(upright).second,
              "tilt must narrow the orthogonal footprint");
        configuration = brush();
        configuration.flow = .1;
        configuration.engineState->rasterizer.airbrushEnabled = true;
        configuration.engineState->rasterizer.airbrushRate = 10;
        point.tiltX = 0;
        auto brief = stamp<E>(chunked, configuration, point, .01);
        auto held = stamp<E>(chunked, configuration, point, 1);
        check((held.pixels[32 * 64 + 32] >> 24) > (brief.pixels[32 * 64 + 32] >> 24),
              "measured elapsed time must affect emitted coverage");
        auto sampledDocument = canvas(chunked);
        E sampled(sampledDocument, "paint");
        check(sampled.setBrush(configuration) && sampled.beginStroke(point), "dense clock stroke begins");
        for (int i = 1; i < 5; ++i) {
            auto update = point; update.time += .2 * i;
            check(sampled.continueStroke(update), "dense clock sample");
        }
        auto end = point; end.time += 1;
        check(sampled.endStroke(end) && pixels(sampledDocument).pixels == held.pixels,
              "same measured duration is independent of stationary event frequency");
        configuration = brush();
        BrushDynamicsBinding binding;
        binding.source = BrushDynamicsSource::Custom;
        binding.target = BrushDynamicsTarget::Opacity;
        binding.customInput = 7;
        configuration.engineState->dynamics.bindings.push_back(binding);
        auto noCustom = stamp<E>(chunked, configuration, point);
        point.custom[7] = 1;
        auto withCustom = stamp<E>(chunked, configuration, point);
        check((withCustom.pixels[32 * 64 + 32] >> 24) > (noCustom.pixels[32 * 64 + 32] >> 24),
              "custom scalar channel reaches pixel dynamics");
        point.custom[7] = 0;

        auto document = canvas(chunked);
        E editor(document, "paint");
        check(editor.setBrush(brush()) && editor.beginStroke(point), "validation stroke begins");
        const auto before = pixels(document).pixels;
        const auto revision = editor.revision();
        auto invalid = point;
        invalid.time = point.time - 1;
        check(!editor.continueStroke(invalid), "backward timestamp must reject");
        invalid = point; invalid.tiltY = std::numeric_limits<double>::quiet_NaN();
        check(!editor.endStroke(invalid), "non-finite tilt must reject");
        invalid = point; invalid.custom[7] = std::numeric_limits<double>::infinity();
        check(!editor.continueStroke(invalid), "non-finite custom scalar must reject");
        check(editor.strokeActive() && editor.revision() == revision
                  && pixels(document).pixels == before,
              "rejected input preserves active stroke pixels and revision");
        point.time += .1;
        check(editor.endStroke(point), "valid input can finish after rejection");
        check(editor.undo() && pixels(document).pixels == std::vector<std::uint32_t>(4096),
              "rich input stroke remains one undo unit");
        check(editor.redo(), "rich input stroke redo");
        auto preserved = pixels(document).pixels;
        check(editor.beginStroke(point), "cancellable rich stroke begins");
        editor.cancelStroke();
        check(pixels(document).pixels == preserved, "rich input cancellation preserves original pixels");
        configuration = brush();
        configuration.engineState->rasterizer.airbrushEnabled = true;
        configuration.engineState->rasterizer.airbrushRate = 100;
        check(editor.setBrush(configuration) && editor.beginStroke(point), "bounded time stroke begins");
        const auto boundedPixels = pixels(document).pixels;
        const auto boundedRevision = editor.revision();
        auto excessive = point; excessive.time += 1000;
        check(!editor.endStroke(excessive) && editor.strokeActive()
                  && editor.revision() == boundedRevision && pixels(document).pixels == boundedPixels,
              "event budget rejection preserves the active stroke and pixels");
        point.time += .01;
        check(editor.endStroke(point), "stroke finishes normally after budget rejection");
        // Braced coordinate calls must retain their original overload and clock.
        check(editor.beginStroke({4, 4}) && editor.endStroke({4, 4}), "legacy braced point API remains usable");
        auto cappedDocument = canvas(chunked);
        E capped(cappedDocument, "paint");
        configuration = brush(); configuration.opacity = .3; configuration.flow = .5;
        check(capped.setBrush(configuration) && capped.beginStroke(point), "capped stroke begins");
        for (int i = 0; i < 8; ++i) {
            point.time += .01;
            check(capped.continueStroke(point), "capped stroke sample");
        }
        check(capped.endStroke(point), "capped stroke ends");
        const auto once = pixels(cappedDocument).pixels[32 * 64 + 32] >> 24;
        check(once > 0 && once <= 77, "stroke opacity cap survives event boundaries");
        check(capped.beginStroke(point) && capped.endStroke(point), "second capped stroke");
        check((pixels(cappedDocument).pixels[32 * 64 + 32] >> 24) > once,
              "new stroke starts with fresh pending coverage");
    }
}
template<class E>
void crossTiles()
{
    if constexpr (richInput<E>) {
        auto document = canvas(true);
        document.infiniteCanvas = {{-128, -128}, 128};
        document.extent = {256, 256};
        E editor(document, "paint");
        auto configuration = brush();
        check(editor.setBrush(configuration), "cross-tile brush");
        StrokePoint point; point.position = {-68, -68}; point.time = 1;
        check(editor.beginStroke(point), "negative tile stroke begins");
        point.position = {-60, -60}; point.time += .1;
        check(editor.continueStroke(point), "negative tile boundary");
        point.position = {60, 60}; point.time += .1;
        check(editor.continueStroke(point), "world and chunk boundary");
        point.position = {68, 68}; point.time += .1;
        check(editor.endStroke(point), "positive tile boundary");
        check((editor.pixelAt(-64, -64).value_or(0) >> 24) > 0
                  && (editor.pixelAt(64, 64).value_or(0) >> 24) > 0,
              "runtime tiles and differently sized document chunks preserve both sides");
        check(editor.undo() && findChunkedRasterAsset(document, "paint")->chunks.empty(),
              "cross-tile undo restores original sparse storage");
        check(editor.redo() && (editor.pixelAt(-64, -64).value_or(0) >> 24) > 0,
              "cross-tile redo restores pixels without input replay");
    }
}
template<class E>
void fileRoundtrip(bool chunked)
{
    if constexpr (richInput<E>) {
        const auto parent = std::filesystem::path(IISHAREDCANVAS_TEST_OUTPUT_DIR);
        std::filesystem::create_directories(parent);
        iiFileProvider::native::TemporaryDirectory temporary(parent);
        if (!temporary.isValid()) { check(false, "input temporary directory"); return; }
        const auto path = (temporary.path() / "input.iisc").string();
        DocumentFile file;
        if (!file.create(path, canvas(chunked)).ok()) {
            check(false, "input working file creation"); return;
        }
        E editor(file, "paint");
        check(editor.setBrush(brush()), "file-bound input brush");
        StrokePoint sample;
        sample.position = {32, 32}; sample.time = 1;
        check(editor.beginStroke(sample), "file-bound measured stroke begins");
        sample.position.x += 4; sample.time += .1;
        check(editor.continueStroke(sample), "file-bound measured stroke continues");
        const auto before = pixels(*file.document()).pixels;
        const auto revision = file.revision();
        auto invalid = sample; invalid.time = 0;
        check(!editor.continueStroke(invalid) && file.revision() == revision
                  && pixels(*file.document()).pixels == before,
              "rejected file-bound sample preserves storage and runtime");
        sample.position.x += 4; sample.time += .1;
        check(editor.endStroke(sample), "file-bound stroke completes after rejection");
        const auto expected = pixels(*file.document()).pixels;
        check(editor.undo() && editor.redo() && pixels(*file.document()).pixels == expected,
              "file-bound rich stroke undo and redo");
        file.close();
        check(file.open(path).ok() && pixels(*file.document()).pixels == expected,
              "file-bound measured stroke survives reopen as pixels");
    }
}
}
int main()
{
    const auto stage = [](const char *name, auto run)
    {
        const auto begin = std::chrono::steady_clock::now();
        run();
        std::cout << "BITMAP_INPUT_STAGE name=" << name << " elapsed_ms="
                  << std::chrono::duration_cast<std::chrono::milliseconds>(
                         std::chrono::steady_clock::now() - begin).count() << std::endl;
    };
    stage("finite-memory", [] { exercise<BitmapEditor>(false); });
    stage("chunked-memory", [] { exercise<ChunkedBitmapEditor>(true); });
    stage("chunked-boundary", [] { crossTiles<ChunkedBitmapEditor>(); });
    stage("finite-file", [] { fileRoundtrip<BitmapEditor>(false); });
    stage("chunked-file", [] { fileRoundtrip<ChunkedBitmapEditor>(true); });
    return failures ? 1 : 0;
}
