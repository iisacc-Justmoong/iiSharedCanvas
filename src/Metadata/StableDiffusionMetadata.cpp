#include "Metadata/StableDiffusionMetadata.h"

#include <cmath>
#include <cstddef>
#include <cstdint>
#include <string>
#include <string_view>
#include <unordered_set>
#include <utility>

namespace iiSharedCanvas {
namespace {

constexpr std::size_t MaximumJsonDepth = 256;

void addIssue(StableDiffusionValidationResult &result,
              StableDiffusionValidationCode code,
              std::string path,
              std::string message)
{
    result.issues.push_back({code, std::move(path), std::move(message)});
}

bool continuation(std::uint8_t value) noexcept
{
    return (value & 0xc0U) == 0x80U;
}

bool validUtf8(std::string_view value) noexcept
{
    const auto *bytes = reinterpret_cast<const std::uint8_t *>(value.data());
    std::size_t index = 0;
    while (index < value.size()) {
        const std::uint8_t first = bytes[index++];
        if (first <= 0x7fU) {
            continue;
        }
        if (first >= 0xc2U && first <= 0xdfU) {
            if (index >= value.size() || !continuation(bytes[index++])) {
                return false;
            }
            continue;
        }
        if (first >= 0xe0U && first <= 0xefU) {
            if (index + 1 >= value.size()) {
                return false;
            }
            const std::uint8_t second = bytes[index++];
            const std::uint8_t third = bytes[index++];
            if (!continuation(second) || !continuation(third)
                || (first == 0xe0U && second < 0xa0U)
                || (first == 0xedU && second > 0x9fU)) {
                return false;
            }
            continue;
        }
        if (first >= 0xf0U && first <= 0xf4U) {
            if (index + 2 >= value.size()) {
                return false;
            }
            const std::uint8_t second = bytes[index++];
            const std::uint8_t third = bytes[index++];
            const std::uint8_t fourth = bytes[index++];
            if (!continuation(second) || !continuation(third)
                || !continuation(fourth)
                || (first == 0xf0U && second < 0x90U)
                || (first == 0xf4U && second > 0x8fU)) {
                return false;
            }
            continue;
        }
        return false;
    }
    return true;
}

class JsonValidator final {
public:
    explicit JsonValidator(std::string_view text) noexcept
        : m_text(text)
    {
    }

    bool validate(bool requireObject) noexcept
    {
        if (!validUtf8(m_text)) {
            return false;
        }
        whitespace();
        if (requireObject && peek() != '{') {
            return false;
        }
        if (!value(0)) {
            return false;
        }
        whitespace();
        return m_position == m_text.size();
    }

private:
    char peek() const noexcept
    {
        return m_position < m_text.size() ? m_text[m_position] : '\0';
    }

    void whitespace() noexcept
    {
        while (m_position < m_text.size()) {
            const char current = m_text[m_position];
            if (current != ' ' && current != '\t'
                && current != '\r' && current != '\n') {
                return;
            }
            ++m_position;
        }
    }

    bool consume(char expected) noexcept
    {
        if (peek() != expected) {
            return false;
        }
        ++m_position;
        return true;
    }

    bool literal(std::string_view expected) noexcept
    {
        if (m_text.substr(m_position, expected.size()) != expected) {
            return false;
        }
        m_position += expected.size();
        return true;
    }

    bool value(std::size_t depth) noexcept
    {
        if (depth >= MaximumJsonDepth) {
            return false;
        }
        whitespace();
        switch (peek()) {
        case '{':
            return object(depth + 1);
        case '[':
            return array(depth + 1);
        case '"':
            return string();
        case 't':
            return literal("true");
        case 'f':
            return literal("false");
        case 'n':
            return literal("null");
        default:
            return number();
        }
    }

    bool object(std::size_t depth) noexcept
    {
        consume('{');
        whitespace();
        if (consume('}')) {
            return true;
        }
        while (string()) {
            whitespace();
            if (!consume(':') || !value(depth)) {
                return false;
            }
            whitespace();
            if (consume('}')) {
                return true;
            }
            if (!consume(',')) {
                return false;
            }
            whitespace();
        }
        return false;
    }

    bool array(std::size_t depth) noexcept
    {
        consume('[');
        whitespace();
        if (consume(']')) {
            return true;
        }
        while (value(depth)) {
            whitespace();
            if (consume(']')) {
                return true;
            }
            if (!consume(',')) {
                return false;
            }
        }
        return false;
    }

    static int hex(char value) noexcept
    {
        if (value >= '0' && value <= '9') {
            return value - '0';
        }
        if (value >= 'a' && value <= 'f') {
            return value - 'a' + 10;
        }
        if (value >= 'A' && value <= 'F') {
            return value - 'A' + 10;
        }
        return -1;
    }

