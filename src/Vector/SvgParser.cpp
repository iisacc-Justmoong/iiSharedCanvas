#include "SvgParser_p.hpp"

#include "Media/MediaIo_p.hpp"
#include "Validation/Validation.h"

#include <QColor>
#include <QMap>
#include <QPainterPathStroker>
#include <QSet>
#include <QTransform>
#include <QXmlStreamReader>

#include <algorithm>
#include <array>
#include <cmath>
#include <limits>
#include <numbers>

namespace iiSharedCanvas::vector_detail {

QPainterPath painterPath(const VectorPath &path)
{
    QPainterPath output;
    output.setFillRule(Qt::OddEvenFill);
    for (const auto &command : path.commands) {
        std::visit([&](const auto &value) {
            using T = std::decay_t<decltype(value)>;
            if constexpr (std::is_same_v<T, MoveTo>) { output.moveTo(value.point.x, value.point.y); }
            else if constexpr (std::is_same_v<T, LineTo>) { output.lineTo(value.point.x, value.point.y); }
            else if constexpr (std::is_same_v<T, QuadraticTo>) { output.quadTo(value.control.x, value.control.y, value.end.x, value.end.y); }
            else if constexpr (std::is_same_v<T, CubicTo>) { output.cubicTo(value.control1.x, value.control1.y, value.control2.x, value.control2.y, value.end.x, value.end.y); }
            else { output.closeSubpath(); }
        }, command);
    }
    return output;
}

VectorPath vectorPath(const QPainterPath &path)
{
    VectorPath output;
    Point start{}, current{};
    bool started = false;
    const auto close = [&] {
        if (started && current.x == start.x && current.y == start.y && output.commands.size() > 1) {
            output.commands.emplace_back(ClosePath{});
        }
    };
    for (int i = 0; i < path.elementCount(); ++i) {
        const auto element = path.elementAt(i);
        current = {element.x, element.y};
        if (element.type == QPainterPath::MoveToElement) {
            // Qt represents closures as a final point, not a separate opcode.
            if (i > 0) {
                const auto previous = path.elementAt(i - 1);
                const auto next = current;
                current = {previous.x, previous.y};
                close();
                current = next;
            }
            start = current;
            started = true;
            output.commands.emplace_back(MoveTo{current});
        } else if (element.type == QPainterPath::LineToElement) {
            output.commands.emplace_back(LineTo{current});
        } else if (element.type == QPainterPath::CurveToElement && i + 2 < path.elementCount()) {
            const auto control2 = path.elementAt(++i);
            const auto end = path.elementAt(++i);
            output.commands.emplace_back(CubicTo{current, {control2.x, control2.y}, {end.x, end.y}});
            current = {end.x, end.y};
        }
    }
    close();
    return output;
}

namespace {
using namespace media_detail;

struct Failure { MediaIoCode code; QString message; };
[[noreturn]] void fail(const QString &message, MediaIoCode code = MediaIoCode::UnsupportedFeature)
{
    throw Failure{code, message};
}

class Numbers {
public:
    explicit Numbers(QString text) : m_text(std::move(text)) {}
    void space() { while (m_pos < m_text.size() && (m_text[m_pos].isSpace() || m_text[m_pos] == ',')) { ++m_pos; } }
    bool done() { space(); return m_pos == m_text.size(); }
    QChar peek() { space(); return m_pos < m_text.size() ? m_text[m_pos] : QChar{}; }
    QChar take() { const auto value = peek(); if (!value.isNull()) { ++m_pos; } return value; }
    double number()
    {
        space();
        const auto start = m_pos;
        if (m_pos < m_text.size() && (m_text[m_pos] == '+' || m_text[m_pos] == '-')) { ++m_pos; }
        bool digit = false;
        while (m_pos < m_text.size() && m_text[m_pos] >= '0' && m_text[m_pos] <= '9') { ++m_pos; digit = true; }
        if (m_pos < m_text.size() && m_text[m_pos] == '.') {
            ++m_pos;
            while (m_pos < m_text.size() && m_text[m_pos] >= '0' && m_text[m_pos] <= '9') { ++m_pos; digit = true; }
        }
        if (!digit) { fail("invalid or incomplete SVG number", MediaIoCode::InvalidData); }
        if (m_pos < m_text.size() && (m_text[m_pos] == 'e' || m_text[m_pos] == 'E')) {
            ++m_pos;
            if (m_pos < m_text.size() && (m_text[m_pos] == '+' || m_text[m_pos] == '-')) { ++m_pos; }
            const auto exponent = m_pos;
            while (m_pos < m_text.size() && m_text[m_pos] >= '0' && m_text[m_pos] <= '9') { ++m_pos; }
            if (exponent == m_pos) { fail("incomplete SVG exponent", MediaIoCode::InvalidData); }
        }
        bool valid = false;
        const double value = m_text.mid(start, m_pos - start).toDouble(&valid);
        if (!valid || !std::isfinite(value)) { fail("non-finite SVG number", MediaIoCode::InvalidData); }
        return value;
    }
    QString remaining() const { return m_text.mid(m_pos).trimmed(); }
private:
    QString m_text;
    qsizetype m_pos = 0;
};

double length(QString text, double reference = 0)
{
    Numbers input(text.trimmed());
    const double value = input.number();
    const auto unit = input.remaining();
    if (unit.isEmpty() || unit == "px") { return value; }
    if (unit == "%" && reference > 0) { return value * reference / 100; }
    if (unit == "in") { return value * 96; }
    if (unit == "cm") { return value * 96 / 2.54; }
    if (unit == "mm") { return value * 96 / 25.4; }
    if (unit == "pt") { return value * 96 / 72; }
    if (unit == "pc") { return value * 16; }
    fail("unsupported SVG length unit: " + unit);
}

double scalar(const QString &text)
{
    Numbers input(text);
    const auto value = input.number();
    if (!input.done()) { fail("unexpected suffix on SVG number", MediaIoCode::InvalidData); }
    return value;
}

double opacity(QString text)
{
    const auto value = text.trimmed().endsWith('%') ? length(text, 1) : scalar(text);
    return std::clamp(value, 0.0, 1.0);
}

QTransform compose(const QTransform &parent, const QTransform &local)
{
    return {parent.m11() * local.m11() + parent.m21() * local.m12(),
            parent.m12() * local.m11() + parent.m22() * local.m12(),
            parent.m11() * local.m21() + parent.m21() * local.m22(),
            parent.m12() * local.m21() + parent.m22() * local.m22(),
            parent.m11() * local.dx() + parent.m21() * local.dy() + parent.dx(),
            parent.m12() * local.dx() + parent.m22() * local.dy() + parent.dy()};
}

QTransform transform(QString text)
{
    QTransform output;
    qsizetype position = 0;
    while (position < text.size()) {
        while (position < text.size() && (text[position].isSpace() || text[position] == ',')) { ++position; }
        if (position == text.size()) { break; }
        const auto start = position;
        while (position < text.size() && text[position].isLetter()) { ++position; }
        const auto name = text.mid(start, position - start);
        while (position < text.size() && text[position].isSpace()) { ++position; }
        if (position >= text.size() || text[position++] != '(') { fail("malformed SVG transform", MediaIoCode::InvalidData); }
        const auto end = text.indexOf(')', position);
        if (end < 0) { fail("unterminated SVG transform", MediaIoCode::InvalidData); }
        Numbers input(text.mid(position, end - position));
        std::vector<double> args;
        while (!input.done() && args.size() <= 6) { args.push_back(input.number()); }
        if (!input.done()) { fail("too many SVG transform parameters", MediaIoCode::InvalidData); }
        QTransform local;
        if (name == "matrix" && args.size() == 6) { local = QTransform(args[0], args[1], args[2], args[3], args[4], args[5]); }
        else if (name == "translate" && (args.size() == 1 || args.size() == 2)) { local.translate(args[0], args.size() == 2 ? args[1] : 0); }
        else if (name == "scale" && (args.size() == 1 || args.size() == 2)) { local.scale(args[0], args.size() == 2 ? args[1] : args[0]); }
        else if (name == "rotate" && (args.size() == 1 || args.size() == 3)) {
            if (args.size() == 3) { local.translate(args[1], args[2]); }
            local.rotate(args[0]);
            if (args.size() == 3) { local.translate(-args[1], -args[2]); }
        } else if ((name == "skewX" || name == "skewY") && args.size() == 1) {
            const double tangent = std::tan(args[0] * std::numbers::pi / 180);
            local.shear(name == "skewX" ? tangent : 0, name == "skewY" ? tangent : 0);
        } else { fail("unsupported or malformed SVG transform: " + name); }
        output = compose(output, local);
        for (double value : {output.m11(), output.m12(), output.m21(), output.m22(), output.dx(), output.dy()}) {
            if (!std::isfinite(value)) { fail("non-finite SVG transform", MediaIoCode::InvalidData); }
        }
        position = end + 1;
    }
    return output;
}

QColor color(QString text, QColor current)
{
    text = text.trimmed();
    if (text == "currentColor") { return current; }
    if (text.startsWith('#') && (text.size() == 5 || text.size() == 9)) {
        bool valid = false;
        const auto value = text.mid(1).toUInt(&valid, 16);
        if (!valid) { fail("invalid SVG color", MediaIoCode::InvalidData); }
        if (text.size() == 5) { return QColor(int((value >> 12) & 15) * 17, int((value >> 8) & 15) * 17, int((value >> 4) & 15) * 17, int(value & 15) * 17); }
        return QColor(int(value >> 24), int((value >> 16) & 255), int((value >> 8) & 255), int(value & 255));
    }
    if (text.startsWith("rgb(") || text.startsWith("rgba(")) {
        const bool alpha = text.startsWith("rgba(");
        if (!text.endsWith(')')) { fail("invalid SVG color", MediaIoCode::InvalidData); }
        auto components = text.mid(alpha ? 5 : 4, text.size() - (alpha ? 6 : 5)).split(',');
        if (components.size() != (alpha ? 4 : 3)) { fail("unsupported SVG color syntax"); }
        std::array<int, 4> channels{0, 0, 0, 255};
        for (int i = 0; i < 3; ++i) {
            const auto value = components[i].trimmed();
            channels[i] = int(std::round(std::clamp(value.endsWith('%') ? length(value, 255) : scalar(value), 0.0, 255.0)));
        }
        if (alpha) { channels[3] = int(std::round(opacity(components[3]) * 255)); }
        return {channels[0], channels[1], channels[2], channels[3]};
    }
    const QColor value(text);
    if (!value.isValid() || text.startsWith("url(")) { fail("unsupported SVG paint: " + text); }
    return value;
}

struct Style {
    std::optional<QColor> fill = QColor(Qt::black), stroke;
    QColor current = Qt::black;
    double fillOpacity = 1, strokeOpacity = 1, width = 1, objectOpacity = 1, miterLimit = 4;
    Qt::FillRule rule = Qt::WindingFill;
    Qt::PenCapStyle cap = Qt::FlatCap;
    Qt::PenJoinStyle join = Qt::SvgMiterJoin;
    QList<qreal> dashes;
    double dashOffset = 0;
    bool displayed = true, visible = true;
};

struct Context { Style style; QTransform matrix; double width = 0, height = 0; };

class Parser {
public:
    Parser(const QByteArray &xml, const VectorImportOptions &options) : m_xml(xml), m_options(options) {}
    VectorImportResult parse()
    {
        VectorImportResult result;
        result.asset.id = m_options.assetId;
        m_result = &result;
        bool root = false;
        while (!m_xml.atEnd()) {
            const auto token = m_xml.readNext();
            unsafeToken(token);
            if (token == QXmlStreamReader::StartElement) {
                if (root || m_xml.name() != u"svg") { fail("SVG requires one svg root element", MediaIoCode::InvalidData); }
                root = true;
                Context context;
                rootContext(context);
                element(context, 1, true);
            }
        }
        if (m_xml.hasError() || !root) { fail("malformed SVG XML: " + m_xml.errorString(), MediaIoCode::InvalidData); }
        Document document;
        document.extent = result.asset.viewport;
        document.assets.emplace_back(result.asset);
        auto validation = validate(document);
        if (!validation.ok()) { fail(QString::fromStdString(validation.issues.front().message), MediaIoCode::InvalidData); }
        return result;
    }
private:
    QXmlStreamReader m_xml;
    const VectorImportOptions &m_options;
    VectorImportResult *m_result = nullptr;
    std::uint64_t m_commands = 0;

