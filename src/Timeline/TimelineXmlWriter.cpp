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

QString rationalTime(std::uint64_t numerator, std::uint32_t denominator)
{
    const auto divisor = std::gcd(numerator, std::uint64_t(denominator));
    const auto reducedNumerator = numerator / divisor;
    const auto reducedDenominator = denominator / divisor;
    return number(reducedNumerator)
        + (reducedDenominator == 1 ? QString{} : QStringLiteral("/") + number(reducedDenominator))
        + QStringLiteral("s");
}

QString time(FrameIndex frames, const Rate &rate)
{
    return rationalTime(std::uint64_t(frames) * rate.denominator, rate.numerator);
}

bool safeMediaPath(const QString &path)
{
    if (path.isEmpty() || !xmlText(path) || QDir::isAbsolutePath(path)
        || path.contains('\\') || path.contains(':') || path.endsWith('/')) { return false; }
    const auto components = path.split('/');
    return std::none_of(components.begin(), components.end(), [](const auto &component) {
        return component.isEmpty() || component == "." || component == "..";
    });
}

bool checkedProduct(std::uint64_t a, std::uint64_t b, std::uint64_t &result)
{
    if (b && a > std::numeric_limits<std::uint64_t>::max() / b) { return false; }
    result = a * b;
    return true;
}

// Reduced numerator/denominator for converting a sample frame to a video frame.
std::pair<std::uint64_t, std::uint64_t> audioFrameRatio(const InterchangeAudioMedia &media, const Rate &rate)
{
    const auto denominator = std::uint64_t(media.sampleRate) * rate.denominator;
    const auto divisor = std::gcd(denominator, std::uint64_t(rate.numerator));
    return {rate.numerator / divisor, denominator / divisor};
}

std::uint64_t audioSourceFrames(const InterchangeAudioMedia &media, const Rate &rate)
{
    const auto [numerator, denominator] = audioFrameRatio(media, rate);
    const auto scaled = media.sampleFrameCount * numerator; // checked during validation
    return scaled / denominator + (scaled % denominator != 0);
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
        || plan.media.size() > maxBytes / 64 || !charge(plan.media.size() * 64)
        || plan.audioTracks.size() > maxBytes / 32 || !charge(plan.audioTracks.size() * 32)
        || plan.audioMedia.size() > maxBytes / 64 || !charge(plan.audioMedia.size() * 64)) { return limitError(); }
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
        if (!safeMediaPath(media.relativePath)) {
            return invalid(QStringLiteral("Timeline media must use safe relative paths inside the package."));
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
    for (const auto &media : plan.audioMedia) {
        if (!charge(std::uint64_t(media.relativePath.size()))) { return limitError(); }
        if (!safeMediaPath(media.relativePath) || media.sampleRate < 8000 || media.sampleRate > 192000
            || (media.channelCount != 1 && media.channelCount != 2) || !media.sampleFrameCount) {
            return invalid(QStringLiteral("Timeline audio requires safe WAV paths, sample rates, and nonempty mono/stereo sources."));
        }
        const auto [numerator, denominator] = audioFrameRatio(media, rate);
        std::uint64_t scaled;
        if (!checkedProduct(media.sampleFrameCount, numerator, scaled)) {
            return invalid(QStringLiteral("Timeline audio sample duration exceeds exact XML timing capacity."));
        }
    }
    for (const auto &track : plan.audioTracks) {
        if (!charge(track.name.size()) || track.clips.size() > maxBytes / 128
            || !charge(track.clips.size() * 128)) { return limitError(); }
        if (!utf8XmlText(track.name) || !std::isfinite(track.gainDb) || track.gainDb < -96 || track.gainDb > 24) {
            return invalid(QStringLiteral("Timeline audio track names and gain must be valid XML values."));
        }
        std::uint64_t previousEnd = 0;
        for (const auto &clip : track.clips) {
            if (!charge(clip.name.size())) { return limitError(); }
            const auto end = std::uint64_t(clip.start) + clip.duration;
            if (!clip.duration || clip.mediaIndex >= plan.audioMedia.size() || clip.start < previousEnd
                || end > plan.frameCount || !utf8XmlText(clip.name)
                || !std::isfinite(clip.gainDb) || clip.gainDb < -96 || clip.gainDb > 24) {
                return invalid(QStringLiteral("Timeline audio clips must be ordered, in range, and have valid media, names and gain."));
            }
            const auto &media = plan.audioMedia[clip.mediaIndex];
            const auto [numerator, denominator] = audioFrameRatio(media, rate);
            if (clip.sourceOffsetSamples % denominator) {
                return media_detail::error(MediaIoCode::UnsupportedFeature,
                    QStringLiteral("Legacy audio source trim must align to exact timeline frames; remap the WAV prefix first."));
            }
            std::uint64_t requested, available;
            if (clip.sourceOffsetSamples > media.sampleFrameCount
                || !checkedProduct(clip.duration, denominator, requested)
                || !checkedProduct(media.sampleFrameCount - clip.sourceOffsetSamples, numerator, available)
                || requested > available) {
                return invalid(QStringLiteral("Timeline audio source range must fit its exact sample-clock duration."));
            }
            previousEnd = end;
        }
    }
    return {};
}