    bool codeUnit(std::uint16_t &result) noexcept
    {
        if (m_position + 4 > m_text.size()) {
            return false;
        }
        result = 0;
        for (int index = 0; index < 4; ++index) {
            const int digit = hex(m_text[m_position++]);
            if (digit < 0) {
                return false;
            }
            result = static_cast<std::uint16_t>((result << 4U) | digit);
        }
        return true;
    }

    bool unicodeEscape() noexcept
    {
        std::uint16_t first = 0;
        if (!codeUnit(first) || (first >= 0xdc00U && first <= 0xdfffU)) {
            return false;
        }
        if (first < 0xd800U || first > 0xdbffU) {
            return true;
        }
        if (m_position + 2 > m_text.size()
            || m_text[m_position] != '\\'
            || m_text[m_position + 1] != 'u') {
            return false;
        }
        m_position += 2;
        std::uint16_t second = 0;
        return codeUnit(second) && second >= 0xdc00U && second <= 0xdfffU;
    }

    bool string() noexcept
    {
        if (!consume('"')) {
            return false;
        }
        while (m_position < m_text.size()) {
            const auto current =
                static_cast<unsigned char>(m_text[m_position++]);
            if (current == '"') {
                return true;
            }
            if (current < 0x20U) {
                return false;
            }
            if (current != '\\') {
                continue;
            }
            if (m_position >= m_text.size()) {
                return false;
            }
            const char escape = m_text[m_position++];
            if (escape == 'u') {
                if (!unicodeEscape()) {
                    return false;
                }
            } else if (escape != '"' && escape != '\\' && escape != '/'
                       && escape != 'b' && escape != 'f' && escape != 'n'
                       && escape != 'r' && escape != 't') {
                return false;
            }
        }
        return false;
    }

    bool number() noexcept
    {
        const std::size_t start = m_position;
        consume('-');
        if (consume('0')) {
            if (peek() >= '0' && peek() <= '9') {
                return false;
            }
        } else {
            if (peek() < '1' || peek() > '9') {
                return false;
            }
            while (peek() >= '0' && peek() <= '9') {
                ++m_position;
            }
        }
        if (consume('.')) {
            if (peek() < '0' || peek() > '9') {
                return false;
            }
            while (peek() >= '0' && peek() <= '9') {
                ++m_position;
            }
        }
        if (peek() == 'e' || peek() == 'E') {
            ++m_position;
            if (peek() == '+' || peek() == '-') {
                ++m_position;
            }
            if (peek() < '0' || peek() > '9') {
                return false;
            }
            while (peek() >= '0' && peek() <= '9') {
                ++m_position;
            }
        }
        return m_position > start;
    }

