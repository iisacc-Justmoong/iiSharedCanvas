#pragma once

#include "Export.h"
#include "Metadata/StableDiffusionMetadata.h"

#include <cstdint>
#include <string>
#include <string_view>
#include <vector>

namespace iiSharedCanvas {

struct StableDiffusionGenerationParameters {
    std::string rawText;
    std::string positivePrompt;
    std::string negativePrompt;
    std::vector<StableDiffusionMetadataEntry> parameters;

    bool operator==(const StableDiffusionGenerationParameters &) const = default;
};

enum class StableDiffusionGenerationParametersParseCode : std::uint8_t {
    EmptyText,
    InvalidUtf8,
    MissingParameterLine,
    InvalidParameterSyntax,
    InvalidInteger,
    InvalidNumber,
    InvalidImageSize,
    InvalidValueRange,
    InvalidMappedMetadata,
};

struct StableDiffusionGenerationParametersParseIssue {
    StableDiffusionGenerationParametersParseCode code;
    std::string path;
    std::string message;
};

struct StableDiffusionGenerationParametersParseResult {
    StableDiffusionGenerationParameters generationParameters;
    StableDiffusionMetadata metadata;
    std::vector<StableDiffusionGenerationParametersParseIssue> issues;

    [[nodiscard]] bool ok() const noexcept { return issues.empty(); }
};

IISHAREDCANVAS_EXPORT StableDiffusionGenerationParametersParseResult
parseStableDiffusionGenerationParameters(std::string_view text);

IISHAREDCANVAS_EXPORT const std::string *findStableDiffusionGenerationParameter(
    const StableDiffusionGenerationParameters &generationParameters,
    std::string_view key) noexcept;

} // namespace iiSharedCanvas
