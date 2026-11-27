#pragma once
#include "Document/DocumentHistory.h"

namespace iiSharedCanvas {
// Toggles only the history entry that was on top when this object was created.
// The history must outlive the toggle. A changed branch cannot redirect it.
class IISHAREDCANVAS_EXPORT LastEditToggle final {
public:
    // Capture the current undo entry without changing the document or cursor.
    explicit LastEditToggle(DocumentHistory &history) noexcept;
    // Undo or redo the captured entry. A mismatch never moves the cursor.
    DocumentHistoryResult toggle();
    // False when the history was empty at capture time.
    [[nodiscard]] bool hasEntry() const noexcept { return id_.has_value(); }
private:
    DocumentHistory *history_;
    std::optional<std::uint64_t> id_;
    bool applied_ = true;
};
} // namespace iiSharedCanvas
