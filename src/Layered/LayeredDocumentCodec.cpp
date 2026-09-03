#include "LayeredDocumentCodec.h"

#include "OpenRasterParser_p.hpp"
#include "PsdParser_p.hpp"
#include "Media/MediaIo_p.hpp"
#include "Validation/Validation.h"

#include <QString>

#include <limits>
#include <new>
#include <stdexcept>

namespace iiSharedCanvas {
namespace {

MediaIoResult checkOptions(const LayeredDocumentImportOptions &options)
{
    if (options.idPrefix.empty() || options.idPrefix.size() > 1024
        || options.idPrefix.find('\0') != std::string::npos
        || QString::fromUtf8(options.idPrefix).toUtf8().toStdString() != options.idPrefix) {
        return media_detail::error(MediaIoCode::InvalidArgument,
                                  "layered import requires a nonempty canonical UTF-8 id prefix of at most 1024 bytes without NUL");
    }
    return {};
}

bool isPsd(std::span<const std::uint8_t> bytes)
{
    return bytes.size() >= 4 && bytes[0] == '8' && bytes[1] == 'B'
        && bytes[2] == 'P' && bytes[3] == 'S';
}

bool isZip(std::span<const std::uint8_t> bytes)
{
    return bytes.size() >= 4 && bytes[0] == 'P' && bytes[1] == 'K'
        && ((bytes[2] == 3 && bytes[3] == 4) || (bytes[2] == 5 && bytes[3] == 6)
            || (bytes[2] == 7 && bytes[3] == 8));
}

} // namespace

std::vector<MediaFormatCapability> layeredDocumentFormats()
{
    return {{"ora", true, false}, {"psd", true, true}};
}

LayeredDocumentImportResult decodeLayeredDocument(
    std::span<const std::uint8_t> bytes, const LayeredDocumentImportOptions &options)
{
    LayeredDocumentImportResult imported;
    imported.result = checkOptions(options);
    if (!imported.ok()) { return imported; }
    if (bytes.empty()) {
        imported.result = media_detail::error(MediaIoCode::InvalidArgument, "layered input bytes are required");
        return imported;
    }
    if (bytes.size() > options.limits.maxInputBytes
        || bytes.size() > std::uint64_t(std::numeric_limits<qsizetype>::max())) {
        imported.result = media_detail::error(MediaIoCode::LimitExceeded, "encoded layered input exceeds the byte limit");
        return imported;
    }

    const auto format = isPsd(bytes) ? "psd" : isZip(bytes) ? "ora" : "";
    imported.format = format;
    try {
        if (imported.format == "psd") {
            imported = layered_detail::decodePsd(bytes, options);
        } else if (imported.format == "ora") {
            imported = layered_detail::decodeOpenRaster(bytes, options);
        } else {
            imported.result = media_detail::error(MediaIoCode::UnsupportedFormat,
                                                  "no layer-preserving reader for this content; supported inputs are OpenRaster and PSD");
        }
        if (imported.ok()) {
            const auto validated = validate(imported.document);
            if (!validated.ok()) {
                imported.result = media_detail::error(MediaIoCode::InvalidData,
                                                      QString::fromStdString(validated.issues.front().message));
            }
        }
    } catch (const std::bad_alloc &) {
        imported.result = media_detail::error(MediaIoCode::LimitExceeded, "cannot allocate bounded layered document storage");
    } catch (const std::length_error &) {
        imported.result = media_detail::error(MediaIoCode::LimitExceeded, "layered document storage exceeds the host container limit");
    }
    imported.format = format;
    // The public result never exposes a prefix of a failed document, even if a
    // reader has already decoded some layers before discovering invalid input.
    if (!imported.ok()) { imported.document = {}; }
    return imported;
}

LayeredDocumentImportResult importLayeredDocument(
    const std::string &path, const LayeredDocumentImportOptions &options)
{
    LayeredDocumentImportResult imported;
    imported.result = checkOptions(options);
    if (!imported.ok()) { return imported; }
    try {
        auto input = media_detail::readFile(path, options.limits);
        if (!input.ok()) { imported.result = std::move(input.result); return imported; }
        return decodeLayeredDocument(input.bytes, options);
    } catch (const std::bad_alloc &) {
        imported.result = media_detail::error(MediaIoCode::LimitExceeded, "cannot allocate bounded layered input storage");
    } catch (const std::length_error &) {
        imported.result = media_detail::error(MediaIoCode::LimitExceeded, "layered input exceeds the host container limit");
    }
    return imported;
}

} // namespace iiSharedCanvas
