#include "Timeline/TimelineInterchange.h"
#include "TimelineXmlWriter_p.hpp"

#include "Audio/AudioCodec.h"
#include "Bitmap/BitmapCodec.h"
#include "Media_p.hpp"
#include "Render/FrameRenderer.h"
#include "Serialization/IiscCodec.h"
#include "Validation/Validation.h"
#include <iiFileProviderNative.h>
#include <iiFileProviderNativeDirectory.h>
#include <iiSDKCore/Core/Text/Number.h>

#include <algorithm>
#include <filesystem>
#include <initializer_list>
#include <limits>
#include <new>
#include <numeric>
#include <span>
#include <stdexcept>
#include <unordered_map>

namespace iiSharedCanvas {
namespace {
using namespace timeline_detail;
namespace fs = std::filesystem;
namespace provider = iiFileProvider::native;

std::string pathText(const fs::path &path)
{
    const auto value = path.u8string(); return {value.begin(), value.end()};
}
fs::path utf8Path(const std::string &path)
{
    return fs::path(std::u8string(path.begin(), path.end()));
}
struct Failure { MediaIoCode code; std::string message; };
[[noreturn]] void fail(MediaIoCode code, const std::string &message) { throw Failure{code, message}; }
void checked(const MediaIoResult &result) { if (!result.ok()) { fail(result.code, result.message); } }
void charge(std::uint64_t &used, std::uint64_t count, std::uint64_t limit)
{
    if (used > limit || count > limit - used) { fail(MediaIoCode::LimitExceeded, "timeline package exceeds its byte or object budget"); }
    used += count;
}
void warn(MediaIoResult &result, const std::string &message)
{
    if (std::find(result.warnings.begin(), result.warnings.end(), message) == result.warnings.end()) { result.warnings.push_back(message); }
}
std::uint64_t sourceSnapshotUpperBound(const Document &document, std::uint64_t limit)
{
    // encodeIisc checks its container size only after building the payload.
    // Count every native field, including unreferenced assets and metadata,
    // without allocating. Fixed allowances also cover its projection tables;
    // raw raster bytes and 64 bytes/command bound all current wire encodings.
    std::uint64_t used = 0;
    const auto items = [&](std::uint64_t count, std::uint64_t width) {
        if (width != 0 && count > (limit - used) / width) {
            fail(MediaIoCode::LimitExceeded, "source snapshot exceeds the export memory budget");
        }
        used += count * width;
    };
    const auto strings = [&](std::initializer_list<const std::string *> values) {
        for (const auto *value : values) { items(1, 4); items(value->size(), 1); }
    };
    const auto raster = [&](const RasterLayer &value) { items(1, 64); items(value.pixels.size(), 4); };
    items(1, 128); // Container/header, canvas, timeline and count fields.
    items(document.assets.size(), 256);
    for (const auto &asset : document.assets) {
        strings({&assetId(asset)});
        if (const auto *pixels = std::get_if<RasterAsset>(&asset)) { raster(pixels->pixels); }
        else if (const auto *chunks = std::get_if<ChunkedRasterAsset>(&asset)) {
            items(chunks->chunks.size(), 64);
            for (const auto &chunk : chunks->chunks) { raster(chunk.pixels); }
        } else {
            const auto &vector = std::get<VectorAsset>(asset);
            items(vector.paths.size(), 64);
            for (const auto &path : vector.paths) { items(path.commands.size(), 64); }
        }
    }
    items(document.layers.size(), 256);
    for (const auto &layer : document.layers) {
        const auto &properties = layerProperties(layer);
        strings({&properties.id, &properties.name});
        if (const auto *source = staticLayerSource(layer)) { strings({&source->assetId}); }
        else { items(keyframedLayerSource(layer)->frameIndices.size(), 8); }
    }
    items(document.audioAssets.size(), 64);
    for (const auto &asset : document.audioAssets) {
        strings({&asset.id}); items(asset.samples.size(), 2);
    }
    items(document.audioTracks.size(), 128);
    for (const auto &track : document.audioTracks) {
        strings({&track.id, &track.name}); items(track.clips.size(), 128);
        for (const auto &clip : track.clips) { strings({&clip.id, &clip.name, &clip.assetId}); }
    }
    items(document.frames.size(), 64);
    for (const auto &frame : document.frames) {
        items(frame.keyframes.size(), 128);
        for (const auto &key : frame.keyframes) { strings({&key.layerId, &key.assetId}); }
    }
    if (document.stableDiffusionMetadata) {
        const auto &metadata = *document.stableDiffusionMetadata;
        items(1, 128);
        strings({&metadata.positivePrompt, &metadata.negativePrompt, &metadata.software,
                 &metadata.softwareVersion, &metadata.createdAt, &metadata.generationParametersText,
                 &metadata.comfyUi.promptJson, &metadata.comfyUi.workflowJson});
        items(metadata.samplingPasses.size(), 128);
        for (const auto &pass : metadata.samplingPasses) { strings({&pass.nodeId, &pass.samplerName, &pass.scheduler}); }
        items(metadata.models.size(), 128);
        for (const auto &model : metadata.models) { strings({&model.role, &model.name, &model.hash, &model.hashType, &model.uri}); }
        items(metadata.loras.size(), 64);
        for (const auto &lora : metadata.loras) { strings({&lora.name, &lora.hash}); }
        for (const auto *entries : {&metadata.comfyUi.extraPngInfo, &metadata.extraParameters}) {
            items(entries->size(), 64);
            for (const auto &entry : *entries) { strings({&entry.key, &entry.value}); }
        }
    }
    return used;
}
void xmlText(const std::string &text)
{
    if (!validTimelineXmlText(text))
        fail(MediaIoCode::InvalidArgument, "timeline names must be valid UTF-8 XML 1.0 text");
}
fs::path destinationPath(const std::string &directory)
{
    if (directory.empty() || !media_detail::validText(directory) || directory.find("://") != directory.npos)
        fail(MediaIoCode::InvalidArgument, "a valid UTF-8 local destination directory is required");
    auto destination = fs::absolute(utf8Path(directory)).lexically_normal();
    while (destination.filename().empty() && destination != destination.root_path())
        destination = destination.parent_path();
    if (destination.filename().empty() || destination.filename() == "." || destination.filename() == "..")
        fail(MediaIoCode::InvalidArgument, "a destination directory name is required");
    std::error_code error;
    const auto status = fs::symlink_status(destination, error);
    if (error && error != std::errc::no_such_file_or_directory)
        fail(MediaIoCode::IoError, error.message());
    if (status.type() != fs::file_type::not_found)
        fail(MediaIoCode::AlreadyExists, "timeline destination already exists; choose a new directory");
    const auto parent = fs::canonical(destination.parent_path(), error);
    if (error || !fs::is_directory(parent))
        fail(MediaIoCode::IoError, "timeline destination parent directory does not exist");
    return parent / destination.filename();
}
std::string padded(std::size_t number)
{
    auto value = std::to_string(number);
    if (value.size() < 4) value.insert(0, 4 - value.size(), '0');
    return value;
}
std::string blendName(RasterBlendMode blend)
{
    switch (blend) {
    case RasterBlendMode::SourceOver: return "normal";
    case RasterBlendMode::Multiply: return "multiply";
    case RasterBlendMode::Screen: return "screen";
    case RasterBlendMode::Overlay: return "overlay";
    default: fail(MediaIoCode::UnsupportedFeature, "unsupported timeline blend mode");
    }
}
struct MediaTask { std::size_t layerIndex; const Asset *asset; };
struct AudioMediaTask { const AudioAsset *asset; std::uint64_t trimSamples; };
struct Prepared {
    InterchangePlan plan;
    std::vector<MediaTask> tasks;
    std::vector<AudioMediaTask> audioTasks;
    std::uint64_t metadataBytes = 0;
};
Prepared prepare(const Document &document, const TimelineInterchangeOptions &options, MediaIoResult &result)
{
    Prepared prepared;
    charge(prepared.metadataBytes, 4096, options.limits.maxDecodedBytes);
    if (options.sequenceName.size() > (options.limits.maxDecodedBytes - prepared.metadataBytes) / 8) {
        fail(MediaIoCode::LimitExceeded, "timeline sequence name exceeds the memory budget");
    }
    charge(prepared.metadataBytes, options.sequenceName.size() * 8ULL, options.limits.maxDecodedBytes);
    if (document.layers.size() > options.maxLayers
        || document.audioTracks.size() > options.maxLayers - document.layers.size() || document.timeline.frameCount > options.limits.maxFrames) {
        fail(MediaIoCode::LimitExceeded, "timeline layer/frame count exceeds the export limits");
    }
    const auto valid = validate(document);
    if (!valid.ok()) { fail(MediaIoCode::InvalidArgument, "invalid source document: " + valid.issues.front().message); }
    if (!document.artboards.empty())
        fail(MediaIoCode::UnsupportedFeature, "timeline interchange cannot preserve artboard groups; select an artboard explicitly");
    checked(media_detail::checkExtent(document.extent, options.limits));
    if (document.extent.width > 16384 || document.extent.height > 16384) {
        fail(MediaIoCode::UnsupportedFeature, "timeline interchange supports canvas dimensions up to 16384 pixels");
    }
    xmlText(options.sequenceName);
    if (options.sequenceName.empty()) { fail(MediaIoCode::InvalidArgument, "timeline sequence name must not be empty"); }
    auto &plan = prepared.plan;
    plan.name = options.sequenceName; plan.frameRate = document.timeline.frameRate;
    const auto divisor = std::gcd(plan.frameRate.numerator, plan.frameRate.denominator);
    plan.frameRate.numerator /= divisor; plan.frameRate.denominator /= divisor;
    plan.extent = document.extent; plan.frameCount = document.timeline.frameCount;
    std::uint64_t clipCount = 0;
    for (std::size_t layerIndex = 0; layerIndex < document.layers.size(); ++layerIndex) {
        const auto &layer = document.layers[layerIndex]; const auto &properties = layerProperties(layer);
        if (properties.appearance != LayerAppearance{})
            fail(MediaIoCode::UnsupportedFeature, "timeline interchange cannot preserve native layer appearance");
        xmlText(properties.name); xmlText(properties.id);
        charge(prepared.metadataBytes, (properties.name.size() + properties.id.size()) * 8ULL + 1024, options.limits.maxDecodedBytes);
        InterchangeTrack track{properties.id, properties.name, properties.visible, properties.opacity, properties.blendMode, {}};
        (void)blendName(properties.blendMode);
        const FrameIndex first = properties.frameRange ? properties.frameRange->firstFrame : 0;
        const FrameIndex end = properties.frameRange ? properties.frameRange->lastFrame + 1 : document.timeline.frameCount;
        std::vector<FrameIndex> boundaries{first};
        if (const auto *keys = keyframedLayerSource(layer)) {
            for (const auto frame : keys->frameIndices) {
                if (frame > first && frame < end) {
                    charge(prepared.metadataBytes, sizeof(FrameIndex) * 2, options.limits.maxDecodedBytes);
                    boundaries.push_back(frame);
                }
            }
        }
        boundaries.push_back(end);
        std::unordered_map<const Asset *, std::size_t> media;
        for (std::size_t index = 0; index + 1 < boundaries.size(); ++index) {
            charge(clipCount, 1, options.maxClips);
            const auto *asset = resolveAssetAt(document, layer, boundaries[index]);
            if (!asset) { fail(MediaIoCode::InvalidData, "cannot resolve timeline layer state"); }
            charge(prepared.metadataBytes, assetId(*asset).size() * 8ULL + 1024, options.limits.maxDecodedBytes);
            auto found = media.find(asset);
            if (found == media.end()) {
                const auto mediaIndex = plan.media.size();
                const auto relative = "media/layer-" + padded(layerIndex + 1) + "-state-" + padded(media.size() + 1) + ".png";
                plan.media.push_back({relative}); prepared.tasks.push_back({layerIndex, asset});
                found = media.emplace(asset, mediaIndex).first;
            }
            track.clips.push_back({boundaries[index], boundaries[index + 1] - boundaries[index], found->second, assetId(*asset)});
        }
        plan.tracks.push_back(std::move(track));
        if (contentKind(layer) == ContentKind::Vector) {
            warn(result, "Native vector paths are rendered into separate PNG clip media; editable vectors remain in source.iisc, not native NLE vector objects");
        }
        const auto &t = properties.transform;
        if (t.m11 != 1 || t.m12 != 0 || t.m21 != 0 || t.m22 != 1 || t.translationX != 0 || t.translationY != 0) {
            warn(result, "Layer transforms are baked into full-canvas PNG media; clip positions, durations, opacity and compositing remain editable");
        }
    }
    for (const auto &sourceTrack : document.audioTracks) {
        xmlText(sourceTrack.id); xmlText(sourceTrack.name);
        charge(prepared.metadataBytes, (sourceTrack.id.size() + sourceTrack.name.size()) * 8ULL + 1024, options.limits.maxDecodedBytes);
        InterchangeAudioTrack track{sourceTrack.id, sourceTrack.name, sourceTrack.muted, sourceTrack.gainDb, {}};
        for (const auto &clip : sourceTrack.clips) {
            charge(clipCount, 1, options.maxClips);
            xmlText(clip.id); xmlText(clip.name); xmlText(clip.assetId);
            charge(prepared.metadataBytes, (clip.id.size() + clip.name.size() + clip.assetId.size()) * 8ULL + 1024, options.limits.maxDecodedBytes);
            const auto *asset = findAudioAsset(document, clip.assetId);
            const auto sampleTicks = std::uint64_t(asset->sampleRate) * plan.frameRate.denominator;
            const auto quantum = sampleTicks / std::gcd(sampleTicks, std::uint64_t(plan.frameRate.numerator));
            const auto trim = clip.sourceOffsetSamples % quantum;
            std::size_t mediaIndex = 0;
            for (; mediaIndex < prepared.audioTasks.size(); ++mediaIndex) {
                const auto &task = prepared.audioTasks[mediaIndex];
                if (task.asset == asset && task.trimSamples == trim) { break; }
            }
            if (mediaIndex == prepared.audioTasks.size()) {
                charge(prepared.metadataBytes, 1024, options.limits.maxDecodedBytes);
                const auto path = "media/audio-" + padded(mediaIndex + 1) + ".wav";
                plan.audioMedia.push_back({path, asset->sampleRate, asset->channelCount,
                    asset->samples.size() / asset->channelCount - trim});
                prepared.audioTasks.push_back({asset, trim});
            }
            track.clips.push_back({clip.startFrame, clip.durationFrames, mediaIndex, clip.id, clip.name,
                clip.assetId, clip.sourceOffsetSamples - trim, clip.gainDb, clip.enabled});
            if (trim) {
                warn(result, "Sample-accurate source trims use WAV files with an adjusted origin for legacy XML; full original PCM and source offsets remain in source.iisc");
            }
        }
        plan.audioTracks.push_back(std::move(track));
    }
    if (!plan.audioTracks.empty()) {
        warn(result, "Track and clip audio gain are combined into editable clip gain; legacy XML uses linked channel tracks for stereo");
    }
    if (document.canvasMode == CanvasMode::Infinite) { warn(result, "Infinite canvas content is clipped to canvasRegion for fixed-resolution editor timelines"); }
    if (document.stableDiffusionMetadata) { warn(result, "Generation metadata remains in source.iisc and is not represented as NLE effects"); }
    warn(result, "Editor color management and blend implementations can differ; no application edit/save round-trip is implied");
    return prepared;
}
void checkRenderBudget(const Asset &asset, std::uint64_t canvasBytes, std::uint64_t metadataBytes,
                       const TimelineInterchangeOptions &options)
{
    auto used = metadataBytes;
    charge(used, canvasBytes * 10, options.limits.maxDecodedBytes);
    std::uint64_t commandCount = 0;
    const auto raster = [&](const RasterLayer &pixels) {
        checked(media_detail::checkRaster(pixels, options.limits));
        charge(used, pixels.pixels.size() * 8ULL, options.limits.maxDecodedBytes);
    };
    if (const auto *pixels = std::get_if<RasterAsset>(&asset)) { raster(pixels->pixels); }
    else if (const auto *chunks = std::get_if<ChunkedRasterAsset>(&asset)) {
        for (const auto &chunk : chunks->chunks) {
            raster(chunk.pixels);
            // The native renderer retains one canvas-sized composition piece per chunk.
            charge(used, canvasBytes * 2, options.limits.maxDecodedBytes);
        }
    } else {
        for (const auto &path : std::get<VectorAsset>(asset).paths) {
            charge(commandCount, path.commands.size(), options.limits.maxVectorCommands);
            charge(used, sizeof(VectorPath) * 2 + path.commands.size() * sizeof(PathCommand) * 2ULL, options.limits.maxDecodedBytes);
            for (const auto &command : path.commands) {
                charge(used, std::holds_alternative<QuadraticTo>(command) || std::holds_alternative<CubicTo>(command) ? 32768 : 128,
                       options.limits.maxDecodedBytes);
            }
        }
    }
}
RasterLayer renderMedia(const Document &document, const MediaTask &task)
{
    Document isolated; isolated.extent = document.extent; isolated.canvasMode = document.canvasMode;
    isolated.infiniteCanvas = document.infiniteCanvas; isolated.assets.push_back(*task.asset);
    const auto &source = document.layers[task.layerIndex];
    isolated.layers.emplace_back(contentKind(source) == ContentKind::Vector ? Layer(StaticVectorLayer{}) : Layer(StaticBitmapLayer{}));
    auto &properties = layerProperties(isolated.layers[0]); properties = layerProperties(source);
    properties.visible = true; properties.opacity = 1; properties.blendMode = RasterBlendMode::SourceOver;
    properties.frameRange.reset(); setLayerSource(isolated.layers[0], StaticSource{assetId(*task.asset)});
    auto rendered = renderFrameLayerTiles(isolated, 0, 0, {{canvasRegion(isolated), isolated.extent}});
    if (!rendered.ok() || rendered.tiles.size() != 1) { fail(MediaIoCode::InvalidData, "cannot render timeline media: " + rendered.message); }
    return std::move(rendered.tiles[0].pixels);
}
class JsonWriter {
public:
    explicit JsonWriter(std::uint64_t limit) : limit_(limit) {}
    void raw(std::string_view value)
    {
        if (value.size() > limit_ - bytes_.size())
            fail(MediaIoCode::LimitExceeded, "timeline manifest exceeds the memory budget");
        bytes_.append(value);
    }
    void text(std::string_view value)
    {
        if (!ii::core::valid_utf8(value))
            fail(MediaIoCode::InvalidArgument, "timeline manifest text must be valid UTF-8");
        static constexpr char hex[] = "0123456789abcdef";
        raw("\"");
        for (const auto character : value) {
            const auto ch = static_cast<unsigned char>(character);
            if (ch == '"') raw("\\\"");
            else if (ch == '\\') raw("\\\\");
            else if (ch < 0x20) {
                const char escaped[] = {'\\', 'u', '0', '0', hex[ch >> 4], hex[ch & 15]};
                raw({escaped, sizeof escaped});
            } else raw({&character, 1});
        }
        raw("\"");
    }
    template <class T> void integer(T value) { raw(std::to_string(value)); }
    void number(double value) { raw(ii::core::format_decimal(value)); }
    void boolean(bool value) { raw(value ? "true" : "false"); }
    std::string take() { return std::move(bytes_); }
private:
    std::uint64_t limit_;
    std::string bytes_;
};
std::string manifest(const Document &document, const InterchangePlan &plan,
                     const MediaIoResult &result, std::uint64_t limit)
{
    JsonWriter out(limit);
    out.raw("{\"format\":\"iiSharedCanvas.timeline-interchange\",\"version\":");
    out.integer(plan.audioTracks.empty() ? 1 : 2);
    out.raw(",\"source\":\"source.iisc\",\"sequenceName\":"); out.text(plan.name);
    out.raw(",\"frameCount\":"); out.integer(plan.frameCount);
    out.raw(",\"frameRate\":{\"numerator\":"); out.integer(plan.frameRate.numerator);
    out.raw(",\"denominator\":"); out.integer(plan.frameRate.denominator);
    out.raw("},\"canvas\":{\"width\":"); out.integer(plan.extent.width);
    out.raw(",\"height\":"); out.integer(plan.extent.height);
    const auto origin = canvasOrigin(document);
    out.raw(",\"originX\":"); out.integer(origin.x);
    out.raw(",\"originY\":"); out.integer(origin.y);
    out.raw("},\"legacyXml\":\"timeline.xml\",\"fcpxml\":\"timeline.fcpxml\",\"tracks\":[");
    for (std::size_t index = 0; index < plan.tracks.size(); ++index) {
        if (index) out.raw(",");
        const auto &track = plan.tracks[index];
        out.raw("{\"layerId\":"); out.text(track.id);
        out.raw(",\"name\":"); out.text(track.name);
        out.raw(",\"kind\":"); out.text(contentKind(document.layers[index]) == ContentKind::Vector ? "vector" : "bitmap");
        out.raw(",\"visible\":"); out.boolean(track.visible);
        out.raw(",\"opacity\":"); out.number(track.opacity);
        out.raw(",\"blendMode\":"); out.text(blendName(track.blendMode));
        out.raw(",\"clips\":[");
        for (std::size_t clipIndex = 0; clipIndex < track.clips.size(); ++clipIndex) {
            if (clipIndex) out.raw(",");
            const auto &clip = track.clips[clipIndex];
            out.raw("{\"startFrame\":"); out.integer(clip.start);
            out.raw(",\"durationFrames\":"); out.integer(clip.duration);
            out.raw(",\"assetId\":"); out.text(clip.assetId);
            out.raw(",\"media\":"); out.text(plan.media[clip.mediaIndex].relativePath); out.raw("}");
        }
        out.raw("]}");
    }
    out.raw("],\"audioTracks\":[");
    for (std::size_t index = 0; index < plan.audioTracks.size(); ++index) {
        if (index) out.raw(",");
        const auto &track = plan.audioTracks[index];
        out.raw("{\"layerId\":"); out.text(track.id);
        out.raw(",\"name\":"); out.text(track.name);
        out.raw(",\"muted\":"); out.boolean(track.muted);
        out.raw(",\"gainDb\":"); out.number(track.gainDb);
        out.raw(",\"clips\":[");
        for (std::size_t clipIndex = 0; clipIndex < track.clips.size(); ++clipIndex) {
            if (clipIndex) out.raw(",");
            const auto &clip = track.clips[clipIndex];
            const auto &native = document.audioTracks[index].clips[clipIndex];
            const auto &media = plan.audioMedia[clip.mediaIndex];
            out.raw("{\"id\":"); out.text(clip.id);
            out.raw(",\"name\":"); out.text(clip.name);
            out.raw(",\"assetId\":"); out.text(clip.assetId);
            out.raw(",\"startFrame\":"); out.integer(clip.start);
            out.raw(",\"durationFrames\":"); out.integer(clip.duration);
            out.raw(",\"sourceOffsetSamples\":"); out.text(std::to_string(native.sourceOffsetSamples));
            out.raw(",\"mediaOffsetSamples\":"); out.text(std::to_string(clip.sourceOffsetSamples));
            out.raw(",\"mediaTrimSamples\":"); out.text(std::to_string(native.sourceOffsetSamples - clip.sourceOffsetSamples));
            out.raw(",\"gainDb\":"); out.number(clip.gainDb);
            out.raw(",\"enabled\":"); out.boolean(clip.enabled);
            out.raw(",\"media\":"); out.text(media.relativePath);
            out.raw(",\"sampleRate\":"); out.integer(media.sampleRate);
            out.raw(",\"channelCount\":"); out.integer(media.channelCount);
            out.raw(",\"sampleFrameCount\":"); out.text(std::to_string(media.sampleFrameCount)); out.raw("}");
        }
        out.raw("]}");
    }
    out.raw("],\"warnings\":[");
    for (std::size_t index = 0; index < result.warnings.size(); ++index) {
        if (index) out.raw(",");
        out.text(result.warnings[index]);
    }
    out.raw("]}");
    return out.take();
}
void write(const fs::path &path, std::span<const std::uint8_t> bytes,
           std::uint64_t &used, std::uint64_t limit)
{
    charge(used, bytes.size(), limit);
    provider::File::create(path, {reinterpret_cast<const char *>(bytes.data()), bytes.size()});
}
void write(const fs::path &path, std::string_view bytes, std::uint64_t &used, std::uint64_t limit)
{
    write(path, {reinterpret_cast<const std::uint8_t *>(bytes.data()), bytes.size()}, used, limit);
}
void publish(const fs::path &stage, const fs::path &destination)
{
    const auto result = provider::publishDirectoryExclusive(stage, destination);
    if (!result.succeeded) {
        const auto code = result.diagnosticSuffix == "already_exists" ? MediaIoCode::AlreadyExists
                        : result.diagnosticSuffix == "unsupported" ? MediaIoCode::UnsupportedFeature
                        : MediaIoCode::IoError;
        fail(code, result.message);
    }
}
} // namespace

MediaIoResult exportTimelineInterchange(const Document &document, const std::string &directory,
                                        const TimelineInterchangeOptions &options)
{
    MediaIoResult result;
    try {
        if (directory.size() > options.limits.maxDecodedBytes / 8)
            fail(MediaIoCode::LimitExceeded, "timeline destination path exceeds the memory budget");
        const auto destination = destinationPath(directory);
        if (std::any_of(document.assets.begin(), document.assets.end(), [](const Asset &asset) {
                return !std::holds_alternative<RasterAsset>(asset)
                    && !std::holds_alternative<ChunkedRasterAsset>(asset)
                    && !std::holds_alternative<VectorAsset>(asset);
            }) || std::any_of(document.layers.begin(), document.layers.end(), [](const Layer &layer) {
                return layerRole(layer) == LayerRole::ControlNet || !layerProperties(layer).motion.empty();
            })) {
            fail(MediaIoCode::UnsupportedFeature,
                 "ControlNet content, native video and motion are not supported by timeline XML interchange; use .iisc or exportVideo");
        }
        const auto snapshotBytes = sourceSnapshotUpperBound(document, options.limits.maxDecodedBytes / 4);
        auto prepared = prepare(document, options, result);
        for (const auto &task : prepared.tasks) {
            checkRenderBudget(*task.asset, std::uint64_t(document.extent.width) * document.extent.height * 4,
                              prepared.metadataBytes, options);
        }
        const auto serializationBudget = (options.limits.maxDecodedBytes - prepared.metadataBytes) / 4;
        if (snapshotBytes > serializationBudget)
            fail(MediaIoCode::LimitExceeded, "source snapshot exceeds the export memory budget");
        auto xml = encodeTimelineXml(prepared.plan, pathText(destination),
                                     std::min(options.limits.maxOutputBytes, serializationBudget));
        checked(xml.result);
        for (const auto &warning : xml.result.warnings) warn(result, warning);
        auto json = manifest(document, prepared.plan, result, serializationBudget);
        provider::TemporaryDirectory stage(destination.parent_path());
        if (!stage.isValid()) fail(MediaIoCode::IoError, "cannot create private timeline package staging directory");
        provider::File::createDirectories(stage.path() / "media");
        std::uint64_t used = 0;
        write(stage.path() / "timeline.xml", xml.legacyXml, used, options.limits.maxOutputBytes);
        write(stage.path() / "timeline.fcpxml", xml.fcpxml, used, options.limits.maxOutputBytes);
        write(stage.path() / "manifest.json", json, used, options.limits.maxOutputBytes);
        xml.legacyXml.clear(); xml.fcpxml.clear(); json.clear();
        {
            SerializationLimits limits;
            limits.maximumContainerBytes = std::min(options.limits.maxOutputBytes - used, serializationBudget);
            limits.maximumCanvasPixels = options.limits.maxPixelsPerFrame;
            limits.maximumTotalRasterPixels = options.limits.maxDecodedBytes / 4;
            limits.maximumTotalPathCommands = options.limits.maxVectorCommands;
            limits.maximumAudioAssets = options.maxClips;
            limits.maximumAudioTracks = options.maxLayers;
            limits.maximumTotalAudioClips = options.maxClips;
            limits.maximumTotalAudioSamples = options.limits.maxDecodedBytes / 2;
            limits.maximumLayers = options.maxLayers; limits.maximumTotalKeyframes = options.maxClips;
            const auto source = encodeIisc(document, limits);
            if (!source.ok()) fail(source.error.code == IiscErrorCode::LimitExceeded ? MediaIoCode::LimitExceeded : MediaIoCode::InvalidArgument, source.error.message);
            write(stage.path() / "source.iisc", source.bytes, used, options.limits.maxOutputBytes);
        }
        for (std::size_t index = 0; index < prepared.tasks.size(); ++index) {
            BitmapExportOptions bitmap; bitmap.extendedCodecs = false; bitmap.limits = options.limits;
            bitmap.limits.maxOutputBytes = std::min(options.limits.maxOutputBytes - used, serializationBudget);
            const auto pixels = renderMedia(document, prepared.tasks[index]);
            const auto png = encodeBitmap(pixels, bitmap); checked(png.result);
            write(stage.path() / utf8Path(prepared.plan.media[index].relativePath), png.bytes, used, options.limits.maxOutputBytes);
        }
        for (std::size_t index = 0; index < prepared.audioTasks.size(); ++index) {
            const auto &task = prepared.audioTasks[index];
            MediaLimits limits = options.limits;
            limits.maxOutputBytes = std::min(options.limits.maxOutputBytes - used, serializationBudget);
            limits.maxDecodedBytes = serializationBudget;
            MediaBytesResult wav;
            if (task.trimSamples == 0) wav = encodeAudioWav(*task.asset, limits);
            else {
                AudioAsset shifted{task.asset->id, task.asset->sampleRate, task.asset->channelCount, {}};
                const auto first = task.asset->samples.begin() + static_cast<std::ptrdiff_t>(task.trimSamples * task.asset->channelCount);
                shifted.samples.assign(first, task.asset->samples.end());
                wav = encodeAudioWav(shifted, limits);
            }
            checked(wav.result);
            write(stage.path() / utf8Path(prepared.plan.audioMedia[index].relativePath), wav.bytes, used, options.limits.maxOutputBytes);
        }
        publish(stage.path(), destination);
        stage.release();
    } catch (const Failure &failure) { result.code = failure.code; result.message = failure.message; }
    catch (const std::bad_alloc &) { result.code = MediaIoCode::LimitExceeded; result.message = "timeline export exhausted its allocation budget"; }
    catch (const std::length_error &) { result.code = MediaIoCode::LimitExceeded; result.message = "timeline export allocation is too large"; }
    catch (const std::exception &failure) { result.code = MediaIoCode::IoError; result.message = failure.what(); }
    return result;
}
} // namespace iiSharedCanvas
