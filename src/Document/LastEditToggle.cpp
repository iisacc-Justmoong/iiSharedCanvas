#include "Document/LastEditToggle.h"

namespace iiSharedCanvas {
LastEditToggle::LastEditToggle(DocumentHistory &history) noexcept
    : history_(&history), id_(history.undoEntryId()) {}

DocumentHistoryResult LastEditToggle::toggle()
{
    if (!id_) return {DocumentHistoryCode::NoUndo};
    const auto current = applied_ ? history_->undoEntryId() : history_->redoEntryId();
    if (current != id_) return {DocumentHistoryCode::Conflict};
    auto result = applied_ ? history_->undo() : history_->redo();
    if (result.ok() && result.changed) applied_ = !applied_;
    return result;
}
} // namespace iiSharedCanvas
