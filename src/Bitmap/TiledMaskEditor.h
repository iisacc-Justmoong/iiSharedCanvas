#pragma once
#include "Bitmap/TiledMask.h"
#include "Bitmap/BitmapEditor.h"
#include <memory>
#include <string>
namespace iiSharedCanvas {
// Detached sparse mask editing session. One stroke becomes one local history entry.
class IISHAREDCANVAS_EXPORT TiledMaskEditor final {
public:
    explicit TiledMaskEditor(TiledMask initial, std::size_t historyBudget = 64*1024*1024);
    ~TiledMaskEditor();
    TiledMaskEditor(const TiledMaskEditor&) = delete;
    TiledMaskEditor& operator=(const TiledMaskEditor&) = delete;
    bool begin(const StrokePoint&, const BitmapBrush&);
    bool extend(const StrokePoint&);
    bool end(const StrokePoint&);
    void cancel() noexcept;
    bool undo();
    bool redo();
    [[nodiscard]] bool canUndo() const noexcept;
    [[nodiscard]] bool canRedo() const noexcept;
    [[nodiscard]] bool strokeActive() const noexcept;
    [[nodiscard]] bool changed() const noexcept;
    [[nodiscard]] std::uint8_t coverage(std::int32_t x, std::int32_t y) const noexcept;
    [[nodiscard]] const TiledMask& mask() const noexcept;
    [[nodiscard]] const std::string& error() const noexcept;
private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
    bool append(const StrokePoint&, bool finish);
};
}
