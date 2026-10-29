#include <Vector/PdfExport.h>
#include <iiFileProviderNative.h>

#include <filesystem>
#include <iostream>
#include <string>
#include <string_view>
#include <utility>

namespace {
using namespace iiSharedCanvas;
namespace fs = std::filesystem;
namespace provider = iiFileProvider::native;
int failures = 0;
void check(bool value, const char *message)
{
    if (!value) { std::cerr << message << '\n'; ++failures; }
}
std::string utf8(const fs::path &path)
{
    const auto value = path.u8string(); return {value.begin(), value.end()};
}
Document fixture()
{
    Document document; document.extent = {8, 6}; document.timeline = {{24, 1}, 2};
    VectorAsset vector; vector.id = "vector"; vector.viewport = document.extent;
    VectorPath path; path.commands = {MoveTo{{1, 1}}, LineTo{{6, 1}},
        QuadraticTo{{7, 3}, {6, 5}}, LineTo{{1, 5}}, ClosePath{}};
    path.fill = SolidPaint{0x80112233U}; path.stroke = StrokeStyle{SolidPaint{0xff445566U}, 1.25};
    vector.paths.push_back(std::move(path));
    document.assets.emplace_back(std::move(vector));
    document.layers.emplace_back(StaticVectorLayer{{"vector-layer", "Vector"}, StaticSource{"vector"}});
    layerProperties(document.layers.back()).transform.translationX = 1e-12;
    document.assets.emplace_back(RasterAsset{"bitmap", makeRasterLayer(2, 2, 0x80402010U)});
    document.layers.emplace_back(StaticBitmapLayer{{"bitmap-layer", "Bitmap"}, StaticSource{"bitmap"}});
    return document;
}
}

int main(int argc, char **argv)
{
    if (argc == 3 && std::string_view(argv[1]) == "--raster-fixture") {
        Document document; document.extent = {8, 8};
        auto pixels = makeRasterLayer(8, 8);
        for (int row = 0; row < 8; ++row)
            for (int column = 0; column < 8; ++column)
                pixels.pixels[std::size_t(row * 8 + column)] = row < 4 ? 0x80ff0000U : 0x800000ffU;
        document.assets.emplace_back(RasterAsset{"pixels", std::move(pixels)});
        document.layers.emplace_back(StaticBitmapLayer{{"raster", "Raster"}, StaticSource{"pixels"}});
        const auto result = exportPdf(document, argv[2]);
        if (!result.ok()) std::cerr << result.message << '\n';
        return result.ok() ? 0 : 1;
    }
    if (argc == 3 && std::string_view(argv[1]) == "--vector-fixture") {
        PdfExportOptions options; options.lastFrame = 1;
        const auto result = exportPdf(fixture(), argv[2], options);
        if (!result.ok()) std::cerr << result.message << '\n';
        return result.ok() ? 0 : 1;
    }
    if (argc == 3 && std::string_view(argv[1]) == "--appearance-fixture") {
        auto document = fixture();
        layerProperties(document.layers[0]).appearance.fillOpacity = 0.5;
        PdfExportOptions options; options.rasterizeUnsupportedBlending = true;
        const auto result = exportPdf(document, argv[2], options);
        if (!result.ok()) std::cerr << result.message << '\n';
        return result.ok() ? 0 : 1;
    }
    fs::create_directories(IISHAREDCANVAS_TEST_OUTPUT_DIR);
    provider::TemporaryDirectory directory{fs::path(IISHAREDCANVAS_TEST_OUTPUT_DIR)};
    auto document = fixture();
    const auto output = directory.path() / u8"canvas-벡터.pdf";
    PdfExportOptions options; options.lastFrame = 1;
    const auto exported = exportPdf(document, utf8(output), options);
    check(exported.ok(), "native PDF must export vector and raster content");
    if (exported.ok()) {
        const auto bytes = provider::File::read(output);
        check(bytes.starts_with("%PDF-") && bytes.find("/Count 2") != bytes.npos,
              "PDF contains two pages for the selected timeline range");
        check(bytes.find("/SMask ") != bytes.npos && bytes.find("/Subtype /Image") != bytes.npos,
              "transparent raster output includes a soft mask");
        check(bytes.find(" c\n") != bytes.npos && bytes.find("B*\n") != bytes.npos,
              "vector path geometry and even-odd fill remain vector PDF operations");
        check(bytes.find("e-12") == bytes.npos && bytes.find("E-12") == bytes.npos,
              "small coordinates use PDF decimal syntax without exponents");
        check(exportPdf(document, utf8(output), options).code == MediaIoCode::AlreadyExists
              && provider::File::read(output) == bytes, "existing output is not overwritten implicitly");
    }
    auto invalid = options; invalid.firstFrame = 2; invalid.lastFrame = 1;
    check(exportPdf(document, utf8(directory.path() / "range.pdf"), invalid).code == MediaIoCode::InvalidArgument,
          "invalid page ranges are rejected");
    auto limited = options; limited.limits.maxOutputBytes = 64;
    const auto tooSmall = directory.path() / "small.pdf";
    check(exportPdf(document, utf8(tooSmall), limited).code == MediaIoCode::LimitExceeded && !fs::exists(tooSmall),
          "output budgets are enforced before publication");
    limited = options; limited.limits.maxVectorCommands = 2;
    check(exportPdf(document, utf8(directory.path() / "commands.pdf"), limited).code == MediaIoCode::LimitExceeded,
          "vector command budgets are enforced");
    auto translucent = document;
    layerProperties(translucent.layers[0]).opacity = 0.5;
    const auto translucentResult = exportPdf(translucent, utf8(directory.path() / "translucent.pdf"), options);
    check(translucentResult.ok() && !translucentResult.warnings.empty(),
          "translucent vector groups are explicitly reported as rasterized");
    auto appearance = document;
    layerProperties(appearance.layers[0]).appearance.fillOpacity = 0.5;
    const auto appearancePath = directory.path() / "appearance.pdf";
    check(exportPdf(appearance, utf8(appearancePath), options).code == MediaIoCode::UnsupportedFeature
          && !fs::exists(appearancePath), "unsupported layer appearance requires explicit rasterization");
    auto flattenAppearance = options; flattenAppearance.rasterizeUnsupportedBlending = true;
    const auto appearanceResult = exportPdf(appearance, utf8(appearancePath), flattenAppearance);
    check(appearanceResult.ok() && !appearanceResult.warnings.empty(),
          "explicit rasterization preserves layer appearance with a warning");
    layerProperties(document.layers[0]).blendMode = RasterBlendMode::Multiply;
    const auto blend = directory.path() / "blend.pdf";
    check(exportPdf(document, utf8(blend), options).code == MediaIoCode::UnsupportedFeature && !fs::exists(blend),
          "unsupported blending never publishes partial output");
    options.rasterizeUnsupportedBlending = true;
    const auto flattened = exportPdf(document, utf8(blend), options);
    check(flattened.ok() && !flattened.warnings.empty(), "explicit frame rasterization preserves non-normal blends");
    options.overwrite = true;
    const auto protectedPath = directory.path() / "canvas.pdf";
    const std::string database("SQLite format 3\0rest", 20);
    provider::File::create(protectedPath, database);
    check(exportPdf(document, utf8(protectedPath), options).code == MediaIoCode::InvalidArgument
          && provider::File::read(protectedPath) == database, "renamed databases are protected from overwrite");
    check(exportPdf(document, utf8(directory.path() / "canvas.IISC"), options).code == MediaIoCode::InvalidArgument,
          "native canvas files are protected from media output");
    return failures ? 1 : 0;
}
