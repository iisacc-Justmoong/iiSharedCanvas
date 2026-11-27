#pragma once
#include "Document/Document.h"
#include "iiSharedCanvas/Export.h"
#include <optional>

namespace iiSharedCanvas {
// Optional criteria are combined; names use a case-sensitive substring match.
struct LayerQuery {
    std::optional<bool> visible;
    std::string nameContains;
    std::optional<FrameIndex> frame;
    std::optional<LayerRole> role;
    std::optional<std::string> groupId;
};
struct LayerQueryResult {
    std::vector<std::string> layerIds;
    std::string error;
    [[nodiscard]] bool ok() const noexcept { return error.empty(); }
};
// Single-owner index. The source must outlive it and tracked edits must stamp
// authorship; queries reject a stale revision instead of returning stale data.
class IISHAREDCANVAS_EXPORT LayerQueryIndex final {
public:
    // Capture a non-owning ordered index at the document's current revision.
    explicit LayerQueryIndex(const Document &document);
    // Revalidate the source and atomically replace the captured ID order.
    [[nodiscard]] LayerQueryResult refresh(const Document &document);
    // Filter in document order; reject a different owner or stale revision.
    [[nodiscard]] LayerQueryResult query(const Document &document, const LayerQuery &) const;
private:
    const Document *owner_ = nullptr;
    std::uint64_t revision_ = 0;
    std::vector<std::string> order_;
};
} // namespace iiSharedCanvas