    void warning(const std::string &message)
    {
        auto &warnings = m_result->result.warnings;
        if (std::find(warnings.begin(), warnings.end(), message) == warnings.end()) { warnings.push_back(message); }
    }
    void unsafeToken(QXmlStreamReader::TokenType token)
    {
        if (token == QXmlStreamReader::DTD || token == QXmlStreamReader::EntityReference) { fail("SVG DTDs and entities are not supported"); }
        if (token == QXmlStreamReader::ProcessingInstruction && m_xml.processingInstructionTarget() != u"xml") {
            fail("SVG processing instructions and external stylesheets are not supported");
        }
    }
    void budget(std::size_t count)
    {
        if (count > m_options.limits.maxVectorCommands - std::min<std::uint64_t>(m_commands, m_options.limits.maxVectorCommands)) {
            fail("SVG command expansion exceeds the limit", MediaIoCode::LimitExceeded);
        }
        m_commands += count;
    }
    void append(VectorPath path, const QTransform &matrix)
    {
        if (path.commands.empty() || (!path.fill && !path.stroke)) { return; }
        budget(path.commands.size());
        const auto mapped = [&](Point point) {
            const auto value = matrix.map(QPointF(point.x, point.y));
            if (!std::isfinite(value.x()) || !std::isfinite(value.y())) { fail("non-finite transformed SVG point", MediaIoCode::InvalidData); }
            return Point{value.x(), value.y()};
        };
        for (auto &command : path.commands) {
            std::visit([&](auto &value) {
                using T = std::decay_t<decltype(value)>;
                if constexpr (std::is_same_v<T, MoveTo> || std::is_same_v<T, LineTo>) { value.point = mapped(value.point); }
                else if constexpr (std::is_same_v<T, QuadraticTo>) { value.control = mapped(value.control); value.end = mapped(value.end); }
                else if constexpr (std::is_same_v<T, CubicTo>) { value.control1 = mapped(value.control1); value.control2 = mapped(value.control2); value.end = mapped(value.end); }
            }, command);
        }
        m_result->asset.paths.push_back(std::move(path));
    }
    void rootContext(Context &context)
    {
        const auto attributes = m_xml.attributes();
        std::vector<double> box;
        if (attributes.hasAttribute("viewBox")) {
            Numbers input(attributes.value("viewBox").toString());
            while (!input.done() && box.size() < 5) { box.push_back(input.number()); }
            if (box.size() != 4 || !input.done() || box[2] <= 0 || box[3] <= 0) { fail("invalid SVG viewBox", MediaIoCode::InvalidData); }
        }
        const double width = attributes.hasAttribute("width") ? length(attributes.value("width").toString()) : (box.empty() ? 300 : box[2]);
        const double height = attributes.hasAttribute("height") ? length(attributes.value("height").toString()) : (box.empty() ? 150 : box[3]);
        if (!std::isfinite(width) || !std::isfinite(height) || width <= 0 || height <= 0
            || width > std::numeric_limits<int>::max() || height > std::numeric_limits<int>::max()) {
            fail("invalid SVG viewport dimensions", MediaIoCode::LimitExceeded);
        }
        m_result->asset.viewport = {int(std::ceil(width)), int(std::ceil(height))};
        auto result = checkExtent(m_result->asset.viewport, m_options.limits);
        if (!result.ok()) { fail(QString::fromStdString(result.message), result.code); }
        context.width = box.empty() ? width : box[2];
        context.height = box.empty() ? height : box[3];
        if (!box.empty()) {
            double sx = width / box[2], sy = height / box[3], tx = 0, ty = 0;
            auto aspect = attributes.value("preserveAspectRatio").toString().simplified();
            if (aspect.isEmpty()) { aspect = "xMidYMid meet"; }
            if (aspect != "none") {
                const auto parts = aspect.split(' ');
                const auto alignment = parts[0];
                if (parts.size() > 2 || (parts.size() == 2 && parts[1] != "meet")
                    || !QSet<QString>{"xMinYMin", "xMidYMin", "xMaxYMin", "xMinYMid", "xMidYMid", "xMaxYMid", "xMinYMax", "xMidYMax", "xMaxYMax"}.contains(alignment)) {
                    fail("unsupported SVG preserveAspectRatio; slice requires clipping");
                }
                sx = sy = std::min(sx, sy);
                tx = (width - box[2] * sx) * (alignment.startsWith("xMax") ? 1 : alignment.startsWith("xMid") ? 0.5 : 0);
                ty = (height - box[3] * sy) * (alignment.endsWith("YMax") ? 1 : alignment.endsWith("YMid") ? 0.5 : 0);
            }
            context.matrix = QTransform(sx, 0, 0, sy, tx - box[0] * sx, ty - box[1] * sy);
        }
    }
    QMap<QString, QString> attributes(const QString &name, bool root)
    {
        QMap<QString, QString> values;
        const QSet<QString> common{"id", "style", "transform", "fill", "stroke", "fill-opacity", "stroke-opacity",
                                   "opacity", "stroke-width", "stroke-linecap", "stroke-linejoin", "stroke-miterlimit",
                                   "stroke-dasharray", "stroke-dashoffset", "fill-rule", "display", "visibility", "color"};
        const QMap<QString, QSet<QString>> geometry{
            {"path", {"d"}}, {"rect", {"x", "y", "width", "height", "rx", "ry"}},
            {"circle", {"cx", "cy", "r"}}, {"ellipse", {"cx", "cy", "rx", "ry"}},
            {"line", {"x1", "y1", "x2", "y2"}}, {"polygon", {"points"}}, {"polyline", {"points"}}};
        for (const auto &attribute : m_xml.attributes()) {
            const auto key = attribute.name().toString();
            if (key.startsWith("on", Qt::CaseInsensitive) || key == "href") { fail("SVG active content and resource references are not supported"); }
            if (!attribute.namespaceUri().isEmpty()) {
                if (attribute.namespaceUri() == u"http://www.w3.org/XML/1998/namespace" && key == "space") { continue; }
                // Foreign namespaced authoring metadata does not affect SVG painting.
                if (attribute.namespaceUri() != u"http://www.w3.org/2000/svg") { continue; }
            }
            const bool viewport = root && QSet<QString>{"width", "height", "viewBox", "preserveAspectRatio", "version", "baseProfile"}.contains(key);
            if (!common.contains(key) && !geometry.value(name).contains(key) && !viewport) {
                fail("unsupported SVG attribute: " + key);
            }
            values[key] = attribute.value().toString();
        }
        const auto style = values.take("style");
        for (const auto &declaration : style.split(';', Qt::SkipEmptyParts)) {
            const auto separator = declaration.indexOf(':');
            if (separator < 1) { fail("malformed SVG style declaration", MediaIoCode::InvalidData); }
            const auto key = declaration.left(separator).trimmed();
            if (!common.contains(key) || key == "style" || key == "transform" || key == "id") { fail("unsupported SVG style property: " + key); }
            values[key] = declaration.mid(separator + 1).trimmed();
        }
        return values;
    }
    void style(Context &context, const QMap<QString, QString> &values, bool group)
    {
        auto &style = context.style;
        style.objectOpacity = 1;
        const auto present = [&](const QString &key) { return values.contains(key) && values[key] != "inherit"; };
        if (present("color")) { style.current = color(values["color"], style.current); }
        for (const auto &key : {QString("fill"), QString("stroke")}) {
            if (!present(key)) { continue; }
            auto &paint = key == "fill" ? style.fill : style.stroke;
            if (values[key] == "none") { paint.reset(); } else { paint = color(values[key], style.current); }
        }
        if (present("fill-opacity")) { style.fillOpacity = opacity(values["fill-opacity"]); }
        if (present("stroke-opacity")) { style.strokeOpacity = opacity(values["stroke-opacity"]); }
        if (present("opacity")) { style.objectOpacity = opacity(values["opacity"]); }
        if (group && style.objectOpacity != 1) { fail("group opacity requires a compositing group, not a single vector asset"); }
        if (present("stroke-width")) { style.width = length(values["stroke-width"], std::hypot(context.width, context.height) / std::sqrt(2.0)); }
        if (!std::isfinite(style.width) || style.width < 0) { fail("invalid SVG stroke width", MediaIoCode::InvalidData); }
        if (present("fill-rule")) {
            if (values["fill-rule"] == "evenodd") { style.rule = Qt::OddEvenFill; }
            else if (values["fill-rule"] == "nonzero") { style.rule = Qt::WindingFill; }
            else { fail("unsupported SVG fill rule"); }
        }
        if (present("stroke-linecap")) {
            if (values["stroke-linecap"] == "butt") { style.cap = Qt::FlatCap; }
            else if (values["stroke-linecap"] == "round") { style.cap = Qt::RoundCap; }
            else if (values["stroke-linecap"] == "square") { style.cap = Qt::SquareCap; }
            else { fail("unsupported SVG stroke cap"); }
        }
        if (present("stroke-linejoin")) {
            if (values["stroke-linejoin"] == "miter") { style.join = Qt::SvgMiterJoin; }
            else if (values["stroke-linejoin"] == "round") { style.join = Qt::RoundJoin; }
            else if (values["stroke-linejoin"] == "bevel") { style.join = Qt::BevelJoin; }
            else { fail("unsupported SVG stroke join"); }
        }
        if (present("stroke-miterlimit")) { style.miterLimit = scalar(values["stroke-miterlimit"]); }
        if (style.miterLimit < 1) { fail("invalid SVG miter limit", MediaIoCode::InvalidData); }
        if (present("stroke-dasharray")) {
            style.dashes.clear();
            if (values["stroke-dasharray"] != "none") {
                Numbers input(values["stroke-dasharray"]);
                while (!input.done()) {
                    const auto value = input.number();
                    if (value < 0 || style.dashes.size() >= 1024) { fail("invalid SVG dash array", MediaIoCode::LimitExceeded); }
                    style.dashes.push_back(value);
                }
                if (std::all_of(style.dashes.begin(), style.dashes.end(), [](auto value) { return value == 0; })) { style.dashes.clear(); }
                if (style.dashes.size() % 2) { style.dashes.append(style.dashes); }
            }
        }
        if (present("stroke-dashoffset")) { style.dashOffset = length(values["stroke-dashoffset"]); }
        if (present("display")) {
            if (values["display"] == "none") { style.displayed = false; }
            else if (values["display"] != "inline") { fail("unsupported SVG display value"); }
        }
        if (present("visibility")) {
            if (values["visibility"] == "hidden" || values["visibility"] == "collapse") { style.visible = false; }
            else if (values["visibility"] == "visible") { style.visible = true; }
            else { fail("unsupported SVG visibility value"); }
        }
        if (values.contains("transform")) { context.matrix = compose(context.matrix, transform(values["transform"])); }
    }