template<typename Media>
QString mediaUrl(const Media &media, const QString &directory)
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

QString audioClipName(const InterchangeAudioTrack &track, const InterchangeAudioClip &clip)
{
    return clip.name.empty() ? text(track.name) : text(track.name) + QStringLiteral(" / ") + text(clip.name);
}

void legacyAudioSample(QXmlStreamWriter &xml, std::uint32_t sampleRate)
{
    xml.writeStartElement("samplecharacteristics");
    xml.writeTextElement("depth", "16");
    xml.writeTextElement("samplerate", number(sampleRate));
    xml.writeEndElement();
}

void legacyAudioFile(QXmlStreamWriter &xml, const InterchangeAudioMedia &media,
                     std::size_t index, const QString &directory, const Rate &rate, bool first)
{
    xml.writeStartElement("file");
    xml.writeAttribute("id", QStringLiteral("audio-file-") + number(index + 1));
    if (first) {
        xml.writeTextElement("name", media.relativePath.section('/', -1));
        xml.writeTextElement("pathurl", mediaUrl(media, directory));
        legacyRate(xml, rate);
        xml.writeTextElement("duration", number(audioSourceFrames(media, rate)));
        xml.writeStartElement("media");
        xml.writeStartElement("audio");
        legacyAudioSample(xml, media.sampleRate);
        xml.writeTextElement("channelcount", number(media.channelCount));
        xml.writeTextElement("layout", media.channelCount == 2 ? "stereo" : "mono");
        for (std::uint16_t channel = 1; channel <= media.channelCount; ++channel) {
            xml.writeStartElement("audiochannel");
            xml.writeTextElement("channellabel", media.channelCount == 1 ? "discrete" : channel == 1 ? "left" : "right");
            xml.writeTextElement("sourcechannel", number(channel));
            xml.writeEndElement();
        }
        xml.writeEndElement(); // audio
        xml.writeEndElement(); // media
    }
    xml.writeEndElement(); // file
}

void legacyAudioGain(QXmlStreamWriter &xml, double gainDb)
{
    xml.writeStartElement("filter");
    xml.writeTextElement("enabled", "TRUE");
    xml.writeStartElement("effect");
    xml.writeTextElement("name", "Audio Levels");
    xml.writeTextElement("effectid", "audiolevels");
    xml.writeTextElement("effectcategory", "audiolevels");
    xml.writeTextElement("effecttype", "audiolevels");
    xml.writeTextElement("mediatype", "audio");
    xml.writeStartElement("parameter");
    xml.writeTextElement("parameterid", "level");
    xml.writeTextElement("name", "Level");
    xml.writeTextElement("value", QString::number(std::pow(10.0, gainDb / 20.0), 'g', 17));
    xml.writeEndElement();
    xml.writeEndElement();
    xml.writeEndElement();
}

void legacyAudioPan(QXmlStreamWriter &xml, int pan)
{
    xml.writeStartElement("filter");
    xml.writeTextElement("enabled", "TRUE");
    xml.writeStartElement("effect");
    xml.writeTextElement("name", "Audio Pan");
    xml.writeTextElement("effectid", "audiopan");
    xml.writeTextElement("effectcategory", "audiopan");
    xml.writeTextElement("effecttype", "audiopan");
    xml.writeTextElement("mediatype", "audio");
    xml.writeStartElement("parameter");
    xml.writeTextElement("parameterid", "pan");
    xml.writeTextElement("name", "Pan");
    xml.writeTextElement("value", QString::number(pan));
    xml.writeEndElement();
    xml.writeEndElement();
    xml.writeEndElement();
}

