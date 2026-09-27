#pragma once

#include "iiSharedCanvas/Export.h"
#include "ControlNet/ControlNet.h"
#include <Layer/RasterLayer.h>
#include <cstdint>
#include <optional>
#include <string>
#include <vector>

namespace iiSharedCanvas {

struct Document;
struct SemanticSegmentLayer;

struct SemanticAttribute {
    std::string key;
    std::string value;
    friend bool operator==(const SemanticAttribute &, const SemanticAttribute &) = default;
};

struct SemanticClass {
    std::uint32_t id = 0; // Stable taxonomy id; zero is valid.
    std::string key; // Machine-readable, unique within the taxonomy.
    std::string name;
    std::string description;
    std::string category;
    std::optional<std::uint32_t> parentId;
    std::uint32_t controlColor = 0xff000000U; // Exact opaque model-palette ARGB.
    std::string externalId;
    std::vector<std::string> aliases;
    friend bool operator==(const SemanticClass &, const SemanticClass &) = default;
};

struct SemanticTaxonomy {
    std::string id;
    std::string version;
    std::string sourceUri;
    std::vector<SemanticClass> classes;
    friend bool operator==(const SemanticTaxonomy &, const SemanticTaxonomy &) = default;
};

enum class SemanticOrigin : std::uint8_t { Manual, Imported, Model };

struct SemanticRegion {
    std::uint32_t id = 0; // Nonzero stable region id, shared across frame masks.
    std::uint32_t classId = 0;
    std::uint32_t maskColor = 0xffffffffU; // Unique opaque identity color, not the model palette.
    std::string name;
    std::string description;
    std::optional<std::uint32_t> instanceId; // Optional cross-region/cross-frame object identity.
    std::optional<double> confidence; // [0,1]; absent means not measured.
    SemanticOrigin origin = SemanticOrigin::Manual;
    std::string generator;
    std::string sourceReference;
    std::vector<SemanticAttribute> attributes;
    friend bool operator==(const SemanticRegion &, const SemanticRegion &) = default;
};

struct SemanticSegmentation {
    SemanticTaxonomy taxonomy;
    std::uint32_t voidMaskColor = 0xff000000U;
    std::uint32_t voidControlColor = 0xff000000U;
    std::vector<SemanticRegion> regions; // May be absent in individual frames.
    friend bool operator==(const SemanticSegmentation &, const SemanticSegmentation &) = default;
};

struct SemanticIssue {
    std::string path;
    std::string message;
};

struct SemanticRegionGeometry {
    std::uint32_t regionId = 0;
    std::uint32_t classId = 0;
    std::uint64_t pixelCount = 0;
    struct Bounds { std::int32_t x = 0, y = 0, width = 0, height = 0; } bounds;
    struct Centroid { double x = 0, y = 0; } centroid; // Pixel centers; zero for absent regions.
};

struct SemanticControlMapResult {
    RasterLayer pixels; // Exact class colors in native mask coordinates; no resampling/blending.
    std::vector<std::uint32_t> classIds;
    std::vector<std::uint32_t> regionIds;
    std::vector<std::uint8_t> validPixels; // Distinguishes void from valid class id zero.
    std::vector<SemanticRegionGeometry> regions; // Definition order, including absent regions.
    std::string message;
    [[nodiscard]] bool ok() const noexcept { return message.empty(); }
};

IISHAREDCANVAS_EXPORT bool isSemanticMaskAsset(const Document &, const std::string &assetId) noexcept;
IISHAREDCANVAS_EXPORT SemanticSegmentLayer *findSemanticSegmentLayer(Document &, const std::string &id) noexcept;
IISHAREDCANVAS_EXPORT const SemanticSegmentLayer *findSemanticSegmentLayer(const Document &, const std::string &id) noexcept;
IISHAREDCANVAS_EXPORT const SemanticClass *findSemanticClass(const SemanticSegmentation &, std::uint32_t id) noexcept;
IISHAREDCANVAS_EXPORT const SemanticRegion *findSemanticRegion(const SemanticSegmentation &, std::uint32_t id) noexcept;
IISHAREDCANVAS_EXPORT std::vector<SemanticIssue> validateSemanticSegment(const Document &, const SemanticSegmentLayer &);
// Ignores display visibility/opacity/transform. Honors control.enabled and source frame range.
// maxPixels bounds the four dense output buffers before allocation.
IISHAREDCANVAS_EXPORT SemanticControlMapResult renderSemanticControlMap(
    const Document &, const std::string &layerId, std::uint32_t frame,
    std::uint64_t maxPixels = 16ULL * 1024ULL * 1024ULL);

} // namespace iiSharedCanvas
