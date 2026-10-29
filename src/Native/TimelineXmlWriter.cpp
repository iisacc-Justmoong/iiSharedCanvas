#include "TimelineXmlWriter_p.hpp"

#include "Media_p.hpp"
#include <iiSDKCore/Core/Text/Number.h>

#include <algorithm>
#include <cmath>
#include <filesystem>
#include <limits>
#include <new>
#include <numeric>
#include <stdexcept>
#include <string_view>

namespace iiSharedCanvas::timeline_detail {
namespace {

struct Rate {
    std::uint32_t numerator = 0, denominator = 1, timebase = 0;
    bool ntsc = false;
};

MediaIoResult invalid(std::string message)
{
    return media_detail::error(MediaIoCode::InvalidArgument, std::move(message));
}
MediaIoResult limitError()
{
    return media_detail::error(MediaIoCode::LimitExceeded, "Combined timeline XML exceeds the output byte limit.");
}

// All caller-provided strings are canonical UTF-8 and XML 1.0 characters.
bool utf8XmlText(std::string_view value)
{
    if (!media_detail::validText(value)) return false;
    for (std::size_t index = 0; index < value.size();) {
        const auto first = std::uint8_t(value[index++]);
        std::uint32_t code = first;
        if (first >= 0x80) {
            const auto count = first < 0xe0 ? 1 : first < 0xf0 ? 2 : 3;
            code &= (1U << (6 - count)) - 1;
            for (int part = 0; part < count; ++part) code = (code << 6) | (std::uint8_t(value[index++]) & 63);
        }
        if ((code < 0x20 && code != 9 && code != 10 && code != 13) || code == 0xfffe || code == 0xffff)
            return false;
    }
    return true;
}
std::string text(const std::string &value) { return value; }
std::string number(std::uint64_t value) { return std::to_string(value); }
std::string decimal(double value) { return ii::core::format_decimal(value); }
std::string rationalTime(std::uint64_t numerator, std::uint32_t denominator)
{
    const auto divisor = std::gcd(numerator, std::uint64_t(denominator));
    return number(numerator / divisor)
        + (denominator / divisor == 1 ? std::string{} : "/" + number(denominator / divisor)) + "s";
}
std::string time(FrameIndex frames, const Rate &rate)
{
    return rationalTime(std::uint64_t(frames) * rate.denominator, rate.numerator);
}
bool safeMediaPath(std::string_view path)
{
    if (path.empty() || !utf8XmlText(path) || path.front() == '/' || path.back() == '/'
        || path.find_first_of("\\:") != path.npos) return false;
    while (!path.empty()) {
        const auto end = path.find('/'); const auto part = path.substr(0, end);
        if (part.empty() || part == "." || part == "..") return false;
        if (end == path.npos) break;
        path.remove_prefix(end + 1);
    }
    return true;
}
bool checkedProduct(std::uint64_t a, std::uint64_t b, std::uint64_t &result)
{
    if (b && a > std::numeric_limits<std::uint64_t>::max() / b) return false;
    result = a * b; return true;
}
std::pair<std::uint64_t, std::uint64_t> audioFrameRatio(const InterchangeAudioMedia &media, const Rate &rate)
{
    const auto denominator = std::uint64_t(media.sampleRate) * rate.denominator;
    const auto divisor = std::gcd(denominator, std::uint64_t(rate.numerator));
    return {rate.numerator / divisor, denominator / divisor};
}
std::uint64_t audioSourceFrames(const InterchangeAudioMedia &media, const Rate &rate)
{
    const auto [numerator, denominator] = audioFrameRatio(media, rate);
    const auto scaled = media.sampleFrameCount * numerator;
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
MediaIoResult validatePlan(const InterchangePlan &plan, const std::string &directory,
                           std::uint64_t maxBytes, Rate &rate)
{
    if (!plan.frameRate.numerator || !plan.frameRate.denominator || !plan.frameCount
        || plan.extent.width <= 0 || plan.extent.height <= 0)
        return invalid("Timeline XML requires positive dimensions, duration, and frame rate.");
    if (!utf8XmlText(directory) || !std::filesystem::path(std::u8string(directory.begin(), directory.end())).is_absolute())
        return invalid("Timeline XML requires an absolute, XML-safe final directory.");
    std::uint64_t minimumBytes = 0;
    const auto charge = [&](std::uint64_t count) {
        if (count > maxBytes - minimumBytes) return false;
        minimumBytes += count; return true;
    };
    if (!charge(directory.size()) || !charge(plan.name.size())
        || plan.tracks.size() > maxBytes / 32 || !charge(plan.tracks.size() * 32)
        || plan.media.size() > maxBytes / 64 || !charge(plan.media.size() * 64)
        || plan.audioTracks.size() > maxBytes / 32 || !charge(plan.audioTracks.size() * 32)
        || plan.audioMedia.size() > maxBytes / 64 || !charge(plan.audioMedia.size() * 64)) return limitError();
    if (!utf8XmlText(plan.name)) return invalid("Timeline sequence name is not valid UTF-8/XML text.");
    const auto divisor = std::gcd(plan.frameRate.numerator, plan.frameRate.denominator);
    rate.numerator = plan.frameRate.numerator / divisor; rate.denominator = plan.frameRate.denominator / divisor;
    if (rate.denominator == 1) rate.timebase = rate.numerator;
    else if ((std::uint64_t(rate.numerator) * 1001) % (std::uint64_t(rate.denominator) * 1000) == 0) {
        const auto timebase = (std::uint64_t(rate.numerator) * 1001) / (std::uint64_t(rate.denominator) * 1000);
        if (timebase > std::numeric_limits<std::uint32_t>::max()) return invalid("Timeline timebase exceeds its field.");
        rate.timebase = std::uint32_t(timebase); rate.ntsc = true;
    } else return media_detail::error(MediaIoCode::UnsupportedFeature,
        "Legacy timeline XML requires an integer frame rate or an exact integer*1000/1001 rate.");
    for (const auto &media : plan.media) {
        if (!charge(media.relativePath.size())) return limitError();
        if (!safeMediaPath(media.relativePath)) return invalid("Timeline media must use safe relative paths inside the package.");
    }
    for (const auto &track : plan.tracks) {
        if (!charge(track.name.size()) || track.clips.size() > maxBytes / 64 || !charge(track.clips.size() * 64))
            return limitError();
        if (!utf8XmlText(track.name) || !std::isfinite(track.opacity) || track.opacity < 0 || track.opacity > 1)
            return invalid("Timeline layer names and opacity must be valid XML values.");
        if (!legacyBlend(track.blendMode)) return media_detail::error(MediaIoCode::UnsupportedFeature,
            "Timeline XML supports only normal, multiply, screen, and overlay compositing.");
        std::uint64_t previousEnd = 0;
        for (const auto &clip : track.clips) {
            const auto end = std::uint64_t(clip.start) + clip.duration;
            if (!clip.duration || clip.mediaIndex >= plan.media.size() || clip.start < previousEnd || end > plan.frameCount)
                return invalid("Timeline holds must be ordered, non-overlapping, in range, and reference existing media.");
            previousEnd = end;
        }
    }
    for (const auto &media : plan.audioMedia) {
        if (!charge(media.relativePath.size())) return limitError();
        if (!safeMediaPath(media.relativePath) || media.sampleRate < 8000 || media.sampleRate > 192000
            || (media.channelCount != 1 && media.channelCount != 2) || !media.sampleFrameCount)
            return invalid("Timeline audio requires safe WAV paths, sample rates, and nonempty mono/stereo sources.");
        const auto [numerator, denominator] = audioFrameRatio(media, rate);
        std::uint64_t scaled;
        if (!checkedProduct(media.sampleFrameCount, numerator, scaled))
            return invalid("Timeline audio sample duration exceeds exact XML timing capacity.");
    }
    for (const auto &track : plan.audioTracks) {
        if (!charge(track.name.size()) || track.clips.size() > maxBytes / 128 || !charge(track.clips.size() * 128))
            return limitError();
        if (!utf8XmlText(track.name) || !std::isfinite(track.gainDb) || track.gainDb < -96 || track.gainDb > 24)
            return invalid("Timeline audio track names and gain must be valid XML values.");
        std::uint64_t previousEnd = 0;
        for (const auto &clip : track.clips) {
            if (!charge(clip.name.size())) return limitError();
            const auto end = std::uint64_t(clip.start) + clip.duration;
            if (!clip.duration || clip.mediaIndex >= plan.audioMedia.size() || clip.start < previousEnd
                || end > plan.frameCount || !utf8XmlText(clip.name) || !std::isfinite(clip.gainDb)
                || clip.gainDb < -96 || clip.gainDb > 24)
                return invalid("Timeline audio clips must be ordered, in range, and have valid media, names and gain.");
            const auto &media = plan.audioMedia[clip.mediaIndex];
            const auto [numerator, denominator] = audioFrameRatio(media, rate);
            if (clip.sourceOffsetSamples % denominator)
                return media_detail::error(MediaIoCode::UnsupportedFeature,
                    "Legacy audio source trim must align to exact timeline frames; remap the WAV prefix first.");
            std::uint64_t requested, available;
            if (clip.sourceOffsetSamples > media.sampleFrameCount || !checkedProduct(clip.duration, denominator, requested)
                || !checkedProduct(media.sampleFrameCount - clip.sourceOffsetSamples, numerator, available)
                || requested > available)
                return invalid("Timeline audio source range must fit its exact sample-clock duration.");
            previousEnd = end;
        }
    }
    return {};
}

std::string basename(const std::string &path)
{
    const auto separator = path.find_last_of('/'); return separator == path.npos ? path : path.substr(separator + 1);
}
template<typename Media>
std::string mediaUrl(const Media &media, const std::string &directory, std::uint64_t limit)
{
    const auto joined = std::filesystem::path(std::u8string(directory.begin(), directory.end()))
        / std::filesystem::path(std::u8string(media.relativePath.begin(), media.relativePath.end()));
    const auto utf8 = joined.generic_u8string();
    std::string path(utf8.begin(), utf8.end());
    std::string url;
#ifdef _WIN32
    url = path.starts_with("//") ? "file:" : "file:///";
#else
    url = "file://";
#endif
    if (url.size() > limit) throw std::length_error("timeline media URL exceeds its byte budget");
    constexpr char hex[] = "0123456789ABCDEF";
    for (const auto raw : path) {
        const auto ch = std::uint8_t(raw);
        const bool plain = (ch >= 'A' && ch <= 'Z') || (ch >= 'a' && ch <= 'z') || (ch >= '0' && ch <= '9')
            || ch == '-' || ch == '_' || ch == '.' || ch == '~' || ch == '/' || ch == ':';
        const auto count = plain ? 1U : 3U;
        if (count > limit - url.size()) throw std::length_error("timeline media URL exceeds its byte budget");
        if (plain) url.push_back(char(ch));
        else { url.push_back('%'); url.push_back(hex[ch >> 4]); url.push_back(hex[ch & 15]); }
    }
    return url;
}

// Restricted writer for fixed element names and prevalidated XML text. It owns
// its buffer, never resolves external entities and enforces the output budget.
class XmlWriter final {
public:
    explicit XmlWriter(std::uint64_t limit) : limit_(limit) {}
    void writeStartDocument() { append("<?xml version=\"1.0\" encoding=\"UTF-8\"?>\n"); }
    void writeDTD(std::string_view text) { append(text); append("\n"); }
    void writeStartElement(std::string_view name)
    {
        closePending(); append("<"); append(name); stack_.emplace_back(name); pending_ = Pending::Start;
    }
    void writeEmptyElement(std::string_view name)
    {
        closePending(); append("<"); append(name); pending_ = Pending::Empty;
    }
    void writeAttribute(std::string_view key, std::string_view value)
    {
        if (pending_ == Pending::None) throw std::logic_error("XML attribute has no open element");
        append(" "); append(key); append("=\""); escape(value, true); append("\"");
    }
    void writeTextElement(std::string_view name, std::string_view value)
    {
        writeStartElement(name); closePending(); escape(value, false); writeEndElement();
    }
    void writeEndElement()
    {
        if (pending_ == Pending::Empty) closePending();
        if (stack_.empty()) throw std::logic_error("XML element stack is empty");
        if (pending_ == Pending::Start) { append("/>"); pending_ = Pending::None; stack_.pop_back(); return; }
        append("</"); append(stack_.back()); append(">"); stack_.pop_back();
    }
    void writeEndDocument()
    {
        closePending(); if (!stack_.empty()) throw std::logic_error("XML document has unclosed elements");
        append("\n");
    }
    [[nodiscard]] bool hasError() const noexcept { return false; }
    [[nodiscard]] std::uint64_t remaining() const noexcept { return limit_ - output_.size(); }
    std::string take() { return std::move(output_); }
private:
    enum class Pending { None, Start, Empty };
    void append(std::string_view value)
    {
        if (output_.size() > limit_ || value.size() > limit_ - output_.size())
            throw std::length_error("timeline XML exceeds its byte budget");
        output_.append(value);
    }
    void closePending()
    {
        if (pending_ == Pending::Start) append(">");
        else if (pending_ == Pending::Empty) append("/>");
        pending_ = Pending::None;
    }
    void escape(std::string_view value, bool attribute)
    {
        for (const auto ch : value) {
            switch (ch) {
            case '&': append("&amp;"); break;
            case '<': append("&lt;"); break;
            case '>': append("&gt;"); break;
            case '"': if (attribute) append("&quot;"); else append({&ch, 1}); break;
            case '\r': append("&#xD;"); break;
            case '\n': if (attribute) append("&#xA;"); else append({&ch, 1}); break;
            case '\t': if (attribute) append("&#x9;"); else append({&ch, 1}); break;
            default: append({&ch, 1}); break;
            }
        }
    }
    std::uint64_t limit_;
    std::string output_;
    std::vector<std::string> stack_;
    Pending pending_ = Pending::None;
};

void legacyRate(XmlWriter &xml, const Rate &rate)
{
    xml.writeStartElement("rate");
    xml.writeTextElement("timebase", number(rate.timebase));
    xml.writeTextElement("ntsc", rate.ntsc ? "TRUE" : "FALSE");
    xml.writeEndElement();
}

void legacySample(XmlWriter &xml, CanvasExtent extent, const Rate &rate)
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

void legacyFile(XmlWriter &xml, const InterchangePlan &plan, const InterchangeClip &clip,
                const std::string &directory, const Rate &rate, bool first)
{
    xml.writeStartElement("file");
    xml.writeAttribute("id", std::string("file-") + number(clip.mediaIndex + 1));
    if (first) {
        const auto &media = plan.media[clip.mediaIndex];
        // Unlike edit positions and clipitem duration, legacy still-file and
        // file/video durations are whole minutes (Apple's still-frame example).
        const auto durationNumerator = std::uint64_t(plan.frameCount) * rate.denominator;
        const auto minuteDenominator = std::uint64_t(rate.numerator) * 60;
        const auto minutes = durationNumerator / minuteDenominator + (durationNumerator % minuteDenominator != 0);
        xml.writeTextElement("name", basename(media.relativePath));
        xml.writeTextElement("pathurl", mediaUrl(media, directory, xml.remaining()));
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

void legacyOpacity(XmlWriter &xml, double opacity)
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
    xml.writeTextElement("value", decimal(opacity * 100.0));
    xml.writeEndElement();
    xml.writeEndElement();
    xml.writeEndElement();
}

std::string audioClipName(const InterchangeAudioTrack &track, const InterchangeAudioClip &clip)
{
    return clip.name.empty() ? text(track.name) : text(track.name) + std::string(" / ") + text(clip.name);
}

void legacyAudioSample(XmlWriter &xml, std::uint32_t sampleRate)
{
    xml.writeStartElement("samplecharacteristics");
    xml.writeTextElement("depth", "16");
    xml.writeTextElement("samplerate", number(sampleRate));
    xml.writeEndElement();
}

void legacyAudioFile(XmlWriter &xml, const InterchangeAudioMedia &media,
                     std::size_t index, const std::string &directory, const Rate &rate, bool first)
{
    xml.writeStartElement("file");
    xml.writeAttribute("id", std::string("audio-file-") + number(index + 1));
    if (first) {
        xml.writeTextElement("name", basename(media.relativePath));
        xml.writeTextElement("pathurl", mediaUrl(media, directory, xml.remaining()));
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

void legacyAudioGain(XmlWriter &xml, double gainDb)
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
    xml.writeTextElement("value", decimal(std::pow(10.0, gainDb / 20.0)));
    xml.writeEndElement();
    xml.writeEndElement();
    xml.writeEndElement();
}

void legacyAudioPan(XmlWriter &xml, int pan)
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
    xml.writeTextElement("value", std::to_string(pan));
    xml.writeEndElement();
    xml.writeEndElement();
    xml.writeEndElement();
}

std::string audioClipId(std::size_t track, std::size_t clip, std::uint16_t channel)
{
    return std::string("audio-clip-") + number(track + 1) + '-' + number(clip + 1) + '-' + number(channel);
}

void legacyAudio(XmlWriter &xml, const InterchangePlan &plan, const std::string &directory, const Rate &rate)
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

void legacyDocument(XmlWriter &xml, const InterchangePlan &plan, const std::string &directory, const Rate &rate)
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
            xml.writeAttribute("id", std::string("clip-") + number(clipNumber++));
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
            xml.writeTextElement("compositemode", std::string(legacyBlend(track.blendMode)));
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

std::string assetId(std::size_t index) { return std::string("r") + number(index + 3); }
std::string audioAssetId(std::size_t index) { return std::string("a") + number(index + 1); }

void fcpxmlFormat(XmlWriter &xml, const InterchangePlan &plan, const Rate &rate, bool still)
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

void fcpxmlDocument(XmlWriter &xml, const InterchangePlan &plan, const std::string &directory, const Rate &rate)
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
        xml.writeAttribute("name", basename(media.relativePath));
        xml.writeAttribute("start", "0s");
        xml.writeAttribute("duration", "0s");
        xml.writeAttribute("hasVideo", "1");
        xml.writeAttribute("hasAudio", "0");
        xml.writeAttribute("videoSources", "1");
        xml.writeAttribute("format", "r2");
        xml.writeAttribute("colorSpaceOverride", "sRGB IEC61966-2.1");
        xml.writeEmptyElement("media-rep");
        xml.writeAttribute("kind", "original-media");
        xml.writeAttribute("src", mediaUrl(media, directory, xml.remaining()));
        xml.writeEndElement();
    }
    for (std::size_t index = 0; index < plan.audioMedia.size(); ++index) {
        if (xml.hasError()) { return; }
        const auto &media = plan.audioMedia[index];
        xml.writeStartElement("asset");
        xml.writeAttribute("id", audioAssetId(index));
        xml.writeAttribute("name", basename(media.relativePath));
        xml.writeAttribute("start", "0s");
        xml.writeAttribute("duration", rationalTime(media.sampleFrameCount, media.sampleRate));
        xml.writeAttribute("hasVideo", "0");
        xml.writeAttribute("hasAudio", "1");
        xml.writeAttribute("audioSources", "1");
        xml.writeAttribute("audioChannels", number(media.channelCount));
        xml.writeAttribute("audioRate", number(media.sampleRate));
        xml.writeEmptyElement("media-rep");
        xml.writeAttribute("kind", "original-media");
        xml.writeAttribute("src", mediaUrl(media, directory, xml.remaining()));
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
            xml.writeAttribute("amount", decimal(track.opacity));
            xml.writeAttribute("mode", std::string(fcpxmlBlend(track.blendMode)));
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
            xml.writeAttribute("lane", std::string("-") + number(index + 1));
            xml.writeAttribute("offset", time(clip.start, rate));
            xml.writeAttribute("start", rationalTime(clip.sourceOffsetSamples, media.sampleRate));
            xml.writeAttribute("duration", time(clip.duration, rate));
            xml.writeAttribute("enabled", !track.muted && clip.enabled ? "1" : "0");
            xml.writeAttribute("srcEnable", "audio");
            xml.writeAttribute("audioRole", std::string("dialogue.iisc-track-") + number(index + 1));
            xml.writeEmptyElement("adjust-volume");
            xml.writeAttribute("amount", decimal(track.gainDb + clip.gainDb) + std::string("dB"));
            if (media.channelCount == 2) {
                xml.writeEmptyElement("audio-channel-source");
                xml.writeAttribute("srcCh", "1,2");
                xml.writeAttribute("outCh", "L,R");
            }
            xml.writeStartElement("metadata");
            for (const auto &entry : {std::pair{"org.iisacc.iiSharedCanvas.audioTrack.name", track.name},
                                      std::pair{"org.iisacc.iiSharedCanvas.audioClip.name", clip.name}}) {
                xml.writeEmptyElement("md");
                xml.writeAttribute("key", std::string(entry.first));
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
MediaIoResult writeXml(std::string &output, std::uint64_t limit, Encode encode)
{
    XmlWriter writer(limit);
    encode(writer);
    output = writer.take();
    return {};
}

} // namespace

bool validTimelineXmlText(std::string_view value)
{
    return utf8XmlText(value);
}

TimelineXmlResult encodeTimelineXml(const InterchangePlan &plan, const std::string &finalDirectory,
                                    std::uint64_t maxBytes)
{
    TimelineXmlResult result;
    try {
        const auto limit = std::min(maxBytes, std::uint64_t(std::numeric_limits<std::size_t>::max()));
        Rate rate;
        result.result = validatePlan(plan, finalDirectory, limit, rate);
        if (!result.result.ok()) return result;
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
    } catch (const std::exception &error) {
        result.result = invalid(error.what());
    }
    if (!result.result.ok()) { result.legacyXml.clear(); result.fcpxml.clear(); }
    return result;
}

} // namespace iiSharedCanvas::timeline_detail