    void arc(VectorPath &path, Point start, Point end, double rx, double ry, double degrees, bool large, bool sweep)
    {
        if (start.x == end.x && start.y == end.y) { return; }
        rx = std::abs(rx); ry = std::abs(ry);
        if (rx == 0 || ry == 0) { path.commands.emplace_back(LineTo{end}); return; }
        const auto phi = std::fmod(degrees, 360.0) * std::numbers::pi / 180;
        const auto cosine = std::cos(phi), sine = std::sin(phi);
        const auto dx = (start.x - end.x) / 2, dy = (start.y - end.y) / 2;
        const auto xp = cosine * dx + sine * dy, yp = -sine * dx + cosine * dy;
        const auto radii = xp * xp / (rx * rx) + yp * yp / (ry * ry);
        if (radii > 1) { rx *= std::sqrt(radii); ry *= std::sqrt(radii); }
        const auto denominator = rx * rx * yp * yp + ry * ry * xp * xp;
        const auto factor = (large == sweep ? -1 : 1) * std::sqrt(std::max(0.0, (rx * rx * ry * ry - denominator) / denominator));
        const auto cxp = factor * rx * yp / ry, cyp = -factor * ry * xp / rx;
        const auto cx = cosine * cxp - sine * cyp + (start.x + end.x) / 2;
        const auto cy = sine * cxp + cosine * cyp + (start.y + end.y) / 2;
        const auto ux = (xp - cxp) / rx, uy = (yp - cyp) / ry;
        const auto vx = (-xp - cxp) / rx, vy = (-yp - cyp) / ry;
        double angle = std::atan2(uy, ux), delta = std::atan2(ux * vy - uy * vx, ux * vx + uy * vy);
        if (!sweep && delta > 0) { delta -= 2 * std::numbers::pi; }
        if (sweep && delta < 0) { delta += 2 * std::numbers::pi; }
        if (!std::isfinite(delta) || !std::isfinite(cx) || !std::isfinite(cy)) { fail("invalid SVG arc geometry", MediaIoCode::InvalidData); }
        const int segments = std::max(1, int(std::ceil(std::abs(delta) / (std::numbers::pi / 2))));
        delta /= segments;
        const auto point = [&](double x, double y) { return Point{cx + rx * cosine * x - ry * sine * y, cy + rx * sine * x + ry * cosine * y}; };
        for (int i = 0; i < segments; ++i) {
            const auto next = angle + delta, tangent = 4.0 / 3.0 * std::tan(delta / 4);
            path.commands.emplace_back(CubicTo{
                point(std::cos(angle) - tangent * std::sin(angle), std::sin(angle) + tangent * std::cos(angle)),
                point(std::cos(next) + tangent * std::sin(next), std::sin(next) - tangent * std::cos(next)),
                i + 1 == segments ? end : point(std::cos(next), std::sin(next))});
            angle = next;
        }
        warning("elliptical arcs converted to cubic Bezier segments (at most 90 degrees per segment)");
    }