QString audioClipId(std::size_t track, std::size_t clip, std::uint16_t channel)
{
    return QStringLiteral("audio-clip-") + number(track + 1) + '-' + number(clip + 1) + '-' + number(channel);
}

void legacyAudio(QXmlStreamWriter &xml, const InterchangePlan &plan, const QString &directory, const Rate &rate)
{
    if (plan.audioTracks.empty()) { return; }
    xml.writeStartElement("audio");
    xml.writeStartElement("format");
    legacyAudioSample(xml, 48000);
    xml.writeEndElement();
    xml.writeStartElement("outputs");
    xml.writeStartElement("group");
    xml.writeTextElement("index", "1");
    xml.writeTextElement("numchannels", "2");
    xml.writeTextElement("downmix", "0");
    for (const auto channel : {1, 2}) {
        xml.writeStartElement("channel");
        xml.writeTextElement("index", number(channel));
        xml.writeEndElement();
    }
    xml.writeEndElement(); // group
    xml.writeEndElement(); // outputs
    std::vector<bool> emitted(plan.audioMedia.size(), false);
    std::size_t physicalTrack = 1;
    for (std::size_t trackIndex = 0; trackIndex < plan.audioTracks.size(); ++trackIndex) {
        const auto &track = plan.audioTracks[trackIndex];
        std::uint16_t channels = 1;
        for (const auto &clip : track.clips) { channels = std::max(channels, plan.audioMedia[clip.mediaIndex].channelCount); }
        for (std::uint16_t channel = 1; channel <= channels; ++channel) {
            xml.writeStartElement("track");
            std::size_t stereoClipIndex = 0;
            for (std::size_t clipIndex = 0; clipIndex < track.clips.size(); ++clipIndex) {
                if (xml.hasError()) { return; }
                const auto &clip = track.clips[clipIndex];
                const auto &media = plan.audioMedia[clip.mediaIndex];
                stereoClipIndex += media.channelCount == 2;
                if (channel > media.channelCount) { continue; }
                const auto [numerator, denominator] = audioFrameRatio(media, rate);
                const auto sourceIn = (clip.sourceOffsetSamples / denominator) * numerator;
                xml.writeStartElement("clipitem");
                xml.writeAttribute("id", audioClipId(trackIndex, clipIndex, channel));
                xml.writeTextElement("name", audioClipName(track, clip));
                xml.writeTextElement("duration", number(audioSourceFrames(media, rate)));
                legacyRate(xml, rate);
                xml.writeTextElement("start", number(clip.start));
                xml.writeTextElement("end", number(std::uint64_t(clip.start) + clip.duration));
                xml.writeTextElement("in", number(sourceIn));
                xml.writeTextElement("out", number(sourceIn + clip.duration));
                xml.writeTextElement("enabled", !track.muted && clip.enabled ? "TRUE" : "FALSE");
                legacyAudioFile(xml, media, clip.mediaIndex, directory, rate, !emitted[clip.mediaIndex]);
                emitted[clip.mediaIndex] = true;
                xml.writeStartElement("sourcetrack");
                xml.writeTextElement("mediatype", "audio");
                xml.writeTextElement("trackindex", number(channel));
                xml.writeEndElement();
                if (media.channelCount == 2) {
                    for (std::uint16_t linkedChannel = 1; linkedChannel <= 2; ++linkedChannel) {
                        xml.writeStartElement("link");
                        xml.writeTextElement("linkclipref", audioClipId(trackIndex, clipIndex, linkedChannel));
                        xml.writeTextElement("mediatype", "audio");
                        xml.writeTextElement("trackindex", number(physicalTrack + linkedChannel - 1));
                        // Mono clips are absent from the right-channel track.
                        xml.writeTextElement("clipindex", number(linkedChannel == 1 ? clipIndex + 1 : stereoClipIndex));
                        xml.writeTextElement("groupindex", "1");
                        xml.writeEndElement();
                    }
                }
                legacyAudioGain(xml, track.gainDb + clip.gainDb);
                legacyAudioPan(xml, media.channelCount == 1 ? 0 : channel == 1 ? -1 : 1);
                xml.writeStartElement("logginginfo");
                xml.writeTextElement("description", text(track.name));
                xml.writeTextElement("lognote", text(clip.name));
                xml.writeEndElement();
                xml.writeEndElement(); // clipitem
            }
            xml.writeTextElement("enabled", track.muted ? "FALSE" : "TRUE");
            xml.writeTextElement("locked", "FALSE");
            xml.writeEndElement(); // track
        }
        physicalTrack += channels;
    }
    xml.writeEndElement(); // audio
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
    legacyAudio(xml, plan, directory, rate);
    xml.writeEndElement(); // media
    xml.writeEndElement(); // sequence
    xml.writeEndElement(); // xmeml
    xml.writeEndDocument();
}

