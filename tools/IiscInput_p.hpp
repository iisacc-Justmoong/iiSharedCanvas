#pragma once

#include <Media/MediaIo.h>

#include <QString>

#include <cstdint>
#include <stdexcept>
#include <string>

namespace iisc_tools {

class Failure final : public std::runtime_error {
public:
    Failure(iiSharedCanvas::MediaIoCode value, const std::string &message)
        : std::runtime_error(message), code(value) {}
    iiSharedCanvas::MediaIoCode code;
};

const char *codeName(iiSharedCanvas::MediaIoCode code);

// Read a detached native snapshot without binding or rewriting the original.
// Working-file backups are private temporary files beside the intended output.
iiSharedCanvas::Document loadDocument(const QString &path, const QString &outputParent,
                                      const iiSharedCanvas::MediaLimits &limits,
                                      std::uint32_t maxLayers);

} // namespace iisc_tools