    VectorPath pathData(const QString &text)
    {
        Numbers input(text);
        VectorPath path;
        QChar operation, previous;
        Point current{}, start{}, control{};
        bool started = false;
        while (!input.done()) {
            if (input.peek().isLetter()) { operation = input.take(); }
            if (operation.isNull()) { fail("SVG path command is missing", MediaIoCode::InvalidData); }
            const bool relative = operation.isLower();
            const auto upper = operation.toUpper();
            if (!started && upper != 'M') { fail("SVG path must begin with moveto", MediaIoCode::InvalidData); }
            const auto point = [&]() {
                const double x = input.number(), y = input.number();
                return Point{x + (relative ? current.x : 0), y + (relative ? current.y : 0)};
            };
            if (upper == 'M') {
                current = point(); start = current; started = true;
                path.commands.emplace_back(MoveTo{current});
                operation = relative ? 'l' : 'L';
            } else if (upper == 'L') { current = point(); path.commands.emplace_back(LineTo{current}); }
            else if (upper == 'H') { const auto x = input.number(); current.x = x + (relative ? current.x : 0); path.commands.emplace_back(LineTo{current}); }
            else if (upper == 'V') { const auto y = input.number(); current.y = y + (relative ? current.y : 0); path.commands.emplace_back(LineTo{current}); }
            else if (upper == 'Q' || upper == 'T') {
                Point first = upper == 'Q' ? point() : (previous == 'Q' || previous == 'T' ? Point{2 * current.x - control.x, 2 * current.y - control.y} : current);
                const auto end = point();
                path.commands.emplace_back(QuadraticTo{first, end}); control = first; current = end;
            } else if (upper == 'C' || upper == 'S') {
                Point first = upper == 'C' ? point() : (previous == 'C' || previous == 'S' ? Point{2 * current.x - control.x, 2 * current.y - control.y} : current);
                const auto second = point(), end = point();
                path.commands.emplace_back(CubicTo{first, second, end}); control = second; current = end;
            } else if (upper == 'A') {
                const auto rx = input.number(), ry = input.number(), angle = input.number();
                const auto large = input.take(), sweep = input.take();
                if ((large != '0' && large != '1') || (sweep != '0' && sweep != '1')) { fail("invalid SVG arc flags", MediaIoCode::InvalidData); }
                const auto end = point();
                arc(path, current, end, rx, ry, angle, large == '1', sweep == '1'); current = end;
            } else if (upper == 'Z') { path.commands.emplace_back(ClosePath{}); current = start; operation = {}; }
            else { fail(QStringLiteral("unsupported SVG path command: ") + upper); }
            previous = upper;
            if (path.commands.size() > m_options.limits.maxVectorCommands) { fail("SVG path exceeds command limit", MediaIoCode::LimitExceeded); }
        }
        return path;
    }

