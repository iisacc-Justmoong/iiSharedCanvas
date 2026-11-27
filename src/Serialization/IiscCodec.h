#pragma once

#include "Document/Document.h"
#include "iiSharedCanvas/Export.h"

#include <cstddef>
#include <cstdint>
#include <functional>
#include <span>
#include <string>
#include <vector>

namespace iiSharedCanvas {

inline constexpr std::size_t IiscHeaderSize = 32;

struct SerializationLimits {
    // Maximum nesting and total bytes of the immediate owned IISC copies.
    // Nested bytes are already contained in the immediate copies.
    std::uint32_t maximumEmbeddedCanvasDepth = 16;
    std::uint64_t maximumEmbeddedCanvasBytes = 1024ULL * 1024ULL * 1024ULL;
    std::uint64_t maximumContainerBytes = 1024ULL * 1024ULL * 1024ULL;
    std::uint64_t maximumCanvasPixels = 256ULL * 1024ULL * 1024ULL;
    std::uint64_t maximumTotalRasterPixels = 256ULL * 1024ULL * 1024ULL;
    std::uint64_t maximumTotalVectorPaths = 1024ULL * 1024ULL;
    std::uint64_t maximumTotalShapeNodes = 65536;
    std::uint64_t maximumTotalPathCommands = 16ULL * 1024ULL * 1024ULL;
    std::uint64_t maximumTotalKeyframes = 16ULL * 1024ULL * 1024ULL;
    std::uint64_t maximumTotalStringBytes = 64ULL * 1024ULL * 1024ULL;
    std::uint32_t maximumStringBytes = 1024U * 1024U;
    std::uint32_t maximumMetadataStringBytes = 16U * 1024U * 1024U;
    std::uint32_t maximumAssets = 65536U;
    std::uint32_t maximumLayers = 65536U;
    std::uint32_t maximumArtboards = 65536U;
    std::uint32_t maximumFrames = 262144U;
    std::uint32_t maximumRasterChunks = 1048576U;
    std::uint32_t maximumMetadataEntries = 65536U;
    std::uint32_t maximumAudioAssets = 65536U;
    std::uint32_t maximumAudioTracks = 65536U;
    std::uint64_t maximumTotalAudioClips = 16ULL * 1024ULL * 1024ULL;
    // Scalar interleaved PCM16 samples, including every channel (two bytes each).
    std::uint64_t maximumTotalAudioSamples = 256ULL * 1024ULL * 1024ULL;
    std::uint64_t maximumTotalVideoFrames = 262144ULL;
    std::uint64_t maximumTotalMotionKeyframes = 1024ULL * 1024ULL;
    std::uint32_t maximumMlsdSegments = 1000000;
    std::uint64_t maximumCannySamples = 64ULL * 1024ULL * 1024ULL;
    std::uint64_t maximumScribbleSamples = 64ULL * 1024ULL * 1024ULL;
    std::uint64_t maximumLineArtSamples = 64ULL * 1024ULL * 1024ULL;
    std::uint64_t maximumNormalMapSamples = 16ULL * 1024ULL * 1024ULL;
    std::uint64_t maximumShuffleSamples = 16ULL * 1024ULL * 1024ULL;
    std::uint64_t maximumTileSamples = 16ULL * 1024ULL * 1024ULL;
    std::uint64_t maximumReferenceSamples = 16ULL * 1024ULL * 1024ULL;
    std::uint64_t maximumIpAdapterValues = 16ULL * 1024ULL * 1024ULL; // All assets, both branches.
    std::uint64_t maximumDepthSamples = 64ULL * 1024ULL * 1024ULL;
    std::uint32_t maximumPosePeople = 4096;
    std::uint32_t maximumPoseExpressions = 65536;
    std::uint32_t maximumPoseDeltas = 1048576;
    std::uint32_t maximumSemanticClasses = 65536;
    std::uint32_t maximumSemanticRegions = 1048576;
    std::uint32_t maximumSemanticEntries = 1048576; // Aliases and region attributes combined.
    std::uint32_t maximumMaskStackEntriesPerLayer = 4096;
    std::uint64_t maximumTotalMaskStackEntries = 65536;
    std::uint32_t maximumFiltersPerLayer = 64;
    std::uint64_t maximumTotalLayerFilters = 65536;
    std::uint64_t maximumTotalFillGradientStops = 65536;
    std::uint32_t maximumStylesPerLayer = 64;
    std::uint64_t maximumTotalLayerStyles = 65536;
    // Bounds on saved selections and structural relations, including all masks.
    std::uint32_t maximumSelectionChannels = 65536;
    std::uint32_t maximumLayerGroups = 65536;
    std::uint32_t maximumLayerLinks = 1048576;
    std::uint64_t maximumSelectionTiles = 65536;
    std::uint64_t maximumSelectionDenseBytes = 32ULL * 1024 * 1024;
    // Independent limits for timeline metadata and document-owned media bytes.
    std::uint64_t maximumTimelineProjectBytes = 64ULL * 1024 * 1024;
    std::uint64_t maximumTimelineMediaBytes = 1024ULL * 1024 * 1024;
    std::uint32_t maximumTimelineMediaAssets = 65536;
};

enum class IiscErrorCode {
    None,
    InvalidDocument,
    UnsupportedVersion,
    InvalidHeader,
    TruncatedData,
    ChecksumMismatch,
    LimitExceeded,
    InvalidData,
    TrailingData,
};

struct IiscError {
    IiscErrorCode code = IiscErrorCode::None;
    std::uint64_t offset = 0;
    std::string message;
};

struct IiscEncodeResult {
    std::vector<std::uint8_t> bytes;
    IiscError error;

