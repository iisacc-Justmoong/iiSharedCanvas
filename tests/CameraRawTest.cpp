#include "Camera/CameraRaw.h"

#include <cmath>
#include <cstdint>
#include <iostream>
#include <limits>
#include <optional>

namespace {

int failures = 0;

void expect(bool condition, const char *message)
{
    if (!condition) {
        std::cerr << "FAIL: " << message << '\n';
        ++failures;
    }
}

bool contains(const iiSharedCanvas::CameraRawValidationResult &result,
              iiSharedCanvas::CameraRawValidationCode code)
{
    for (const auto &issue : result.issues) {
        if (issue.code == code) {
            return true;
        }
    }
    return false;
}

iiSharedCanvas::CameraRawData validCfaRaw()
{
    using namespace iiSharedCanvas;

    CameraRawData raw;
    raw.image.kind = CameraRawImageKind::ColorFilterArray;
    raw.image.extent = {4, 2};
    raw.image.bitsPerSample = 12;
    raw.image.samplesPerPixel = 1;
    raw.image.samples = {64, 65, 66, 67, 68, 69, 70, 71};
    raw.image.orientation = CameraRawOrientation::Rotate90Clockwise;
    raw.image.activeArea = CameraRawRegion{{1, 0}, {2, 2}};
    raw.image.defaultCrop = CameraRawRegion{{1, 0}, {2, 1}};
    raw.image.colorChannels = {
        {CameraRawChannelRole::Red, "red"},
        {CameraRawChannelRole::Green, "green"},
        {CameraRawChannelRole::Blue, "blue"},
    };
    raw.image.cfaPattern = CameraRawCfaPattern{2, 2, {0, 1, 1, 2}};
    raw.image.blackLevel = CameraRawLevelPattern{2, 2, {64.0, 65.0, 66.0, 67.0}};
    raw.image.whiteLevel = {4095.0};

    raw.color.asShotNeutral = {0.5, 1.0, 0.75};
    raw.color.calibrations.push_back(CameraRawColorCalibration{
        "D65",
        {
            0.62, 0.21, 0.17,
            0.18, 0.72, 0.10,
            0.03, 0.12, 0.85,
        },
    });

    raw.camera.manufacturer = "Example Camera Company";
    raw.camera.model = "Reference 1";
    raw.camera.uniqueModel = "Example Camera Company Reference 1";
    raw.camera.serialNumber = "BODY-001";

    raw.lens.manufacturer = "Example Optics";
    raw.lens.model = "24-70mm";
    raw.lens.serialNumber = "LENS-001";
    raw.lens.minFocalLengthMm = 24.0;
    raw.lens.maxFocalLengthMm = 70.0;
    raw.lens.minFNumberAtMinFocal = 2.8;
    raw.lens.minFNumberAtMaxFocal = 2.8;

    raw.capture.exposureTimeSeconds = CameraRawRational{1, 125};
    raw.capture.fNumber = 5.6;
    raw.capture.isoSpeed = 400;
    raw.capture.focalLengthMm = 50.0;
    raw.capture.focusDistanceMeters = 2.5;
    raw.capture.exposureCompensationEv = -0.333333333333;
    return raw;
}

} // namespace

