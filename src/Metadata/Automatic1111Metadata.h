#pragma once

#include "Export.h"
#include "Metadata/StableDiffusionMetadata.h"

#include <cstdint>
#include <string>
#include <string_view>
#include <vector>

namespace iiSharedCanvas {

struct Automatic1111Infotext {
    std::string rawInfotext;
    std::string positivePrompt;
    std::string negativePrompt;
    std::vector<StableDiffusionMetadataEntry> parameters;

    bool operator==(const Automatic1111Infotext &) const = default;
};

enum class Automatic1111ParseCode : std::uint8_t {
    EmptyInfotext,
    InvalidUtf8,
    MissingParameterLine,
    InvalidParameterSyntax,
    InvalidInteger,
    InvalidNumber,
    InvalidImageSize,
    InvalidValueRange,
    InvalidMappedMetadata,
};

struct Automatic1111ParseIssue {
    Automatic1111ParseCode code;
    std::string path;
    std::string message;
};

struct Automatic1111ParseResult {
    Automatic1111Infotext infotext;
    StableDiffusionMetadata metadata;
    std::vector<Automatic1111ParseIssue> issues;

    [[nodiscard]] bool ok() const noexcept { return issues.empty(); }
};

IISHAREDCANVAS_EXPORT Automatic1111ParseResult
parseAutomatic1111Infotext(std::string_view infotext);

IISHAREDCANVAS_EXPORT const std::string *findAutomatic1111Parameter(
    const Automatic1111Infotext &infotext,
    std::string_view key) noexcept;

} // namespace iiSharedCanvas
