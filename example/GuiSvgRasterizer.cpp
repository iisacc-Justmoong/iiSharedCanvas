#include "GuiSvgRasterizer.h"

#include <iiCompression/Gzip.h>
#include <iiGUI/Image.h>
#include <iiSDKCore/Core/Text/Utf8.h>
#include <iiXml/Xml.h>

#include <algorithm>
#include <array>
#include <charconv>
#include <cmath>
#include <initializer_list>
#include <numbers>
#include <limits>
#include <new>
#include <stdexcept>
#include <string_view>
#include <unordered_set>
#include <utility>
#include <vector>

namespace iiSharedCanvas::example {
namespace {
constexpr std::string_view svgNamespace = "http://www.w3.org/2000/svg";
constexpr std::size_t guiInputLimit = 1024 * 1024;
constexpr std::uint64_t guiPixelLimit = 64ULL * 1024 * 1024;

struct Failure { MediaIoCode code; std::string message; };
[[noreturn]] void fail(MediaIoCode code, std::string message) { throw Failure{code, std::move(message)}; }
bool whitespace(char c) { return c == ' ' || c == '\t' || c == '\n' || c == '\r'; }
bool letter(char c) { return (c >= 'A' && c <= 'Z') || (c >= 'a' && c <= 'z'); }
bool digit(char c) { return c >= '0' && c <= '9'; }
std::string_view trim(std::string_view value)
{
    while (!value.empty() && whitespace(value.front())) value.remove_prefix(1);
    while (!value.empty() && whitespace(value.back())) value.remove_suffix(1);
    return value;
}
bool oneOf(std::string_view value, std::initializer_list<std::string_view> choices)
{
    return std::find(choices.begin(), choices.end(), value) != choices.end();
}
std::size_t bounded(std::uint64_t value)
{
    return static_cast<std::size_t>(std::min<std::uint64_t>(value, std::numeric_limits<std::size_t>::max()));
}

// Restrict coordinates before they enter the rasterizer, and keep SVG's
// optional comma and adjacent signed-number rules distinct from whitespace.
class Numbers {
public:
    explicit Numbers(std::string_view text) : text_(text) {}
    [[nodiscard]] bool done() const { return next() == text_.size(); }
    [[nodiscard]] char peek() const { const auto i = next(); return i == text_.size() ? '\0' : text_[i]; }
    char command() { position_ = next(); return text_[position_++]; }
    double number(bool following)
    {
        const auto before = position_;
        position_ = next();
        const bool spaced = position_ != before;
        if (position_ < text_.size() && text_[position_] == ',') {
            if (!following) fail(MediaIoCode::InvalidData, "unexpected comma in SVG numbers");
            ++position_;
            position_ = next();
            if (position_ == text_.size() || text_[position_] == ',')
                fail(MediaIoCode::InvalidData, "incomplete SVG number list");
        } else if (following && !spaced && position_ < text_.size()
                   && text_[position_] != '+' && text_[position_] != '-' && text_[position_] != '.') {
            fail(MediaIoCode::InvalidData, "missing SVG number separator");
        }
        if (position_ == text_.size()) fail(MediaIoCode::InvalidData, "missing SVG number");
        if (text_[position_] == '+') {
            ++position_;
            if (position_ == text_.size() || text_[position_] == '+' || text_[position_] == '-')
                fail(MediaIoCode::InvalidData, "invalid SVG number");
        }
        double value = 0;
        const auto *first = text_.data() + position_;
        const auto *last = text_.data() + text_.size();
        const auto [end, error] = std::from_chars(first, last, value, std::chars_format::general);
        if (error != std::errc{} || end == first || !std::isfinite(value) || std::abs(value) > 1e9)
            fail(MediaIoCode::InvalidData, "invalid or out-of-range SVG number");
        position_ = static_cast<std::size_t>(end - text_.data());
        return value;
    }
    // Arc flags are single digits and SVG permits adjacent flags with no
    // separator. A comma still requires a preceding numeric value.
    bool flag(bool following)
    {
        position_ = next();
        if (position_ < text_.size() && text_[position_] == ',') {
            if (!following) fail(MediaIoCode::InvalidData, "unexpected SVG flag separator");
            ++position_; position_ = next();
        }
        if (position_ == text_.size() || (text_[position_] != '0' && text_[position_] != '1'))
            fail(MediaIoCode::InvalidData, "invalid SVG arc flag");
        return text_[position_++] == '1';
    }
private:
    [[nodiscard]] std::size_t next() const
    {
        auto i = position_;
        while (i < text_.size() && whitespace(text_[i])) ++i;
        return i;
    }
    std::string_view text_;
    std::size_t position_ = 0;
};

double scalar(std::string_view text, bool percent = false, bool pixels = false)
{
    text = trim(text);
    double multiplier = 1;
    if (percent && text.ends_with('%')) { text.remove_suffix(1); multiplier = 0.01; }
    else if (pixels && text.ends_with("px")) text.remove_suffix(2);
    Numbers numbers(text);
    const auto value = numbers.number(false) * multiplier;
    if (!numbers.done()) fail(MediaIoCode::InvalidData, "unexpected SVG number suffix");
    return value;
}
void box(std::string_view text)
{
    Numbers numbers(text);
    (void)numbers.number(false);
    (void)numbers.number(true);
    const auto width = numbers.number(true), height = numbers.number(true);
    if (!numbers.done() || width <= 0 || height <= 0)
        fail(MediaIoCode::InvalidData, "invalid SVG viewBox");
}
void count(std::uint64_t &commands, const MediaLimits &limits)
{
    if (commands >= limits.maxVectorCommands)
        fail(MediaIoCode::LimitExceeded, "SVG geometry exceeds its command limit");
    ++commands;
}
void pathData(std::string_view text, std::uint64_t &commands, const MediaLimits &limits, bool extended)
{
    Numbers numbers(text);
    char op = '\0';
    bool started = false, following = false;
    while (!numbers.done()) {
        if (letter(numbers.peek())) {
            op = numbers.command();
            following = false;
            if (!started && op != 'M' && op != 'm')
                fail(MediaIoCode::InvalidData, "SVG path must start with a move");
            if (op == 'Z' || op == 'z') {
                count(commands, limits);
                op = '\0';
                continue;
            }
        } else if (!op) fail(MediaIoCode::InvalidData, "missing SVG path command");
        if ((op == 'A' || op == 'a') && extended) {
            const auto rx = numbers.number(following), ry = numbers.number(true);
            (void)numbers.number(true); (void)numbers.flag(true); (void)numbers.flag(true);
            (void)numbers.number(true); (void)numbers.number(true);
            if (rx < 0 || ry < 0) fail(MediaIoCode::InvalidData, "negative SVG arc radius");
            // The rasterizer may expand an arc into four cubic segments.
            for (int i = 0; i < 4; ++i) count(commands, limits);
            following = true;
            continue;
        }
        unsigned parameters = 0;
        switch (op) {
        case 'M': case 'm': case 'L': case 'l': case 'T': case 't': parameters = 2; break;
        case 'H': case 'h': case 'V': case 'v': parameters = 1; break;
        case 'Q': case 'q': case 'S': case 's': parameters = 4; break;
        case 'C': case 'c': parameters = 6; break;
        default: fail(MediaIoCode::UnsupportedFeature, "unsupported SVG path command");
        }
        for (unsigned i = 0; i < parameters; ++i) {
            (void)numbers.number(following);
            following = true;
        }
        count(commands, limits);
        if (op == 'M' || op == 'm') { started = true; op = op == 'M' ? 'L' : 'l'; }
    }
    if (!started) fail(MediaIoCode::InvalidData, "empty SVG path");
}

using SvgMatrix = std::array<double, 6>;
// A composed matrix is bounded both within one transform list and across XML
// ancestors, preventing nested groups from evading the renderer's limits.
SvgMatrix composeMatrix(const SvgMatrix &a, const SvgMatrix &b)
{
    SvgMatrix value{a[0]*b[0]+a[2]*b[1], a[1]*b[0]+a[3]*b[1],
        a[0]*b[2]+a[2]*b[3], a[1]*b[2]+a[3]*b[3],
        a[0]*b[4]+a[2]*b[5]+a[4], a[1]*b[4]+a[3]*b[5]+a[5]};
    if (std::any_of(value.begin(), value.end(), [](double v) { return !std::isfinite(v) || std::abs(v) > 1e9; }))
        fail(MediaIoCode::LimitExceeded, "SVG transform exceeds coordinate limits");
    return value;
}
// Validate transform syntax and accumulated affine coefficients before the
// renderer sees them. Composition is bounded to limit pathological matrices.
SvgMatrix transformList(std::string_view text, std::uint64_t &commands, const MediaLimits &limits)
{
    std::array<double, 6> total{1, 0, 0, 1, 0, 0};
    std::size_t position = 0, transforms = 0;
    while (position < text.size()) {
        while (position < text.size() && whitespace(text[position])) ++position;
        if (position == text.size()) break;
        if (text[position] == ',') {
            if (!transforms) fail(MediaIoCode::InvalidData, "unexpected SVG transform separator");
            ++position;
            while (position < text.size() && whitespace(text[position])) ++position;
            if (position == text.size() || text[position] == ',')
                fail(MediaIoCode::InvalidData, "incomplete SVG transform list");
        }
        const auto begin = position;
        while (position < text.size() && letter(text[position])) ++position;
        const auto name = text.substr(begin, position - begin);
        while (position < text.size() && whitespace(text[position])) ++position;
        if (name.empty() || position == text.size() || text[position++] != '(')
            fail(MediaIoCode::InvalidData, "invalid SVG transform");
        const auto end = text.find(')', position);
        if (end == text.npos) fail(MediaIoCode::InvalidData, "unterminated SVG transform");
        Numbers numbers(text.substr(position, end - position));
        std::vector<double> args;
        while (!numbers.done()) {
            if (args.size() == 6) fail(MediaIoCode::InvalidData, "too many SVG transform arguments");
            args.push_back(numbers.number(!args.empty()));
        }
        std::array<double, 6> local{1, 0, 0, 1, 0, 0};
        if (name == "matrix" && args.size() == 6) std::copy(args.begin(), args.end(), local.begin());
        else if (name == "translate" && (args.size() == 1 || args.size() == 2)) {
            local[4] = args[0]; local[5] = args.size() == 2 ? args[1] : 0;
        } else if (name == "scale" && (args.size() == 1 || args.size() == 2)) {
            local[0] = args[0]; local[3] = args.size() == 2 ? args[1] : args[0];
        } else if (name == "rotate" && (args.size() == 1 || args.size() == 3)) {
            const auto angle = args[0] * std::numbers::pi / 180;
            const auto c = std::cos(angle), s = std::sin(angle);
            local = {c, s, -s, c, 0, 0};
            if (args.size() == 3) {
                local[4] = args[1] - c * args[1] + s * args[2];
                local[5] = args[2] - s * args[1] - c * args[2];
            }
        } else if ((name == "skewX" || name == "skewY") && args.size() == 1) {
            local[name == "skewX" ? 2 : 1] = std::tan(args[0] * std::numbers::pi / 180);
        } else fail(MediaIoCode::UnsupportedFeature, "unsupported SVG transform or argument count");
        if (++transforms > 32) fail(MediaIoCode::LimitExceeded, "SVG transform list is too long");
        count(commands, limits); total = composeMatrix(total, local); position = end + 1;
    }
    if (!transforms) fail(MediaIoCode::InvalidData, "empty SVG transform");
    return total;
}

// Polygon and polyline points are complete coordinate pairs. Each pair uses
// the same geometry budget as a path command; malformed or excessive input is
// rejected before it can reach the rasterizer.
void pointList(std::string_view text, std::uint64_t &commands, const MediaLimits &limits)
{
    Numbers numbers(text);
    bool following = false;
    while (!numbers.done()) {
        (void)numbers.number(following);
        (void)numbers.number(true);
        following = true;
        count(commands, limits);
    }
}
bool validId(std::string_view id)
{
    if (id.empty() || !(letter(id.front()) || id.front() == '_')) return false;
    return std::all_of(id.begin() + 1, id.end(), [](char c) {
        return letter(c) || digit(c) || c == '_' || c == '-' || c == '.';
    });
}
void color(std::string_view value)
{
    value = trim(value);
    if (oneOf(value, {"black", "white", "red", "green", "blue", "yellow", "gray", "grey"})) return;
    if ((value.size() == 4 || value.size() == 7) && value.front() == '#'
        && std::all_of(value.begin() + 1, value.end(), [](char c) {
            return digit(c) || (c >= 'a' && c <= 'f') || (c >= 'A' && c <= 'F');
        })) return;
    fail(MediaIoCode::UnsupportedFeature, "unsupported SVG color");
}
void paint(std::string_view value, std::vector<std::string> &references)
{
    value = trim(value);
    if (value == "none") return;
    if (value.starts_with("url(#") && value.ends_with(')')) {
        auto id = value.substr(5, value.size() - 6);
        if (!validId(id)) fail(MediaIoCode::UnsupportedFeature, "invalid local SVG paint reference");
        references.emplace_back(id);
        return;
    }
    color(value);
}

// Accept only the same bounded solid/local-gradient presentation properties
// supported as SVG attributes. Other CSS remains outside this raster contract.
bool paintProperty(std::string_view key, std::string_view value, std::vector<std::string> &references)
{
    if (oneOf(key, {"fill", "stroke"})) paint(value, references);
    else if (oneOf(key, {"fill-opacity", "stroke-opacity"})) {
        const auto v = scalar(value);
        if (v < 0 || v > 1) fail(MediaIoCode::InvalidData, "SVG opacity is outside [0,1]");
    } else if (key == "fill-rule") {
        if (!oneOf(trim(value), {"evenodd", "nonzero"}))
            fail(MediaIoCode::UnsupportedFeature, "unsupported SVG fill rule");
    } else if (key == "stroke-width") {
        if (scalar(value, false, true) < 0) fail(MediaIoCode::InvalidData, "negative SVG stroke width");
    } else if (key == "stroke-linecap") {
        if (!oneOf(trim(value), {"butt", "round", "square"}))
            fail(MediaIoCode::UnsupportedFeature, "unsupported SVG stroke cap");
    } else if (key == "stroke-linejoin") {
        if (!oneOf(trim(value), {"miter", "round", "bevel"}))
            fail(MediaIoCode::UnsupportedFeature, "unsupported SVG stroke join");
    } else if (key == "stroke-miterlimit") {
        if (scalar(value) < 1) fail(MediaIoCode::InvalidData, "invalid SVG miter limit");
    } else return false;
    return true;
}

// A small declaration limit bounds parsing independently of XML attributes.
// Each declaration is validated before the original SVG reaches iiGUI.
void inlineStyle(std::string_view text, std::vector<std::string> &references)
{
    std::size_t position = 0, count = 0;
    while (position < text.size()) {
        const auto end = text.find(';', position);
        const auto declaration = trim(text.substr(position, end == text.npos ? end : end - position));
        if (!declaration.empty()) {
            if (++count > 64) fail(MediaIoCode::LimitExceeded, "too many SVG style declarations");
            const auto colon = declaration.find(':');
            if (colon == declaration.npos || trim(declaration.substr(0, colon)).empty()
                || !paintProperty(trim(declaration.substr(0, colon)), trim(declaration.substr(colon + 1)), references))
                fail(MediaIoCode::UnsupportedFeature, "unsupported SVG inline style");
        }
        if (end == text.npos) break;
        position = end + 1;
    }
}

struct Element {
    std::string name;
    std::size_t stops = 0;
    SvgMatrix matrix{1, 0, 0, 1, 0, 0};
};
void inspectSvg(const std::string &source, const MediaLimits &limits, bool compressed, bool extended)
{
    ii::xml::Limits xmlLimits;
    xmlLimits.input_bytes = guiInputLimit;
    xmlLimits.text_bytes = bounded(compressed ? limits.maxDecodedBytes
                                             : std::min(limits.maxInputBytes, limits.maxDecodedBytes));
    xmlLimits.depth = limits.maxXmlDepth;
    xmlLimits.attributes = 16;
    xmlLimits.events = 100000;
    auto reader = ii::xml::Reader::open(source, xmlLimits);
    if (!reader) fail(reader.error().code == ii::xml::ErrorCode::limit_exceeded ? MediaIoCode::LimitExceeded
                                                                                : MediaIoCode::InvalidData,
                      reader.error().message);
    std::vector<Element> stack;
    std::unordered_set<std::string> gradients;
    std::vector<std::string> references;
    std::uint64_t commands = 0;
    bool sawRoot = false, closedRoot = false;
    std::string rootNamespace;
    for (;;) {
        auto next = reader->next();
        if (!next) fail(next.error().code == ii::xml::ErrorCode::limit_exceeded ? MediaIoCode::LimitExceeded
                                                                               : MediaIoCode::InvalidData,
                        next.error().message);
        if (!*next) break;
        const auto &event = **next;
        if (event.kind == ii::xml::Kind::comment) continue;
        if (event.kind == ii::xml::Kind::text) {
            if (!std::all_of(event.value.begin(), event.value.end(), whitespace))
                fail(MediaIoCode::UnsupportedFeature, "SVG text content is not supported");
            continue;
        }
        if (event.kind == ii::xml::Kind::processing_instruction || event.kind == ii::xml::Kind::doctype)
            fail(MediaIoCode::UnsupportedFeature, "SVG declarations and processing instructions are not supported");
        if (event.kind == ii::xml::Kind::end_element) {
            if (stack.empty() || event.local_name != stack.back().name || event.namespace_uri != rootNamespace)
                fail(MediaIoCode::InvalidData, "unexpected SVG closing element");
            if (stack.back().name == "linearGradient" && !stack.back().stops)
                fail(MediaIoCode::InvalidData, "SVG gradient has no stops");
            stack.pop_back();
            if (stack.empty()) closedRoot = true;
            continue;
        }
        if (event.kind != ii::xml::Kind::start_element)
            fail(MediaIoCode::UnsupportedFeature, "unsupported SVG XML event");
        const auto name = std::string_view(event.local_name);
        if (!sawRoot) {
            if (event.depth != 0 || name != "svg"
                || (!event.namespace_uri.empty() && event.namespace_uri != svgNamespace))
                fail(MediaIoCode::InvalidData, "SVG requires a supported root element");
            sawRoot = true;
            rootNamespace = event.namespace_uri;
        } else {
            if (closedRoot || stack.empty() || event.depth != stack.size() || event.namespace_uri != rootNamespace)
                fail(MediaIoCode::InvalidData, "invalid SVG element nesting");
            const auto &parent = stack.back().name;
            const bool drawing = oneOf(parent, {"svg", "g"})
                && oneOf(name, {"g", "defs", "rect", "path", "circle", "ellipse", "line", "polygon", "polyline"});
            const bool gradient = parent == "defs" && name == "linearGradient";
            const bool stop = parent == "linearGradient" && name == "stop";
            if (!drawing && !gradient && !stop)
                fail(MediaIoCode::UnsupportedFeature, "unsupported SVG element or nesting");
            if (stop) ++stack.back().stops;
        }
        const bool drawing = oneOf(name, {"svg", "g", "rect", "path", "circle", "ellipse", "line", "polygon", "polyline"});
        auto matrix = stack.empty() ? SvgMatrix{1, 0, 0, 1, 0, 0} : stack.back().matrix;
        bool hasWidth = false, hasHeight = false, hasPath = false, hasId = false;
        for (const auto &attribute : event.attributes) {
            if (name == "svg" && attribute.name == "xmlns") {
                if (!attribute.value.empty() && attribute.value != svgNamespace)
                    fail(MediaIoCode::UnsupportedFeature, "unsupported SVG namespace");
                continue;
            }
            if (!attribute.namespace_uri.empty() || attribute.name != attribute.local_name)
                fail(MediaIoCode::UnsupportedFeature, "namespaced SVG attributes are not supported");
            const auto key = std::string_view(attribute.name), value = std::string_view(attribute.value);
            if (drawing && key == "transform" && extended)
                matrix = composeMatrix(matrix, transformList(value, commands, limits));
            else if (drawing && key == "style" && extended) inlineStyle(value, references);
            else if (drawing && paintProperty(key, value, references)) { /* validated above */ }
            else if ((name == "svg" || name == "rect") && oneOf(key, {"width", "height"})) {
                const auto v = scalar(value, false, true);
                if (v < 0 || (name == "svg" && v == 0)) fail(MediaIoCode::InvalidData, "invalid SVG extent");
                if (key == "width") hasWidth = true; else hasHeight = true;
            } else if (name == "rect" && oneOf(key, {"x", "y", "rx", "ry"})) {
                const auto v = scalar(value, false, true);
                if ((key == "rx" || key == "ry") && v < 0) fail(MediaIoCode::InvalidData, "negative SVG corner radius");
            } else if ((name == "circle" || name == "ellipse") && oneOf(key, {"cx", "cy", "r", "rx", "ry"})) {
                if ((name == "circle" && (key == "rx" || key == "ry")) || (name == "ellipse" && key == "r"))
                    fail(MediaIoCode::UnsupportedFeature, "radius attribute does not match SVG shape");
                const auto v = scalar(value, false, true);
                if ((key == "r" || key == "rx" || key == "ry") && v < 0)
                    fail(MediaIoCode::InvalidData, "negative SVG shape radius");
            } else if (name == "line" && oneOf(key, {"x1", "y1", "x2", "y2"})) {
                (void)scalar(value, false, true);
            } else if ((name == "polygon" || name == "polyline") && key == "points") {
                pointList(value, commands, limits);
            } else if (name == "svg" && key == "viewBox") box(value);
            else if (name == "svg" && key == "version") {
                if (trim(value) != "1.1") fail(MediaIoCode::UnsupportedFeature, "unsupported SVG version");
            } else if (name == "path" && key == "d") {
                pathData(value, commands, limits, extended);
                hasPath = true;
            } else if (name == "linearGradient" && key == "id") {
                if (!validId(value) || !gradients.emplace(value).second)
                    fail(MediaIoCode::InvalidData, "invalid or duplicate SVG gradient ID");
                hasId = true;
            } else if (name == "linearGradient" && oneOf(key, {"x1", "x2", "y1", "y2"})) {
                (void)scalar(value, true);
            } else if (name == "linearGradient" && key == "gradientUnits") {
                if (!oneOf(trim(value), {"userSpaceOnUse", "objectBoundingBox"}))
                    fail(MediaIoCode::UnsupportedFeature, "unsupported SVG gradient units");
            } else if (name == "stop" && key == "offset") {
                const auto v = scalar(value, true);
                if (v < 0 || v > 1) fail(MediaIoCode::InvalidData, "SVG gradient offset is outside [0,1]");
            } else if (name == "stop" && key == "stop-color") color(value);
            else if (name == "stop" && key == "stop-opacity") {
                const auto v = scalar(value);
                if (v < 0 || v > 1) fail(MediaIoCode::InvalidData, "SVG stop opacity is outside [0,1]");
            } else fail(MediaIoCode::UnsupportedFeature, "unsupported SVG attribute: " + attribute.name);
        }
        if (name == "svg" && (!hasWidth || !hasHeight))
            fail(MediaIoCode::UnsupportedFeature, "SVG requires explicit width and height");
        if (name == "linearGradient" && (!hasId || event.empty))
            fail(MediaIoCode::InvalidData, "SVG gradient requires an ID and stops");
        if (name == "path" && !hasPath) fail(MediaIoCode::InvalidData, "SVG path requires data");
        if (oneOf(name, {"rect", "circle", "ellipse", "line"})) count(commands, limits);
        if (event.empty) {
            if (name == "svg") closedRoot = true;
        } else stack.push_back({event.local_name, 0, matrix});
    }
    if (!sawRoot || !closedRoot || !stack.empty()) fail(MediaIoCode::InvalidData, "incomplete SVG document");
    for (const auto &id : references)
        if (!gradients.contains(id)) fail(MediaIoCode::InvalidData, "SVG references a missing gradient");
}
} // namespace

BitmapImportResult rasterizeGuiSvg(std::span<const std::uint8_t> bytes, const GuiSvgRasterOptions &options)
{
    try {
        const auto &limits = options.limits;
        if (options.assetId.empty() || options.assetId.find('\0') != std::string::npos
            || !ii::core::valid_utf8(options.assetId))
            fail(MediaIoCode::InvalidArgument, "SVG raster asset ID must be nonempty UTF-8 without NUL");
        const auto width = options.outputExtent.width, height = options.outputExtent.height;
        if (width <= 0 || height <= 0 || width > int(ii::gui::maximum_image_dimension)
            || height > int(ii::gui::maximum_image_dimension))
            fail(MediaIoCode::InvalidArgument, "SVG raster dimensions must be between 1 and 8192");
        const auto pixels = std::uint64_t(width) * std::uint64_t(height);
        if (!limits.maxFrames || pixels > limits.maxPixelsPerFrame || pixels > guiPixelLimit
            || pixels * 4 > limits.maxDecodedBytes)
            fail(MediaIoCode::LimitExceeded, "SVG raster exceeds the frame, pixel or decoded byte limit");
        if (bytes.size() > limits.maxInputBytes)
            fail(MediaIoCode::LimitExceeded, "SVG input exceeds its byte limit");
        if (bytes.empty()) fail(MediaIoCode::InvalidData, "SVG input is empty");
        const bool compressed = bytes.size() >= 2 && bytes[0] == 0x1f && bytes[1] == 0x8b;
        ii::compression::Bytes expanded;
        if (compressed) {
            const auto expandedLimit = std::min<std::uint64_t>(limits.maxDecodedBytes, guiInputLimit);
            auto result = ii::compression::gzip_decompress(bytes, {limits.maxInputBytes, expandedLimit});
            if (!result) {
                const auto code = result.error().code == ii::compression::ErrorCode::limit_exceeded
                    ? MediaIoCode::LimitExceeded : result.error().code == ii::compression::ErrorCode::invalid_data
                        ? MediaIoCode::InvalidData : MediaIoCode::IoError;
                fail(code, result.error().message);
            }
            expanded = std::move(*result);
            bytes = expanded;
        }
        if (bytes.empty()) fail(MediaIoCode::InvalidData, "expanded SVG input is empty");
        if (bytes.size() > guiInputLimit) fail(MediaIoCode::LimitExceeded, "SVG exceeds the GUI codec input limit");
        const std::string svg(reinterpret_cast<const char *>(bytes.data()), bytes.size());
        inspectSvg(svg, limits, compressed, options.extendedGeometry);
        const auto image = ii::gui::svg_image(svg, std::uint32_t(width), std::uint32_t(height),
                                              ii::gui::ImageBudget{std::min(limits.maxPixelsPerFrame, guiPixelLimit)});
        if (!image) {
            const auto code = image.error().code == ii::core::ErrorCode::unsupported
                ? MediaIoCode::UnsupportedFeature : image.error().code == ii::core::ErrorCode::invalid_argument
                    ? MediaIoCode::InvalidData : MediaIoCode::IoError;
            fail(code, image.error().message);
        }
        const auto rgba = (*image)->rgba();
        if ((*image)->width() != std::uint32_t(width) || (*image)->height() != std::uint32_t(height)
            || rgba.size() != pixels * 4)
            fail(MediaIoCode::InvalidData, "GUI SVG rasterizer returned an unexpected image");
        BitmapImportResult result;
        result.asset.id = options.assetId;
        result.asset.pixels = makeRasterLayer(width, height);
        for (std::size_t i = 0; i < result.asset.pixels.pixels.size(); ++i) {
            const auto offset = 4 * i;
            result.asset.pixels.pixels[i] = (std::uint32_t(rgba[offset + 3]) << 24)
                | (std::uint32_t(rgba[offset]) << 16) | (std::uint32_t(rgba[offset + 1]) << 8)
                | std::uint32_t(rgba[offset + 2]);
        }
        result.format = compressed ? "svgz" : "svg";
        result.result.warnings.emplace_back("vector document rasterized: paths, gradients and editability are not retained");
        return result;
    } catch (const Failure &error) {
        return {{}, {}, {}, {error.code, error.message, {}}};
    } catch (const std::bad_alloc &) {
        return {{}, {}, {}, {MediaIoCode::LimitExceeded, "SVG raster allocation failed", {}}};
    } catch (const std::length_error &) {
        return {{}, {}, {}, {MediaIoCode::LimitExceeded, "SVG raster allocation is too large", {}}};
    } catch (const std::exception &error) {
        return {{}, {}, {}, {MediaIoCode::InvalidData, error.what(), {}}};
    }
}
} // namespace iiSharedCanvas::example
