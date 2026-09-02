#pragma once

#include "Export.h"

#include <cstdint>
#include <optional>
#include <string>
#include <vector>

namespace iiSharedCanvas {

struct StableDiffusionImageExtent {
    std::uint32_t width = 0;
    std::uint32_t height = 0;

    bool operator==(const StableDiffusionImageExtent &) const = default;
};

struct StableDiffusionSamplingPass {
    std::string nodeId;
    std::optional<std::uint64_t> seed;
    std::optional<std::uint32_t> steps;
    std::optional<double> cfgScale;
    std::string samplerName;
    std::string scheduler;
    std::optional<double> denoiseStrength;
    std::optional<std::uint32_t> startStep;
    std::optional<std::uint32_t> endStep;

    bool operator==(const StableDiffusionSamplingPass &) const = default;
};

struct StableDiffusionModelResource {
    std::string role;
    std::string name;
    std::string hash;
    std::string hashType;
    std::string uri;

    bool operator==(const StableDiffusionModelResource &) const = default;
};

struct StableDiffusionLora {
    std::string name;
    std::string hash;
    double modelStrength = 1.0;
    double clipStrength = 1.0;

    bool operator==(const StableDiffusionLora &) const = default;
};

struct StableDiffusionMetadataEntry {
    std::string key;
    std::string value;

    bool operator==(const StableDiffusionMetadataEntry &) const = default;
};

struct ComfyUiMetadata {
    std::string promptJson;
    std::string workflowJson;
    std::vector<StableDiffusionMetadataEntry> extraPngInfo;

    bool operator==(const ComfyUiMetadata &) const = default;
};

struct StableDiffusionMetadata {
    std::string positivePrompt;
    std::string negativePrompt;
    std::optional<StableDiffusionImageExtent> outputExtent;
    std::optional<std::uint32_t> batchSize;
    std::optional<std::uint32_t> clipSkip;
    std::vector<StableDiffusionSamplingPass> samplingPasses;
    std::vector<StableDiffusionModelResource> models;
    std::vector<StableDiffusionLora> loras;
    std::string software;
    std::string softwareVersion;
    std::string createdAt;
    std::string generationParametersText;
    ComfyUiMetadata comfyUi;
    std::vector<StableDiffusionMetadataEntry> extraParameters;

    bool operator==(const StableDiffusionMetadata &) const = default;
};

enum class StableDiffusionValidationCode : std::uint8_t {
    EmptyMetadata,
    InvalidOutputExtent,
    InvalidBatchSize,
    InvalidClipSkip,
    InvalidSamplingPass,
    InvalidModelResource,
    InvalidLora,
    InvalidComfyUiMetadata,
    DuplicateMetadataKey,
};

struct StableDiffusionValidationIssue {
    StableDiffusionValidationCode code;
    std::string path;
    std::string message;
};

struct StableDiffusionValidationResult {
    std::vector<StableDiffusionValidationIssue> issues;

    [[nodiscard]] bool ok() const noexcept { return issues.empty(); }
};

IISHAREDCANVAS_EXPORT StableDiffusionValidationResult
validateStableDiffusionMetadata(const StableDiffusionMetadata &metadata);

} // namespace iiSharedCanvas
