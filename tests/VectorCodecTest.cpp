#include <iiSharedCanvas.h>

#include <QBuffer>
#include <QDir>
#include <QFile>
#include <QGuiApplication>
#include <QImageReader>
#include <QTemporaryDir>

#include <cmath>
#include <iostream>
#include <limits>

namespace {
int failures = 0;
void expect(bool value, const std::string &message)
{
    if (!value) { std::cerr << message << '\n'; ++failures; }
}
std::vector<std::uint8_t> bytes(const std::string &text) { return {text.begin(), text.end()}; }
iiSharedCanvas::Document documentFor(const iiSharedCanvas::VectorAsset &asset)
{
    using namespace iiSharedCanvas;
    Document document;
    document.extent = asset.viewport;
    document.assets.emplace_back(asset);
    document.layers.emplace_back(VectorLayer{{"vector", "Vector"}, StaticSource{asset.id}});
    return document;
}
}

int main(int argc, char **argv)
{
    QGuiApplication app(argc, argv);
    using namespace iiSharedCanvas;
    QDir().mkpath(IISHAREDCANVAS_TEST_OUTPUT_DIR);
    QTemporaryDir directory(QStringLiteral(IISHAREDCANVAS_TEST_OUTPUT_DIR "/vector-codec-XXXXXX"));
    const auto source = bytes(R"svg(<svg xmlns="http://www.w3.org/2000/svg" width="80" height="60" viewBox="0 0 80 60">
      <g transform="translate(3,4)" fill="#ff3311">
        <path d="M 1 1 h 15 v 12 h -15 z"/>
        <path fill="none" stroke="#112233" stroke-width="2" stroke-linecap="round" stroke-linejoin="round"
          d="M25 10 Q30 0 35 10 T45 10 C50 0 55 0 60 10 S70 20 72 10"/>
        <circle cx="12" cy="32" r="5"/>
        <ellipse cx="30" cy="32" rx="7" ry="3"/>
        <polyline fill="none" stroke="blue" points="3,45 15,50 20,43"/>
        <polygon points="45,30 55,45 40,45"/>
        <rect x="58" y="30" width="14" height="20" rx="3"/>
      </g>
    </svg>)svg");
    VectorImportOptions options;
    options.assetId = "art";
    auto imported = decodeSvg(source, options);
    expect(imported.ok(), "import editable SVG shapes: " + imported.result.message);
    if (!imported.ok()) { return 1; }
    expect(imported.asset.id == "art" && imported.asset.viewport.width == 80
           && imported.asset.paths.size() >= 7, "SVG asset geometry and viewport");
    const auto &first = std::get<MoveTo>(imported.asset.paths[0].commands[0]).point;
    expect(first.x == 4 && first.y == 5, "SVG nested transforms are applied in the right order");
    bool quadratic = false, cubic = false, linear = false;
    for (const auto &path : imported.asset.paths) {
        for (const auto &command : path.commands) {
            quadratic |= std::holds_alternative<QuadraticTo>(command);
            cubic |= std::holds_alternative<CubicTo>(command);
            linear |= std::holds_alternative<LineTo>(command);
        }
    }
    expect(quadratic && cubic && linear, "linear and quadratic/cubic paths remain editable");
    expect(validate(documentFor(imported.asset)).ok(), "SVG maps to a valid typed vector layer");
    for (bool compressed : {false, true}) {
        VectorExportOptions output;
        output.compressed = compressed;
        auto encoded = encodeSvg(imported.asset, output);
        expect(encoded.ok(), "encode SVG or SVGZ");
        auto roundTrip = decodeSvg(encoded.bytes, options);
        expect(roundTrip.ok(), "decode emitted SVG or SVGZ: " + roundTrip.result.message);
        if (roundTrip.ok()) {
            expect(renderFrame(documentFor(imported.asset), 0).pixels.pixels
                   == renderFrame(documentFor(roundTrip.asset), 0).pixels.pixels,
                   "native SVG/SVGZ render round-trip must preserve pixels");
        }
    }
    const auto vectorPath = directory.filePath("editable.svgz").toStdString();
    VectorExportOptions compressed;
    compressed.compressed = true;
    expect(exportSvg(imported.asset, vectorPath, compressed).ok()
           && importSvg(vectorPath, options).ok(), "SVGZ real-file round-trip");
    expect(exportSvg(imported.asset, vectorPath, compressed).code == MediaIoCode::AlreadyExists,
           "SVG exports are non-destructive by default");

    auto arc = decodeSvg(bytes(R"(<svg width="40" height="30"><path fill="none" stroke="red" stroke-linecap="round" stroke-linejoin="round" d="M5 15 A10 8 30 0 1 30 20"/></svg>)"));
    expect(arc.ok() && !arc.result.warnings.empty(), "elliptic arcs become editable cubic Beziers with a conversion notice");
    auto relative = decodeSvg(bytes(R"(<svg width="10" height="10"><path fill-rule="evenodd" d="m1 1 4 0 0 4-4 0z"/></svg>)"));
    expect(relative.ok() && relative.asset.paths[0].commands.size() == 5,
           "relative moves, implicit line repeats and adjacent signed numbers");
    auto aspect = decodeSvg(bytes(R"(<svg width="100" height="100" viewBox="10 20 20 10"><rect x="10" y="20" width="20" height="10"/></svg>)"));
    expect(aspect.ok(), "viewBox scaling and letterbox translation");
    if (aspect.ok()) {
        const auto image = renderFrame(documentFor(aspect.asset), 0).pixels;
        expect(image.pixels[0] == 0 && image.pixels[50 * 100 + 50] == 0xff000000U,
               "default SVG aspect ratio is centered meet");
    }
    auto transformOrder = decodeSvg(bytes(R"svg(<svg width="40" height="40"><g transform="translate(3 4) scale(2)"><path fill-rule="evenodd" fill="red" style="fill:blue" d="M1 1L4 1L4 4Z"/></g></svg>)svg"));
    expect(transformOrder.ok(), "nested transform and inline style import");
    if (transformOrder.ok()) {
        const auto p = std::get<MoveTo>(transformOrder.asset.paths[0].commands[0]).point;
        expect(p.x == 5 && p.y == 6 && transformOrder.asset.paths[0].fill->argb == 0xff0000ffU,
               "SVG transform list order and style precedence follow the source");
    }
    auto winding = decodeSvg(bytes(R"(<svg width="20" height="20"><path d="M1 1H19V19H1Z M5 5H15V15H5Z"/></svg>)"));
    auto evenOdd = decodeSvg(bytes(R"(<svg width="20" height="20"><path fill-rule="evenodd" d="M1 1H19V19H1Z M5 5H15V15H5Z"/></svg>)"));
    expect(winding.ok() && evenOdd.ok(), "import both winding rules");
    if (winding.ok() && evenOdd.ok()) {
        expect(renderFrame(documentFor(winding.asset), 0).pixels.pixels[10 * 20 + 10] == 0xff000000U
               && renderFrame(documentFor(evenOdd.asset), 0).pixels.pixels[10 * 20 + 10] == 0,
               "nonzero normalization must not punch an even-odd hole into solid geometry");
    }
    for (const std::string invalid : {
             "<svg width='10' height='10'><path d='M0 0 L1'/></svg>",
             "<svg width='10' height='10'><text>Hello</text></svg>",
             "<svg width='10' height='10'><path fill='url(#gradient)' d='M0 0L1 1'/></svg>",
             "<svg width='10' height='10'><g opacity='.5'><rect width='10' height='10'/></g></svg>",
             "<svg width='10' height='10'><use href='https://example.invalid/a.svg#x'/></svg>",
             "<!DOCTYPE svg [<!ENTITY x SYSTEM 'file:///etc/passwd'>]><svg width='1' height='1'>&x;</svg>",
             "<svg width='1' height='1'><rect width='1' height='1' onclick='bad()'/></svg>",
             "<svg width='10' height='10'><path d='Mnan 0L1 1'/></svg>"}) {
        auto rejected = decodeSvg(bytes(invalid));
        expect(!rejected.ok() && rejected.asset.paths.empty(), "reject unsupported/unsafe SVG without partial success: " + invalid);
    }
    options.limits.maxVectorCommands = 1;
    expect(decodeSvg(source, options).result.code == MediaIoCode::LimitExceeded,
           "bound vector command expansion");
    options = {};
    options.limits.maxInputBytes = 8;
    expect(decodeSvg(source, options).result.code == MediaIoCode::LimitExceeded,
           "bound SVG input bytes");
    options = {};
    options.limits.maxXmlDepth = 2;
    expect(decodeSvg(bytes("<svg width='1' height='1'><g><g/></g></svg>"), options).result.code == MediaIoCode::LimitExceeded,
           "bound SVG nesting even for empty groups");
    auto zipped = encodeSvg(imported.asset, compressed);
    if (zipped.ok()) {
        auto truncated = zipped.bytes;
        truncated.pop_back();
        expect(!decodeSvg(truncated).ok(), "reject truncated gzip footer");
        auto concatenated = zipped.bytes;
        concatenated.insert(concatenated.end(), zipped.bytes.begin(), zipped.bytes.end());
        expect(!decodeSvg(concatenated).ok(), "reject concatenated SVGZ documents");
        options = {};
        options.limits.maxDecodedBytes = 32;
        expect(decodeSvg(zipped.bytes, options).result.code == MediaIoCode::LimitExceeded,
               "bound decompressed SVGZ bytes");
    }

    const auto pdfPath = directory.filePath("vector.pdf").toStdString();
    Document document = documentFor(imported.asset);
    expect(exportPdf(document, pdfPath).ok(), "native vector PDF export");
    QFile pdf(QString::fromStdString(pdfPath));
    expect(pdf.open(QIODevice::ReadOnly) && pdf.peek(8).startsWith("%PDF-"), "real PDF signature");
    const auto pdfData = pdf.readAll();
    expect(!pdfData.contains("/Subtype /Image"), "pure vector PDF must not be a flattened bitmap");
    RasterizedVectorImportOptions rasterOptions;
    rasterOptions.outputExtent = {80, 60};
    auto invalidRasterOptions = rasterOptions;
    invalidRasterOptions.assetId = std::string("bad\0id", 6);
    expect(rasterizeVectorFile(pdfPath, invalidRasterOptions).result.code == MediaIoCode::InvalidArgument,
           "rasterized vector imports require a valid UTF-8 asset id without embedded NUL");
    invalidRasterOptions = rasterOptions;
    invalidRasterOptions.page = std::uint32_t(std::numeric_limits<int>::max()) + 1;
    invalidRasterOptions.limits.maxFrames = std::numeric_limits<std::uint32_t>::max();
    expect(rasterizeVectorFile(pdfPath, invalidRasterOptions).result.code == MediaIoCode::InvalidArgument,
           "vector page index must fit the image plugin API");
    auto raster = rasterizeVectorFile(pdfPath, rasterOptions);
    // Rasterization depends on the deployed Qt PDF plugin, not the native SVG contract.
    if (raster.result.code != MediaIoCode::UnsupportedFormat) {
        expect(raster.ok() && raster.asset.pixels.width == 80 && !raster.result.warnings.empty(),
               "PDF page raster import must be explicitly lossy: " + raster.result.message);
        expect(raster.asset.pixels.pixels[7 * 80 + 7] == 0xffff3311U,
               "independent PDF rasterizer sees the expected vector fill");
    }
    Document pages = document;
    pages.timeline.frameCount = 3;
    layerProperties(pages.layers[0]).frameRange = LayerFrameRange{1, 1};
    PdfExportOptions pdfOptions;
    pdfOptions.lastFrame = 2;
    const auto pagesPath = directory.filePath("pages.pdf").toStdString();
    expect(exportPdf(pages, pagesPath, pdfOptions).ok(), "inclusive frame range to multipage PDF");
    QImageReader pdfReader(QString::fromStdString(pagesPath));
    if (pdfReader.canRead()) { expect(pdfReader.imageCount() == 3, "PDF export emits the actual page count"); }
    layerProperties(pages.layers[0]).blendMode = RasterBlendMode::Multiply;
    const auto blendPath = directory.filePath("blend.pdf").toStdString();
    expect(exportPdf(pages, blendPath, pdfOptions).code == MediaIoCode::UnsupportedFeature
           && !QFile::exists(QString::fromStdString(blendPath)), "unsupported PDF blend must not publish partial earlier pages");
    pdfOptions.rasterizeUnsupportedBlending = true;
    expect(exportPdf(pages, blendPath, pdfOptions).ok(), "explicit PDF rasterization preserves unsupported blend appearance");
    pdfOptions = {};
    pdfOptions.limits.maxOutputBytes = 32;
    expect(exportPdf(document, directory.filePath("too-small.pdf").toStdString(), pdfOptions).code == MediaIoCode::LimitExceeded,
           "PDF encoder output is byte bounded");

    const auto gradientPath = directory.filePath("gradient.svg");
    QFile gradient(gradientPath);
    expect(gradient.open(QIODevice::WriteOnly), "create independent gradient SVG fixture");
    gradient.write("<svg xmlns='http://www.w3.org/2000/svg' width='80' height='60'><defs><linearGradient id='g'><stop stop-color='red'/><stop offset='1' stop-color='blue'/></linearGradient></defs><rect width='80' height='60' fill='url(#g)'/></svg>");
    gradient.close();
    expect(!importSvg(gradientPath.toStdString()).ok(), "gradient does not masquerade as editable solid paths");
    auto rasterized = rasterizeVectorFile(gradientPath.toStdString(), rasterOptions);
    if (rasterized.result.code != MediaIoCode::UnsupportedFormat) {
        expect(rasterized.ok(), "explicit raster fallback accepts gradient SVG: " + rasterized.result.message);
        if (rasterized.ok()) {
            const auto left = rasterized.asset.pixels.pixels[30 * 80 + 1];
            const auto right = rasterized.asset.pixels.pixels[30 * 80 + 78];
            expect(((left >> 16) & 255) > 240 && (right & 255) > 240,
                   "independent SVG rasterizer preserves the gradient endpoints");
        }
    }
    return failures == 0 ? 0 : 1;
}