    [[nodiscard]] bool ok() const noexcept
    {
        return error.code == IiscErrorCode::None;
    }
};

struct IiscDecodeResult {
    Document document;
    IiscError error;

    [[nodiscard]] bool ok() const noexcept
    {
        return error.code == IiscErrorCode::None;
    }
};

IISHAREDCANVAS_EXPORT IiscEncodeResult encodeIisc(
    const Document &document,
    SerializationLimits limits = {});
IISHAREDCANVAS_EXPORT IiscDecodeResult decodeIisc(
    std::span<const std::uint8_t> bytes,
    SerializationLimits limits = {});

struct IiscRasterChange {
    std::string asset;
    std::vector<CanvasRegion> regions;
};

struct IiscEditImpact {
    bool changed = false;
    bool header = false;
    std::vector<std::string> layers, assets, audioTracks, artboards;
    std::vector<IiscRasterChange> pixels;
};

// Report the document records and local pixel regions touched by an edit.
IISHAREDCANVAS_EXPORT IiscError describeIiscEdit(
    std::span<const std::uint8_t> bytes, IiscEditImpact &impact, SerializationLimits limits = {});

// A versioned transaction against a known document revision. The caller owns
// revision ordering. Portable records and raster tiles carry only the edit.
IISHAREDCANVAS_EXPORT IiscEncodeResult encodeIiscEdit(
    const Document &before, const Document &after, SerializationLimits limits = {});

// For a journal which guarantees that only this existing bitmap and authorship
// changed, restrict comparison to the bitmap and its optional local bounds.
IISHAREDCANVAS_EXPORT IiscEncodeResult encodeIiscBitmapEdit(
    const Document &before, const Document &after, const std::string &asset,
    std::optional<CanvasRegion> dirty = {}, SerializationLimits limits = {});

// Retains unchanged backing storage. A rejected accept callback, invalid edit,
// or exception restores the input. The callback must not reenter the document.
IISHAREDCANVAS_EXPORT IiscError applyIiscEdit(
    Document &document, std::span<const std::uint8_t> bytes,
    SerializationLimits limits = {}, const std::function<bool(const Document &)> &accept = {});

} // namespace iiSharedCanvas
