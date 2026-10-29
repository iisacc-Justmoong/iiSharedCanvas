#include <iiSharedCanvasNative.h>
#include <iiFileProviderNative.h>
#include <libxml/parser.h>
#include <zlib.h>

#include <cmath>
#include <filesystem>
#include <iostream>
#include <limits>
#include <memory>
#include <string>
#include <vector>

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
    const auto text = path.u8string(); return {text.begin(), text.end()};
}
std::string attribute(xmlNode *node, const char *name)
{
    auto *raw = xmlGetProp(node, reinterpret_cast<const xmlChar *>(name));
    if (!raw) return {};
    const std::string value(reinterpret_cast<const char *>(raw)); xmlFree(raw); return value;
}
VectorAsset fixture()
{
    VectorAsset asset; asset.id = "shape"; asset.viewport = {8, 6};
    VectorPath path;
    path.commands = {MoveTo{{1, 2}}, LineTo{{3, 4}}, QuadraticTo{{4.5, 5}, {6, 5.5}},
                     CubicTo{{6, 4}, {7, 3}, {7, 2}}, ClosePath{}};
    path.fill = SolidPaint{0x80112233U};
    path.stroke = StrokeStyle{SolidPaint{0xff445566U}, 1.25};
    asset.paths.push_back(std::move(path));
    return asset;
}
std::vector<std::uint8_t> gunzip(const std::vector<std::uint8_t> &input)
{
    z_stream stream{};
    if (inflateInit2(&stream, 15 + 16) != Z_OK) return {};
    stream.next_in = const_cast<Bytef *>(input.data()); stream.avail_in = uInt(input.size());
    std::vector<std::uint8_t> output(1024 * 1024);
    stream.next_out = output.data(); stream.avail_out = uInt(output.size());
    const auto status = inflate(&stream, Z_FINISH);
    const auto size = stream.total_out; const auto remaining = stream.avail_in;
    inflateEnd(&stream);
    if (status != Z_STREAM_END || remaining != 0) return {};
    output.resize(size); return output;
}
}

int main()
{
    const auto asset = fixture();
    const auto svg = encodeSvg(asset);
    check(svg.ok() && !svg.bytes.empty(), "native SVG export must produce XML");
    if (!svg.ok()) return 1;
    using DocumentPtr = std::unique_ptr<xmlDoc, decltype(&xmlFreeDoc)>;
    DocumentPtr parsed(xmlReadMemory(reinterpret_cast<const char *>(svg.bytes.data()), int(svg.bytes.size()),
                                     "fixture.svg", "UTF-8", XML_PARSE_NONET), xmlFreeDoc);
    check(bool(parsed), "independent XML parser accepts generated SVG");
    if (parsed) {
        auto *root = xmlDocGetRootElement(parsed.get());
        check(root && std::string(reinterpret_cast<const char *>(root->name)) == "svg"
              && root->ns && std::string(reinterpret_cast<const char *>(root->ns->href)) == "http://www.w3.org/2000/svg"
              && attribute(root, "width") == "8" && attribute(root, "height") == "6"
              && attribute(root, "viewBox") == "0 0 8 6", "SVG viewport and namespace survive");
        auto *path = root ? root->children : nullptr;
        while (path && path->type != XML_ELEMENT_NODE) path = path->next;
        check(path && std::string(reinterpret_cast<const char *>(path->name)) == "path"
              && attribute(path, "d") == "M 1 2 L 3 4 Q 4.5 5 6 5.5 C 6 4 7 3 7 2 Z"
              && attribute(path, "fill-rule") == "evenodd" && attribute(path, "fill") == "#112233"
              && attribute(path, "stroke") == "#445566" && attribute(path, "stroke-width") == "1.25"
              && attribute(path, "stroke-linecap") == "round" && attribute(path, "stroke-linejoin") == "round",
              "independent parser observes all native path commands and paint attributes");
        if (path) {
            const auto opacity = attribute(path, "fill-opacity");
            check(!opacity.empty() && std::abs(std::stod(opacity) - 128.0 / 255.0) < 1e-14,
                  "SVG retains translucent fill alpha");
        }
    }
    VectorExportOptions zipped; zipped.compressed = true;
    const auto svgz = encodeSvg(asset, zipped);
    check(svgz.ok() && svgz.bytes.size() > 2 && svgz.bytes[0] == 0x1f && svgz.bytes[1] == 0x8b
          && gunzip(svgz.bytes) == svg.bytes, "SVGZ is a real gzip stream containing the same XML");
    auto limited = VectorExportOptions{}; limited.limits.maxVectorCommands = 2;
    check(encodeSvg(asset, limited).result.code == MediaIoCode::LimitExceeded, "vector command budget is enforced");
    limited = {}; limited.limits.maxDecodedBytes = 8 * 6 * 4;
    check(encodeSvg(asset, limited).result.code == MediaIoCode::LimitExceeded,
          "vector validation is rejected before copying beyond its memory budget");
    limited = {}; limited.limits.maxOutputBytes = 10;
    check(encodeSvg(asset, limited).result.code == MediaIoCode::LimitExceeded, "output budget is enforced before publication");
    auto invalid = asset; invalid.paths[0].stroke->width = std::numeric_limits<double>::quiet_NaN();
    check(!encodeSvg(invalid).ok(), "non-finite vector data is rejected");

    fs::create_directories(IISHAREDCANVAS_TEST_OUTPUT_DIR);
    provider::TemporaryDirectory directory{fs::path(IISHAREDCANVAS_TEST_OUTPUT_DIR)};
    const auto target = directory.path() / u8"벡터.svg";
    check(exportSvg(asset, utf8(target)).ok(), "SVG file publishes to a Unicode path");
    const auto first = provider::File::read(target);
    check(first == std::string(svg.bytes.begin(), svg.bytes.end()), "published SVG matches encoded bytes");
    check(exportSvg(asset, utf8(target)).code == MediaIoCode::AlreadyExists
          && provider::File::read(target) == first, "SVG output never overwrites without permission");
    VectorExportOptions overwrite; overwrite.overwrite = true;
    provider::File::write(target, "old SVG");
    check(exportSvg(asset, utf8(target), overwrite).ok() && provider::File::read(target) == first,
          "explicit overwrite replaces an ordinary media file");
    const auto disguised = directory.path() / "canvas.svg";
    const std::string sqlite("SQLite format 3\0rest", 20);
    provider::File::create(disguised, sqlite);
    check(exportSvg(asset, utf8(disguised), overwrite).code == MediaIoCode::InvalidArgument
          && provider::File::read(disguised) == sqlite, "SVG overwrite refuses a renamed database");
    check(exportSvg(asset, utf8(directory.path() / "protect.IISC")).code == MediaIoCode::InvalidArgument,
          "SVG export refuses an IISC document path");
    return failures ? 1 : 0;
}
