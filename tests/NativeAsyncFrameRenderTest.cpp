#include <iiSharedCanvasNative.h>

#include <algorithm>
#include <atomic>
#include <deque>
#include <functional>
#include <iostream>
#include <memory>
#include <mutex>
#include <stdexcept>
#include <thread>
#include <vector>

namespace {
using namespace iiSharedCanvas;
int failures = 0;
void check(bool condition, const char *message)
{
    if (!condition) { std::cerr << message << '\n'; ++failures; }
}

class ManualExecutor final : public FrameRenderExecutor {
public:
    void post(std::function<void()> task) override
    {
        std::lock_guard lock(mutex_);
        if (reject_) { reject_ = false; throw std::runtime_error("executor rejected task"); }
        tasks_.push_back(std::move(task));
    }
    std::size_t maxConcurrency() const noexcept override { return 4; }
    void rejectNext() { std::lock_guard lock(mutex_); reject_ = true; }
    std::size_t size() { std::lock_guard lock(mutex_); return tasks_.size(); }
    bool runOne()
    {
        std::function<void()> task;
        { std::lock_guard lock(mutex_); if (tasks_.empty()) return false;
          task = std::move(tasks_.front()); tasks_.pop_front(); }
        task();
        return true;
    }
    void runAll()
    {
        for (int count = 0; runOne(); ++count) {
            if (count > 1000) { check(false, "executor failed to drain"); break; }
        }
    }
    void runParallel()
    {
        for (int round = 0; round < 100 && size() != 0; ++round) {
            std::vector<std::thread> threads;
            const auto count = size();
            for (std::size_t i = 0; i < count; ++i)
                threads.emplace_back([this] { (void)runOne(); });
            for (auto &thread : threads) thread.join();
        }
        check(size() == 0, "parallel executor failed to drain");
    }
private:
    std::mutex mutex_;
    std::deque<std::function<void()>> tasks_;
    bool reject_ = false;
};
class InlineExecutor final : public FrameRenderExecutor {
public:
    void post(std::function<void()> task) override { task(); }
    std::size_t maxConcurrency() const noexcept override { return 2; }
};

Document fixture()
{
    Document document; document.extent = {4, 4}; document.timeline = {{24, 1}, 2};
    for (int i = 0; i < 4; ++i) {
        const auto id = "asset-" + std::to_string(i);
        document.assets.emplace_back(RasterAsset{id, makeRasterLayer(4, 4, 0xff010101U * std::uint32_t(i + 1))});
        StaticBitmapLayer layer; layer.properties.id = "layer-" + std::to_string(i);
        layer.properties.opacity = 0.5; layer.content = StaticSource{id};
        document.layers.emplace_back(std::move(layer));
    }
    return document;
}
const std::vector<FrameRenderTileRequest> requests{{{{0, 0}, {4, 4}}, {4, 4}}};
}