int main()
{
    using namespace iiSharedCanvas;

    CameraRawData raw = validCfaRaw();
    const CameraRawValidationResult valid = validateCameraRaw(raw);
    expect(valid.ok(), "a complete CFA sensor payload must validate");

    expect(cameraRawSampleAt(raw.image, 2, 1, 0)
               == std::optional<std::uint32_t>{70},
           "sample access must use canonical row-pixel-plane order");
    expect(!cameraRawSampleAt(raw.image, 4, 0, 0),
           "sample access must reject coordinates outside the sensor extent");
    expect(cameraRawChannelIndexAt(raw.image, 1, 0, 0)
               == std::optional<std::uint16_t>{0},
           "the CFA pattern must start at the active-area origin");
    expect(cameraRawChannelIndexAt(raw.image, 2, 1, 0)
               == std::optional<std::uint16_t>{2},
           "CFA channel lookup must repeat in row-column order");
    expect(!cameraRawChannelIndexAt(raw.image, 0, 0, 0),
           "CFA lookup must reject masked pixels outside the active area");
    expect(cameraRawBlackLevelAt(raw.image, 2, 1, 0)
               == std::optional<double>{67.0},
           "black-level patterns must repeat from the active-area origin");
    expect(cameraRawWhiteLevelAt(raw.image, 0)
               == std::optional<double>{4095.0},
           "an explicit white level must be exposed per sample plane");

    const CameraRawRegion active = cameraRawActiveArea(raw.image);
    const CameraRawRegion crop = cameraRawDefaultCrop(raw.image);
    expect(active.origin.x == 1 && active.extent.width == 2,
           "the explicit active area must be preserved");
    expect(crop.origin.y == 0 && crop.extent.height == 1,
           "the explicit default crop must be preserved");

    CameraRawData linear;
    linear.image.kind = CameraRawImageKind::LinearRaw;
    linear.image.extent = {1, 1};
    linear.image.bitsPerSample = 16;
    linear.image.samplesPerPixel = 3;
    linear.image.samples = {1, 2, 3};
    linear.image.colorChannels = {
        {CameraRawChannelRole::Red, "red"},
        {CameraRawChannelRole::Green, "green"},
        {CameraRawChannelRole::Blue, "blue"},
    };
    expect(validateCameraRaw(linear).ok(),
           "linear raw data must support interleaved multi-plane samples");
    expect(cameraRawChannelIndexAt(linear.image, 0, 0, 2)
               == std::optional<std::uint16_t>{2},
           "linear raw sample planes must map directly to color channels");
    expect(cameraRawWhiteLevelAt(linear.image, 2)
               == std::optional<double>{65535.0},
           "an omitted white level must default to the integer sample maximum");
    expect(cameraRawActiveArea(linear.image).extent.width == 1
               && cameraRawDefaultCrop(linear.image).extent.height == 1,
           "omitted active area and crop must fall back to the full sensor area");

    CameraRawData monochrome;
    monochrome.image.kind = CameraRawImageKind::Monochrome;
    monochrome.image.extent = {1, 1};
    monochrome.image.bitsPerSample = 14;
    monochrome.image.samplesPerPixel = 1;
    monochrome.image.samples = {2048};
    monochrome.image.colorChannels = {
        {CameraRawChannelRole::Luminance, "luminance"},
    };
    expect(validateCameraRaw(monochrome).ok(),
           "monochrome raw data must not require a CFA or color calibration");

    CameraRawData wrongCount = raw;
    wrongCount.image.samples.pop_back();
    expect(contains(validateCameraRaw(wrongCount),
                    CameraRawValidationCode::InvalidSampleCount),
           "the exact sensor sample count must be enforced");

    CameraRawData outOfRange = raw;
    outOfRange.image.samples.front() = 4096;
    expect(contains(validateCameraRaw(outOfRange),
                    CameraRawValidationCode::SampleOutOfRange),
           "sample codes above the declared bit depth must fail");

    CameraRawData badActive = raw;
    badActive.image.activeArea = CameraRawRegion{{3, 0}, {2, 2}};
    expect(contains(validateCameraRaw(badActive),
                    CameraRawValidationCode::InvalidActiveArea),
           "the active area must remain inside the sensor extent");

    CameraRawData badCrop = raw;
    badCrop.image.defaultCrop = CameraRawRegion{{0, 0}, {2, 1}};
    expect(contains(validateCameraRaw(badCrop),
                    CameraRawValidationCode::InvalidDefaultCrop),
           "the default crop must remain inside the active area");

    CameraRawData badCfa = raw;
    badCfa.image.cfaPattern->channelIndices.back() = 3;
    expect(contains(validateCameraRaw(badCfa),
                    CameraRawValidationCode::InvalidCfaPattern),
           "CFA cells must reference an existing color channel");

    CameraRawData missingCfa = raw;
    missingCfa.image.cfaPattern.reset();
    expect(contains(validateCameraRaw(missingCfa),
                    CameraRawValidationCode::InvalidCfaPattern),
           "a CFA image must contain one pattern");

    CameraRawData badChannels = linear;
    badChannels.image.colorChannels.pop_back();
    expect(contains(validateCameraRaw(badChannels),
                    CameraRawValidationCode::InvalidColorChannels),
           "linear sample planes and color channels must correspond");

    CameraRawData badBlack = raw;
    badBlack.image.blackLevel->values.pop_back();
    expect(contains(validateCameraRaw(badBlack),
                    CameraRawValidationCode::InvalidBlackLevel),
           "black-level storage must match repeat dimensions and sample planes");

    CameraRawData blackAtWhite = raw;
    blackAtWhite.image.blackLevel->values.front() = 4095.0;
    expect(contains(validateCameraRaw(blackAtWhite),
                    CameraRawValidationCode::InvalidWhiteLevel),
           "white level must be greater than every matching black level");

    CameraRawData badWhiteBalance = raw;
    badWhiteBalance.color.asShotNeutral = {1.0, 0.0, 1.0};
    expect(contains(validateCameraRaw(badWhiteBalance),
                    CameraRawValidationCode::InvalidWhiteBalance),
           "white-balance neutral coordinates must be finite and positive");

    CameraRawData badMatrix = raw;
    badMatrix.color.calibrations.front().xyzToCamera.pop_back();
    expect(contains(validateCameraRaw(badMatrix),
                    CameraRawValidationCode::InvalidColorCalibration),
           "each XYZ-to-camera matrix must have colorChannels times three values");

    CameraRawData nonFiniteMatrix = raw;
    nonFiniteMatrix.color.calibrations.front().xyzToCamera.front() =
        std::numeric_limits<double>::quiet_NaN();
    expect(contains(validateCameraRaw(nonFiniteMatrix),
                    CameraRawValidationCode::InvalidColorCalibration),
           "color calibration values must be finite");

    CameraRawData badLens = raw;
    badLens.lens.minFocalLengthMm = 80.0;
    expect(contains(validateCameraRaw(badLens),
                    CameraRawValidationCode::InvalidLensMetadata),
           "lens focal ranges must be positive and ordered");

    CameraRawData badCapture = raw;
    badCapture.capture.exposureTimeSeconds = CameraRawRational{1, 0};
    expect(contains(validateCameraRaw(badCapture),
                    CameraRawValidationCode::InvalidCaptureMetadata),
           "capture rationals must reject a zero denominator");

    CameraRawData badBits = raw;
    badBits.image.bitsPerSample = 0;
    expect(contains(validateCameraRaw(badBits),
                    CameraRawValidationCode::InvalidSampleLayout),
           "integer sensor samples must declare a bit depth from one through 32");

    return failures == 0 ? 0 : 1;
}
