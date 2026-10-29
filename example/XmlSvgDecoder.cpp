#include "XmlSvgDecoder.h"

#include <Validation/Validation.h>
#include <iiCompression/Gzip.h>
#include <iiSDKCore/Core/Text/Utf8.h>
#include <iiXml/Xml.h>

#include <algorithm>
#include <charconv>
#include <cmath>
#include <initializer_list>
#include <limits>
#include <map>
#include <new>
#include <optional>
#include <stdexcept>
#include <string_view>
#include <utility>

namespace iiSharedCanvas::example {
namespace {
constexpr std::string_view svgNamespace = "http://www.w3.org/2000/svg";

struct Failure {
    MediaIoCode code;
    std::string message;
};
[[noreturn]] void fail(MediaIoCode code, std::string message)
{
    throw Failure{code, std::move(message)};
}
bool space(char value)
{
    return value == ' ' || value == '\t' || value == '\r' || value == '\n';
}
std::size_t boundedSize(std::uint64_t value)
{
    return static_cast<std::size_t>(std::min<std::uint64_t>(
        value, std::numeric_limits<std::size_t>::max()));
}

class Numbers {
public:
    explicit Numbers(std::string_view text) : text_(text) {}
    bool done() { skip(); return position_ == text_.size(); }
    char command()
    {
        skip();
        if (position_ == text_.size()) fail(MediaIoCode::InvalidData, "missing SVG path command");
        return text_[position_++];
    }
    double number()
    {
        skip();
        double value = 0;
        const auto *first = text_.data() + position_;
        const auto *last = text_.data() + text_.size();
        const auto [end, error] = std::from_chars(first, last, value, std::chars_format::general);
        if (error != std::errc{} || end == first || !std::isfinite(value))
            fail(MediaIoCode::InvalidData, "invalid or non-finite SVG number");
        position_ = static_cast<std::size_t>(end - text_.data());
        return value;
    }
private:
    void skip()
    {
        while (position_ < text_.size() && space(text_[position_]))
            ++position_;
    }
    std::string_view text_;
    std::size_t position_ = 0;
};

using Attributes = std::map<std::string, std::string, std::less<>>;
Attributes attributes(const ii::xml::Event &event, bool root)
{
    Attributes result;
    for (const auto &attribute : event.attributes) {
        if (root && attribute.name == "xmlns") {
            if (!attribute.value.empty() && attribute.value != svgNamespace)
                fail(MediaIoCode::UnsupportedFeature, "unsupported SVG namespace");
            continue;
        }
        if (!attribute.namespace_uri.empty() || attribute.name != attribute.local_name)
            fail(MediaIoCode::UnsupportedFeature, "namespaced SVG attributes are not supported");
        if (!result.emplace(attribute.name, attribute.value).second)
            fail(MediaIoCode::InvalidData, "duplicate SVG attribute");
    }
    return result;
}
const std::string &required(const Attributes &values, std::string_view key)
{
    const auto it = values.find(key);
    if (it == values.end()) fail(MediaIoCode::UnsupportedFeature, "missing canonical SVG attribute: " + std::string(key));
    return it->second;
}
void checkAttributes(const Attributes &values, std::initializer_list<std::string_view> allowed)
{
    for (const auto &[key, value] : values) {
        (void)value;
        if (std::find(allowed.begin(), allowed.end(), key) == allowed.end())
            fail(MediaIoCode::UnsupportedFeature, "unsupported SVG attribute: " + key);
    }
}
int extent(const std::string &text)
{
    int value = 0;
    const auto [end, error] = std::from_chars(text.data(), text.data() + text.size(), value);
    if (error != std::errc{} || end != text.data() + text.size() || value <= 0)
        fail(MediaIoCode::InvalidData, "invalid SVG viewport extent");
    return value;
}
double scalar(std::string_view text)
{
    Numbers numbers(text);
    const auto value = numbers.number();
    if (!numbers.done()) fail(MediaIoCode::InvalidData, "invalid SVG scalar");
    return value;
}
std::optional<SolidPaint> paint(const Attributes &values, std::string_view name)
{
    const auto &text = required(values, name);
    const auto opacityName = std::string(name) + "-opacity";
    const auto opacity = values.find(opacityName);
    if (text == "none") {
        if (opacity != values.end()) fail(MediaIoCode::UnsupportedFeature, "opacity on absent SVG paint");
        return {};
    }
    if (text.size() != 7 || text.front() != '#')
        fail(MediaIoCode::UnsupportedFeature, "only canonical solid RGB SVG paint is supported");
    std::uint32_t rgb = 0;
    const auto [end, error] = std::from_chars(text.data() + 1, text.data() + text.size(), rgb, 16);
    if (error != std::errc{} || end != text.data() + text.size())
        fail(MediaIoCode::InvalidData, "invalid SVG RGB color");
    const double alpha = opacity == values.end() ? 1.0 : scalar(opacity->second);
    if (alpha < 0 || alpha > 1) fail(MediaIoCode::InvalidData, "SVG opacity is outside [0,1]");
    const double scaled = alpha * 255;
    const auto quantized = std::lround(scaled);
    if (std::abs(scaled - static_cast<double>(quantized)) > 1e-9)
        fail(MediaIoCode::UnsupportedFeature, "SVG opacity cannot be represented as an 8-bit alpha value");
    return SolidPaint{(static_cast<std::uint32_t>(quantized) << 24) | rgb};
}

class Budget {
public:
    Budget(const MediaLimits &limits, std::size_t idBytes) : limits_(limits)
    {
        consume(sizeof(VectorAsset));
        consume(idBytes);
    }
    void path() { consume(sizeof(VectorPath)); }
    void command()
    {
        if (commands_ >= limits_.maxVectorCommands)
            fail(MediaIoCode::LimitExceeded, "SVG vector command count exceeds its limit");
        ++commands_;
        consume(sizeof(PathCommand));
    }
private:
    void consume(std::uint64_t amount)
    {
        if (amount > limits_.maxDecodedBytes - used_)
            fail(MediaIoCode::LimitExceeded, "SVG vector storage exceeds its decoded byte limit");
        used_ += amount;
    }
    const MediaLimits &limits_;
    std::uint64_t used_ = 0;
    std::uint64_t commands_ = 0;
};

VectorPath path(const Attributes &values, Budget &budget)
{
    checkAttributes(values, {"d", "fill-rule", "fill", "fill-opacity", "stroke", "stroke-opacity",
                             "stroke-width", "stroke-linecap", "stroke-linejoin"});
    if (required(values, "fill-rule") != "evenodd")
        fail(MediaIoCode::UnsupportedFeature, "only even-odd SVG fills are supported");
    VectorPath result;
    result.fill = paint(values, "fill");
    const auto stroke = paint(values, "stroke");
    if (stroke) {
        if (required(values, "stroke-linecap") != "round" || required(values, "stroke-linejoin") != "round")
            fail(MediaIoCode::UnsupportedFeature, "only round SVG stroke caps and joins are supported");
        const auto width = scalar(required(values, "stroke-width"));
        if (width <= 0) fail(MediaIoCode::InvalidData, "SVG stroke width must be positive");
        result.stroke = StrokeStyle{*stroke, width};
    } else if (values.contains("stroke-width") || values.contains("stroke-linecap") || values.contains("stroke-linejoin")) {
        fail(MediaIoCode::UnsupportedFeature, "stroke attributes on absent SVG stroke");
    }
    if (!result.fill && !result.stroke)
        fail(MediaIoCode::InvalidData, "SVG path has no visible paint");
    budget.path();
    Numbers input(required(values, "d"));
    while (!input.done()) {
        const char operation = input.command();
        budget.command();
        const auto point = [&] { return Point{input.number(), input.number()}; };
        switch (operation) {
        case 'M': result.commands.emplace_back(MoveTo{point()}); break;
        case 'L': result.commands.emplace_back(LineTo{point()}); break;
        case 'Q': {
            const auto control = point(), end = point();
            result.commands.emplace_back(QuadraticTo{control, end});
            break;
        }
        case 'C': {
            const auto first = point(), second = point(), end = point();
            result.commands.emplace_back(CubicTo{first, second, end});
            break;
        }
        case 'Z': result.commands.emplace_back(ClosePath{}); break;
        default: fail(MediaIoCode::UnsupportedFeature, "unsupported SVG path command");
        }
    }
    if (result.commands.empty() || !std::holds_alternative<MoveTo>(result.commands.front()))
        fail(MediaIoCode::InvalidData, "SVG path must begin with a move command");
    return result;
}

void root(const ii::xml::Event &event, VectorAsset &asset, const MediaLimits &limits)
{
    const auto values = attributes(event, true);
    checkAttributes(values, {"version", "width", "height", "viewBox"});
    if (auto version = values.find("version"); version != values.end() && version->second != "1.1")
        fail(MediaIoCode::UnsupportedFeature, "unsupported SVG version");
    asset.viewport = {extent(required(values, "width")), extent(required(values, "height"))};
    const auto pixels = std::uint64_t(asset.viewport.width) * std::uint64_t(asset.viewport.height);
    if (pixels > limits.maxPixelsPerFrame)
        fail(MediaIoCode::LimitExceeded, "SVG viewport exceeds its pixel limit");
    Numbers box(required(values, "viewBox"));
    const auto x = box.number(), y = box.number(), width = box.number(), height = box.number();
    if (!box.done() || x != 0 || y != 0 || width != asset.viewport.width || height != asset.viewport.height)
        fail(MediaIoCode::UnsupportedFeature, "noncanonical SVG viewBox needs normalization");
}
MediaIoCode xmlError(const ii::xml::Error &error)
{
    return error.code == ii::xml::ErrorCode::limit_exceeded ? MediaIoCode::LimitExceeded : MediaIoCode::InvalidData;
}
} // namespace

CanvasSvgDecodeResult decodeCanvasSvg(std::span<const std::uint8_t> bytes, std::string assetId,
                                      const MediaLimits &limits)
{
    try {
        if (bytes.size() > limits.maxInputBytes)
            fail(MediaIoCode::LimitExceeded, "SVG input exceeds its byte limit");
        if (assetId.empty() || assetId.find('\0') != assetId.npos || !ii::core::valid_utf8(assetId))
            fail(MediaIoCode::InvalidArgument, "SVG asset ID must be nonempty UTF-8 without NUL");
        if (bytes.empty()) fail(MediaIoCode::InvalidData, "SVG input is empty");
        const bool compressed = bytes.size() >= 2 && bytes[0] == 0x1f && bytes[1] == 0x8b;
        ii::compression::Bytes expanded;
        if (compressed) {
            ii::compression::Limits gzipLimits{limits.maxInputBytes, limits.maxDecodedBytes};
            auto result = ii::compression::gzip_decompress(bytes, gzipLimits);
            if (!result) {
                const auto code = result.error().code == ii::compression::ErrorCode::limit_exceeded
                    ? MediaIoCode::LimitExceeded
                    : result.error().code == ii::compression::ErrorCode::invalid_data
                        ? MediaIoCode::InvalidData : MediaIoCode::IoError;
                fail(code, result.error().message);
            }
            expanded = std::move(*result);
            bytes = std::span<const std::uint8_t>(expanded);
            if (bytes.empty()) fail(MediaIoCode::InvalidData, "expanded SVG input is empty");
        }
        Budget budget(limits, assetId.size());
        ii::xml::Limits xmlLimits;
        const auto xmlInputLimit = compressed ? limits.maxDecodedBytes : limits.maxInputBytes;
        xmlLimits.input_bytes = boundedSize(xmlInputLimit);
        xmlLimits.text_bytes = boundedSize(std::min(xmlInputLimit, limits.maxDecodedBytes));
        xmlLimits.depth = limits.maxXmlDepth;
        xmlLimits.attributes = 16;
        xmlLimits.events = boundedSize(std::uint64_t(limits.maxVectorCommands) * 2 + 32);
        std::string input(reinterpret_cast<const char *>(bytes.data()), bytes.size());
        auto reader = ii::xml::Reader::open(std::move(input), xmlLimits);
        if (!reader) fail(xmlError(reader.error()), reader.error().message);
        VectorAsset asset;
        asset.id = std::move(assetId);
        bool sawRoot = false, closedRoot = false;
        std::string rootNamespace;
        for (;;) {
            auto next = reader->next();
            if (!next) fail(xmlError(next.error()), next.error().message);
            if (!*next) break;
            const auto &event = **next;
            if (event.kind == ii::xml::Kind::comment) continue;
            if (event.kind == ii::xml::Kind::text) {
                if (!std::all_of(event.value.begin(), event.value.end(), space))
                    fail(MediaIoCode::UnsupportedFeature, "SVG text content is not supported");
                continue;
            }
            if (event.kind == ii::xml::Kind::processing_instruction || event.kind == ii::xml::Kind::doctype)
                fail(MediaIoCode::UnsupportedFeature, "SVG declarations and processing instructions are not supported");
            if (event.kind == ii::xml::Kind::start_element) {
                if (!sawRoot) {
                    if (event.depth != 0 || event.local_name != "svg"
                        || (!event.namespace_uri.empty() && event.namespace_uri != svgNamespace))
                        fail(MediaIoCode::InvalidData, "SVG requires a supported svg root");
                    sawRoot = true;
                    rootNamespace = event.namespace_uri;
                    root(event, asset, limits);
                    closedRoot = event.empty;
                } else {
                    if (closedRoot || event.depth != 1 || event.local_name != "path"
                        || event.namespace_uri != rootNamespace || !event.empty)
                        fail(MediaIoCode::UnsupportedFeature, "only direct, empty SVG path elements are supported");
                    asset.paths.push_back(path(attributes(event, false), budget));
                }
            } else if (event.kind == ii::xml::Kind::end_element) {
                if (!sawRoot || closedRoot || event.depth != 0 || event.local_name != "svg"
                    || event.namespace_uri != rootNamespace)
                    fail(MediaIoCode::InvalidData, "unexpected SVG closing element");
                closedRoot = true;
            }
        }
        if (!sawRoot || !closedRoot) fail(MediaIoCode::InvalidData, "SVG root is incomplete");
        Document document;
        document.extent = asset.viewport;
        document.assets.emplace_back(asset);
        const auto valid = validate(document);
        if (!valid.ok()) fail(MediaIoCode::InvalidData, valid.issues.front().message);
        return {std::move(asset), {}};
    } catch (const Failure &error) {
        return {{}, {error.code, error.message, {}}};
    } catch (const std::bad_alloc &) {
        return {{}, {MediaIoCode::LimitExceeded, "SVG allocation failed", {}}};
    } catch (const std::length_error &) {
        return {{}, {MediaIoCode::LimitExceeded, "SVG allocation is too large", {}}};
    } catch (const std::exception &error) {
        return {{}, {MediaIoCode::InvalidData, error.what(), {}}};
    }
}
} // namespace iiSharedCanvas::example
