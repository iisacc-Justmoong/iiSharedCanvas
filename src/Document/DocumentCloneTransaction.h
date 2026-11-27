#pragma once

#include "Serialization/IiscCodec.h"
#include "iiSharedCanvas/Export.h"

namespace iiSharedCanvas {

// A detached document or a codec/validation failure. The result never owns
// a file binding, document-history cursor or active editing session.
struct DocumentCloneResult {
    Document document;
    IiscError error;
    [[nodiscard]] bool ok() const noexcept { return error.code==IiscErrorCode::None; }
};

// Creates an independent, validated copy using the versioned document value
// representation. The caller owns the source and must not mutate it during
// execution. No paths, clocks, global storage or history objects are accessed.
class IISHAREDCANVAS_EXPORT DocumentCloneTransaction final {
public:
    // Preserves authored, generation and publication metadata; clears the
    // transient active author. The caller creates a new history for the copy.
    // Limits bound the portable snapshot and decode; the operation can require
    // memory for both the encoded snapshot and the detached document.
    [[nodiscard]] DocumentCloneResult operator()(const Document &source,
        SerializationLimits limits = {}) const;
};

} // namespace iiSharedCanvas
