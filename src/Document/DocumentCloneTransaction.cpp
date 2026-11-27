#include "Document/DocumentCloneTransaction.h"

#include <utility>

namespace iiSharedCanvas {
DocumentCloneResult DocumentCloneTransaction::operator()(const Document &source,
    SerializationLimits limits) const
{
    auto encoded=encodeIisc(source,limits);
    if (!encoded.ok()) return {{},std::move(encoded.error)};
    auto decoded=decodeIisc(encoded.bytes,limits);
    if (!decoded.ok()) return {{},std::move(decoded.error)};
    decoded.document.authorship.clearActiveAuthor();
    return {std::move(decoded.document),{}};
}
} // namespace iiSharedCanvas