    VectorPath geometry(const QString &name, const QMap<QString, QString> &values, const Context &context)
    {
        if (name == "path") { return pathData(values.value("d")); }
        const auto x = [&](const QString &key, double fallback = 0) { return values.contains(key) ? length(values[key], context.width) : fallback; };
        const auto y = [&](const QString &key, double fallback = 0) { return values.contains(key) ? length(values[key], context.height) : fallback; };
        VectorPath path;
        if (name == "line") { path.commands = {MoveTo{{x("x1"), y("y1")}}, LineTo{{x("x2"), y("y2")}}}; return path; }
        if (name == "polygon" || name == "polyline") {
            Numbers input(values.value("points"));
            while (!input.done()) {
                const Point point{input.number(), input.number()};
                if (path.commands.empty()) { path.commands.emplace_back(MoveTo{point}); } else { path.commands.emplace_back(LineTo{point}); }
                if (path.commands.size() > m_options.limits.maxVectorCommands) { fail("SVG points exceed command limit", MediaIoCode::LimitExceeded); }
            }
            if (name == "polygon" && !path.commands.empty()) { path.commands.emplace_back(ClosePath{}); }
            return path;
        }
        QPainterPath shape;
        if (name == "rect") {
            const auto width = x("width"), height = y("height");
            if (width < 0 || height < 0) { fail("negative SVG rectangle dimensions", MediaIoCode::InvalidData); }
            if (!width || !height) { return {}; }
            const auto rx = x("rx", y("ry")), ry = y("ry", x("rx"));
            if (rx < 0 || ry < 0) { fail("negative SVG corner radius", MediaIoCode::InvalidData); }
            if (rx || ry) { shape.addRoundedRect(x("x"), y("y"), width, height, std::min(rx, width / 2), std::min(ry, height / 2), Qt::AbsoluteSize); }
            else { shape.addRect(x("x"), y("y"), width, height); }
        } else if (name == "circle" || name == "ellipse") {
            const auto radius = values.contains("r") ? length(values["r"], std::hypot(context.width, context.height) / std::sqrt(2.0)) : 0;
            const auto rx = name == "circle" ? radius : x("rx"), ry = name == "circle" ? radius : y("ry");
            if (rx < 0 || ry < 0) { fail("negative SVG ellipse radius", MediaIoCode::InvalidData); }
            if (!rx || !ry) { return {}; }
            shape.addEllipse(QPointF(x("cx"), y("cy")), rx, ry);
        }
        return vectorPath(shape);
    }

