#pragma once
#include "iiSharedCanvas/Export.h"
#include "ControlNet/ControlNet.h"
#include <Layer/RasterLayer.h>
#include <array>
#include <cstdint>
#include <optional>
#include <span>
#include <string>
#include <vector>

namespace iiSharedCanvas {
struct Document; struct PoseAsset; struct PoseLayer; struct VectorAsset;
inline constexpr std::size_t PoseEyeAnchorCount = 68;
inline constexpr std::size_t PoseMouthAnchorCount = 143;
inline constexpr std::size_t PoseFaceAnchorCount = 523;
inline constexpr std::size_t PosePersonAnchorCount = 590;

enum class PoseVisibility : std::uint8_t { Missing, Visible, Occluded };
// Normalized viewport coordinates, x right/y down. Optional z is relative depth,
// not metric reconstruction. Missing is explicit; (0,0) is a valid location.
struct PoseAnchor {
    double x = 0, y = 0;
    std::optional<double> z;
    double confidence = 1;
    PoseVisibility visibility = PoseVisibility::Missing;
    bool locked = false;
    friend bool operator==(const PoseAnchor &, const PoseAnchor &) = default;
};
enum class PoseBodyJoint : std::uint8_t {
    Nose, Neck, RightShoulder, RightElbow, RightWrist, LeftShoulder, LeftElbow, LeftWrist,
    MidHip, RightHip, RightKnee, RightAnkle, LeftHip, LeftKnee, LeftAnkle,
    RightEye, LeftEye, RightEar, LeftEar, LeftBigToe, LeftSmallToe, LeftHeel,
    RightBigToe, RightSmallToe, RightHeel,
};
enum class PoseHandJoint : std::uint8_t {
    Wrist, ThumbCMC, ThumbMCP, ThumbIP, ThumbTip,
    IndexMCP, IndexPIP, IndexDIP, IndexTip, MiddleMCP, MiddlePIP, MiddleDIP, MiddleTip,
    RingMCP, RingPIP, RingDIP, RingTip, LittleMCP, LittlePIP, LittleDIP, LittleTip,
};
struct PoseEye {
    std::array<PoseAnchor,17> upperLid, lowerLid, crease;
    std::array<PoseAnchor,16> iris;
    std::array<PoseAnchor,1> pupil;
    friend bool operator==(const PoseEye &, const PoseEye &) = default;
};
struct PoseMouth {
    std::array<PoseAnchor,25> outerUpper, outerLower, innerUpper, innerLower;
    std::array<PoseAnchor,17> upperTeeth, lowerTeeth;
    std::array<PoseAnchor,9> tongue;
    friend bool operator==(const PoseMouth &, const PoseMouth &) = default;
};
struct PoseFace {
    std::array<PoseAnchor,33> outline;
    std::array<PoseAnchor,17> leftBrow, rightBrow;
    std::array<PoseAnchor,9> noseBridge;
    std::array<PoseAnchor,17> noseContour;
    PoseEye leftEye, rightEye;
    PoseMouth mouth;
    std::array<PoseAnchor,17> leftCheek, rightCheek, forehead;
    std::array<PoseAnchor,25> leftNasolabialFold, rightNasolabialFold, leftUnderEye, rightUnderEye;
    friend bool operator==(const PoseFace &, const PoseFace &) = default;
};
// Stable V1 group order is part of the native pose wire format.
enum class PoseGroup : std::uint8_t {
    Body, LeftHand, RightHand, Outline, LeftBrow, RightBrow, NoseBridge, NoseContour,
    LeftUpperLid, LeftLowerLid, LeftCrease, LeftIris, LeftPupil,
    RightUpperLid, RightLowerLid, RightCrease, RightIris, RightPupil,
    MouthOuterUpper, MouthOuterLower, MouthInnerUpper, MouthInnerLower,
    UpperTeeth, LowerTeeth, Tongue, LeftCheek, RightCheek, Forehead,
    LeftNasolabialFold, RightNasolabialFold, LeftUnderEye, RightUnderEye, Count,
};
struct PoseAnchorDelta {
    PoseGroup group = PoseGroup::Body;
    std::uint32_t index = 0;
    double dx = 0, dy = 0, dz = 0;
    friend bool operator==(const PoseAnchorDelta &, const PoseAnchorDelta &) = default;
};
struct PoseExpression {
    std::string id, name;
    double weight = 0; // [0,1], sparse weighted offsets applied on top of authored anchors.
    std::vector<PoseAnchorDelta> deltas;
    friend bool operator==(const PoseExpression &, const PoseExpression &) = default;
};
struct PosePerson {
    std::string id, name, trackId;
    bool enabled = true;
    std::array<PoseAnchor,25> body;
    std::array<PoseAnchor,21> leftHand, rightHand;
    PoseFace face;
    std::vector<PoseExpression> expressions;
    friend bool operator==(const PosePerson &, const PosePerson &) = default;
};
// NativeDetailed is the editable dense topology; OpenPose raster uses COCO18,
// 21+21 hands, 70 projected face points. Neither implies inference compatibility.
enum class PoseRenderProfile : std::uint8_t { NativeDetailed, OpenPose };
struct PoseRenderOptions {
    PoseRenderProfile profile = PoseRenderProfile::NativeDetailed;
    double minimumConfidence = 0.05;
    bool includeOccluded = true;
    double pointRadius = 1.5, lineWidth = 1.0; // Output pixels.
    std::uint64_t maximumPixels = 16ULL * 1024ULL * 1024ULL;
};
struct PoseControlMapResult {
    RasterLayer pixels;
    std::uint64_t nativeAnchorCount = 0, emittedAnchorCount = 0;
    std::vector<std::string> warnings;
    std::string message;
    [[nodiscard]] bool ok() const noexcept { return message.empty(); }
};
struct PoseKeypoint { double x = 0, y = 0, confidence = 0; };
struct OpenPosePerson {
    std::string id;
    std::array<PoseKeypoint,25> body;
    std::array<PoseKeypoint,21> leftHand, rightHand;
    std::array<PoseKeypoint,70> face;
};
struct OpenPoseKeypointsResult {
    std::vector<OpenPosePerson> people;
    std::vector<std::string> warnings;
    std::string message;
    [[nodiscard]] bool ok() const noexcept { return message.empty(); }
};
IISHAREDCANVAS_EXPORT std::span<PoseAnchor> poseAnchors(PosePerson &, PoseGroup) noexcept;
IISHAREDCANVAS_EXPORT std::span<const PoseAnchor> poseAnchors(const PosePerson &, PoseGroup) noexcept;
IISHAREDCANVAS_EXPORT PosePerson makeNeutralPosePerson(std::string id);
IISHAREDCANVAS_EXPORT PoseAsset *findPoseAsset(Document &, const std::string &) noexcept;
IISHAREDCANVAS_EXPORT const PoseAsset *findPoseAsset(const Document &, const std::string &) noexcept;
IISHAREDCANVAS_EXPORT PoseLayer *findPoseLayer(Document &, const std::string &) noexcept;
IISHAREDCANVAS_EXPORT const PoseLayer *findPoseLayer(const Document &, const std::string &) noexcept;
IISHAREDCANVAS_EXPORT std::string validatePoseAsset(const PoseAsset &);
// Evaluates sparse expression targets without mutating authored data; invalid addresses throw invalid_argument.
IISHAREDCANVAS_EXPORT PosePerson evaluatePosePerson(const PosePerson &);
// Throws std::invalid_argument for invalid assets/options; frame rendering passes validated values.
IISHAREDCANVAS_EXPORT VectorAsset poseVectorPreview(const PoseAsset &, const PoseRenderOptions & = {});
IISHAREDCANVAS_EXPORT PoseControlMapResult renderPoseControlMap(const Document &, const std::string &, std::uint32_t frame, const PoseRenderOptions & = {});
IISHAREDCANVAS_EXPORT OpenPoseKeypointsResult exportOpenPoseKeypoints(const Document &, const std::string &, std::uint32_t frame);
} // namespace iiSharedCanvas
