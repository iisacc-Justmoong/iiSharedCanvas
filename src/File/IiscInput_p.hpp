#pragma once

#include "File/DocumentFile.h"

#include <stdexcept>
#include <string>

namespace iiSharedCanvas::detail {

class IISHAREDCANVAS_EXPORT IiscInputFailure final : public std::runtime_error {
public:
    IiscInputFailure(DocumentFileCode value, const std::string &message)
        : std::runtime_error(message), code(value) {}
    DocumentFileCode code;
};

// Shared bounded, detached reader for native upgrade/export adapters. No writable
// connection ever touches a source database; WAL data belongs to its snapshot.
IISHAREDCANVAS_EXPORT Document loadIiscDocumentReadOnly(
    const std::string &path, const std::string &temporaryParent, SerializationLimits limits);

} // namespace iiSharedCanvas::detail
