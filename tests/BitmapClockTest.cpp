#include <iiSharedCanvas.h>
#include <iiFileProviderNative.h>
#include <Brush/BrushResolve.h>
#include <cmath>
#include <cstdint>
#include <filesystem>
#include <iostream>
#include <limits>

using namespace iiSharedCanvas;
namespace {
int failures = 0;
void check(bool value, const char* message)
{
    if (!value) { std::cerr << message << '\n'; ++failures; }
}
template<class E>
constexpr bool hasClock = requires(E& editor) {
    editor.advanceStrokeTime(1.0);
    editor.rebaseStrokeTime(1.0);
};
Document canvas(bool chunked)
{
    Document value; value.extent = {64, 64};
    if (chunked) {
        value.canvasMode = CanvasMode::Infinite;
        value.infiniteCanvas = {{0, 0}, 64};
        value.assets.emplace_back(ChunkedRasterAsset{"paint", {}});
    } else value.assets.emplace_back(RasterAsset{"paint", makeRasterLayer(64, 64)});
    value.layers.emplace_back(StaticBitmapLayer{{"layer", "Paint"}, StaticSource{"paint"}});
    return value;
}
BitmapBrush airbrush()
{
    BitmapBrush value; value.size = 4; value.flow = .02; value.argb = 0xff3366ccU;
    BrushPreset preset; preset.size = value.size;
    preset.stroke.airbrushEnabled = true; preset.stroke.airbrushRate = 10;
    value.engineState = resolveBrushPreset(preset, value.argb).brush;
    return value;
}
unsigned alphaForDabs(int count)
{
    // Each full-mask dab has round(255*.02)=5 alpha. This is the
    // independent source-over equation, with quantization only at output.
    return unsigned(std::lround(255 * (1 - std::pow(250.0 / 255, count))));
}
template<class E>
std::uint32_t hold(bool chunked, double duration, bool dense)
{
    auto document = canvas(chunked); E editor(document, "paint");
    check(editor.setBrush(airbrush()), "clock brush accepted");
    StrokePoint point; point.position = {32, 32}; point.time = 1;
    check(editor.beginStroke(point), "stationary stroke begins");
    if (dense) for (int i = 1; i < 100; ++i)
        check(editor.advanceStrokeTime(1 + duration * i / 100), "dense clock tick");
    check(editor.advanceStrokeTime(1 + duration), "stationary clock advances");
    const auto pixel = editor.pixelAt(32, 32).value_or(0);
    point.time = 1 + duration;
    check(editor.endStroke(point) && editor.pixelAt(32, 32).value_or(0) == pixel,
          "release keeps the stationary preview pixels");
    return pixel;
}
template<class E>
void exercise(bool chunked)
{
    if constexpr (!hasClock<E>) check(false, "bitmap editor lacks stroke clock boundary");
    else {
        for (const double duration : {.2, 1.0, 2.0}) {
            const auto pixel = hold<E>(chunked, duration, false);
            check((pixel >> 24) == alphaForDabs(1 + int(std::lround(duration * 10))),
                  "stationary dose follows elapsed seconds and configured rate");
            check(hold<E>(chunked, duration, true) == pixel,
                  "clock tick frequency cannot change stationary dose");
        }
        auto document = canvas(chunked); E editor(document, "paint");
        auto invalidBrush = airbrush();
        invalidBrush.engineState->rasterizer.smoothingCatchUpRate = std::numeric_limits<double>::infinity();
        check(!editor.setBrush(invalidBrush), "non-finite clock brush settings reject before painting");
        check(!editor.advanceStrokeTime(1) && !editor.rebaseStrokeTime(1), "inactive clock rejects");
        check(editor.setBrush(airbrush()), "pause brush accepted");
        StrokePoint point; point.position = {32, 32}; point.time = 1;
        check(editor.beginStroke(point) && editor.advanceStrokeTime(1.2), "pre-pause dose");
        const auto pixel = editor.pixelAt(32, 32);
        const auto revision = editor.revision();
        check(editor.rebaseStrokeTime(100) && editor.revision() == revision
                  && editor.pixelAt(32, 32) == pixel,
              "resume rebases clock without drawing or revising document");
        check(editor.advanceStrokeTime(100) && editor.revision() == revision,
              "same-time tick adds no dose");
        check(editor.advanceStrokeTime(100.1)
                  && (editor.pixelAt(32, 32).value_or(0) >> 24) == alphaForDabs(4),
              "suspended time cannot produce a catch-up spray burst");
        const auto stable = editor.pixelAt(32, 32); const auto stableRevision = editor.revision();
        check(!editor.advanceStrokeTime(99) && !editor.rebaseStrokeTime(99)
                  && !editor.advanceStrokeTime(std::numeric_limits<double>::infinity())
                  && !editor.rebaseStrokeTime(std::numeric_limits<double>::quiet_NaN())
                  && !editor.advanceStrokeTime(1e6),
              "clock rejects backward and non-finite values");
        check(editor.pixelAt(32, 32) == stable && editor.revision() == stableRevision && editor.strokeActive(),
              "rejected clock preserves active stroke pixels and revision");
        editor.cancelStroke();
        check(editor.pixelAt(32, 32).value_or(0) == 0, "time-based stroke cancellation");

        auto settings = airbrush(); settings.flow = 1;
        auto& raster = settings.engineState->rasterizer;
        raster.airbrushEnabled = false;
        raster.smoothingEnabled = true; raster.smoothing = 1;
        raster.smoothingMode = BrushSmoothingMode::PulledString;
        raster.smoothingRadius = 10; raster.smoothingCatchUp = true;
        raster.smoothingCatchUpRate = 10; raster.smoothingCatchUpOnEnd = false;
        check(editor.setBrush(settings), "catch-up brush accepted");
        point.position = {8, 16}; point.time = 1;
        check(editor.beginStroke(point), "catch-up stroke begins");
        point.position = {40, 16}; point.time = 1.1;
        check(editor.continueStroke(point) && editor.pixelAt(39, 16).value_or(0) == 0,
              "pulled-string target remains ahead of filtered tip");
        check(editor.advanceStrokeTime(1.5) && (editor.pixelAt(39, 16).value_or(0) >> 24) > 0,
              "clock catches filtered tip up to stationary raw target");
        editor.cancelStroke();
        settings = airbrush(); settings.engineState->rasterizer.airbrushEnabled = false;
        check(editor.setBrush(settings) && editor.beginStroke(point), "ordinary stroke begins");
        const auto ordinaryRevision = editor.revision();
        check(!editor.advanceStrokeTime(2) && editor.revision() == ordinaryRevision,
              "ordinary brush does not synthesize stationary paint");
        check(editor.rebaseStrokeTime(10) && editor.revision() == ordinaryRevision,
              "ordinary brush can exclude suspended time from later velocity");
        editor.cancelStroke();
    }
}
template<class E>
void fileClock(bool chunked)
{
    if constexpr (hasClock<E>) {
        const auto parent = std::filesystem::path(IISHAREDCANVAS_TEST_OUTPUT_DIR);
        std::filesystem::create_directories(parent);
        iiFileProvider::native::TemporaryDirectory directory(parent);
        if (!directory.isValid()) { check(false, "clock temporary directory"); return; }
        const auto path = (directory.path() / "clock.iisc").string();
        DocumentFile file;
        if (!file.create(path, canvas(chunked)).ok()) { check(false, "clock file creation"); return; }
        E editor(file, "paint");
        check(editor.setBrush(airbrush()), "file clock brush");
        StrokePoint point; point.position = {32, 32}; point.time = 1;
        check(editor.beginStroke(point) && editor.advanceStrokeTime(1.2), "file clock dose");
        const auto revision = file.revision();
        check(editor.rebaseStrokeTime(100) && file.revision() == revision, "file rebase has no saved edit");
        check(editor.advanceStrokeTime(100.1), "file resumed dose");
        point.time = 100.1;
        check(editor.endStroke(point), "file clock stroke ends");
        const auto expected = editor.pixelAt(32, 32);
        check(editor.undo() && editor.pixelAt(32, 32).value_or(0) == 0 && editor.redo(),
              "file clock stroke is one undo unit");
        file.close();
        check(file.open(path).ok(), "clock file reopen");
        E reopened(file, "paint");
        check(reopened.pixelAt(32, 32) == expected, "clock pixels survive reopen");
    }
}
}
int main()
{
    exercise<BitmapEditor>(false);
    exercise<ChunkedBitmapEditor>(true);
    fileClock<BitmapEditor>(false);
    fileClock<ChunkedBitmapEditor>(true);
    return failures ? 1 : 0;
}
