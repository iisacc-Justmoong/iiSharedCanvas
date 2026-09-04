#pragma once

#include "iiSharedCanvas/Export.h"

#include <cstdint>
#include <optional>
#include <string>
#include <vector>

namespace iiSharedCanvas {

struct CameraRawExtent {
    std::uint32_t width = 0;
    std::uint32_t height = 0;
};

struct CameraRawOrigin {
    std::uint32_t x = 0;
    std::uint32_t y = 0;
};

struct CameraRawRegion {
    CameraRawOrigin origin;
    CameraRawExtent extent;
};

enum class CameraRawImageKind : std::uint8_t {
    ColorFilterArray,
    Monochrome,
    LinearRaw,
};

enum class CameraRawOrientation : std::uint8_t {
    Identity,
    MirrorHorizontal,
    Rotate180,
    MirrorVertical,
    Transpose,
    Rotate90Clockwise,
    Transverse,
    Rotate270Clockwise,
};

enum class CameraRawChannelRole : std::uint8_t {
    Red,
    Green,
    Blue,
    Cyan,
    Magenta,
    Yellow,
    White,
    Luminance,
    Infrared,
    Ultraviolet,
    Other,
};

struct CameraRawColorChannel {
    CameraRawChannelRole role = CameraRawChannelRole::Other;
    std::string name;
};

struct CameraRawCfaPattern {
    std::uint16_t columns = 0;
    std::uint16_t rows = 0;
    std::vector<std::uint16_t> channelIndices;
};

struct CameraRawLevelPattern {
    std::uint16_t columns = 0;
    std::uint16_t rows = 0;
    std::vector<double> values;
};

struct CameraRawSensorImage {
    CameraRawImageKind kind = CameraRawImageKind::ColorFilterArray;
    CameraRawExtent extent;
    std::uint16_t bitsPerSample = 0;
    std::uint16_t samplesPerPixel = 1;
    std::vector<std::uint32_t> samples;
    CameraRawOrientation orientation = CameraRawOrientation::Identity;
    std::optional<CameraRawRegion> activeArea;
    std::optional<CameraRawRegion> defaultCrop;
    std::vector<CameraRawColorChannel> colorChannels;
    std::optional<CameraRawCfaPattern> cfaPattern;
    std::optional<CameraRawLevelPattern> blackLevel;
    std::vector<double> whiteLevel;
};

struct CameraRawColorCalibration {
    std::string illuminant;
    std::vector<double> xyzToCamera;
};

struct CameraRawColorProfile {
    std::vector<double> asShotNeutral;
    std::vector<CameraRawColorCalibration> calibrations;
};

struct CameraRawCameraMetadata {
    std::string manufacturer;
    std::string model;
    std::string uniqueModel;
    std::string serialNumber;
};

struct CameraRawLensMetadata {
    std::string manufacturer;
    std::string model;
    std::string serialNumber;
    std::optional<double> minFocalLengthMm;
    std::optional<double> maxFocalLengthMm;
    std::optional<double> minFNumberAtMinFocal;
    std::optional<double> minFNumberAtMaxFocal;
};

struct CameraRawRational {
    std::uint32_t numerator = 0;
    std::uint32_t denominator = 1;
};

struct CameraRawCaptureMetadata {
    std::optional<CameraRawRational> exposureTimeSeconds;
    std::optional<double> fNumber;
    std::optional<std::uint32_t> isoSpeed;
    std::optional<double> focalLengthMm;
    std::optional<double> focusDistanceMeters;
    std::optional<double> exposureCompensationEv;
};

struct CameraRawData {
    CameraRawSensorImage image;
    CameraRawColorProfile color;
    CameraRawCameraMetadata camera;
    CameraRawLensMetadata lens;
    CameraRawCaptureMetadata capture;
};

enum class CameraRawValidationCode : std::uint8_t {
    InvalidExtent,
    InvalidSampleLayout,
    InvalidSampleCount,
    SampleOutOfRange,
    InvalidActiveArea,
    InvalidDefaultCrop,
    InvalidColorChannels,
    InvalidCfaPattern,
    InvalidBlackLevel,
    InvalidWhiteLevel,
    InvalidWhiteBalance,
    InvalidColorCalibration,
    InvalidLensMetadata,
    InvalidCaptureMetadata,
};

struct CameraRawValidationIssue {
    CameraRawValidationCode code;
    std::string path;
    std::string message;
};

struct CameraRawValidationResult {
    std::vector<CameraRawValidationIssue> issues;

    [[nodiscard]] bool ok() const noexcept { return issues.empty(); }
};

IISHAREDCANVAS_EXPORT CameraRawRegion cameraRawActiveArea(
    const CameraRawSensorImage &image) noexcept;
IISHAREDCANVAS_EXPORT CameraRawRegion cameraRawDefaultCrop(
    const CameraRawSensorImage &image) noexcept;
IISHAREDCANVAS_EXPORT std::optional<std::uint32_t> cameraRawSampleAt(
    const CameraRawSensorImage &image,
    std::uint32_t x,
    std::uint32_t y,
    std::uint16_t samplePlane = 0) noexcept;
IISHAREDCANVAS_EXPORT std::optional<std::uint16_t> cameraRawChannelIndexAt(
    const CameraRawSensorImage &image,
    std::uint32_t x,
    std::uint32_t y,
    std::uint16_t samplePlane = 0) noexcept;
IISHAREDCANVAS_EXPORT std::optional<double> cameraRawBlackLevelAt(
    const CameraRawSensorImage &image,
    std::uint32_t x,
    std::uint32_t y,
    std::uint16_t samplePlane = 0) noexcept;
IISHAREDCANVAS_EXPORT std::optional<double> cameraRawWhiteLevelAt(
    const CameraRawSensorImage &image,
    std::uint16_t samplePlane = 0) noexcept;
IISHAREDCANVAS_EXPORT CameraRawValidationResult validateCameraRaw(
    const CameraRawData &raw);

} // namespace iiSharedCanvas