    void paint(VectorPath path, const Context &context)
    {
        const auto &style = context.style;
        if (!style.displayed || !style.visible || path.commands.empty() || style.objectOpacity == 0) { return; }
        if (style.objectOpacity != 1 && style.fill && style.stroke && style.width > 0) {
            fail("shape opacity with both fill and stroke requires a compositing group");
        }
        const auto paintColor = [&](QColor value, double alpha) {
            value.setAlpha(int(std::round(value.alpha() * alpha * style.objectOpacity)));
            return SolidPaint{value.rgba()};
        };
        QPainterPath qtPath = painterPath(path);
        if (style.fill) {
            VectorPath fill = path;
            if (style.rule == Qt::WindingFill) {
                qtPath.setFillRule(Qt::WindingFill);
                fill = vectorPath(qtPath.simplified());
                warning("nonzero fills normalized to even-odd editable outlines; curve flattening may occur");
            }
            fill.fill = paintColor(*style.fill, style.fillOpacity);
            append(std::move(fill), context.matrix);
        }
        if (style.stroke && style.width > 0) {
            const auto &m = context.matrix;
            const double sx = std::hypot(m.m11(), m.m12()), sy = std::hypot(m.m21(), m.m22());
            const bool similarity = sx > 0 && std::abs(sx - sy) <= 1e-10 * std::max(sx, sy)
                && std::abs(m.m11() * m.m21() + m.m12() * m.m22()) <= 1e-10 * sx * sy;
            if (style.cap == Qt::RoundCap && style.join == Qt::RoundJoin && style.dashes.empty() && similarity) {
                path.stroke = StrokeStyle{paintColor(*style.stroke, style.strokeOpacity), style.width * sx};
                append(std::move(path), context.matrix);
            } else {
                QPainterPathStroker stroker;
                stroker.setWidth(style.width);
                stroker.setCapStyle(style.cap);
                stroker.setJoinStyle(style.join);
                stroker.setMiterLimit(style.miterLimit / 2);
                if (!style.dashes.empty()) {
                    QList<qreal> pattern;
                    double cycle = 0;
                    for (auto dash : style.dashes) { pattern.push_back(dash / style.width); cycle += dash; }
                    const auto bounds = qtPath.controlPointRect();
                    if (cycle > 0 && (bounds.width() + bounds.height()) / cycle > m_options.limits.maxVectorCommands / 8) {
                        fail("SVG dash expansion exceeds command budget", MediaIoCode::LimitExceeded);
                    }
                    stroker.setDashPattern(pattern);
                    stroker.setDashOffset(style.dashOffset / style.width);
                }
                auto outline = vectorPath(stroker.createStroke(qtPath).simplified());
                outline.fill = paintColor(*style.stroke, style.strokeOpacity);
                append(std::move(outline), context.matrix);
                warning("stroke caps, joins, dashes or nonuniform scaling converted to editable filled outlines");
            }
        }
    }