QString assetId(std::size_t index) { return QStringLiteral("r") + number(index + 3); }
QString audioAssetId(std::size_t index) { return QStringLiteral("a") + number(index + 1); }

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
    for (std::size_t index = 0; index < plan.audioMedia.size(); ++index) {
        if (xml.hasError()) { return; }
        const auto &media = plan.audioMedia[index];
        xml.writeStartElement("asset");
        xml.writeAttribute("id", audioAssetId(index));
        xml.writeAttribute("name", media.relativePath.section('/', -1));
        xml.writeAttribute("start", "0s");
        xml.writeAttribute("duration", rationalTime(media.sampleFrameCount, media.sampleRate));
        xml.writeAttribute("hasVideo", "0");
        xml.writeAttribute("hasAudio", "1");
        xml.writeAttribute("audioSources", "1");
        xml.writeAttribute("audioChannels", number(media.channelCount));
        xml.writeAttribute("audioRate", number(media.sampleRate));
        xml.writeEmptyElement("media-rep");
        xml.writeAttribute("kind", "original-media");
        xml.writeAttribute("src", mediaUrl(media, directory));
        xml.writeEndElement(); // asset
    }
    xml.writeEndElement(); // resources
    xml.writeStartElement("project");
    xml.writeAttribute("name", text(plan.name));
    xml.writeStartElement("sequence");
    xml.writeAttribute("format", "r1");
    xml.writeAttribute("duration", time(plan.frameCount, rate));
    xml.writeAttribute("tcStart", "0s");
    xml.writeAttribute("tcFormat", "NDF");
    if (!plan.audioTracks.empty()) {
        xml.writeAttribute("audioLayout", "stereo");
        xml.writeAttribute("audioRate", "48k");
    }
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
    for (std::size_t index = 0; index < plan.audioTracks.size(); ++index) {
        const auto &track = plan.audioTracks[index];
        for (const auto &clip : track.clips) {
            if (xml.hasError()) { return; }
            const auto &media = plan.audioMedia[clip.mediaIndex];
            xml.writeStartElement("asset-clip");
            xml.writeAttribute("ref", audioAssetId(clip.mediaIndex));
            xml.writeAttribute("name", audioClipName(track, clip));
            xml.writeAttribute("lane", QStringLiteral("-") + number(index + 1));
            xml.writeAttribute("offset", time(clip.start, rate));
            xml.writeAttribute("start", rationalTime(clip.sourceOffsetSamples, media.sampleRate));
            xml.writeAttribute("duration", time(clip.duration, rate));
            xml.writeAttribute("enabled", !track.muted && clip.enabled ? "1" : "0");
            xml.writeAttribute("srcEnable", "audio");
            xml.writeAttribute("audioRole", QStringLiteral("dialogue.iisc-track-") + number(index + 1));
            xml.writeEmptyElement("adjust-volume");
            xml.writeAttribute("amount", QString::number(track.gainDb + clip.gainDb, 'g', 17) + QStringLiteral("dB"));
            if (media.channelCount == 2) {
                xml.writeEmptyElement("audio-channel-source");
                xml.writeAttribute("srcCh", "1,2");
                xml.writeAttribute("outCh", "L,R");
            }
            xml.writeStartElement("metadata");
            for (const auto &entry : {std::pair{"org.iisacc.iiSharedCanvas.audioTrack.name", track.name},
                                      std::pair{"org.iisacc.iiSharedCanvas.audioClip.name", clip.name}}) {
                xml.writeEmptyElement("md");
                xml.writeAttribute("key", QString::fromLatin1(entry.first));
                xml.writeAttribute("value", text(entry.second));
            }
            xml.writeEndElement(); // metadata
            xml.writeEndElement(); // asset-clip
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
