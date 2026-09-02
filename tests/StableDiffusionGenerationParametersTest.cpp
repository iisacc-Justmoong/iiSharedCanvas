#include <iiSharedCanvas.h>

#include <algorithm>
#include <cstdint>
#include <iostream>
#include <optional>
#include <string>

namespace {

int failures = 0;

void expect(bool condition, const std::string &message)
{
    if (!condition) {
        std::cerr << message << '\n';
        ++failures;
    }
}

bool contains(const iiSharedCanvas::StableDiffusionGenerationParametersParseResult &result,
              iiSharedCanvas::StableDiffusionGenerationParametersParseCode code)
{
    return std::any_of(result.issues.begin(), result.issues.end(),
                       [code](const auto &issue) { return issue.code == code; });
}

const std::string *extra(const iiSharedCanvas::StableDiffusionMetadata &metadata,
                         const std::string &key)
{
    const auto found = std::find_if(
        metadata.extraParameters.begin(), metadata.extraParameters.end(),
        [&key](const auto &entry) { return entry.key == key; });
    return found == metadata.extraParameters.end() ? nullptr : &found->value;
}

} // namespace

int main()
{
    using namespace iiSharedCanvas;

    const std::string canonical =
        "cinematic portrait,\n"
        "with quoted detail\n"
        "Negative prompt: watermark,\n"
        "bad hands\n"
        "Steps: 30, Sampler: DPM++ 2M SDE, Schedule type: Karras, "
        "CFG scale: 6.5, Seed: 4294967293, Size: 1024x768, "
        "Batch size: 2, Model hash: 0123456789, Model: portraitXL, "
        "VAE hash: abcdef0123, VAE: sdxl_vae.safetensors, "
        "Denoising strength: 0.42, Clip skip: 2, Hires upscale: 1.5, "
        "Hires steps: 12, Hires upscaler: \"4x, UltraSharp\", "
        "Hires sampler: Euler a, Hires schedule type: Normal, "
        "Hires checkpoint: highresXL.safetensors, "
        "Refiner: refinerXL.safetensors, Version: v1.10.1, "
        "Extension value: \"one, two: three\", "
        "Escaped value: \"line\\n\\uD83D\\uDE00\"";

    const StableDiffusionGenerationParametersParseResult parsed =
        parseStableDiffusionGenerationParameters(canonical);
    expect(parsed.ok(), "canonical AUTOMATIC1111 infotext must parse");
    expect(parsed.generationParameters.rawText == canonical,
           "the complete AUTOMATIC1111 infotext must remain byte-exact");
    expect(parsed.generationParameters.positivePrompt
               == "cinematic portrait,\nwith quoted detail"
               && parsed.generationParameters.negativePrompt
                   == "watermark,\nbad hands",
           "multiline positive and negative prompts must retain line structure");
    const std::string *quoted =
        findStableDiffusionGenerationParameter(parsed.generationParameters, "Extension value");
    const std::string *escaped =
        findStableDiffusionGenerationParameter(parsed.generationParameters, "Escaped value");
    expect(quoted && *quoted == "one, two: three",
           "quoted commas and colons must decode as one parameter value");
    expect(escaped && *escaped == "line\n😀",
           "JSON string escapes and surrogate pairs must decode to UTF-8");

    const StableDiffusionMetadata &metadata = parsed.metadata;
    expect(metadata.generationParametersText == canonical
               && metadata.positivePrompt == parsed.generationParameters.positivePrompt
               && metadata.negativePrompt == parsed.generationParameters.negativePrompt,
           "the common metadata projection must retain raw and typed prompts");
    expect(metadata.software.empty()
               && metadata.softwareVersion == "v1.10.1"
               && metadata.outputExtent
               && *metadata.outputExtent
                   == StableDiffusionImageExtent{1024, 768}
               && metadata.batchSize == std::optional<std::uint32_t>{2}
               && metadata.clipSkip == std::optional<std::uint32_t>{2},
           "the source software must not be inferred while version, output extent, batch size, and CLIP skip map directly");
    expect(metadata.samplingPasses.size() == 2,
           "a hires recipe must model main and hires sampling passes separately");
    if (metadata.samplingPasses.size() == 2) {
        const StableDiffusionSamplingPass &main = metadata.samplingPasses[0];
        const StableDiffusionSamplingPass &hires = metadata.samplingPasses[1];
        expect(main.nodeId == "stable-diffusion.main"
                   && main.seed == std::optional<std::uint64_t>{4294967293ULL}
                   && main.steps == std::optional<std::uint32_t>{30}
                   && main.cfgScale == std::optional<double>{6.5}
                   && main.samplerName == "DPM++ 2M SDE"
                   && main.scheduler == "Karras"
                   && !main.denoiseStrength,
               "main sampler fields must map without borrowing hires denoise");
        expect(hires.nodeId == "stable-diffusion.hires"
                   && hires.steps == std::optional<std::uint32_t>{12}
                   && hires.samplerName == "Euler a"
                   && hires.scheduler == "Normal"
                   && hires.denoiseStrength == std::optional<double>{0.42},
               "hires-specific sampler fields must map to the second pass");
    }
    expect(metadata.models.size() == 4
               && metadata.models[0].role == "checkpoint"
               && metadata.models[0].name == "portraitXL"
               && metadata.models[0].hash == "0123456789"
               && metadata.models[0].hashType == "sha256-prefix-10"
               && metadata.models[1].role == "vae"
               && metadata.models[1].hashType == "sha256-prefix-10"
               && metadata.models[2].role == "hires-checkpoint"
               && metadata.models[3].role == "refiner",
           "checkpoint, VAE, hires checkpoint, and refiner identities must remain distinct");
    expect(extra(metadata, "Hires upscale")
               && *extra(metadata, "Hires upscale") == "1.5"
               && extra(metadata, "Hires upscaler")
               && *extra(metadata, "Hires upscaler") == "4x, UltraSharp"
               && extra(metadata, "Extension value")
               && *extra(metadata, "Extension value") == "one, two: three",
           "non-common AUTOMATIC1111 and extension fields must survive as extras");
    expect(validateStableDiffusionMetadata(metadata).ok(),
           "a successful AUTOMATIC1111 projection must satisfy common validation");

    const StableDiffusionGenerationParametersParseResult defaults =
        parseStableDiffusionGenerationParameters(
            "plain prompt\nSteps: 20, Sampler: Euler, CFG scale: 7");
    expect(defaults.ok() && defaults.metadata.clipSkip == 1
               && defaults.metadata.samplingPasses.size() == 1
               && defaults.metadata.samplingPasses.front().scheduler
                   == "Automatic",
           "missing Clip skip and Schedule type must use official parser defaults");

    const StableDiffusionGenerationParametersParseResult legacyHires =
        parseStableDiffusionGenerationParameters(
            "plain prompt\nSteps: 20, Sampler: Euler, CFG scale: 7, "
            "Denoising strength: 0.5, Hires upscale: 2");
    expect(legacyHires.ok()
               && legacyHires.metadata.samplingPasses.size() == 2
               && legacyHires.metadata.samplingPasses.back().samplerName
                   == "Euler"
               && legacyHires.metadata.samplingPasses.back().scheduler
                   == "Automatic",
           "legacy hires infotext must inherit the main sampler and scheduler defaults");

    const StableDiffusionGenerationParametersParseResult zeroHiresSteps =
        parseStableDiffusionGenerationParameters(
            "plain prompt\nSteps: 20, Sampler: Euler, CFG scale: 7, "
            "Denoising strength: 0.5, Hires upscale: 2, Hires steps: 0");
    expect(zeroHiresSteps.ok()
               && zeroHiresSteps.metadata.samplingPasses.size() == 2
               && zeroHiresSteps.metadata.samplingPasses.back().steps
                   == std::optional<std::uint32_t>{20},
           "Hires steps zero must mean reuse the main step count");

    const StableDiffusionGenerationParametersParseResult duplicates =
        parseStableDiffusionGenerationParameters(
            "prompt\nSteps: 20, Sampler: Euler, CFG scale: 7, Seed: 1, "
            "Seed: 2, Future field: alpha, Future field: beta");
    expect(duplicates.ok()
               && duplicates.generationParameters.parameters.size() == 7
               && findStableDiffusionGenerationParameter(duplicates.generationParameters, "Seed")
               && *findStableDiffusionGenerationParameter(
                      duplicates.generationParameters, "Seed") == "2"
               && duplicates.metadata.samplingPasses.front().seed == 2
               && extra(duplicates.metadata, "Future field")
               && *extra(duplicates.metadata, "Future field") == "beta",
           "ordered raw parameters must preserve duplicates while semantic lookup uses the last value");

    const StableDiffusionGenerationParametersParseResult fullHash =
        parseStableDiffusionGenerationParameters(
            "prompt\nSteps: 20, Sampler: Euler, CFG scale: 7, "
            "Model: full, Model hash: "
            "0123456789abcdef0123456789abcdef0123456789abcdef0123456789abcdef");
    const StableDiffusionGenerationParametersParseResult legacyHash =
        parseStableDiffusionGenerationParameters(
            "prompt\nSteps: 20, Sampler: Euler, CFG scale: 7, "
            "Model: legacy, Model hash: 01234567");
    expect(fullHash.ok() && fullHash.metadata.models.size() == 1
               && fullHash.metadata.models.front().hashType == "sha256"
               && legacyHash.ok() && legacyHash.metadata.models.size() == 1
               && legacyHash.metadata.models.front().hashType
                   == "sha256-partial-prefix-8",
           "full SHA-256 and legacy partial-file hashes must not be conflated");

    const StableDiffusionGenerationParametersParseResult missing =
        parseStableDiffusionGenerationParameters(
            "not parameters\nSteps: 20, Seed: 1");
    expect(!missing.ok()
               && contains(missing, StableDiffusionGenerationParametersParseCode::MissingParameterLine),
           "fewer than three terminal fields must remain prompt text, matching AUTOMATIC1111");

    const StableDiffusionGenerationParametersParseResult badInteger =
        parseStableDiffusionGenerationParameters(
            "prompt\nSteps: twelve, Sampler: Euler, CFG scale: 7, Seed: 1");
    expect(!badInteger.ok()
               && contains(badInteger, StableDiffusionGenerationParametersParseCode::InvalidInteger),
           "typed integer conversion must fail closed");
    const StableDiffusionGenerationParametersParseResult badNumber =
        parseStableDiffusionGenerationParameters(
            "prompt\nSteps: 20, Sampler: Euler, CFG scale: nan, Seed: 1");
    expect(!badNumber.ok()
               && contains(badNumber, StableDiffusionGenerationParametersParseCode::InvalidNumber),
           "typed floating-point conversion must reject non-finite text");
    const StableDiffusionGenerationParametersParseResult badSize =
        parseStableDiffusionGenerationParameters(
            "prompt\nSteps: 20, Sampler: Euler, CFG scale: 7, "
            "Size: 1024-by-768");
    expect(!badSize.ok()
               && contains(badSize, StableDiffusionGenerationParametersParseCode::InvalidImageSize),
           "malformed image dimensions must fail closed");
    const StableDiffusionGenerationParametersParseResult badRange =
        parseStableDiffusionGenerationParameters(
            "prompt\nSteps: 0, Sampler: Euler, CFG scale: 7, "
            "Denoising strength: 1.5");
    expect(!badRange.ok()
               && contains(badRange,
                           StableDiffusionGenerationParametersParseCode::InvalidValueRange),
           "non-positive steps and out-of-range denoise must fail closed");
    const StableDiffusionGenerationParametersParseResult badSyntax =
        parseStableDiffusionGenerationParameters(
            "prompt\nSteps: 20, Sampler: Euler, CFG scale: 7, Seed: 1, "
            "Extension: \"bad\\q\"");
    expect(!badSyntax.ok()
               && contains(badSyntax,
                           StableDiffusionGenerationParametersParseCode::InvalidParameterSyntax),
           "malformed JSON-quoted parameter strings must fail closed");

    const std::string invalidUtf8 =
        std::string{"prompt\nSteps: 20, Sampler: Euler, CFG scale: 7, Note: "}
        + std::string{"\xc0\xaf", 2};
    const StableDiffusionGenerationParametersParseResult badEncoding =
        parseStableDiffusionGenerationParameters(invalidUtf8);
    expect(!badEncoding.ok()
               && contains(badEncoding, StableDiffusionGenerationParametersParseCode::InvalidUtf8)
               && badEncoding.generationParameters.rawText == invalidUtf8,
           "non-canonical UTF-8 must be rejected without discarding source bytes");

    Document document;
    document.formatVersion = {1, 2};
    document.extent = {1, 1};
    document.timeline = {{24, 1}, 1};
    document.assets.emplace_back(
        RasterAsset{"raster", makeRasterLayer(1, 1, 0xff000000U)});
    document.layers.emplace_back(BitmapLayer{
        {"layer", "Layer", true, 1.0, {}, RasterBlendMode::SourceOver},
        StaticSource{"raster"},
    });
    DocumentEditor editor(document);
    const DocumentEditResult edit = editor.setStableDiffusionMetadata(metadata);
    const IiscEncodeResult encoded = encodeIisc(document);
    const IiscDecodeResult decoded = encoded.ok()
        ? decodeIisc(encoded.bytes)
        : IiscDecodeResult{};
    expect(edit.ok() && encoded.ok() && decoded.ok()
               && decoded.document.stableDiffusionMetadata == metadata
               && decoded.document.stableDiffusionMetadata
                      ->generationParametersText == canonical,
           "parsed AUTOMATIC1111 metadata must round-trip through format 1.2 byte-exactly");

    return failures == 0 ? 0 : 1;
}