    std::string_view m_text;
    std::size_t m_position = 0;
};

bool validJson(std::string_view value, bool requireObject) noexcept
{
    return !value.empty() && JsonValidator(value).validate(requireObject);
}

bool hasMetadataPayload(const StableDiffusionMetadata &metadata) noexcept
{
    return !metadata.positivePrompt.empty()
        || !metadata.negativePrompt.empty()
        || metadata.outputExtent
        || metadata.batchSize
        || metadata.clipSkip
        || !metadata.samplingPasses.empty()
        || !metadata.models.empty()
        || !metadata.loras.empty()
        || !metadata.software.empty()
        || !metadata.softwareVersion.empty()
        || !metadata.createdAt.empty()
        || !metadata.automatic1111Parameters.empty()
        || !metadata.comfyUi.promptJson.empty()
        || !metadata.comfyUi.workflowJson.empty()
        || !metadata.comfyUi.extraPngInfo.empty()
        || !metadata.extraParameters.empty();
}

bool samplingPassHasSettings(const StableDiffusionSamplingPass &pass) noexcept
{
    return pass.seed || pass.steps || pass.cfgScale
        || !pass.samplerName.empty() || !pass.scheduler.empty()
        || pass.denoiseStrength || pass.startStep || pass.endStep;
}

} // namespace

StableDiffusionValidationResult validateStableDiffusionMetadata(
    const StableDiffusionMetadata &metadata)
{
    StableDiffusionValidationResult result;
    if (!hasMetadataPayload(metadata)) {
        addIssue(result, StableDiffusionValidationCode::EmptyMetadata, {},
                 "generation metadata must contain at least one prompt, parameter, resource, or compatibility payload");
    }
    if (metadata.outputExtent
        && (metadata.outputExtent->width == 0 || metadata.outputExtent->height == 0)) {
        addIssue(result, StableDiffusionValidationCode::InvalidOutputExtent,
                 "outputExtent", "generated output extent must be positive");
    }
    if (metadata.batchSize && *metadata.batchSize == 0) {
        addIssue(result, StableDiffusionValidationCode::InvalidBatchSize,
                 "batchSize", "generation batch size must be positive");
    }
    if (metadata.clipSkip && *metadata.clipSkip == 0) {
        addIssue(result, StableDiffusionValidationCode::InvalidClipSkip,
                 "clipSkip", "CLIP skip must be positive when present");
    }

    for (std::size_t index = 0; index < metadata.samplingPasses.size(); ++index) {
        const StableDiffusionSamplingPass &pass = metadata.samplingPasses[index];
        const bool invalidSteps = pass.steps && *pass.steps == 0;
        const bool invalidCfg = pass.cfgScale
            && (!std::isfinite(*pass.cfgScale) || *pass.cfgScale < 0.0);
        const bool invalidDenoise = pass.denoiseStrength
            && (!std::isfinite(*pass.denoiseStrength)
                || *pass.denoiseStrength < 0.0
                || *pass.denoiseStrength > 1.0);
        const bool invalidRange = pass.startStep && pass.endStep
            && *pass.startStep > *pass.endStep;
        if (!samplingPassHasSettings(pass) || invalidSteps || invalidCfg
            || invalidDenoise || invalidRange) {
            addIssue(result, StableDiffusionValidationCode::InvalidSamplingPass,
                     "samplingPasses[" + std::to_string(index) + "]",
                     "a sampling pass needs settings; steps must be positive, CFG finite and non-negative, denoise within zero to one, and step bounds ordered");
        }
    }

    for (std::size_t index = 0; index < metadata.models.size(); ++index) {
        const StableDiffusionModelResource &model = metadata.models[index];
        if (model.role.empty() || model.name.empty()) {
            addIssue(result, StableDiffusionValidationCode::InvalidModelResource,
                     "models[" + std::to_string(index) + "]",
                     "a model resource requires a role and name");
        }
    }
    for (std::size_t index = 0; index < metadata.loras.size(); ++index) {
        const StableDiffusionLora &lora = metadata.loras[index];
        if (lora.name.empty() || !std::isfinite(lora.modelStrength)
            || !std::isfinite(lora.clipStrength)) {
            addIssue(result, StableDiffusionValidationCode::InvalidLora,
                     "loras[" + std::to_string(index) + "]",
                     "a LoRA requires a name and finite model and CLIP strengths");
        }
    }

    if ((!metadata.comfyUi.promptJson.empty()
         && !validJson(metadata.comfyUi.promptJson, true))
        || (!metadata.comfyUi.workflowJson.empty()
            && !validJson(metadata.comfyUi.workflowJson, true))) {
        addIssue(result, StableDiffusionValidationCode::InvalidComfyUiMetadata,
                 "comfyUi", "ComfyUI prompt and workflow values must be valid JSON objects");
    }

    std::unordered_set<std::string> comfyKeys;
    for (std::size_t index = 0;
         index < metadata.comfyUi.extraPngInfo.size();
         ++index) {
        const StableDiffusionMetadataEntry &entry =
            metadata.comfyUi.extraPngInfo[index];
        if (entry.key.empty() || entry.key == "prompt" || entry.key == "workflow"
            || !validJson(entry.value, false)) {
            addIssue(result, StableDiffusionValidationCode::InvalidComfyUiMetadata,
                     "comfyUi.extraPngInfo[" + std::to_string(index) + "]",
                     "ComfyUI extension metadata requires a non-reserved key and valid JSON value");
        } else if (!comfyKeys.insert(entry.key).second) {
            addIssue(result, StableDiffusionValidationCode::DuplicateMetadataKey,
                     "comfyUi.extraPngInfo[" + std::to_string(index) + "].key",
                     "ComfyUI extension metadata keys must be unique");
        }
    }

    std::unordered_set<std::string> extraKeys;
    for (std::size_t index = 0; index < metadata.extraParameters.size(); ++index) {
        const StableDiffusionMetadataEntry &entry = metadata.extraParameters[index];
        if (entry.key.empty()) {
            addIssue(result, StableDiffusionValidationCode::DuplicateMetadataKey,
                     "extraParameters[" + std::to_string(index) + "].key",
                     "extra generation parameter keys must not be empty");
        } else if (!extraKeys.insert(entry.key).second) {
            addIssue(result, StableDiffusionValidationCode::DuplicateMetadataKey,
                     "extraParameters[" + std::to_string(index) + "].key",
                     "extra generation parameter keys must be unique");
        }
    }
    return result;
}

} // namespace iiSharedCanvas