int main()
{
    using namespace iiSharedCanvas;
    auto executor = std::make_shared<ManualExecutor>();
    std::atomic<int> notifications{0};
    AsyncFrameRenderQueue queue(executor, [&] { ++notifications; });
    const auto original = fixture();
    const auto expected = renderFrameLayers(original, 0, requests);
    const auto expectedFrame = composeFrameLayers(expected);

    auto document = original;
    const auto first = queue.request(document, 0, requests);
    std::get<RasterAsset>(document.assets[0]).pixels.pixels.clear();
    check(first != 0 && queue.busy(), "an accepted snapshot must be queued immediately");
    check(executor->runOne() && executor->size() == 4, "preflight must dispatch one bounded worker per layer");
    executor->runParallel();
    auto result = queue.takeCompleted();
    check(result && result->requestId == first && result->status == AsyncFrameRenderStatus::Rendered,
          "parallel rendering must publish its request id");
    check(result && result->layers.ok() && result->frame.ok() && result->layers.layers.size() == 4
          && result->frame.tiles.size() == 1 && expectedFrame.ok()
          && result->frame.tiles[0].pixels.pixels == expectedFrame.tiles[0].pixels.pixels,
          "parallel layer output must match the synchronous composition of the owned snapshot");
    if (result && result->layers.layers.size() == 4) {
        for (std::size_t i = 0; i < 4; ++i)
            check(result->layers.layers[i].layerId == expected.layers[i].layerId,
                  "parallel layer slots must stay in document order");
    }
    check(!queue.busy() && !queue.takeCompleted() && notifications == 1,
          "completion is consumed once and notifies the caller");

    const auto olderCompletion = queue.request(original, 0, requests);
    executor->runAll();
    const auto newerCompletion = queue.request(original, 1, requests);
    result = queue.takeCompleted();
    check(result && result->requestId == olderCompletion && queue.busy(),
          "an untaken previous completion stays observable while a newer request runs");
    executor->runAll(); result = queue.takeCompleted();
    check(result && result->requestId == newerCompletion && result->frame.ok(),
          "a later completion remains individually identifiable");
    const auto replacedCompletion = queue.request(original, 0, requests);
    executor->runAll();
    const auto replacement = queue.request(original, 1, requests);
    executor->runAll(); result = queue.takeCompleted();
    check(replacedCompletion && result && result->requestId == replacement && !queue.takeCompleted(),
          "a newer completion replaces an untaken earlier result");
    notifications = 1; // Keep subsequent expectations independent of the observations above.

    const auto superseded = queue.request(original, 0, requests);
    check(executor->runOne(), "start the first render before coalescing");
    const auto intermediate = queue.request(original, 0, requests);
    const auto latest = queue.request(original, 1, requests);
    check(superseded && intermediate > superseded && latest > intermediate && queue.busy(),
          "requests receive increasing ids while one render is active");
    executor->runParallel();
    result = queue.takeCompleted();
    check(result && result->requestId == latest && result->frame.ok()
          && !queue.takeCompleted() && notifications == 2,
          "only the latest pending request may publish after coalescing");

    const auto cancelled = queue.request(original, 0, requests);
    check(cancelled && executor->runOne(), "start cancellable work");
    queue.cancel();
    check(queue.busy(), "a running worker stays busy until it has stopped");
    executor->runParallel();
    check(!queue.busy() && !queue.takeCompleted() && notifications == 2,
          "cancelled work must not publish or notify");
    check(queue.request(std::shared_ptr<const Document>{}, 0, requests) == 0,
          "a null snapshot is refused");

    executor->rejectNext();
    const auto rejected = queue.request(original, 0, requests);
    result = queue.takeCompleted();
    check(rejected && result && result->requestId == rejected
          && result->status == AsyncFrameRenderStatus::DispatchFailed && !queue.busy(),
          "executor rejection must complete with a dispatch error rather than strand the queue");

    const auto workerFallback = queue.request(original, 0, requests);
    executor->rejectNext();
    executor->runParallel(); result = queue.takeCompleted();
    check(result && result->requestId == workerFallback && result->status == AsyncFrameRenderStatus::Rendered
          && result->frame.ok(), "a rejected layer worker must finish on the accepted executor task");

    const auto invalid = queue.request(original, 2, requests);
    executor->runAll(); result = queue.takeCompleted();
    check(result && result->requestId == invalid && result->status == AsyncFrameRenderStatus::Rendered
          && result->layers.status == FrameRenderStatus::FrameOutOfRange,
          "render validation errors must be delivered as ordinary render results");

    (void)queue.request(original, 0, requests);
    check(executor->runOne(), "start render before concurrent requests");
    std::vector<std::uint64_t> ids(12);
    std::vector<std::thread> requesters;
    for (std::size_t i = 0; i < ids.size(); ++i)
        requesters.emplace_back([&, i] { ids[i] = queue.request(original, 0, requests); });
    for (auto &requester : requesters) requester.join();
    const auto latestConcurrent = *std::max_element(ids.begin(), ids.end());
    executor->runParallel(); result = queue.takeCompleted();
    check(result && result->requestId == latestConcurrent && result->frame.ok() && !queue.takeCompleted(),
          "concurrent callers must publish only the request with the latest assigned id");

    {
        AsyncFrameRenderQueue temporary(executor, [&] { ++notifications; });
        check(temporary.request(original, 0, requests) != 0, "queue work before destruction");
    }
    const auto before = notifications.load();
    executor->runAll();
    check(notifications == before, "destroyed queues must not start queued work or notify");

    auto inlineExecutor = std::make_shared<InlineExecutor>();
    int reentrantNotifications = 0;
    AsyncFrameRenderQueue *inlineQueue = nullptr;
    std::optional<AsyncFrameRenderResult> inlineResult;
    AsyncFrameRenderQueue direct(inlineExecutor, [&] {
        ++reentrantNotifications;
        inlineResult = inlineQueue->takeCompleted();
    });
    inlineQueue = &direct;
    const auto inlineId = direct.request(original, 0, requests);
    check(inlineResult && inlineResult->requestId == inlineId && inlineResult->frame.ok()
          && reentrantNotifications == 1 && !direct.busy(),
          "inline execution and reentrant wakeup must not deadlock or lose completion");

    std::weak_ptr<ManualExecutor> executorLifetime;
    {
        auto disposable = std::make_shared<ManualExecutor>();
        executorLifetime = disposable;
        { AsyncFrameRenderQueue temporary(disposable); (void)temporary.request(original, 0, requests); }
    }
    check(executorLifetime.expired(), "queued tasks must not retain their executor through a reference cycle");
    return failures ? 1 : 0;
}
