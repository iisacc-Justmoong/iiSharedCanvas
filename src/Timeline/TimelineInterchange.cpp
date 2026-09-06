#include "TimelineInterchange.h"
#include "TimelineXmlWriter_p.hpp"

#include "Audio/AudioCodec.h"
#include "Bitmap/BitmapCodec.h"
#include "Media/MediaIo_p.hpp"
#include "Render/FrameRenderer.h"
#include "Serialization/IiscCodec.h"
#include "Validation/Validation.h"

#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QTemporaryDir>

#include <algorithm>
#include <cerrno>
#include <initializer_list>
#include <limits>
#include <new>
#include <numeric>
#include <stdexcept>
#include <unordered_map>

#if defined(__APPLE__)
#include <stdio.h>
#elif defined(__linux__)
#include <fcntl.h>
#include <sys/syscall.h>
#include <unistd.h>
#elif defined(_WIN32)
#define NOMINMAX
#include <windows.h>
#endif

namespace iiSharedCanvas {
namespace {
using namespace timeline_detail;
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
        if (const auto *source = std::get_if<StaticSource>(&layerSource(layer))) { strings({&source->assetId}); }
        else { items(std::get<KeyframedSource>(layerSource(layer)).frameIndices.size(), 8); }
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
    const auto decoded = QString::fromUtf8(text);
    if (decoded.toUtf8().toStdString() != text) { fail(MediaIoCode::InvalidArgument, "timeline text must be valid UTF-8"); }
    for (const auto ch : decoded) {
        const auto value = ch.unicode();
        if ((value < 32 && value != 9 && value != 10 && value != 13) || value == 0xfffe || value == 0xffff) {
            fail(MediaIoCode::InvalidArgument, "timeline names must contain only XML 1.0 characters");
        }
    }
}
QString destinationPath(const std::string &directory)
{
    if (directory.empty() || directory.find('\0') != std::string::npos
        || QString::fromUtf8(directory).toUtf8().toStdString() != directory
        || directory.find("://") != std::string::npos) {
        fail(MediaIoCode::InvalidArgument, "a valid UTF-8 local destination directory is required");
    }
    const QFileInfo destination(QString::fromUtf8(directory));
    if (destination.exists() || destination.isSymLink()) { fail(MediaIoCode::AlreadyExists, "timeline destination already exists; choose a new directory"); }
    const auto parent = destination.dir().canonicalPath();
    if (parent.isEmpty() || !QFileInfo(parent).isDir()) { fail(MediaIoCode::IoError, "timeline destination parent directory does not exist"); }
    return QDir(parent).filePath(destination.fileName());
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
        xmlText(properties.name); xmlText(properties.id);
        charge(prepared.metadataBytes, (properties.name.size() + properties.id.size()) * 8ULL + 1024, options.limits.maxDecodedBytes);
        InterchangeTrack track{properties.id, properties.name, properties.visible, properties.opacity, properties.blendMode, {}};
        (void)blendName(properties.blendMode);
        const FrameIndex first = properties.frameRange ? properties.frameRange->firstFrame : 0;
        const FrameIndex end = properties.frameRange ? properties.frameRange->lastFrame + 1 : document.timeline.frameCount;
        std::vector<FrameIndex> boundaries{first};
        if (const auto *keys = std::get_if<KeyframedSource>(&layerSource(layer))) {
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
                const auto relative = QStringLiteral("media/layer-%1-state-%2.png")
                    .arg(layerIndex + 1, 4, 10, QChar('0')).arg(media.size() + 1, 4, 10, QChar('0'));
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
                const auto path = QStringLiteral("media/audio-%1.wav").arg(mediaIndex + 1, 4, 10, QChar('0'));
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
    isolated.layers.emplace_back(contentKind(source) == ContentKind::Vector ? Layer(VectorLayer{}) : Layer(BitmapLayer{}));
    auto &properties = layerProperties(isolated.layers[0]); properties = layerProperties(source);
    properties.visible = true; properties.opacity = 1; properties.blendMode = RasterBlendMode::SourceOver;
    properties.frameRange.reset(); layerSource(isolated.layers[0]) = StaticSource{assetId(*task.asset)};
    auto rendered = renderFrameLayerTiles(isolated, 0, 0, {{canvasRegion(isolated), isolated.extent}});
    if (!rendered.ok() || rendered.tiles.size() != 1) { fail(MediaIoCode::InvalidData, "cannot render timeline media: " + rendered.message); }
    return std::move(rendered.tiles[0].pixels);
}
QByteArray manifest(const Document &document, const InterchangePlan &plan, const MediaIoResult &result)
{
    QJsonArray tracks;
    for (std::size_t index = 0; index < plan.tracks.size(); ++index) {
        const auto &track = plan.tracks[index]; QJsonArray clips;
        for (const auto &clip : track.clips) {
            clips.push_back(QJsonObject{{"startFrame", qint64(clip.start)}, {"durationFrames", qint64(clip.duration)},
                {"assetId", QString::fromUtf8(clip.assetId)}, {"media", plan.media[clip.mediaIndex].relativePath}});
        }
        tracks.push_back(QJsonObject{{"layerId", QString::fromUtf8(track.id)}, {"name", QString::fromUtf8(track.name)},
            {"kind", contentKind(document.layers[index]) == ContentKind::Vector ? "vector" : "bitmap"},
            {"visible", track.visible}, {"opacity", track.opacity}, {"blendMode", QString::fromStdString(blendName(track.blendMode))}, {"clips", clips}});
    }
    QJsonArray audioTracks;
    for (std::size_t trackIndex = 0; trackIndex < plan.audioTracks.size(); ++trackIndex) {
        const auto &track = plan.audioTracks[trackIndex]; QJsonArray clips;
        for (std::size_t i = 0; i < track.clips.size(); ++i) {
            const auto &clip = track.clips[i]; const auto &native = document.audioTracks[trackIndex].clips[i];
            const auto &media = plan.audioMedia[clip.mediaIndex];
            clips.push_back(QJsonObject{{"id", QString::fromUtf8(clip.id)}, {"name", QString::fromUtf8(clip.name)},
                {"assetId", QString::fromUtf8(clip.assetId)}, {"startFrame", qint64(clip.start)}, {"durationFrames", qint64(clip.duration)},
                {"sourceOffsetSamples", QString::number(native.sourceOffsetSamples)},
                {"mediaOffsetSamples", QString::number(clip.sourceOffsetSamples)},
                {"mediaTrimSamples", QString::number(native.sourceOffsetSamples - clip.sourceOffsetSamples)},
                {"gainDb", clip.gainDb}, {"enabled", clip.enabled}, {"media", media.relativePath},
                {"sampleRate", qint64(media.sampleRate)}, {"channelCount", media.channelCount},
                {"sampleFrameCount", QString::number(media.sampleFrameCount)}});
        }
        audioTracks.push_back(QJsonObject{{"layerId", QString::fromUtf8(track.id)}, {"name", QString::fromUtf8(track.name)},
            {"muted", track.muted}, {"gainDb", track.gainDb}, {"clips", clips}});
    }
    QJsonArray warnings; for (const auto &warning : result.warnings) { warnings.push_back(QString::fromUtf8(warning)); }
    const auto origin = canvasOrigin(document);
    return QJsonDocument(QJsonObject{{"format", "iiSharedCanvas.timeline-interchange"}, {"version", plan.audioTracks.empty() ? 1 : 2},
        {"source", "source.iisc"}, {"sequenceName", QString::fromUtf8(plan.name)}, {"frameCount", qint64(plan.frameCount)},
        {"frameRate", QJsonObject{{"numerator", qint64(plan.frameRate.numerator)}, {"denominator", qint64(plan.frameRate.denominator)}}},
        {"canvas", QJsonObject{{"width", plan.extent.width}, {"height", plan.extent.height}, {"originX", origin.x}, {"originY", origin.y}}},
        {"legacyXml", "timeline.xml"}, {"fcpxml", "timeline.fcpxml"}, {"tracks", tracks}, {"audioTracks", audioTracks}, {"warnings", warnings}}).toJson();
}
void write(const QString &path, std::span<const std::uint8_t> bytes, std::uint64_t &used, std::uint64_t limit)
{
    charge(used, bytes.size(), limit);
    if (bytes.size() > std::uint64_t(std::numeric_limits<qint64>::max())) { fail(MediaIoCode::LimitExceeded, "package file is too large"); }
    QFile file(path);
    if (!file.open(QIODevice::WriteOnly | QIODevice::NewOnly)
        || file.write(reinterpret_cast<const char *>(bytes.data()), qint64(bytes.size())) != qint64(bytes.size()) || !file.flush()) {
        fail(MediaIoCode::IoError, "cannot write complete timeline package file: " + file.errorString().toStdString());
    }
}
void write(const QString &path, const QByteArray &bytes, std::uint64_t &used, std::uint64_t limit)
{
    write(path, {reinterpret_cast<const std::uint8_t *>(bytes.constData()), std::size_t(bytes.size())}, used, limit);
}
void publish(const QString &temporary, const QString &destination)
{
    // Ordinary rename can replace an existing empty directory. Use an exclusive
    // kernel operation so even a concurrent destination creator is preserved.
#if defined(__APPLE__)
    const auto status = renamex_np(QFile::encodeName(temporary).constData(), QFile::encodeName(destination).constData(), RENAME_EXCL);
#elif defined(__linux__) && defined(SYS_renameat2)
    const auto status = syscall(SYS_renameat2, AT_FDCWD, QFile::encodeName(temporary).constData(),
                                AT_FDCWD, QFile::encodeName(destination).constData(), 1 /* RENAME_NOREPLACE */);
#elif defined(_WIN32)
    if (MoveFileExW(reinterpret_cast<LPCWSTR>(temporary.utf16()), reinterpret_cast<LPCWSTR>(destination.utf16()), 0)) { return; }
    const auto error = GetLastError();
    fail(error == ERROR_ALREADY_EXISTS || error == ERROR_FILE_EXISTS ? MediaIoCode::AlreadyExists : MediaIoCode::IoError,
         "cannot publish completed timeline package without replacement");
#else
    fail(MediaIoCode::UnsupportedFeature, "exclusive directory publishing is unavailable on this platform");
#endif
#if defined(__APPLE__) || (defined(__linux__) && defined(SYS_renameat2))
    if (status != 0) {
        fail(errno == EEXIST || errno == ENOTEMPTY ? MediaIoCode::AlreadyExists : MediaIoCode::IoError,
             "cannot publish completed timeline package without replacement");
    }
#endif
}
}

MediaIoResult exportTimelineInterchange(const Document &document, const std::string &directory,
                                        const TimelineInterchangeOptions &options)
{
    MediaIoResult result;
    try {
        if (directory.size() > options.limits.maxDecodedBytes / 8) {
            fail(MediaIoCode::LimitExceeded, "timeline destination path exceeds the memory budget");
        }
        const auto destination = destinationPath(directory);
        // Preflight before validation can build any source-sized lookup tables.
        const auto snapshotBytes = sourceSnapshotUpperBound(document, options.limits.maxDecodedBytes / 4);
        auto prepared = prepare(document, options, result);
        for (const auto &task : prepared.tasks) {
            checkRenderBudget(*task.asset, std::uint64_t(document.extent.width) * document.extent.height * 4,
                              prepared.metadataBytes, options);
        }
        // Reserve room for conversion copies and bounded XML/JSON materialization.
        const auto serializationBudget = (options.limits.maxDecodedBytes - prepared.metadataBytes) / 4;
        if (snapshotBytes > serializationBudget) {
            fail(MediaIoCode::LimitExceeded, "source snapshot exceeds the export memory budget");
        }
        auto xml = encodeTimelineXml(prepared.plan, destination, std::min(options.limits.maxOutputBytes, serializationBudget));
        checked(xml.result);
        for (const auto &warning : xml.result.warnings) { warn(result, warning); }
        auto json = manifest(document, prepared.plan, result);
        if (std::uint64_t(json.size()) > serializationBudget) { fail(MediaIoCode::LimitExceeded, "timeline manifest exceeds the memory budget"); }
        QTemporaryDir stage(QFileInfo(destination).dir().filePath(".iisc-timeline-XXXXXX"));
        if (!stage.isValid() || !QDir(stage.path()).mkdir("media")) { fail(MediaIoCode::IoError, "cannot create private timeline package staging directory"); }
        std::uint64_t used = 0;
        write(stage.filePath("timeline.xml"), xml.legacyXml, used, options.limits.maxOutputBytes);
        write(stage.filePath("timeline.fcpxml"), xml.fcpxml, used, options.limits.maxOutputBytes);
        write(stage.filePath("manifest.json"), json, used, options.limits.maxOutputBytes);
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
            if (!source.ok()) { fail(source.error.code == IiscErrorCode::LimitExceeded ? MediaIoCode::LimitExceeded : MediaIoCode::InvalidArgument, source.error.message); }
            write(stage.filePath("source.iisc"), source.bytes, used, options.limits.maxOutputBytes);
        }
        for (std::size_t index = 0; index < prepared.tasks.size(); ++index) {
            BitmapExportOptions bitmap; bitmap.extendedCodecs = false; bitmap.limits = options.limits;
            bitmap.limits.maxOutputBytes = std::min(options.limits.maxOutputBytes - used, serializationBudget);
            const auto pixels = renderMedia(document, prepared.tasks[index]);
            const auto png = encodeBitmap(pixels, bitmap); checked(png.result);
            write(stage.filePath(prepared.plan.media[index].relativePath), png.bytes, used, options.limits.maxOutputBytes);
        }
        for (std::size_t index = 0; index < prepared.audioTasks.size(); ++index) {
            const auto &task = prepared.audioTasks[index];
            MediaLimits limits = options.limits;
            limits.maxOutputBytes = std::min(options.limits.maxOutputBytes - used, serializationBudget);
            limits.maxDecodedBytes = serializationBudget;
            MediaBytesResult wav;
            if (task.trimSamples == 0) { wav = encodeAudioWav(*task.asset, limits); }
            else {
                AudioAsset shifted{task.asset->id, task.asset->sampleRate, task.asset->channelCount, {}};
                const auto first = task.asset->samples.begin() + static_cast<std::ptrdiff_t>(task.trimSamples * task.asset->channelCount);
                shifted.samples.assign(first, task.asset->samples.end());
                wav = encodeAudioWav(shifted, limits);
            }
            checked(wav.result);
            write(stage.filePath(prepared.plan.audioMedia[index].relativePath), wav.bytes, used, options.limits.maxOutputBytes);
        }
        publish(stage.path(), destination); stage.setAutoRemove(false);
    } catch (const Failure &failure) { result.code = failure.code; result.message = failure.message; }
    catch (const std::bad_alloc &) { result.code = MediaIoCode::LimitExceeded; result.message = "timeline export exhausted its allocation budget"; }
    catch (const std::length_error &) { result.code = MediaIoCode::LimitExceeded; result.message = "timeline export allocation is too large"; }
    catch (const std::exception &failure) { result.code = MediaIoCode::IoError; result.message = failure.what(); }
    return result;
}
} // namespace iiSharedCanvas
