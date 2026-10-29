#include <iiSharedCanvas.h>
#include <iiFileProviderNative.h>
#include <Brush/BrushResolve.h>
#include <filesystem>
#include <iostream>

using namespace iiSharedCanvas;
namespace {
int failures = 0;
void check(bool value, const char* message)
{
    if (!value) { std::cerr << message << '\n'; ++failures; }
}
Document canvas(bool chunked, bool negative = false)
{
    Document document;
    document.extent = {128, 64};
    auto image = makeRasterLayer(128, 64);
    for (int y = 0; y < 64; ++y)
        for (int x = 0; x < 64; ++x) image.pixels[std::size_t(y) * 128 + x] = 0xff0000ffU;
    if (!chunked) document.assets.emplace_back(RasterAsset{"paint", std::move(image)});
    else {
        document.canvasMode = CanvasMode::Infinite;
        document.infiniteCanvas = {{negative ? -64 : 0, 0}, 64};
        ChunkedRasterAsset asset{"paint", {}};
        for (int column = 0; column < 2; ++column)
            asset.chunks.push_back({column - (negative ? 1 : 0), 0,
                makeRasterLayer(64, 64, column == 0 ? 0xff0000ffU : 0)});
        document.assets.emplace_back(std::move(asset));
    }
    document.assets.emplace_back(RasterAsset{"untouched", makeRasterLayer(8, 8, 0xff00ff00U)});
    document.layers.emplace_back(StaticBitmapLayer{{"layer", "Paint"}, StaticSource{"paint"}});
    return document;
}
BitmapBrush brush(bool smudge = false)
{
    BitmapBrush value;
    value.argb = 0xffff0000U;
    value.size = 8;
    value.spacingRatio = .25;
    BrushPreset preset;
    preset.size = value.size;
    preset.material.simulation.enabled = true;
    preset.material.simulation.model = smudge ? BrushSimulationModel::Smudge : BrushSimulationModel::Mixer;
    preset.material.simulation.wetness = 1;
    preset.material.simulation.pickup = 1;
    preset.material.simulation.mixStrength = 1;
    preset.material.simulation.smudgeStrength = smudge ? 1 : 0;
    preset.material.simulation.deposit = smudge ? 0 : .5;
    value.engineState = resolveBrushPreset(preset, value.argb).brush;
    check(value.engineState.has_value(), "wet brush fixture resolves");
    return value;
}
template<class E>
std::vector<std::uint32_t> image(const E& editor, int origin = 0)
{
    std::vector<std::uint32_t> result;
    result.reserve(8192);
    for (int y = 0; y < 64; ++y)
        for (int x = 0; x < 128; ++x) result.push_back(editor.pixelAt(origin + x, y).value_or(0));
    return result;
}
template<class E>
std::uint32_t stamp(BitmapBrush configuration, bool chunked, int x = 32, bool negative = false)
{
    auto document = canvas(chunked, negative);
    E editor(document, "paint");
    // Settle at the stroke opacity cap. Material flow attenuates each dab;
    // saturation isolates sampled color from that independent coverage factor.
    configuration.engineState->rasterizer.airbrushEnabled = true;
    configuration.engineState->rasterizer.airbrushRate = 100;
    check(editor.setBrush(configuration), "wet brush accepted");
    StrokePoint point;
    point.position = {double(x), 32}; point.time = 1;
    check(editor.beginStroke(point), "wet stroke begins");
    point.time = 2;
    check(editor.endStroke(point), "wet stroke commits at opacity cap");
    return editor.pixelAt(x, 32).value_or(0);
}
template<class E>
std::vector<std::uint32_t> partitioned(bool chunked, bool negative, bool dense, bool smudge)
{
    auto document = canvas(chunked, negative);
    E editor(document, "paint");
    auto configuration = brush(smudge);
    configuration.opacity = .7;
    configuration.flow = .3;
    check(editor.setBrush(configuration), "partitioned wet brush");
    const int offset = negative ? -64 : 0;
    StrokePoint point; point.position = {48.0 + offset, 32}; point.time = 1;
    check(editor.beginStroke(point), "partitioned wet begin");
    if (dense) for (int i = 1; i < 4; ++i) {
        point.position.x = 48.0 + offset + 8 * i; point.time = 1 + .25 * i;
        check(editor.continueStroke(point), "partitioned wet update");
    }
    point.position.x = 80.0 + offset; point.time = 2;
    check(editor.endStroke(point), "partitioned wet end");
    return image(editor, offset);
}
template<class E>
void exercise(bool chunked)
{
    auto configuration = brush();
    const auto mixed = stamp<E>(configuration, chunked);
    check(((mixed >> 16) & 255) >= 127 && ((mixed >> 16) & 255) <= 128
              && (mixed & 255) >= 127 && (mixed & 255) <= 128 && ((mixed >> 8) & 255) == 0,
          "mixer must combine red pigment with the bound blue source");
    auto varied = configuration;
    auto& simulation = varied.engineState->material.simulation;
    simulation.pickup = .5; simulation.deposit = .1; simulation.mixStrength = .8;
    const auto reference = stamp<E>(varied, chunked);
    simulation.mixStrength = .2;
    check(stamp<E>(varied, chunked) != reference, "Mix changes source mixing pixels");
    simulation.mixStrength = .8; simulation.wetness = .2;
    check(stamp<E>(varied, chunked) != reference, "Wetness changes source mixing pixels");
    simulation.wetness = 1; simulation.pickup = .1;
    check(stamp<E>(varied, chunked) != reference, "Pickup changes source mixing pixels");
    configuration = brush(true);
    configuration.engineState->material.simulation.wetness = .5;
    check(stamp<E>(configuration, chunked, 64) == 0x800000ffU,
          "unloaded smudge pulls blue alpha into transparency across the chunk boundary");
    check(stamp<E>(configuration, chunked, 100) == 0,
          "unloaded smudge on empty content cannot inject foreground red");
    check(partitioned<E>(chunked, false, false, false) == partitioned<E>(chunked, false, true, false),
          "mixer reads one stroke-start source regardless of event partition");
    check(partitioned<E>(chunked, false, false, true) == partitioned<E>(chunked, false, true, true),
          "smudge reads one stroke-start source regardless of event partition");
    auto document = canvas(chunked);
    E editor(document, "paint");
    const auto original = image(editor);
    const auto other = findRasterAsset(document, "untouched")->pixels.pixels;
    check(editor.setBrush(brush()), "history mixer configuration");
    StrokePoint point; point.position = {32, 32}; point.time = 1;
    check(editor.beginStroke(point) && editor.endStroke(point), "history mixer commit");
    const auto committed = image(editor);
    check(committed != original && editor.undo() && image(editor) == original,
          "mixer is one undoable pixel change");
    check(editor.redo() && image(editor) == committed, "mixer redo restores mixed pixels");
    check(editor.beginStroke(point), "cancellable mixer begins");
    editor.cancelStroke();
    check(image(editor) == committed, "mixer cancellation restores pre-stroke pixels");
    check(findRasterAsset(document, "untouched")->pixels.pixels == other,
          "sampling never writes another asset");
}
template<class E>
void fileRoundtrip(bool chunked)
{
    const auto parent = std::filesystem::path(IISHAREDCANVAS_TEST_OUTPUT_DIR);
    std::filesystem::create_directories(parent);
    iiFileProvider::native::TemporaryDirectory temporary(parent);
    if (!temporary.isValid()) { check(false, "wet file temporary directory"); return; }
    const auto path = (temporary.path() / "wet.iisc").string();
    DocumentFile file;
    if (!file.create(path, canvas(chunked)).ok()) { check(false, "wet file creation"); return; }
    E editor(file, "paint");
    check(editor.setBrush(brush()), "file-bound mixer brush");
    StrokePoint point; point.position = {32, 32}; point.time = 1;
    check(editor.beginStroke(point), "file-bound mixer begins");
    point.position.x += 2; point.time += .1;
    check(editor.endStroke(point), "file-bound mixer ends");
    const auto expected = image(editor);
    check(editor.undo() && editor.redo() && image(editor) == expected, "file-bound wet undo redo");
    file.close();
    check(file.open(path).ok(), "wet file reopens");
    E reopened(file, "paint");
    check(image(reopened) == expected, "mixed pixels survive file reopen");
}
}
int main()
{
    exercise<BitmapEditor>(false);
    exercise<ChunkedBitmapEditor>(true);
    auto configuration = brush(true); configuration.engineState->material.simulation.wetness = .5;
    check(stamp<ChunkedBitmapEditor>(configuration, true, 0, true) == 0x800000ffU,
          "negative source origin samples the adjacent negative chunk");
    check(partitioned<BitmapEditor>(false, false, false, true)
              == partitioned<ChunkedBitmapEditor>(true, true, true, true),
          "finite and negative chunked smudge share world-space source semantics");
    fileRoundtrip<BitmapEditor>(false);
    fileRoundtrip<ChunkedBitmapEditor>(true);
    return failures ? 1 : 0;
}
