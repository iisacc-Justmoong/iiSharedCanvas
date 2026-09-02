#include "Metadata/Automatic1111Metadata.h"

#include <algorithm>
#include <charconv>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <limits>
#include <optional>
#include <string>
#include <string_view>
#include <unordered_map>
#include <unordered_set>
#include <utility>
#include <vector>

namespace iiSharedCanvas {
namespace {

struct ParameterLineParse {
    std::vector<StableDiffusionMetadataEntry> parameters;
    bool syntaxError = false;
    std::size_t errorOffset = 0;
};

void addIssue(Automatic1111ParseResult &result,
              Automatic1111ParseCode code,
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

bool horizontalSpace(char value) noexcept
{
    return value == ' ' || value == '\t' || value == '\r';
}

std::string_view trim(std::string_view value) noexcept
{
    while (!value.empty()
           && (horizontalSpace(value.front()) || value.front() == '\n')) {
        value.remove_prefix(1);
    }
    while (!value.empty()
           && (horizontalSpace(value.back()) || value.back() == '\n')) {
        value.remove_suffix(1);
    }
    return value;
}

std::vector<std::string_view> splitLines(std::string_view value)
{
    std::vector<std::string_view> lines;
    std::size_t start = 0;
    for (std::size_t index = 0; index < value.size(); ++index) {
        if (value[index] != '\n' && value[index] != '\r') {
            continue;
        }
        lines.push_back(value.substr(start, index - start));
        if (value[index] == '\r' && index + 1 < value.size()
            && value[index + 1] == '\n') {
            ++index;
        }
        start = index + 1;
    }
    lines.push_back(value.substr(start));
    return lines;
}

bool validParameterKey(std::string_view key) noexcept
{
    if (key.empty()) {
        return false;
    }
    const auto first = static_cast<unsigned char>(key.front());
    const bool firstWord = first >= 0x80U
        || (first >= '0' && first <= '9')
        || (first >= 'A' && first <= 'Z')
        || (first >= 'a' && first <= 'z') || first == '_';
    if (!firstWord) {
        return false;
    }
    return std::all_of(key.begin() + 1, key.end(), [](char value) {
        const auto byte = static_cast<unsigned char>(value);
        return byte >= 0x80U || (byte >= '0' && byte <= '9')
            || (byte >= 'A' && byte <= 'Z')
            || (byte >= 'a' && byte <= 'z') || byte == '_'
            || byte == ' ' || byte == '-' || byte == '/';
    });
}

int hexDigit(char value) noexcept
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

bool readCodeUnit(std::string_view text,
                  std::size_t &position,
                  std::uint16_t &unit) noexcept
{
    if (position + 4 > text.size()) {
        return false;
    }
    unit = 0;
    for (int index = 0; index < 4; ++index) {
        const int digit = hexDigit(text[position++]);
        if (digit < 0) {
            return false;
        }
        unit = static_cast<std::uint16_t>((unit << 4U) | digit);
    }
    return true;
}

void appendCodePoint(std::string &output, std::uint32_t codePoint)
{
    if (codePoint <= 0x7fU) {
        output.push_back(static_cast<char>(codePoint));
    } else if (codePoint <= 0x7ffU) {
        output.push_back(static_cast<char>(0xc0U | (codePoint >> 6U)));
        output.push_back(static_cast<char>(0x80U | (codePoint & 0x3fU)));
    } else if (codePoint <= 0xffffU) {
        output.push_back(static_cast<char>(0xe0U | (codePoint >> 12U)));
        output.push_back(
            static_cast<char>(0x80U | ((codePoint >> 6U) & 0x3fU)));
        output.push_back(static_cast<char>(0x80U | (codePoint & 0x3fU)));
    } else {
        output.push_back(static_cast<char>(0xf0U | (codePoint >> 18U)));
        output.push_back(
            static_cast<char>(0x80U | ((codePoint >> 12U) & 0x3fU)));
        output.push_back(
            static_cast<char>(0x80U | ((codePoint >> 6U) & 0x3fU)));
        output.push_back(static_cast<char>(0x80U | (codePoint & 0x3fU)));
    }
}

bool decodeQuotedString(std::string_view text,
                        std::size_t &position,
                        std::string &output)
{
    if (position >= text.size() || text[position++] != '"') {
        return false;
    }
    while (position < text.size()) {
        const auto current = static_cast<unsigned char>(text[position++]);
        if (current == '"') {
            return true;
        }
        if (current < 0x20U) {
            return false;
        }
        if (current != '\\') {
            output.push_back(static_cast<char>(current));
            continue;
        }
        if (position >= text.size()) {
            return false;
        }
        switch (text[position++]) {
        case '"': output.push_back('"'); break;
        case '\\': output.push_back('\\'); break;
        case '/': output.push_back('/'); break;
        case 'b': output.push_back('\b'); break;
        case 'f': output.push_back('\f'); break;
        case 'n': output.push_back('\n'); break;
        case 'r': output.push_back('\r'); break;
        case 't': output.push_back('\t'); break;
        case 'u': {
            std::uint16_t first = 0;
            if (!readCodeUnit(text, position, first)
                || (first >= 0xdc00U && first <= 0xdfffU)) {
                return false;
            }
            std::uint32_t codePoint = first;
            if (first >= 0xd800U && first <= 0xdbffU) {
                if (position + 2 > text.size() || text[position] != '\\'
                    || text[position + 1] != 'u') {
                    return false;
                }
                position += 2;
                std::uint16_t second = 0;
                if (!readCodeUnit(text, position, second)
                    || second < 0xdc00U || second > 0xdfffU) {
                    return false;
                }
                codePoint = 0x10000U
                    + ((static_cast<std::uint32_t>(first) - 0xd800U) << 10U)
                    + (static_cast<std::uint32_t>(second) - 0xdc00U);
            }
            appendCodePoint(output, codePoint);
            break;
        }
        default:
            return false;
        }
    }
    return false;
}

ParameterLineParse parseParameterLine(std::string_view line)
{
    ParameterLineParse result;
    std::size_t position = 0;
    while (position < line.size()) {
        while (position < line.size() && horizontalSpace(line[position])) {
            ++position;
        }
        if (position == line.size()) {
            return result;
        }

        const std::size_t keyStart = position;
        while (position < line.size() && line[position] != ':'
               && line[position] != ',') {
            ++position;
        }
        if (position == line.size() || line[position] != ':') {
            result.syntaxError = true;
            result.errorOffset = keyStart;
            return result;
        }
        const std::string_view keyView = trim(
            line.substr(keyStart, position - keyStart));
        if (!validParameterKey(keyView)) {
            result.syntaxError = true;
            result.errorOffset = keyStart;
            return result;
        }
        ++position;
        while (position < line.size() && horizontalSpace(line[position])) {
            ++position;
        }

        std::string value;
        if (position < line.size() && line[position] == '"') {
            if (!decodeQuotedString(line, position, value)) {
                result.syntaxError = true;
                result.errorOffset = position;
                return result;
            }
            while (position < line.size() && horizontalSpace(line[position])) {
                ++position;
            }
            if (position < line.size() && line[position] != ',') {
                result.syntaxError = true;
                result.errorOffset = position;
                return result;
            }
        } else {
            const std::size_t valueStart = position;
            while (position < line.size() && line[position] != ',') {
                ++position;
            }
            const std::string_view valueView = trim(
                line.substr(valueStart, position - valueStart));
            value.assign(valueView);
        }
        result.parameters.push_back({std::string(keyView), std::move(value)});

        if (position == line.size()) {
            return result;
        }
        ++position;
        std::size_t next = position;
        while (next < line.size() && horizontalSpace(line[next])) {
            ++next;
        }
        if (next == line.size()) {
            return result;
        }
        position = next;
    }
    return result;
}

std::string joinPromptLines(const std::vector<std::string_view> &lines,
                            std::size_t lineCount,
                            bool negative)
{
    constexpr std::string_view NegativePrefix = "Negative prompt:";
    std::string output;
    bool inNegative = false;
    for (std::size_t index = 0; index < lineCount; ++index) {
        std::string_view line = trim(lines[index]);
        if (!inNegative && line.starts_with(NegativePrefix)) {
            inNegative = true;
            line = trim(line.substr(NegativePrefix.size()));
        }
        if (inNegative != negative) {
            continue;
        }
        if (!output.empty()) {
            output.push_back('\n');
        }
        output.append(line);
    }
    return output;
}

const StableDiffusionMetadataEntry *findParameterEntry(
    const Automatic1111Infotext &infotext,
    std::string_view key) noexcept
{
    const auto found = std::find_if(
        infotext.parameters.rbegin(), infotext.parameters.rend(),
        [key](const auto &entry) { return entry.key == key; });
    return found == infotext.parameters.rend() ? nullptr : &*found;
}

bool hasParameter(const Automatic1111Infotext &infotext,
                  std::string_view key) noexcept
{
    return findParameterEntry(infotext, key) != nullptr;
}

template<typename Integer>
std::optional<Integer> unsignedInteger(Automatic1111ParseResult &result,
                                       std::string_view key,
                                       bool positive)
{
    const StableDiffusionMetadataEntry *entry =
        findParameterEntry(result.infotext, key);
    if (!entry) {
        return std::nullopt;
    }
    Integer value = 0;
    const char *begin = entry->value.data();
    const char *end = begin + entry->value.size();
    const auto parsed = std::from_chars(begin, end, value);
    if (entry->value.empty() || parsed.ec != std::errc{}
        || parsed.ptr != end) {
        addIssue(result, Automatic1111ParseCode::InvalidInteger,
                 "parameters." + std::string(key),
                 std::string(key) + " must be an unsigned integer");
        return std::nullopt;
    }
    if (positive && value == 0) {
        addIssue(result, Automatic1111ParseCode::InvalidValueRange,
                 "parameters." + std::string(key),
                 std::string(key) + " must be positive");
        return std::nullopt;
    }
    return value;
}

std::optional<double> finiteNumber(Automatic1111ParseResult &result,
                                   std::string_view key,
                                   double minimum,
                                   double maximum)
{
    const StableDiffusionMetadataEntry *entry =
        findParameterEntry(result.infotext, key);
    if (!entry) {
        return std::nullopt;
    }
    double value = 0.0;
    const char *begin = entry->value.data();
    const char *end = begin + entry->value.size();
    const auto parsed = std::from_chars(
        begin, end, value, std::chars_format::general);
    if (entry->value.empty() || parsed.ec != std::errc{}
        || parsed.ptr != end || !std::isfinite(value)) {
        addIssue(result, Automatic1111ParseCode::InvalidNumber,
                 "parameters." + std::string(key),
                 std::string(key) + " must be a finite number");
        return std::nullopt;
    }
    if (value < minimum || value > maximum) {
        addIssue(result, Automatic1111ParseCode::InvalidValueRange,
                 "parameters." + std::string(key),
                 std::string(key) + " is outside its supported range");
        return std::nullopt;
    }
    return value;
}

std::optional<StableDiffusionImageExtent> imageExtent(
    Automatic1111ParseResult &result)
{
    const StableDiffusionMetadataEntry *entry =
        findParameterEntry(result.infotext, "Size");
    if (!entry) {
        return std::nullopt;
    }
    const std::size_t separator = entry->value.find('x');
    if (separator == std::string::npos
        || entry->value.find('x', separator + 1) != std::string::npos) {
        addIssue(result, Automatic1111ParseCode::InvalidImageSize,
                 "parameters.Size", "Size must use WIDTHxHEIGHT");
        return std::nullopt;
    }
    std::uint32_t width = 0;
    std::uint32_t height = 0;
    const std::string_view widthText(entry->value.data(), separator);
    const std::string_view heightText(
        entry->value.data() + separator + 1,
        entry->value.size() - separator - 1);
    const auto widthResult = std::from_chars(
        widthText.data(), widthText.data() + widthText.size(), width);
    const auto heightResult = std::from_chars(
        heightText.data(), heightText.data() + heightText.size(), height);
    if (widthText.empty() || heightText.empty()
        || widthResult.ec != std::errc{}
        || heightResult.ec != std::errc{}
        || widthResult.ptr != widthText.data() + widthText.size()
        || heightResult.ptr != heightText.data() + heightText.size()
        || width == 0 || height == 0) {
        addIssue(result, Automatic1111ParseCode::InvalidImageSize,
                 "parameters.Size",
                 "Size must contain positive unsigned WIDTHxHEIGHT values");
        return std::nullopt;
    }
    return StableDiffusionImageExtent{width, height};
}

bool hexadecimal(std::string_view value) noexcept
{
    return !value.empty()
        && std::all_of(value.begin(), value.end(), [](char character) {
            return hexDigit(character) >= 0;
        });
}

std::string hashType(std::string_view value)
{
    if (hexadecimal(value) && value.size() == 64) {
        return "sha256";
    }
    if (hexadecimal(value) && value.size() == 10) {
        return "sha256-prefix-10";
    }
    if (hexadecimal(value) && value.size() == 8) {
        return "automatic1111-legacy-model-hash";
    }
    return value.empty() ? std::string{} : "automatic1111-hash";
}

bool specialSelection(std::string_view value,
                      std::string_view first,
                      std::string_view second = {}) noexcept
{
    return value.empty() || value == first || (!second.empty() && value == second);
}

void projectParameters(Automatic1111ParseResult &result)
{
    StableDiffusionMetadata &metadata = result.metadata;
    const Automatic1111Infotext &infotext = result.infotext;
    std::unordered_set<std::string> consumed;
    const auto consume = [&consumed](std::string_view key) {
        consumed.emplace(key);
    };

    metadata.outputExtent = imageExtent(result);
    if (hasParameter(infotext, "Size")) {
        consume("Size");
    }
    metadata.batchSize = unsignedInteger<std::uint32_t>(
        result, "Batch size", true);
    if (hasParameter(infotext, "Batch size")) {
        consume("Batch size");
    }
    if (hasParameter(infotext, "Clip skip")) {
        metadata.clipSkip = unsignedInteger<std::uint32_t>(
            result, "Clip skip", true);
        consume("Clip skip");
    } else {
        metadata.clipSkip = 1;
    }

    const auto steps = unsignedInteger<std::uint32_t>(result, "Steps", true);
    const auto seed = unsignedInteger<std::uint64_t>(result, "Seed", false);
    const auto cfg = finiteNumber(
        result, "CFG scale", 0.0, std::numeric_limits<double>::max());
    const auto denoise = finiteNumber(result, "Denoising strength", 0.0, 1.0);
    for (std::string_view key : {"Steps", "Seed", "CFG scale",
                                 "Denoising strength"}) {
        if (hasParameter(infotext, key)) {
            consume(key);
        }
    }

    const std::string *sampler = findAutomatic1111Parameter(infotext, "Sampler");
    const std::string *schedule =
        findAutomatic1111Parameter(infotext, "Schedule type");
    if (sampler) {
        consume("Sampler");
    }
    if (schedule) {
        consume("Schedule type");
    }
    const bool hasMainSettings = steps || seed || cfg || denoise || sampler
        || schedule || hasParameter(infotext, "Steps")
        || hasParameter(infotext, "Seed") || hasParameter(infotext, "CFG scale");

    const bool hasHires = std::any_of(
        infotext.parameters.begin(), infotext.parameters.end(),
        [](const auto &entry) { return entry.key.starts_with("Hires "); });
    if (hasMainSettings) {
        StableDiffusionSamplingPass pass;
        pass.nodeId = "automatic1111.main";
        pass.seed = seed;
        pass.steps = steps;
        pass.cfgScale = cfg;
        pass.samplerName = sampler ? *sampler : std::string{};
        pass.scheduler = schedule ? *schedule : "Automatic";
        if (!hasHires) {
            pass.denoiseStrength = denoise;
        }
        metadata.samplingPasses.push_back(std::move(pass));
    }

    const std::string *hiresSampler =
        findAutomatic1111Parameter(infotext, "Hires sampler");
    const std::string *hiresSchedule =
        findAutomatic1111Parameter(infotext, "Hires schedule type");
    auto hiresSteps = unsignedInteger<std::uint32_t>(
        result, "Hires steps", false);
    if (hiresSteps && *hiresSteps == 0) {
        hiresSteps = steps;
    }
    for (std::string_view key : {"Hires steps", "Hires sampler",
                                 "Hires schedule type"}) {
        if (hasParameter(infotext, key)) {
            consume(key);
        }
    }
    if (hasHires && (hiresSteps || hiresSampler || hiresSchedule || denoise)) {
        StableDiffusionSamplingPass pass;
        pass.nodeId = "automatic1111.hires";
        pass.steps = hiresSteps;
        pass.samplerName = hiresSampler ? *hiresSampler : "Use same sampler";
        if (pass.samplerName == "Use same sampler") {
            pass.samplerName = sampler ? *sampler : std::string{};
        }
        pass.scheduler = hiresSchedule
            ? *hiresSchedule
            : "Use same scheduler";
        if (pass.scheduler == "Use same scheduler") {
            pass.scheduler = schedule ? *schedule : "Automatic";
        }
        pass.denoiseStrength = denoise;
        metadata.samplingPasses.push_back(std::move(pass));
    }

    const std::string *version =
        findAutomatic1111Parameter(infotext, "Version");
    if (version) {
        metadata.softwareVersion = *version;
        consume("Version");
    }

    const auto addModel = [&](std::string_view role,
                              std::string_view nameKey,
                              std::string_view hashKey,
                              std::string_view sentinel,
                              std::string_view alternateSentinel = {}) {
        const std::string *name = findAutomatic1111Parameter(infotext, nameKey);
        if (!name || specialSelection(*name, sentinel, alternateSentinel)) {
            return;
        }
        StableDiffusionModelResource resource;
        resource.role = role;
        resource.name = *name;
        if (!hashKey.empty()) {
            if (const std::string *hash =
                    findAutomatic1111Parameter(infotext, hashKey)) {
                resource.hash = *hash;
                resource.hashType = hashType(*hash);
                consume(hashKey);
            }
        }
        metadata.models.push_back(std::move(resource));
        consume(nameKey);
    };
    addModel("checkpoint", "Model", "Model hash", "None");
    addModel("vae", "VAE", "VAE hash", "None", "Automatic");
    addModel("hires-checkpoint", "Hires checkpoint", {},
             "Use same checkpoint", "None");
    addModel("refiner", "Refiner", {}, "None");

    std::unordered_map<std::string, std::size_t> extraIndices;
    for (const StableDiffusionMetadataEntry &entry : infotext.parameters) {
        if (consumed.contains(entry.key)) {
            continue;
        }
        const auto found = extraIndices.find(entry.key);
        if (found == extraIndices.end()) {
            extraIndices.emplace(entry.key, metadata.extraParameters.size());
            metadata.extraParameters.push_back(entry);
        } else {
            metadata.extraParameters[found->second].value = entry.value;
        }
    }
}

} // namespace

Automatic1111ParseResult parseAutomatic1111Infotext(std::string_view infotext)
{
    Automatic1111ParseResult result;
    result.infotext.rawInfotext.assign(infotext);
    result.metadata.automatic1111Parameters.assign(infotext);
    result.metadata.software = "AUTOMATIC1111";

    if (infotext.empty() || trim(infotext).empty()) {
        addIssue(result, Automatic1111ParseCode::EmptyInfotext, {},
                 "AUTOMATIC1111 infotext must not be empty");
        return result;
    }
    if (!validUtf8(infotext)) {
        addIssue(result, Automatic1111ParseCode::InvalidUtf8, {},
                 "AUTOMATIC1111 infotext must use canonical UTF-8");
        return result;
    }

    const std::string_view semanticText = trim(infotext);
    const std::vector<std::string_view> lines = splitLines(semanticText);
    const std::string_view parameterLine = trim(lines.back());
    ParameterLineParse parsedLine = parseParameterLine(parameterLine);
    if (parsedLine.parameters.size() < 3) {
        result.infotext.positivePrompt = joinPromptLines(lines, lines.size(), false);
        result.infotext.negativePrompt = joinPromptLines(lines, lines.size(), true);
        result.metadata.positivePrompt = result.infotext.positivePrompt;
        result.metadata.negativePrompt = result.infotext.negativePrompt;
        addIssue(result, Automatic1111ParseCode::MissingParameterLine,
                 "parameters",
                 "the final line must contain at least three key-value parameters");
        return result;
    }

    result.infotext.parameters = std::move(parsedLine.parameters);
    result.infotext.positivePrompt =
        joinPromptLines(lines, lines.size() - 1, false);
    result.infotext.negativePrompt =
        joinPromptLines(lines, lines.size() - 1, true);
    result.metadata.positivePrompt = result.infotext.positivePrompt;
    result.metadata.negativePrompt = result.infotext.negativePrompt;
    if (parsedLine.syntaxError) {
        addIssue(result, Automatic1111ParseCode::InvalidParameterSyntax,
                 "parameters[" + std::to_string(parsedLine.errorOffset) + "]",
                 "the final parameter line has invalid key-value or JSON string syntax");
    }

    projectParameters(result);
    if (result.issues.empty()) {
        const StableDiffusionValidationResult validation =
            validateStableDiffusionMetadata(result.metadata);
        for (const StableDiffusionValidationIssue &issue : validation.issues) {
            addIssue(result, Automatic1111ParseCode::InvalidMappedMetadata,
                     issue.path, issue.message);
        }
    }
    return result;
}

const std::string *findAutomatic1111Parameter(
    const Automatic1111Infotext &infotext,
    std::string_view key) noexcept
{
    const StableDiffusionMetadataEntry *entry =
        findParameterEntry(infotext, key);
    return entry ? &entry->value : nullptr;
}

} // namespace iiSharedCanvas
