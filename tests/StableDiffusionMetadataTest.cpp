#include <iiSharedCanvas.h>

#include <algorithm>
#include <cmath>
#include <iostream>
#include <limits>
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

bool contains(const iiSharedCanvas::StableDiffusionValidationResult &result,
              iiSharedCanvas::StableDiffusionValidationCode code)
{
    return std::any_of(result.issues.begin(), result.issues.end(),
                       [code](const auto &issue) { return issue.code == code; });
}

iiSharedCanvas::StableDiffusionMetadata completeMetadata()
{
    using namespace iiSharedCanvas;

    StableDiffusionMetadata metadata;
    metadata.positivePrompt = "cinematic city at blue hour";
    metadata.negativePrompt = "text, watermark, low quality";
    metadata.outputExtent = StableDiffusionImageExtent{1024, 768};
    metadata.batchSize = 2;
    metadata.clipSkip = 2;
    metadata.samplingPasses.push_back({
        "3",
        156680208700286ULL,
        28,
        6.5,
        "dpmpp_2m",
        "karras",
        1.0,
        std::nullopt,
        std::nullopt,
    });
    metadata.samplingPasses.push_back({
        "17",
        156680208700286ULL,
        12,
        4.0,
        "euler",
        "normal",
        0.35,
        4,
        12,
    });
    metadata.models.push_back({
        "checkpoint",
        "sd_xl_base_1.0.safetensors",
        "abc123",
        "sha256",
        "https://example.invalid/sd_xl_base_1.0.safetensors",
    });
    metadata.models.push_back({
        "vae", "sdxl_vae.safetensors", "def456", "sha256", {},
    });
    metadata.loras.push_back({"detail.safetensors", "987fed", 0.75, 0.6});
    metadata.software = "ComfyUI";
    metadata.softwareVersion = "0.3.x";
    metadata.createdAt = "2026-09-02T12:34:56Z";
    metadata.automatic1111Parameters =
        "Steps: 28, Sampler: DPM++ 2M, CFG scale: 6.5, Seed: 156680208700286";
    metadata.comfyUi.promptJson = R"json({"3":{"inputs":{"seed":156680208700286,"steps":28,"cfg":6.5,"sampler_name":"dpmpp_2m","scheduler":"karras","denoise":1.0},"class_type":"KSampler"}})json";
    metadata.comfyUi.workflowJson =
        R"json({"version":1,"state":{"lastNodeId":9,"lastLinkId":12},"nodes":[],"links":[]})json";
    metadata.comfyUi.extraPngInfo.push_back(
        {"custom_node_data", R"json({"enabled":true,"weight":0.25})json"});
    metadata.extraParameters.push_back({"hiresUpscaler", "4x-UltraSharp"});
    return metadata;
}

} // namespace

