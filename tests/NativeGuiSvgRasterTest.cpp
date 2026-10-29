#include "GuiSvgRasterizer.h"

#include <iiCompression/Gzip.h>

#include <cstdint>
#include <iostream>
#include <string>
#include <string_view>
#include <vector>

namespace {
int failures = 0;
void expect(bool value, std::string_view message)
{
    if (!value) { ++failures; std::cerr << message << '\n'; }
}
std::vector<std::uint8_t> bytes(std::string_view value)
{
    return {value.begin(), value.end()};
}
}

int main()
{
    using namespace iiSharedCanvas;
    example::GuiSvgRasterOptions options;
    options.outputExtent = {8, 4};
    options.assetId = "gradient";
    const auto svg = bytes(
        "<svg xmlns='http://www.w3.org/2000/svg' width='8' height='4' viewBox='0 0 8 4'>"
        "<defs><linearGradient id='g' x1='0%' y1='0%' x2='100%' y2='0%'>"
        "<stop offset='0%' stop-color='#ff0000'/><stop offset='100%' stop-color='#0000ff'/>"
        "</linearGradient></defs><rect width='8' height='4' fill='url(#g)'/></svg>");
    const auto raster = example::rasterizeGuiSvg(svg, options);
    expect(raster.ok(), "rasterize an SVG gradient with the native GUI SDK");
    if (raster.ok()) {
        expect(raster.asset.id == "gradient" && raster.asset.pixels.width == 8
               && raster.asset.pixels.height == 4 && raster.format == "svg"
               && !raster.result.warnings.empty(), "return the requested bitmap and an editability warning");
        const auto &pixels = raster.asset.pixels.pixels;
        const auto left = pixels[8], right = pixels[15];
        expect((left >> 24) == 255 && ((left >> 16) & 255) > (left & 255)
               && (right >> 24) == 255 && (right & 255) > ((right >> 16) & 255),
               "independent gradient fixture preserves color and opacity");
    }
    const auto alpha = example::rasterizeGuiSvg(bytes(
        "<svg width='8' height='4'><rect width='4' height='4' fill='#ff0000' fill-opacity='0.5'/></svg>"), options);
    expect(alpha.ok(), "rasterize translucent SVG paint");
    if (alpha.ok()) {
        const auto pixel = alpha.asset.pixels.pixels[9];
        const auto opacity = pixel >> 24;
        if ((opacity != 127 && opacity != 128) || (pixel & 0x00ffffffU) != 0x00ff0000U
            || alpha.asset.pixels.pixels[15] != 0)
            std::cerr << "translucent pixel=" << std::hex << alpha.asset.pixels.pixels[9]
                      << " transparent pixel=" << alpha.asset.pixels.pixels[15] << std::dec << '\n';
        expect((opacity == 127 || opacity == 128) && (pixel & 0x00ffffffU) == 0x00ff0000U
               && alpha.asset.pixels.pixels[15] == 0,
               "convert straight RGBA to straight ARGB, allowing one alpha quantization step");
    }
    const auto path = example::rasterizeGuiSvg(bytes(
        "<svg width='8' height='4'><path fill='#00ff00' d='M0 0 L4 0 L4 4 L0 4 Z'/></svg>"), options);
    expect(path.ok(), "rasterize an SVG path");
    if (path.ok())
        expect(path.asset.pixels.pixels[9] == 0xff00ff00U && path.asset.pixels.pixels[15] == 0,
               "rasterized path occupies its expected pixels");
    const auto relative = example::rasterizeGuiSvg(bytes(
        "<svg width='8' height='4'><g fill='#00ff00'><path d='m0 0 4 0 0 4-4 0z'/></g></svg>"), options);
    expect(relative.ok() && path.ok() && relative.asset.pixels.pixels == path.asset.pixels.pixels,
           "relative and implicit path commands preserve the same pixels");
    const auto evenOdd = example::rasterizeGuiSvg(bytes(
        "<svg width='8' height='4'><path fill-rule='evenodd' d='M0 0 H8 V4 H0 Z M2 1 H6 V3 H2 Z'/></svg>"), options);
    const auto nonzero = example::rasterizeGuiSvg(bytes(
        "<svg width='8' height='4'><path fill-rule='nonzero' d='M0 0 H8 V4 H0 Z M2 1 H6 V3 H2 Z'/></svg>"), options);
    expect(evenOdd.ok() && nonzero.ok() && evenOdd.asset.pixels.pixels[19] == 0
           && nonzero.asset.pixels.pixels[19] == 0xff000000U,
           "even-odd and nonzero fill rules preserve their distinct interiors");
    const auto stroke = example::rasterizeGuiSvg(bytes(
        "<svg width='8' height='4'><path fill='none' stroke='#00ff00' stroke-width='2' "
        "d='M1 2 L7 2'/></svg>"), options);
    expect(stroke.ok() && stroke.asset.pixels.pixels[12] == 0xff00ff00U
           && stroke.asset.pixels.pixels[0] == 0, "SVG stroke is rasterized at the expected coordinates");
    const auto smooth = example::rasterizeGuiSvg(bytes(
        "<svg width='8' height='4'><path fill='none' stroke='#0f0' "
        "d='M0 2 Q2 0 4 2 T8 2 M0 2 C2 0 3 0 4 2 S6 4 8 2'/></svg>"), options);
    const auto explicitCurves = example::rasterizeGuiSvg(bytes(
        "<svg width='8' height='4'><path fill='none' stroke='#00ff00' "
        "d='M0 2 Q2 0 4 2 Q6 4 8 2 M0 2 C2 0 3 0 4 2 C5 4 6 4 8 2'/></svg>"), options);
    expect(smooth.ok() && explicitCurves.ok()
           && smooth.asset.pixels.pixels == explicitCurves.asset.pixels.pixels,
           "smooth quadratic and cubic commands match explicit control points");
    const auto compressed = ii::compression::gzip_compress(svg);
    expect(compressed.has_value(), "create an SVGZ fixture");
    if (compressed) {
        const auto restored = example::rasterizeGuiSvg(*compressed, options);
        expect(restored.ok() && restored.format == "svgz" && raster.ok()
               && restored.asset.pixels.pixels == raster.asset.pixels.pixels,
               "SVGZ and SVG rasterization have identical output");
        auto truncated = *compressed;
        truncated.pop_back();
        expect(!example::rasterizeGuiSvg(truncated, options).ok(), "reject truncated SVGZ");
        auto concatenated = *compressed;
        concatenated.insert(concatenated.end(), compressed->begin(), compressed->end());
        expect(!example::rasterizeGuiSvg(concatenated, options).ok(), "reject concatenated SVGZ members");
        auto limited = options;
        limited.limits.maxDecodedBytes = 16;
        expect(example::rasterizeGuiSvg(*compressed, limited).result.code == MediaIoCode::LimitExceeded,
               "bound decompressed SVGZ and raster bytes");
    }
    std::string longPath = "<svg width='8' height='4'><path d='M0 0";
    for (unsigned i = 0; i < 300; ++i) longPath += " L1 1";
    longPath += "'/></svg>";
    const auto compact = ii::compression::gzip_compress(bytes(longPath));
    if (compact) {
        auto bounded = options;
        bounded.limits.maxInputBytes = compact->size();
        bounded.limits.maxDecodedBytes = longPath.size();
        expect(compact->size() < longPath.size()
               && example::rasterizeGuiSvg(*compact, bounded).ok(),
               "compressed input and expanded XML use separate byte limits");
    } else expect(false, "create a compact SVGZ input");
    const auto tooLargeXml = ii::compression::gzip_compress(bytes(std::string(1024 * 1024 + 1, ' ')));
    expect(tooLargeXml && example::rasterizeGuiSvg(*tooLargeXml, options).result.code == MediaIoCode::LimitExceeded,
           "stop SVGZ expansion at the GUI codec input limit");
    for (const auto input : {
        "<svg width='1' height='1'><script/></svg>",
        "<svg width='1' height='1'><filter id='f'/><rect filter='url(#f)'/></svg>",
        "<svg width='1' height='1'><unknown/></svg>",
        "<svg width='1' height='1'><rect style='fill:red' width='1' height='1'/></svg>",
        "<svg width='1' height='1'><g transform='translate(1)'><rect width='1' height='1'/></g></svg>",
        "<svg width='1' height='1'><rect onclick='bad()' width='1' height='1'/></svg>",
        "<svg width='1' height='1'><path d='M0 0 L1'/></svg>",
        "<svg width='1' height='1'><path d='M0 0 A1 1 0 0 0 1 1'/></svg>",
        "<svg width='1' height='1'><rect fill-opacity='NaN' width='1' height='1'/></svg>",
        "<svg width='1' height='1'><rect fill='url(https://example.invalid/a)' width='1' height='1'/></svg>",
        "<svg width='1' height='1'><rect fill='url(#missing)' width='1' height='1'/></svg>",
        "<!DOCTYPE svg [<!ENTITY e SYSTEM 'file:///etc/passwd'>]><svg width='1' height='1'>&e;</svg>",
        "<svg width='1' height='1'><rect width='1' height='1'></svg>"}) {
        const auto rejected = example::rasterizeGuiSvg(bytes(input), options);
        expect(!rejected.ok() && rejected.asset.pixels.pixels.empty(),
               "reject malformed, unsupported or external SVG without partial pixels");
    }
    auto limited = options;
    limited.limits.maxInputBytes = svg.size() - 1;
    expect(example::rasterizeGuiSvg(svg, limited).result.code == MediaIoCode::LimitExceeded,
           "bound SVG input bytes");
    limited = options;
    limited.limits.maxPixelsPerFrame = 31;
    expect(example::rasterizeGuiSvg(svg, limited).result.code == MediaIoCode::LimitExceeded,
           "bound output pixels");
    limited = options;
    limited.limits.maxDecodedBytes = 127;
    expect(example::rasterizeGuiSvg(svg, limited).result.code == MediaIoCode::LimitExceeded,
           "bound logical raster storage");
    limited = options;
    limited.limits.maxXmlDepth = 1;
    expect(example::rasterizeGuiSvg(svg, limited).result.code == MediaIoCode::LimitExceeded,
           "bound SVG nesting");
    limited = options;
    limited.limits.maxVectorCommands = 4;
    expect(example::rasterizeGuiSvg(bytes(
        "<svg width='8' height='4'><path d='M0 0 L4 0 L4 4 L0 4 Z'/></svg>"), limited).result.code
            == MediaIoCode::LimitExceeded, "bound path commands");
    limited = options;
    limited.assetId = std::string("bad\0id", 6);
    expect(example::rasterizeGuiSvg(svg, limited).result.code == MediaIoCode::InvalidArgument,
           "reject invalid raster asset IDs");
    limited = options;
    limited.outputExtent = {0, 4};
    expect(example::rasterizeGuiSvg(svg, limited).result.code == MediaIoCode::InvalidArgument,
           "require positive output dimensions");
    return failures ? 1 : 0;
}
