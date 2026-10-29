#pragma once

#include "Render/FrameRenderer.h"

#include <cstdint>
#include <functional>
#include <memory>
#include <optional>
#include <string>
#include <vector>

namespace iiSharedCanvas {

// The application owns scheduling policy. post() either accepts a task once or
// throws before accepting it; accepted tasks must eventually run while the
// queue is alive. It must be safe to call concurrently and may
// run tasks inline. maxConcurrency() is a scheduling hint, not a reservation.
class IISHAREDCANVAS_EXPORT FrameRenderExecutor {
public:
    virtual ~FrameRenderExecutor() = default;
    virtual void post(std::function<void()> task) = 0;
    [[nodiscard]] virtual std::size_t maxConcurrency() const noexcept = 0;
};

enum class AsyncFrameRenderStatus { Rendered, DispatchFailed, InternalError };
struct AsyncFrameRenderResult {
    std::uint64_t requestId = 0;
    AsyncFrameRenderStatus status = AsyncFrameRenderStatus::Rendered;
    std::string message;
    FrameLayerBatchRenderResult layers;
    FrameTileRenderResult frame;
};

// A latest-request-wins queue. Methods other than destruction can be called
// concurrently. Each request owns its document copy or shares a caller-provided
// immutable snapshot; the caller must not mutate a shared snapshot concurrently.
// A zero id denotes a null snapshot or an exhausted request-id space.
//
// The optional wakeup is invoked outside the queue lock on a worker, possibly
// inline when the executor does so. It is only a hint: poll takeCompleted(),
// and use a lifetime-safe capture because it can race cancellation/destruction.
// Destruction cancels without waiting for already-running work.
class IISHAREDCANVAS_EXPORT AsyncFrameRenderQueue final {
public:
    explicit AsyncFrameRenderQueue(std::shared_ptr<FrameRenderExecutor> executor,
                                   std::function<void()> wakeup = {});
    ~AsyncFrameRenderQueue();
    AsyncFrameRenderQueue(const AsyncFrameRenderQueue &) = delete;
    AsyncFrameRenderQueue &operator=(const AsyncFrameRenderQueue &) = delete;

    std::uint64_t request(Document document, FrameIndex frame,
                          std::vector<FrameRenderTileRequest> requests);
    std::uint64_t request(std::shared_ptr<const Document> document, FrameIndex frame,
                          std::vector<FrameRenderTileRequest> requests);
    void cancel();
    [[nodiscard]] bool busy() const;
    [[nodiscard]] std::optional<AsyncFrameRenderResult> takeCompleted();

private:
    struct State;
    std::shared_ptr<State> state_;
};

} // namespace iiSharedCanvas