int main()
{
    using namespace iiSharedCanvas;

    const StableDiffusionMetadata metadata = completeMetadata();
    expect(validateStableDiffusionMetadata(metadata).ok(),
           "complete Stable Diffusion and ComfyUI metadata must validate");
    expect(metadata.samplingPasses.size() == 2
               && metadata.samplingPasses.front().steps == 28
               && metadata.samplingPasses.front().cfgScale == 6.5
               && metadata.samplingPasses.front().samplerName == "dpmpp_2m"
               && metadata.samplingPasses.front().scheduler == "karras"
               && metadata.samplingPasses.front().denoiseStrength == 1.0,
           "sampling passes must expose seed, steps, CFG, sampler, scheduler, and denoise directly");
    expect(metadata.comfyUi.promptJson.find("KSampler") != std::string::npos
               && metadata.comfyUi.workflowJson.find("\"nodes\"") != std::string::npos,
           "ComfyUI prompt and workflow JSON must remain available without flattening the graph");

    StableDiffusionMetadata jsonForms = metadata;
    jsonForms.comfyUi.promptJson =
        R"json({"unicode":"\uD83D\uDE00","number":-1.25e+2})json";
    jsonForms.comfyUi.extraPngInfo.push_back(
        {"scalar_json", R"json("extension value")json"});
    expect(validateStableDiffusionMetadata(jsonForms).ok(),
           "strict JSON validation must accept escaped Unicode, exponent numbers, and scalar extension values");

    StableDiffusionMetadata workflowOnly = metadata;
    workflowOnly.comfyUi.promptJson.clear();
    expect(validateStableDiffusionMetadata(workflowOnly).ok(),
           "a ComfyUI workflow graph must remain valid without an API prompt graph");
    StableDiffusionMetadata promptOnly = metadata;
    promptOnly.comfyUi.workflowJson.clear();
    expect(validateStableDiffusionMetadata(promptOnly).ok(),
           "a ComfyUI API prompt graph must remain valid without a UI workflow graph");

    StableDiffusionMetadata empty;
    expect(contains(validateStableDiffusionMetadata(empty),
                    StableDiffusionValidationCode::EmptyMetadata),
           "an attached but empty metadata object must be rejected");

    StableDiffusionMetadata invalidSampling = metadata;
    invalidSampling.samplingPasses.front().steps = 0;
    invalidSampling.samplingPasses.front().cfgScale =
        std::numeric_limits<double>::quiet_NaN();
    invalidSampling.samplingPasses.front().denoiseStrength = 1.25;
    expect(contains(validateStableDiffusionMetadata(invalidSampling),
                    StableDiffusionValidationCode::InvalidSamplingPass),
           "steps, CFG, and denoise must be finite and inside their generation ranges");

    StableDiffusionMetadata invalidExtent = metadata;
    invalidExtent.outputExtent = StableDiffusionImageExtent{0, 768};
    invalidExtent.batchSize = 0;
    invalidExtent.clipSkip = 0;
    const StableDiffusionValidationResult invalidShape =
        validateStableDiffusionMetadata(invalidExtent);
    expect(contains(invalidShape, StableDiffusionValidationCode::InvalidOutputExtent)
               && contains(invalidShape, StableDiffusionValidationCode::InvalidBatchSize)
               && contains(invalidShape, StableDiffusionValidationCode::InvalidClipSkip),
           "output size, batch size, and clip skip must reject zero values");

    StableDiffusionMetadata invalidResources = metadata;
    invalidResources.models.front().role.clear();
    invalidResources.loras.front().modelStrength =
        std::numeric_limits<double>::infinity();
    const StableDiffusionValidationResult resources =
        validateStableDiffusionMetadata(invalidResources);
    expect(contains(resources, StableDiffusionValidationCode::InvalidModelResource)
               && contains(resources, StableDiffusionValidationCode::InvalidLora),
           "model resources and LoRA strengths must carry usable finite identity data");

    StableDiffusionMetadata invalidJson = metadata;
    invalidJson.comfyUi.promptJson = R"json({"3": [})json";
    invalidJson.comfyUi.workflowJson = "[]";
    invalidJson.comfyUi.extraPngInfo.push_back(
        {"workflow", R"json({"nodes":[]})json"});
    expect(contains(validateStableDiffusionMetadata(invalidJson),
                    StableDiffusionValidationCode::InvalidComfyUiMetadata),
           "ComfyUI prompt and workflow fields must be valid JSON objects with reserved keys separated");

    StableDiffusionMetadata excessiveJsonDepth = metadata;
    std::string nestedJson(257, '[');
    nestedJson += '0';
    nestedJson.append(257, ']');
    excessiveJsonDepth.comfyUi.extraPngInfo.front().value = nestedJson;
    expect(contains(validateStableDiffusionMetadata(excessiveJsonDepth),
                    StableDiffusionValidationCode::InvalidComfyUiMetadata),
           "ComfyUI extension JSON must reject excessive nesting before recursive parsing can exhaust the stack");

    StableDiffusionMetadata invalidJsonEncoding = metadata;
    invalidJsonEncoding.comfyUi.promptJson =
        std::string{"{\"value\":\"", 10} + std::string{"\xc0\xaf", 2} + "\"}";
    expect(contains(validateStableDiffusionMetadata(invalidJsonEncoding),
                    StableDiffusionValidationCode::InvalidComfyUiMetadata),
           "ComfyUI JSON must reject non-canonical UTF-8");

    StableDiffusionMetadata duplicateExtras = metadata;
    duplicateExtras.extraParameters.push_back({"hiresUpscaler", "duplicate"});
    duplicateExtras.comfyUi.extraPngInfo.push_back(
        {"custom_node_data", R"json(null)json"});
    expect(contains(validateStableDiffusionMetadata(duplicateExtras),
                    StableDiffusionValidationCode::DuplicateMetadataKey),
           "generic and ComfyUI extension keys must remain unique in their namespaces");

    return failures == 0 ? 0 : 1;
}
