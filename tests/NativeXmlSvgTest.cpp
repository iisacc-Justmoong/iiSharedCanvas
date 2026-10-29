#include "XmlSvgDecoder.h"

#include <iiSharedCanvas.h>
#include <iiCompression/Gzip.h>

#include <algorithm>
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
iiSharedCanvas::Document documentFor(const iiSharedCanvas::VectorAsset &asset)
{
    using namespace iiSharedCanvas;
    Document document;
    document.extent = asset.viewport;
    document.assets.emplace_back(asset);
    document.layers.emplace_back(StaticVectorLayer{{"layer", "Vector"}, StaticSource{asset.id}});
    return document;
}
}

int main()
{
    using namespace iiSharedCanvas;
    VectorAsset source{"source", {32, 24}, {}};
    VectorPath filled;
    filled.fill = SolidPaint{0x804080c0};
    filled.commands = {MoveTo{{2.25, 3}}, LineTo{{15, 3}}, QuadraticTo{{20, 4}, {18, 12}},
                       CubicTo{{17, 18}, {6, 20}, {2.25, 3}}, ClosePath{}};
    VectorPath stroked;
    stroked.stroke = StrokeStyle{SolidPaint{0xffcc3300}, 2.5};
    stroked.commands = {MoveTo{{4, 20}}, LineTo{{28, 19}}};
    source.paths = {filled, stroked};

    const auto encoded = encodeSvg(source);
    expect(encoded.ok(), "encode the native editable SVG fixture");
    if (!encoded.ok()) return 1;
    const auto decoded = example::decodeCanvasSvg(encoded.bytes, "imported");
    expect(decoded.ok(), "decode SVG emitted by the native exporter");
    if (decoded.ok()) {
        expect(decoded.asset.id == "imported" && decoded.asset.viewport.width == 32
               && decoded.asset.viewport.height == 24 && decoded.asset.paths.size() == 2,
               "preserve the caller ID, viewport and editable paths");
        if (decoded.asset.paths.size() == 2) {
            const auto &commands = decoded.asset.paths[0].commands;
            expect(commands.size() == filled.commands.size()
                   && decoded.asset.paths[1].commands.size() == stroked.commands.size(),
                   "preserve the editable command sequence");
            expect(commands.size() >= 4 && std::holds_alternative<QuadraticTo>(commands[2])
                   && std::holds_alternative<CubicTo>(commands[3]),
                   "quadratic and cubic commands remain editable");
        }
        const auto original = renderFrame(documentFor(source), 0);
        const auto roundTrip = renderFrame(documentFor(decoded.asset), 0);
        expect(original.ok() && roundTrip.ok()
               && std::any_of(original.pixels.pixels.begin(), original.pixels.pixels.end(),
                              [](std::uint32_t pixel) { return (pixel >> 24) != 0; })
               && original.pixels.pixels == roundTrip.pixels.pixels,
               "native SVG round trip preserves actual rendered pixels and alpha");
    }
    const auto independent = example::decodeCanvasSvg(bytes(
        "<svg xmlns='http://www.w3.org/2000/svg' width='8' height='6' viewBox='0 0 8 6'>"
        "<path d='M 1 2 L 3 4 Q 4.5 5 6 5.5 C 6 4 7 3 7 2 Z' fill-rule='evenodd' "
        "fill='#112233' fill-opacity='0.5019607843137255' stroke='#445566' "
        "stroke-width='1.25' stroke-linecap='round' stroke-linejoin='round'/></svg>"));
    expect(independent.ok() && independent.asset.paths.size() == 1,
           "decode an independently specified canonical SVG");
    if (independent.ok() && independent.asset.paths.size() == 1) {
        const auto &p = independent.asset.paths[0];
        expect(p.fill && p.fill->argb == 0x80112233U && p.stroke
               && p.stroke->paint.argb == 0xff445566U && p.stroke->width == 1.25,
               "independent SVG paint values decode exactly");
        if (p.commands.size() == 5 && std::holds_alternative<MoveTo>(p.commands[0])
            && std::holds_alternative<QuadraticTo>(p.commands[2])
            && std::holds_alternative<CubicTo>(p.commands[3])) {
            expect(std::get<MoveTo>(p.commands[0]).point.x == 1
                   && std::get<MoveTo>(p.commands[0]).point.y == 2
                   && std::get<QuadraticTo>(p.commands[2]).control.x == 4.5
                   && std::get<QuadraticTo>(p.commands[2]).end.y == 5.5
                   && std::get<CubicTo>(p.commands[3]).control2.x == 7,
                   "independent SVG coordinates decode exactly");
        } else expect(false, "independent SVG commands retain their types");
    }

    for (const std::string input : {
        "<svg width='2' height='2' viewBox='0 0 2 2'><rect width='2' height='2'/></svg>",
        "<svg width='2' height='2' viewBox='0 0 2 2'><path fill-rule='evenodd' fill='red' stroke='none' d='M0 0 L1 1'/></svg>",
        "<svg width='2' height='2' viewBox='0 0 2 2'><path fill-rule='evenodd' fill='#000000' stroke='none' d='M0 0 L1'/></svg>",
        "<svg width='2' height='2' viewBox='0 0 2 2'><path transform='scale(2)' fill-rule='evenodd' fill='#000000' stroke='none' d='M0 0'/></svg>",
        "<svg width='2' height='2' viewBox='0 0 2 2'><path fill-rule='nonzero' fill='#000000' stroke='none' d='M0 0'/></svg>",
        "<svg width='2' height='2' viewBox='0 0 2 2'><path fill-rule='evenodd' fill='#000000' fill-opacity='0.5' stroke='none' d='M0 0'/></svg>",
        "<svg width='2' height='2' viewBox='0 0 2 2'><g><path fill-rule='evenodd' fill='#000000' stroke='none' d='M0 0'/></g></svg>",
        "<!DOCTYPE svg [<!ENTITY x SYSTEM 'file:///etc/passwd'>]><svg width='1' height='1'>&x;</svg>",
        "<svg width='2' height='2' viewBox='0 0 2 2'><path onclick='x()' fill-rule='evenodd' fill='#000000' stroke='none' d='M0 0'/></svg>"}) {
        const auto rejected = example::decodeCanvasSvg(bytes(input));
        expect(!rejected.ok() && rejected.asset.paths.empty(), "unsupported or unsafe SVG returns no partial asset");
    }
    MediaLimits limits;
    limits.maxInputBytes = 4;
    expect(example::decodeCanvasSvg(encoded.bytes, "small", limits).result.code == MediaIoCode::LimitExceeded,
           "bound SVG input bytes");
    limits = {};
    limits.maxVectorCommands = 1;
    expect(example::decodeCanvasSvg(encoded.bytes, "small", limits).result.code == MediaIoCode::LimitExceeded,
           "bound decoded vector commands");
    expect(example::decodeCanvasSvg(encoded.bytes, std::string("bad\0id", 6)).result.code == MediaIoCode::InvalidArgument,
           "reject an asset ID with an embedded NUL");
    expect(example::decodeCanvasSvg(encoded.bytes, std::string("\xff", 1)).result.code == MediaIoCode::InvalidArgument,
           "reject a non-UTF-8 asset ID");
    limits = {};
    limits.maxDecodedBytes = 1;
    expect(example::decodeCanvasSvg(encoded.bytes, "small", limits).result.code == MediaIoCode::LimitExceeded,
           "bound logical decoded vector storage");
    limits = {};
    limits.maxPixelsPerFrame = 767;
    expect(example::decodeCanvasSvg(encoded.bytes, "small", limits).result.code == MediaIoCode::LimitExceeded,
           "bound the vector viewport");
    limits = {};
    limits.maxXmlDepth = 1;
    expect(example::decodeCanvasSvg(encoded.bytes, "small", limits).result.code == MediaIoCode::LimitExceeded,
           "bound XML nesting");
    auto gzip = encodeSvg(source, VectorExportOptions{.compressed = true});
    expect(gzip.ok(), "encode a real SVGZ fixture");
    if (gzip.ok()) {
        const auto restored = example::decodeCanvasSvg(gzip.bytes, "svgz");
        expect(restored.ok() && restored.asset.paths.size() == source.paths.size(),
               "decode native SVGZ into editable paths");
        if (restored.ok()) {
            const auto image = renderFrame(documentFor(restored.asset), 0);
            const auto original = renderFrame(documentFor(source), 0);
            expect(image.ok() && original.ok() && image.pixels.pixels == original.pixels.pixels,
                   "SVGZ round trip preserves rendered pixels");
        }
        auto truncated = gzip.bytes;
        truncated.pop_back();
        const auto invalid = example::decodeCanvasSvg(truncated);
        expect(!invalid.ok() && invalid.asset.paths.empty(), "reject truncated SVGZ without partial output");
        auto concatenated = gzip.bytes;
        concatenated.insert(concatenated.end(), gzip.bytes.begin(), gzip.bytes.end());
        const auto multiple = example::decodeCanvasSvg(concatenated);
        expect(!multiple.ok() && multiple.asset.paths.empty(), "reject concatenated SVGZ without partial output");
        MediaLimits compressedLimit;
        compressedLimit.maxInputBytes = gzip.bytes.size() - 1;
        expect(example::decodeCanvasSvg(gzip.bytes, "svgz", compressedLimit).result.code == MediaIoCode::LimitExceeded,
               "bound compressed SVGZ input");
        compressedLimit = {};
        compressedLimit.maxDecodedBytes = 16;
        expect(example::decodeCanvasSvg(gzip.bytes, "svgz", compressedLimit).result.code == MediaIoCode::LimitExceeded,
               "bound expanded SVGZ bytes");
    }
    const auto emptyGzip = ii::compression::gzip_compress(std::span<const std::uint8_t>{});
    expect(emptyGzip && !example::decodeCanvasSvg(*emptyGzip).ok(), "reject a gzip member containing no XML");
    VectorAsset alphaAsset{"alpha", {2, 2}, {}};
    VectorPath alphaPath;
    alphaPath.commands = {MoveTo{{0, 0}}, LineTo{{1, 1}}};
    alphaPath.fill = SolidPaint{};
    alphaAsset.paths.push_back(alphaPath);
    for (std::uint32_t alpha = 0; alpha <= 255; ++alpha) {
        alphaAsset.paths[0].fill = SolidPaint{(alpha << 24) | 0x123456};
        const auto svg = encodeSvg(alphaAsset);
        const auto restored = svg.ok() ? example::decodeCanvasSvg(svg.bytes) : example::CanvasSvgDecodeResult{};
        expect(svg.ok() && restored.ok() && restored.asset.paths.size() == 1
               && restored.asset.paths[0].fill && restored.asset.paths[0].fill->argb == ((alpha << 24) | 0x123456),
               "all native alpha values survive the SVG round trip");
    }
    return failures ? 1 : 0;
}
