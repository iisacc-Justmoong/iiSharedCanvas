#include "Render/DocumentPreview.h"

#include "File/IiscInput_p.hpp"
#include "Render/FrameRenderer.h"

#include <algorithm>
#include <cmath>
#include <exception>
#include <filesystem>
#include <new>
#include <utility>

namespace iiSharedCanvas {
namespace {
DocumentPreviewResult failure(MediaIoCode code, std::string message)
{
    DocumentPreviewResult result;
    result.result = {code, std::move(message), {}};
    return result;
}
bool validBounds(CanvasExtent maximum)
{
    return maximum.width > 0 && maximum.height > 0 && maximum.width <= 4096 && maximum.height <= 4096;
}
DocumentPreviewResult invalidBounds(CanvasExtent maximum)
{
    return failure(maximum.width <= 0 || maximum.height <= 0 ? MediaIoCode::InvalidArgument : MediaIoCode::LimitExceeded,
        "preview bounds must be between 1 and 4096 pixels on each axis");
}
}

DocumentPreviewResult renderDocumentPreview(const Document &document, DocumentPreviewOptions options)
{
    if (!validBounds(options.maximumExtent)) return invalidBounds(options.maximumExtent);
    const auto region = documentViewRegion(document);
    if (region.extent.width <= 0 || region.extent.height <= 0)
        return failure(MediaIoCode::InvalidData, "document has no valid canvas region");
    const double scale = std::min({1.0, double(options.maximumExtent.width) / region.extent.width,
        double(options.maximumExtent.height) / region.extent.height});
    const CanvasExtent output{std::max(1, int(std::floor(region.extent.width * scale))),
        std::max(1, int(std::floor(region.extent.height * scale)))};
    try {
        // A native-size preview must preserve the editor's pixel/mask coverage.
        // Smooth sampling is needed only when reducing the composed view.
        auto rendered = renderFrameRegion(document, options.frame, region, output,
            output == region.extent ? RasterSampling::Nearest : RasterSampling::Smooth);
        if (!rendered.ok()) return failure(rendered.status == FrameRenderStatus::FrameOutOfRange
            ? MediaIoCode::InvalidArgument : MediaIoCode::InvalidData, std::move(rendered.message));
        DocumentPreviewResult result;
        result.region = region;
        result.pixels = std::move(rendered.pixels);
        return result;
    } catch (const std::bad_alloc &) {
        return failure(MediaIoCode::LimitExceeded, "not enough memory to render document preview");
    } catch (const std::exception &error) {
        return failure(MediaIoCode::InvalidData, error.what());
    }
}

DocumentPreviewResult readIiscPreview(const std::string &path, DocumentPreviewOptions options, SerializationLimits limits)
{
    if (!validBounds(options.maximumExtent)) return invalidBounds(options.maximumExtent);
    if (path.empty() || path.find('\0') != std::string::npos || path.find("://") != std::string::npos)
        return failure(MediaIoCode::InvalidArgument, "a UTF-8 local canvas file path is required");
    try {
        const auto temporary = std::filesystem::temp_directory_path().u8string();
        const auto document = detail::loadIiscDocumentReadOnly(path,
            std::string(temporary.begin(), temporary.end()), limits);
        return renderDocumentPreview(document, options);
    } catch (const detail::IiscInputFailure &error) {
        const auto code = error.code == DocumentFileCode::LimitExceeded ? MediaIoCode::LimitExceeded
            : error.code == DocumentFileCode::UnsupportedFormat ? MediaIoCode::UnsupportedFormat
            : error.code == DocumentFileCode::CorruptFile || error.code == DocumentFileCode::InvalidDocument
                ? MediaIoCode::InvalidData : MediaIoCode::IoError;
        return failure(code, error.what());
    } catch (const std::bad_alloc &) {
        return failure(MediaIoCode::LimitExceeded, "not enough memory to read document preview");
    } catch (const std::exception &error) {
        return failure(MediaIoCode::IoError, error.what());
    }
}
} // namespace iiSharedCanvas
