#include "TimelineXmlWriter_p.hpp"

#include "Media/MediaIo_p.hpp"

#include <QDir>
#include <QUrl>
#include <QXmlStreamWriter>

#include <algorithm>
#include <cmath>
#include <limits>
#include <new>
#include <numeric>
#include <stdexcept>
#include <string_view>

namespace iiSharedCanvas::timeline_detail {
namespace {

struct Rate {
    std::uint32_t numerator = 0;
    std::uint32_t denominator = 1;
    std::uint32_t timebase = 0;
    bool ntsc = false;
};

MediaIoResult invalid(const QString &message)
{
    return media_detail::error(MediaIoCode::InvalidArgument, message);
}

MediaIoResult limitError()
{
    return media_detail::error(MediaIoCode::LimitExceeded,
                              QStringLiteral("Combined timeline XML exceeds the output byte limit."));
}

bool xmlText(const QString &value)
{
    for (qsizetype index = 0; index < value.size(); ++index) {
        const auto code = value[index].unicode();
        if (QChar::isHighSurrogate(code)) {
            if (++index == value.size() || !value[index].isLowSurrogate()) { return false; }
        } else if (QChar::isLowSurrogate(code) || code == 0xfffe || code == 0xffff
                   || (code < 0x20 && code != 9 && code != 10 && code != 13)) {
            return false;
        }
    }
    return true;
}

bool utf8XmlText(const std::string &value)
{
    const auto decoded = QString::fromUtf8(value.data(), qsizetype(value.size()));
    const auto encoded = decoded.toUtf8();
    return std::string_view(encoded.constData(), std::size_t(encoded.size())) == value && xmlText(decoded);
}

QString text(const std::string &value)
{
    return QString::fromUtf8(value.data(), qsizetype(value.size()));
}

QString number(std::uint64_t value) { return QString::number(qulonglong(value)); }

QString time(FrameIndex frames, const Rate &rate)
{
    // Both multiplicands are uint32, so their product fits uint64. FCPXML time
    // uses a 64-bit numerator and a 32-bit denominator, never decimal seconds.
    const auto numerator = std::uint64_t(frames) * rate.denominator;
    const auto divisor = std::gcd(numerator, std::uint64_t(rate.numerator));
    const auto reducedNumerator = numerator / divisor;
    const auto reducedDenominator = rate.numerator / divisor;
    return number(reducedNumerator)
        + (reducedDenominator == 1 ? QString{} : QStringLiteral("/") + number(reducedDenominator))
        + QStringLiteral("s");
}

const char *legacyBlend(RasterBlendMode mode)
{
    switch (mode) {
    case RasterBlendMode::SourceOver: return "normal";
    case RasterBlendMode::Multiply: return "multiply";
    case RasterBlendMode::Screen: return "screen";
    case RasterBlendMode::Overlay: return "overlay";
    default: return nullptr;
    }
}

const char *fcpxmlBlend(RasterBlendMode mode)
{
    switch (mode) {
    case RasterBlendMode::SourceOver: return "0";
    case RasterBlendMode::Multiply: return "4";
    case RasterBlendMode::Screen: return "10";
    case RasterBlendMode::Overlay: return "14";
    default: return nullptr;
    }
}

MediaIoResult validatePlan(const InterchangePlan &plan, const QString &directory,
                           std::uint64_t maxBytes, Rate &rate)
{
    if (!plan.frameRate.numerator || !plan.frameRate.denominator || !plan.frameCount
        || plan.extent.width <= 0 || plan.extent.height <= 0) {
        return invalid(QStringLiteral("Timeline XML requires positive dimensions, duration, and frame rate."));
    }
    if (!QDir::isAbsolutePath(directory) || !xmlText(directory)) {
        return invalid(QStringLiteral("Timeline XML requires an absolute, XML-safe final directory."));
    }
    // Charge a lower bound on output before allocating decoded strings, URLs,
    // or the media-reference bitmap. The bounded sinks enforce the exact total.
    std::uint64_t minimumBytes = 0;
    const auto charge = [&](std::uint64_t value) {
        if (value > maxBytes - minimumBytes) { return false; }
        minimumBytes += value;
        return true;
    };
    if (!charge(std::uint64_t(directory.size())) || !charge(plan.name.size())
        || plan.tracks.size() > maxBytes / 32 || !charge(plan.tracks.size() * 32)
        || plan.media.size() > maxBytes / 64 || !charge(plan.media.size() * 64)) { return limitError(); }
    if (!utf8XmlText(plan.name)) { return invalid(QStringLiteral("Timeline sequence name is not valid UTF-8/XML text.")); }
    const auto divisor = std::gcd(plan.frameRate.numerator, plan.frameRate.denominator);
    rate.numerator = plan.frameRate.numerator / divisor;
    rate.denominator = plan.frameRate.denominator / divisor;
    if (rate.denominator == 1) { rate.timebase = rate.numerator; }
    else if ((std::uint64_t(rate.numerator) * 1001) % (std::uint64_t(rate.denominator) * 1000) == 0) {
        // Reduction can cancel factors of 1001 (e.g. 7000/1001 = 1000/143).
        rate.timebase = std::uint32_t((std::uint64_t(rate.numerator) * 1001)
                                     / (std::uint64_t(rate.denominator) * 1000));
        rate.ntsc = true;
    } else {
        return media_detail::error(MediaIoCode::UnsupportedFeature,
            QStringLiteral("Legacy timeline XML requires an integer frame rate or an exact integer*1000/1001 rate."));
    }
    for (const auto &media : plan.media) {
        if (!charge(std::uint64_t(media.relativePath.size()))) { return limitError(); }
        if (media.relativePath.isEmpty() || !xmlText(media.relativePath)
            || QDir::isAbsolutePath(media.relativePath) || media.relativePath.contains('\\')
            || media.relativePath.contains(':') || media.relativePath.endsWith('/')) {
            return invalid(QStringLiteral("Timeline media must use safe relative paths inside the package."));
        }
        const auto components = media.relativePath.split('/');
        if (std::any_of(components.begin(), components.end(), [](const auto &component) {
                return component.isEmpty() || component == "." || component == "..";
            })) {
            return invalid(QStringLiteral("Timeline media paths cannot contain traversal or empty components."));
        }
    }
    for (const auto &track : plan.tracks) {
        if (!charge(track.name.size()) || track.clips.size() > maxBytes / 64
            || !charge(track.clips.size() * 64)) { return limitError(); }
        if (!utf8XmlText(track.name) || !std::isfinite(track.opacity) || track.opacity < 0 || track.opacity > 1) {
            return invalid(QStringLiteral("Timeline layer names and opacity must be valid XML values."));
        }
        if (!legacyBlend(track.blendMode)) {
            return media_detail::error(MediaIoCode::UnsupportedFeature,
                QStringLiteral("Timeline XML supports only normal, multiply, screen, and overlay compositing."));
        }
        std::uint64_t previousEnd = 0;
        for (const auto &clip : track.clips) {
            const auto end = std::uint64_t(clip.start) + clip.duration;
            if (!clip.duration || clip.mediaIndex >= plan.media.size() || clip.start < previousEnd
                || end > plan.frameCount) {
                return invalid(QStringLiteral("Timeline holds must be ordered, non-overlapping, in range, and reference existing media."));
            }
            previousEnd = end;
        }
    }
    return {};
}

QString mediaUrl(const InterchangeMedia &media, const QString &directory)
{
    return QUrl::fromLocalFile(QDir(directory).filePath(media.relativePath)).toString(QUrl::FullyEncoded);
}

void legacyRate(QXmlStreamWriter &xml, const Rate &rate)
{
    xml.writeStartElement("rate");
    xml.writeTextElement("timebase", number(rate.timebase));
    xml.writeTextElement("ntsc", rate.ntsc ? "TRUE" : "FALSE");
    xml.writeEndElement();
}

void legacySample(QXmlStreamWriter &xml, CanvasExtent extent, const Rate &rate)
{
    xml.writeStartElement("samplecharacteristics");
    legacyRate(xml, rate);
    xml.writeTextElement("width", number(std::uint32_t(extent.width)));
    xml.writeTextElement("height", number(std::uint32_t(extent.height)));
    xml.writeTextElement("anamorphic", "FALSE");
    xml.writeTextElement("pixelaspectratio", "square");
    xml.writeTextElement("fielddominance", "none");
    xml.writeEndElement();
}

void legacyFile(QXmlStreamWriter &xml, const InterchangePlan &plan, const InterchangeClip &clip,
                const QString &directory, const Rate &rate, bool first)
{
    xml.writeStartElement("file");
    xml.writeAttribute("id", QStringLiteral("file-") + number(clip.mediaIndex + 1));
    if (first) {
        const auto &media = plan.media[clip.mediaIndex];
        // Unlike edit positions and clipitem duration, legacy still-file and
        // file/video durations are whole minutes (Apple's still-frame example).
        const auto durationNumerator = std::uint64_t(plan.frameCount) * rate.denominator;
        const auto minuteDenominator = std::uint64_t(rate.numerator) * 60;
        const auto minutes = durationNumerator / minuteDenominator + (durationNumerator % minuteDenominator != 0);
        xml.writeTextElement("name", media.relativePath.section('/', -1));
        xml.writeTextElement("pathurl", mediaUrl(media, directory));
        legacyRate(xml, rate);
        xml.writeTextElement("duration", number(minutes));
        xml.writeStartElement("media");
        xml.writeStartElement("video");
        xml.writeTextElement("duration", number(minutes));
        xml.writeTextElement("stillframe", "TRUE");
        xml.writeTextElement("alphatype", "straight");
        legacySample(xml, plan.extent, rate);
        xml.writeEndElement();
        xml.writeEndElement();
    }
    xml.writeEndElement();
}

void legacyOpacity(QXmlStreamWriter &xml, double opacity)
{
    xml.writeStartElement("filter");
    xml.writeTextElement("enabled", "TRUE");
    xml.writeStartElement("effect");
    xml.writeTextElement("name", "Opacity");
    xml.writeTextElement("effectid", "opacity");
    xml.writeTextElement("effectcategory", "motion");
    xml.writeTextElement("effecttype", "motion");
    xml.writeTextElement("mediatype", "video");
    xml.writeStartElement("parameter");
    xml.writeTextElement("parameterid", "opacity");
    xml.writeTextElement("name", "Opacity");
    xml.writeTextElement("valuemin", "0");
    xml.writeTextElement("valuemax", "100");
    xml.writeTextElement("value", QString::number(opacity * 100.0, 'g', 17));
    xml.writeEndElement();
    xml.writeEndElement();
    xml.writeEndElement();
}

void legacyDocument(QXmlStreamWriter &xml, const InterchangePlan &plan, const QString &directory, const Rate &rate)
{
    xml.writeStartDocument();
    xml.writeDTD("<!DOCTYPE xmeml>");
    xml.writeStartElement("xmeml");
    xml.writeAttribute("version", "5");
    xml.writeStartElement("sequence");
    xml.writeAttribute("id", "iisc-sequence");
    xml.writeTextElement("name", text(plan.name));
    xml.writeTextElement("duration", number(plan.frameCount));
    legacyRate(xml, rate);
    xml.writeTextElement("in", "0");
    xml.writeTextElement("out", number(plan.frameCount));
    xml.writeStartElement("timecode");
    legacyRate(xml, rate);
    xml.writeTextElement("frame", "0");
    xml.writeTextElement("displayformat", "NDF");
    xml.writeEndElement();
    xml.writeStartElement("media");
    xml.writeStartElement("video");
    xml.writeStartElement("format");
    legacySample(xml, plan.extent, rate);
    xml.writeEndElement();
    std::vector<bool> emitted(plan.media.size(), false);
    std::uint64_t clipNumber = 1;
    for (const auto &track : plan.tracks) {
        xml.writeStartElement("track");
        for (const auto &clip : track.clips) {
            if (xml.hasError()) { return; }
            xml.writeStartElement("clipitem");
            xml.writeAttribute("id", QStringLiteral("clip-") + number(clipNumber++));
            xml.writeTextElement("name", text(track.name));
            xml.writeTextElement("duration", number(plan.frameCount));
            legacyRate(xml, rate);
            xml.writeTextElement("start", number(clip.start));
            xml.writeTextElement("end", number(std::uint64_t(clip.start) + clip.duration));
            xml.writeTextElement("in", "0");
            xml.writeTextElement("out", number(clip.duration));
            xml.writeTextElement("enabled", track.visible ? "TRUE" : "FALSE");
            xml.writeTextElement("stillframe", "TRUE");
            xml.writeTextElement("alphatype", "straight");
            xml.writeTextElement("anamorphic", "FALSE");
            legacyFile(xml, plan, clip, directory, rate, !emitted[clip.mediaIndex]);
            emitted[clip.mediaIndex] = true;
            xml.writeTextElement("compositemode", QString::fromLatin1(legacyBlend(track.blendMode)));
            legacyOpacity(xml, track.opacity);
            xml.writeEndElement();
        }
        xml.writeTextElement("enabled", track.visible ? "TRUE" : "FALSE");
        xml.writeTextElement("locked", "FALSE");
        xml.writeEndElement();
    }
    xml.writeEndElement(); // video
    xml.writeEndElement(); // media
    xml.writeEndElement(); // sequence
    xml.writeEndElement(); // xmeml
    xml.writeEndDocument();
}

QString assetId(std::size_t index) { return QStringLiteral("r") + number(index + 3); }

void fcpxmlFormat(QXmlStreamWriter &xml, const InterchangePlan &plan, const Rate &rate, bool still)
{
    xml.writeEmptyElement("format");
    xml.writeAttribute("id", still ? "r2" : "r1");
    if (still) { xml.writeAttribute("name", "FFVideoFormatRateUndefined"); }
    else {
        xml.writeAttribute("frameDuration", time(1, rate));
        xml.writeAttribute("fieldOrder", "progressive");
        xml.writeAttribute("colorSpace", "1-1-1 (Rec. 709)");
    }
    xml.writeAttribute("width", number(std::uint32_t(plan.extent.width)));
    xml.writeAttribute("height", number(std::uint32_t(plan.extent.height)));
    xml.writeAttribute("paspH", "1");
    xml.writeAttribute("paspV", "1");
}

void fcpxmlDocument(QXmlStreamWriter &xml, const InterchangePlan &plan, const QString &directory, const Rate &rate)
{
    xml.writeStartDocument();
    xml.writeDTD("<!DOCTYPE fcpxml>");
    xml.writeStartElement("fcpxml");
    xml.writeAttribute("version", "1.9");
    xml.writeStartElement("resources");
    fcpxmlFormat(xml, plan, rate, false);
    fcpxmlFormat(xml, plan, rate, true);
    for (std::size_t index = 0; index < plan.media.size(); ++index) {
        if (xml.hasError()) { return; }
        const auto &media = plan.media[index];
        xml.writeStartElement("asset");
        xml.writeAttribute("id", assetId(index));
        xml.writeAttribute("name", media.relativePath.section('/', -1));
        xml.writeAttribute("start", "0s");
        xml.writeAttribute("duration", "0s");
        xml.writeAttribute("hasVideo", "1");
        xml.writeAttribute("hasAudio", "0");
        xml.writeAttribute("videoSources", "1");
        xml.writeAttribute("format", "r2");
        xml.writeAttribute("colorSpaceOverride", "sRGB IEC61966-2.1");
        xml.writeEmptyElement("media-rep");
        xml.writeAttribute("kind", "original-media");
        xml.writeAttribute("src", mediaUrl(media, directory));
        xml.writeEndElement();
    }
    xml.writeEndElement(); // resources
    xml.writeStartElement("project");
    xml.writeAttribute("name", text(plan.name));
    xml.writeStartElement("sequence");
    xml.writeAttribute("format", "r1");
    xml.writeAttribute("duration", time(plan.frameCount, rate));
    xml.writeAttribute("tcStart", "0s");
    xml.writeAttribute("tcFormat", "NDF");
    xml.writeStartElement("spine");
    xml.writeStartElement("gap");
    xml.writeAttribute("name", "iisc Timeline");
    xml.writeAttribute("offset", "0s");
    xml.writeAttribute("start", "0s");
    xml.writeAttribute("duration", time(plan.frameCount, rate));
    for (std::size_t index = 0; index < plan.tracks.size(); ++index) {
        const auto &track = plan.tracks[index];
        for (const auto &clip : track.clips) {
            if (xml.hasError()) { return; }
            // A timeless image must be a video inside a clip, not an asset-clip.
            // The wrapper owns timing, lane, opacity and blend independently of
            // the image's straight-alpha pixels. Its local source time is zero.
            xml.writeStartElement("clip");
            xml.writeAttribute("name", text(track.name));
            xml.writeAttribute("lane", number(index + 1));
            xml.writeAttribute("offset", time(clip.start, rate));
            xml.writeAttribute("start", "0s");
            xml.writeAttribute("duration", time(clip.duration, rate));
            xml.writeAttribute("enabled", track.visible ? "1" : "0");
            xml.writeAttribute("format", "r1");
            xml.writeEmptyElement("adjust-blend");
            xml.writeAttribute("amount", QString::number(track.opacity, 'g', 17));
            xml.writeAttribute("mode", QString::fromLatin1(fcpxmlBlend(track.blendMode)));
            xml.writeStartElement("video");
            xml.writeAttribute("ref", assetId(clip.mediaIndex));
            xml.writeAttribute("offset", "0s");
            xml.writeAttribute("start", "0s");
            xml.writeAttribute("duration", time(clip.duration, rate));
            xml.writeEmptyElement("adjust-conform");
            xml.writeAttribute("type", "none");
            xml.writeEndElement(); // video
            xml.writeEndElement(); // clip
        }
    }
    xml.writeEndElement(); // gap
    xml.writeEndElement(); // spine
    xml.writeEndElement(); // sequence
    xml.writeEndElement(); // project
    xml.writeEndElement(); // fcpxml
    xml.writeEndDocument();
}

template <typename Encode>
MediaIoResult writeXml(QByteArray &output, std::uint64_t limit, Encode encode)
{
    media_detail::BoundedBuffer buffer(&output, limit);
    if (!buffer.open(QIODevice::WriteOnly)) {
        return media_detail::error(MediaIoCode::IoError, QStringLiteral("Cannot create timeline XML buffer."));
    }
    QXmlStreamWriter writer(&buffer);
    writer.setAutoFormatting(true);
    encode(writer);
    if (buffer.exceeded) { return limitError(); }
    if (writer.hasError()) { return invalid(QStringLiteral("Cannot encode timeline XML text.")); }
    return {};
}

} // namespace

TimelineXmlResult encodeTimelineXml(const InterchangePlan &plan, const QString &finalDirectory,
                                    std::uint64_t maxBytes)
{
    TimelineXmlResult result;
    try {
        const auto limit = std::min(maxBytes, std::uint64_t(std::numeric_limits<qsizetype>::max()));
        Rate rate;
        result.result = validatePlan(plan, finalDirectory, limit, rate);
        if (!result.result.ok()) { return result; }
        result.result = writeXml(result.legacyXml, limit, [&](auto &xml) {
            legacyDocument(xml, plan, finalDirectory, rate);
        });
        if (result.result.ok()) {
            result.result = writeXml(result.fcpxml, limit - std::uint64_t(result.legacyXml.size()), [&](auto &xml) {
                fcpxmlDocument(xml, plan, finalDirectory, rate);
            });
        }
    } catch (const std::bad_alloc &) {
        result.result = limitError();
    } catch (const std::length_error &) {
        result.result = limitError();
    }
    if (!result.result.ok()) { result.legacyXml.clear(); result.fcpxml.clear(); }
    return result;
}

} // namespace iiSharedCanvas::timeline_detail