    void ignoredElement(std::uint32_t depth)
    {
        while (!m_xml.atEnd()) {
            const auto token = m_xml.readNext();
            unsafeToken(token);
            if (token == QXmlStreamReader::EndElement) { return; }
            if (token == QXmlStreamReader::StartElement) {
                if (depth >= m_options.limits.maxXmlDepth) { fail("SVG nesting exceeds depth limit", MediaIoCode::LimitExceeded); }
                ignoredElement(depth + 1);
            }
        }
    }
    void element(Context context, std::uint32_t depth, bool root = false)
    {
        if (depth > m_options.limits.maxXmlDepth) { fail("SVG nesting exceeds depth limit", MediaIoCode::LimitExceeded); }
        const auto name = m_xml.name().toString();
        if (!m_xml.namespaceUri().isEmpty() && m_xml.namespaceUri() != u"http://www.w3.org/2000/svg") { fail("foreign SVG drawing elements are not supported"); }
        if (name == "title" || name == "desc" || name == "metadata") { ignoredElement(depth); return; }
        const bool group = name == "g" || (name == "svg" && root);
        if (!group && !QSet<QString>{"path", "rect", "circle", "ellipse", "line", "polyline", "polygon"}.contains(name)) {
            fail("unsupported SVG element: " + name);
        }
        const auto values = attributes(name, root);
        style(context, values, group);
        if (!group) { paint(geometry(name, values, context), context); }
        while (!m_xml.atEnd()) {
            const auto token = m_xml.readNext();
            unsafeToken(token);
            if (token == QXmlStreamReader::EndElement) { return; }
            if (token == QXmlStreamReader::StartElement) {
                if (!group && m_xml.name() != u"title" && m_xml.name() != u"desc" && m_xml.name() != u"metadata") {
                    fail("unsupported nested SVG shape content");
                }
                element(context, depth + 1);
            } else if (token == QXmlStreamReader::Characters && !m_xml.isWhitespace()) {
                fail("unexpected SVG text content");
            }
        }
    }
};
}

VectorImportResult parseSvg(const QByteArray &xml, const VectorImportOptions &options)
{
    try { return Parser(xml, options).parse(); }
    catch (const Failure &failure) { return {{}, error(failure.code, failure.message)}; }
    catch (const std::bad_alloc &) { return {{}, error(MediaIoCode::LimitExceeded, "SVG allocation failed")}; }
}

} // namespace iiSharedCanvas::vector_detail
