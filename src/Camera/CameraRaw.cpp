#include "Camera/CameraRaw.h"

#include <algorithm>
#include <cmath>
#include <limits>
#include <string>

namespace iiSharedCanvas {
namespace {

void addIssue(CameraRawValidationResult &result,
              CameraRawValidationCode code,
              std::string path,
              std::string message)
{
    result.issues.push_back({code, std::move(path), std::move(message)});
}

bool checkedMultiply(std::size_t left,
                     std::size_t right,
                     std::size_t &product) noexcept
{
    if (left != 0 && right > std::numeric_limits<std::size_t>::max() / left) {
        return false;
    }
    product = left * right;
    return true;
}

std::optional<std::size_t> expectedSampleCount(
    const CameraRawSensorImage &image) noexcept
{
    std::size_t pixels = 0;
    std::size_t samples = 0;
    if (!checkedMultiply(static_cast<std::size_t>(image.extent.width),
                         static_cast<std::size_t>(image.extent.height),
                         pixels)
        || !checkedMultiply(pixels,
                            static_cast<std::size_t>(image.samplesPerPixel),
                            samples)) {
        return std::nullopt;
    }
    return samples;
}

bool regionHasArea(const CameraRawRegion &region) noexcept
{
    return region.extent.width != 0 && region.extent.height != 0;
}

bool regionInsideExtent(const CameraRawRegion &region,
                        const CameraRawExtent &extent) noexcept
{
    return regionHasArea(region)
        && region.origin.x <= extent.width
        && region.origin.y <= extent.height
        && region.extent.width <= extent.width - region.origin.x
        && region.extent.height <= extent.height - region.origin.y;
}

bool regionContains(const CameraRawRegion &outer,
                    const CameraRawRegion &inner) noexcept
{
    if (!regionHasArea(outer) || !regionHasArea(inner)
        || inner.origin.x < outer.origin.x
        || inner.origin.y < outer.origin.y) {
        return false;
    }
    const std::uint32_t relativeX = inner.origin.x - outer.origin.x;
    const std::uint32_t relativeY = inner.origin.y - outer.origin.y;
    return relativeX <= outer.extent.width
        && relativeY <= outer.extent.height
        && inner.extent.width <= outer.extent.width - relativeX
        && inner.extent.height <= outer.extent.height - relativeY;
}

bool regionContainsPoint(const CameraRawRegion &region,
                         std::uint32_t x,
                         std::uint32_t y) noexcept
{
    return regionHasArea(region)
        && x >= region.origin.x
        && y >= region.origin.y
        && x - region.origin.x < region.extent.width
        && y - region.origin.y < region.extent.height;
}

std::optional<std::uint32_t> maximumSampleCode(std::uint16_t bits) noexcept
{
    if (bits == 0 || bits > 32) {
        return std::nullopt;
    }
    if (bits == 32) {
        return std::numeric_limits<std::uint32_t>::max();
    }
    return (std::uint32_t{1} << bits) - 1U;
}

bool finitePositive(const std::optional<double> &value) noexcept
{
    return !value || (std::isfinite(*value) && *value > 0.0);
}

std::optional<double> explicitWhiteLevelAt(const CameraRawSensorImage &image,
                                           std::uint16_t samplePlane) noexcept
{
    if (samplePlane >= image.samplesPerPixel || image.whiteLevel.empty()) {
        return std::nullopt;
    }
    if (image.whiteLevel.size() == 1) {
        return image.whiteLevel.front();
    }
    if (image.whiteLevel.size() != image.samplesPerPixel) {
        return std::nullopt;
    }
    return image.whiteLevel[samplePlane];
}

} // namespace

CameraRawRegion cameraRawActiveArea(const CameraRawSensorImage &image) noexcept
{
    return image.activeArea.value_or(CameraRawRegion{{0, 0}, image.extent});
}

CameraRawRegion cameraRawDefaultCrop(const CameraRawSensorImage &image) noexcept
{
    return image.defaultCrop.value_or(cameraRawActiveArea(image));
}

std::optional<std::uint32_t> cameraRawSampleAt(const CameraRawSensorImage &image,
                                               std::uint32_t x,
                                               std::uint32_t y,
                                               std::uint16_t samplePlane) noexcept
{
    if (x >= image.extent.width
        || y >= image.extent.height
        || samplePlane >= image.samplesPerPixel) {
        return std::nullopt;
    }

    std::size_t rowOffset = 0;
    std::size_t pixelOffset = 0;
    std::size_t sampleOffset = 0;
    if (!checkedMultiply(static_cast<std::size_t>(y),
                         static_cast<std::size_t>(image.extent.width),
                         rowOffset)
        || rowOffset > std::numeric_limits<std::size_t>::max() - x
        || !checkedMultiply(rowOffset + x,
                            static_cast<std::size_t>(image.samplesPerPixel),
                            pixelOffset)
        || pixelOffset > std::numeric_limits<std::size_t>::max() - samplePlane) {
        return std::nullopt;
    }
    sampleOffset = pixelOffset + samplePlane;
    return sampleOffset < image.samples.size()
        ? std::optional<std::uint32_t>{image.samples[sampleOffset]}
        : std::nullopt;
}

std::optional<std::uint16_t> cameraRawChannelIndexAt(
    const CameraRawSensorImage &image,
    std::uint32_t x,
    std::uint32_t y,
    std::uint16_t samplePlane) noexcept
{
    if (x >= image.extent.width
        || y >= image.extent.height
        || samplePlane >= image.samplesPerPixel) {
        return std::nullopt;
    }

    const CameraRawRegion active = cameraRawActiveArea(image);
    if (!regionContainsPoint(active, x, y)) {
        return std::nullopt;
    }

    switch (image.kind) {
    case CameraRawImageKind::ColorFilterArray: {
        if (samplePlane != 0 || !image.cfaPattern
            || image.cfaPattern->columns == 0
            || image.cfaPattern->rows == 0) {
            return std::nullopt;
        }
        const std::uint32_t column =
            (x - active.origin.x) % image.cfaPattern->columns;
        const std::uint32_t row =
            (y - active.origin.y) % image.cfaPattern->rows;
        const std::size_t index = static_cast<std::size_t>(row)
            * image.cfaPattern->columns + column;
        if (index >= image.cfaPattern->channelIndices.size()) {
            return std::nullopt;
        }
        const std::uint16_t channel = image.cfaPattern->channelIndices[index];
        return channel < image.colorChannels.size()
            ? std::optional<std::uint16_t>{channel}
            : std::nullopt;
    }
    case CameraRawImageKind::Monochrome:
        return samplePlane == 0 && !image.colorChannels.empty()
            ? std::optional<std::uint16_t>{0}
            : std::nullopt;
    case CameraRawImageKind::LinearRaw:
        return samplePlane < image.colorChannels.size()
            ? std::optional<std::uint16_t>{samplePlane}
            : std::nullopt;
    }
    return std::nullopt;
}

std::optional<double> cameraRawBlackLevelAt(const CameraRawSensorImage &image,
                                            std::uint32_t x,
                                            std::uint32_t y,
                                            std::uint16_t samplePlane) noexcept
{
    if (x >= image.extent.width
        || y >= image.extent.height
        || samplePlane >= image.samplesPerPixel) {
        return std::nullopt;
    }

    const CameraRawRegion active = cameraRawActiveArea(image);
    if (!regionContainsPoint(active, x, y)) {
        return std::nullopt;
    }
    if (!image.blackLevel) {
        return 0.0;
    }
    if (image.blackLevel->columns == 0 || image.blackLevel->rows == 0) {
        return std::nullopt;
    }

    const std::size_t column =
        (x - active.origin.x) % image.blackLevel->columns;
    const std::size_t row =
        (y - active.origin.y) % image.blackLevel->rows;
    const std::size_t index = (row * image.blackLevel->columns + column)
        * image.samplesPerPixel + samplePlane;
    if (index >= image.blackLevel->values.size()) {
        return std::nullopt;
    }
    const double value = image.blackLevel->values[index];
    return std::isfinite(value) && value >= 0.0
        ? std::optional<double>{value}
        : std::nullopt;
}

std::optional<double> cameraRawWhiteLevelAt(const CameraRawSensorImage &image,
                                            std::uint16_t samplePlane) noexcept
{
    if (samplePlane >= image.samplesPerPixel) {
        return std::nullopt;
    }
    if (const auto explicitLevel = explicitWhiteLevelAt(image, samplePlane)) {
        return std::isfinite(*explicitLevel) && *explicitLevel > 0.0
            ? explicitLevel
            : std::nullopt;
    }
    if (!image.whiteLevel.empty()) {
        return std::nullopt;
    }
    const auto maximum = maximumSampleCode(image.bitsPerSample);
    return maximum ? std::optional<double>{static_cast<double>(*maximum)}
                   : std::nullopt;
}

CameraRawValidationResult validateCameraRaw(const CameraRawData &raw)
{
    CameraRawValidationResult result;
    const CameraRawSensorImage &image = raw.image;

    if (image.extent.width == 0 || image.extent.height == 0) {
        addIssue(result, CameraRawValidationCode::InvalidExtent,
                 "image.extent",
                 "sensor width and height must both be positive");
    }

    const auto maximum = maximumSampleCode(image.bitsPerSample);
    if (!maximum || image.samplesPerPixel == 0) {
        addIssue(result, CameraRawValidationCode::InvalidSampleLayout,
                 "image",
                 "integer raw data requires one through 32 bits and at least one sample plane");
    }

    const auto expectedSamples = expectedSampleCount(image);
    if (!expectedSamples || image.samples.size() != *expectedSamples) {
        addIssue(result, CameraRawValidationCode::InvalidSampleCount,
                 "image.samples",
                 "sample storage must equal width times height times samplesPerPixel");
    }
    if (maximum) {
        const auto invalidSample = std::find_if(
            image.samples.begin(), image.samples.end(),
            [maximum](std::uint32_t sample) { return sample > *maximum; });
        if (invalidSample != image.samples.end()) {
            addIssue(result, CameraRawValidationCode::SampleOutOfRange,
                     "image.samples["
                         + std::to_string(std::distance(image.samples.begin(), invalidSample))
                         + "]",
                     "a sample exceeds the maximum code for bitsPerSample");
        }
    }

    const CameraRawRegion active = cameraRawActiveArea(image);
    if (!regionInsideExtent(active, image.extent)) {
        addIssue(result, CameraRawValidationCode::InvalidActiveArea,
                 "image.activeArea",
                 "the active area must be positive and contained by the sensor extent");
    }
    const CameraRawRegion crop = cameraRawDefaultCrop(image);
    if (!regionContains(active, crop)) {
        addIssue(result, CameraRawValidationCode::InvalidDefaultCrop,
                 "image.defaultCrop",
                 "the default crop must be positive and contained by the active area");
    }

    switch (image.kind) {
    case CameraRawImageKind::ColorFilterArray:
        if (image.samplesPerPixel != 1) {
            addIssue(result, CameraRawValidationCode::InvalidSampleLayout,
                     "image.samplesPerPixel",
                     "CFA raw data contains one sensor sample at each pixel");
        }
        if (image.colorChannels.empty()) {
            addIssue(result, CameraRawValidationCode::InvalidColorChannels,
                     "image.colorChannels",
                     "CFA raw data requires at least one color channel");
        }
        if (!image.cfaPattern) {
            addIssue(result, CameraRawValidationCode::InvalidCfaPattern,
                     "image.cfaPattern",
                     "CFA raw data requires a repeating channel pattern");
        }
        break;
    case CameraRawImageKind::Monochrome:
        if (image.samplesPerPixel != 1) {
            addIssue(result, CameraRawValidationCode::InvalidSampleLayout,
                     "image.samplesPerPixel",
                     "monochrome raw data contains one sample at each pixel");
        }
        if (image.colorChannels.size() != 1) {
            addIssue(result, CameraRawValidationCode::InvalidColorChannels,
                     "image.colorChannels",
                     "monochrome raw data requires exactly one channel");
        }
        if (image.cfaPattern) {
            addIssue(result, CameraRawValidationCode::InvalidCfaPattern,
                     "image.cfaPattern",
                     "monochrome raw data must not carry a CFA pattern");
        }
        break;
    case CameraRawImageKind::LinearRaw:
        if (image.colorChannels.size() != image.samplesPerPixel) {
            addIssue(result, CameraRawValidationCode::InvalidColorChannels,
                     "image.colorChannels",
                     "linear raw sample planes must correspond to color channels");
        }
        if (image.cfaPattern) {
            addIssue(result, CameraRawValidationCode::InvalidCfaPattern,
                     "image.cfaPattern",
                     "linear raw data must not carry a CFA pattern");
        }
        break;
    default:
        addIssue(result, CameraRawValidationCode::InvalidSampleLayout,
                 "image.kind", "the raw image kind is unknown");
        break;
    }

    if (image.cfaPattern) {
        std::size_t patternCells = 0;
        const bool validDimensions = image.cfaPattern->columns != 0
            && image.cfaPattern->rows != 0
            && checkedMultiply(image.cfaPattern->columns,
                               image.cfaPattern->rows,
                               patternCells);
        const bool validCount = validDimensions
            && image.cfaPattern->channelIndices.size() == patternCells;
        const bool validReferences = validCount && std::all_of(
            image.cfaPattern->channelIndices.begin(),
            image.cfaPattern->channelIndices.end(),
            [&image](std::uint16_t channel) {
                return channel < image.colorChannels.size();
            });
        if (!validReferences) {
            addIssue(result, CameraRawValidationCode::InvalidCfaPattern,
                     "image.cfaPattern",
                     "CFA dimensions, cell count, and channel references must agree");
        }
    }

    bool validBlackLevel = true;
    if (image.blackLevel) {
        std::size_t repeatCells = 0;
        std::size_t levelCount = 0;
        validBlackLevel = image.blackLevel->columns != 0
            && image.blackLevel->rows != 0
            && checkedMultiply(image.blackLevel->columns,
                               image.blackLevel->rows,
                               repeatCells)
            && checkedMultiply(repeatCells,
                               image.samplesPerPixel,
                               levelCount)
            && image.blackLevel->values.size() == levelCount;
        if (validBlackLevel && maximum) {
            validBlackLevel = std::all_of(
                image.blackLevel->values.begin(), image.blackLevel->values.end(),
                [maximum](double value) {
                    return std::isfinite(value)
                        && value >= 0.0
                        && value <= static_cast<double>(*maximum);
                });
        }
        if (!validBlackLevel) {
            addIssue(result, CameraRawValidationCode::InvalidBlackLevel,
                     "image.blackLevel",
                     "black levels must be finite sample codes in row-column-plane order");
        }
    }

    bool validWhiteLevel = image.whiteLevel.empty()
        || image.whiteLevel.size() == 1
        || image.whiteLevel.size() == image.samplesPerPixel;
    if (validWhiteLevel && maximum) {
        validWhiteLevel = std::all_of(
            image.whiteLevel.begin(), image.whiteLevel.end(),
            [maximum](double value) {
                return std::isfinite(value)
                    && value > 0.0
                    && value <= static_cast<double>(*maximum);
            });
    }
    if (validWhiteLevel && validBlackLevel && image.blackLevel) {
        for (std::size_t index = 0;
             index < image.blackLevel->values.size();
             ++index) {
            const std::uint16_t plane = static_cast<std::uint16_t>(
                index % image.samplesPerPixel);
            const auto white = cameraRawWhiteLevelAt(image, plane);
            if (!white || image.blackLevel->values[index] >= *white) {
                validWhiteLevel = false;
                break;
            }
        }
    }
    if (!validWhiteLevel) {
        addIssue(result, CameraRawValidationCode::InvalidWhiteLevel,
                 "image.whiteLevel",
                 "white levels must be finite, within the sample range, and above black level");
    }

    if (!raw.color.asShotNeutral.empty()) {
        const bool validNeutral =
            raw.color.asShotNeutral.size() == image.colorChannels.size()
            && std::all_of(raw.color.asShotNeutral.begin(),
                           raw.color.asShotNeutral.end(),
                           [](double value) {
                               return std::isfinite(value) && value > 0.0;
                           });
        if (!validNeutral) {
            addIssue(result, CameraRawValidationCode::InvalidWhiteBalance,
                     "color.asShotNeutral",
                     "as-shot neutral coordinates must contain one finite positive value per color channel");
        }
    }

    std::size_t matrixSize = 0;
    const bool matrixSizeValid = checkedMultiply(image.colorChannels.size(),
                                                 std::size_t{3},
                                                 matrixSize);
    for (std::size_t index = 0;
         index < raw.color.calibrations.size();
         ++index) {
        const CameraRawColorCalibration &calibration =
            raw.color.calibrations[index];
        const bool validMatrix = matrixSizeValid
            && calibration.xyzToCamera.size() == matrixSize
            && std::all_of(calibration.xyzToCamera.begin(),
                           calibration.xyzToCamera.end(),
                           [](double value) { return std::isfinite(value); });
        if (!validMatrix) {
            addIssue(result, CameraRawValidationCode::InvalidColorCalibration,
                     "color.calibrations[" + std::to_string(index) + "]",
                     "an XYZ-to-camera matrix requires three finite values per color channel");
        }
    }

    const bool validLensValues = finitePositive(raw.lens.minFocalLengthMm)
        && finitePositive(raw.lens.maxFocalLengthMm)
        && finitePositive(raw.lens.minFNumberAtMinFocal)
        && finitePositive(raw.lens.minFNumberAtMaxFocal);
    const bool validLensRange = !raw.lens.minFocalLengthMm
        || !raw.lens.maxFocalLengthMm
        || *raw.lens.minFocalLengthMm <= *raw.lens.maxFocalLengthMm;
    if (!validLensValues || !validLensRange) {
        addIssue(result, CameraRawValidationCode::InvalidLensMetadata,
                 "lens",
                 "lens focal lengths and f-numbers must be finite, positive, and ordered");
    }

    bool validCapture = finitePositive(raw.capture.fNumber)
        && finitePositive(raw.capture.focalLengthMm)
        && finitePositive(raw.capture.focusDistanceMeters)
        && (!raw.capture.isoSpeed || *raw.capture.isoSpeed != 0)
        && (!raw.capture.exposureCompensationEv
            || std::isfinite(*raw.capture.exposureCompensationEv));
    if (raw.capture.exposureTimeSeconds) {
        validCapture = validCapture
            && raw.capture.exposureTimeSeconds->numerator != 0
            && raw.capture.exposureTimeSeconds->denominator != 0;
    }
    if (!validCapture) {
        addIssue(result, CameraRawValidationCode::InvalidCaptureMetadata,
                 "capture",
                 "capture values must be finite and positive, with non-zero rational terms");
    }

    return result;
}

} // namespace iiSharedCanvas
