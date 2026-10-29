#include "Render/AsyncFrameRenderQueue.h"
#include "Render/FrameRenderDispatch_p.hpp"

#include <algorithm>
#include <atomic>
#include <exception>
#include <limits>
#include <mutex>
#include <stdexcept>
#include <utility>

namespace iiSharedCanvas {

struct AsyncFrameRenderQueue::State final {
    struct Request {
        std::uint64_t id;
        std::shared_ptr<const Document> document;
        FrameIndex frame;
        std::vector<FrameRenderTileRequest> tiles;
    };
    struct Job {
        explicit Job(Request value) : request(std::move(value)) {}
        Request request;
        std::atomic<bool> cancelled{false}, failed{false};
        std::atomic_size_t nextLayer{0}, remaining{0};
        std::vector<std::optional<FrameLayerTileRenderResult>> layers;
        std::mutex errorMutex;
        std::string error;
    };

    std::mutex mutex;
    std::shared_ptr<FrameRenderExecutor> executor;
    std::function<void()> wakeup;
    std::shared_ptr<Job> active;
    std::shared_ptr<Job> pending;
    std::optional<AsyncFrameRenderResult> completed;
    std::uint64_t nextId = 1;
    bool closed = false;

    static void finish(const std::shared_ptr<State> &state, const std::shared_ptr<Job> &job,
                       std::optional<AsyncFrameRenderResult> result)
    {
        std::shared_ptr<Job> next;
        std::function<void()> wakeup;
        {
            std::lock_guard lock(state->mutex);
            if (state->active != job) return;
            state->active.reset();
            if (!state->closed && state->pending) {
                next = std::move(state->pending);
                state->active = next;
            } else if (!state->closed && !job->cancelled.load(std::memory_order_acquire) && result) {
                state->completed = std::move(result);
                wakeup = state->wakeup;
            }
        }
        if (next) schedule(state, next);
        if (wakeup) { try { wakeup(); } catch (...) { /* A wakeup cannot break queue state. */ } }
    }

    static void fail(const std::shared_ptr<State> &state, const std::shared_ptr<Job> &job,
                     AsyncFrameRenderStatus status, std::string message)
    {
        AsyncFrameRenderResult result;
        result.requestId = job->request.id; result.status = status; result.message = std::move(message);
        finish(state, job, std::move(result));
    }

    static void finishLayers(const std::shared_ptr<State> &state, const std::shared_ptr<Job> &job)
    {
        if (job->cancelled.load(std::memory_order_acquire)) { finish(state, job, std::nullopt); return; }
        if (job->failed.load(std::memory_order_acquire)) {
            std::lock_guard lock(job->errorMutex);
            fail(state, job, AsyncFrameRenderStatus::InternalError, job->error);
            return;
        }
        try {
            AsyncFrameRenderResult result;
            result.requestId = job->request.id;
            result.layers.requests = job->request.tiles;
            result.layers.artboards = job->request.document->artboards;
            result.layers.layers.reserve(job->layers.size());
            for (auto &layer : job->layers) {
                if (!layer) throw std::runtime_error("parallel rendering ended without a layer result");
                if (result.layers.ok() && !layer->ok()) {
                    result.layers.status = layer->status; result.layers.message = layer->message;
                }
                result.layers.layers.push_back(std::move(*layer));
            }
            result.frame = composeFrameLayers(result.layers);
            finish(state, job, std::move(result));
        } catch (const std::exception &error) {
            fail(state, job, AsyncFrameRenderStatus::InternalError, error.what());
        } catch (...) {
            fail(state, job, AsyncFrameRenderStatus::InternalError, "unknown rendering error");
        }
    }

    static void worker(const std::weak_ptr<State> &weak, const std::shared_ptr<Job> &job)
    {
        try {
            while (!job->cancelled.load(std::memory_order_acquire)
                   && !job->failed.load(std::memory_order_acquire)) {
                const auto index = job->nextLayer.fetch_add(1, std::memory_order_relaxed);
                if (index >= job->layers.size()) break;
                job->layers[index] = render_detail::renderPreflightedFrameLayerTiles(
                    *job->request.document, job->request.frame, index, job->request.tiles);
            }
        } catch (const std::exception &error) {
            std::lock_guard lock(job->errorMutex);
            if (!job->failed.exchange(true, std::memory_order_acq_rel)) job->error = error.what();
        } catch (...) {
            std::lock_guard lock(job->errorMutex);
            if (!job->failed.exchange(true, std::memory_order_acq_rel)) job->error = "unknown layer rendering error";
        }
        if (job->remaining.fetch_sub(1, std::memory_order_acq_rel) == 1) {
            if (auto state = weak.lock()) finishLayers(state, job);
        }
    }

    static void start(const std::weak_ptr<State> &weak, const std::shared_ptr<Job> &job)
    {
        const auto state = weak.lock();
        if (!state) return;
        if (job->cancelled.load(std::memory_order_acquire)) { finish(state, job, std::nullopt); return; }
        try {
            auto preflight = render_detail::preflightFrameLayerRender(
                *job->request.document, job->request.frame, job->request.tiles);
            if (job->cancelled.load(std::memory_order_acquire)) { finish(state, job, std::nullopt); return; }
            if (!preflight.ok() || job->request.document->layers.empty()) {
                AsyncFrameRenderResult result; result.requestId = job->request.id;
                result.layers = std::move(preflight); result.frame = composeFrameLayers(result.layers);
                finish(state, job, std::move(result));
                return;
            }
            job->layers.resize(job->request.document->layers.size());
            const auto workers = std::min({job->layers.size(),
                std::max<std::size_t>(1, state->executor->maxConcurrency()), std::size_t{64}});
            job->remaining.store(workers, std::memory_order_relaxed);
            for (std::size_t i = 0; i < workers; ++i) {
                try {
                    state->executor->post([weak, job] { worker(weak, job); });
                } catch (...) {
                    // An already accepted job remains completable if the executor
                    // temporarily declines an individual layer worker.
                    worker(weak, job);
                }
            }
        } catch (const std::exception &error) {
            fail(state, job, AsyncFrameRenderStatus::InternalError, error.what());
        } catch (...) {
            fail(state, job, AsyncFrameRenderStatus::InternalError, "unknown preflight error");
        }
    }

    static void schedule(const std::shared_ptr<State> &state, const std::shared_ptr<Job> &job)
    {
        const std::weak_ptr<State> weak = state;
        try {
            state->executor->post([weak, job] { start(weak, job); });
        } catch (const std::exception &error) {
            fail(state, job, AsyncFrameRenderStatus::DispatchFailed, error.what());
        } catch (...) {
            fail(state, job, AsyncFrameRenderStatus::DispatchFailed, "executor rejected render work");
        }
    }
};

AsyncFrameRenderQueue::AsyncFrameRenderQueue(std::shared_ptr<FrameRenderExecutor> executor,
                                             std::function<void()> wakeup)
    : state_(std::make_shared<State>())
{
    if (!executor) throw std::invalid_argument("a frame render executor is required");
    state_->executor = std::move(executor); state_->wakeup = std::move(wakeup);
}

AsyncFrameRenderQueue::~AsyncFrameRenderQueue()
{
    std::lock_guard lock(state_->mutex);
    state_->closed = true;
    state_->pending.reset();
    if (state_->active) state_->active->cancelled.store(true, std::memory_order_release);
    state_->wakeup = {};
}

std::uint64_t AsyncFrameRenderQueue::request(Document document, FrameIndex frame,
                                             std::vector<FrameRenderTileRequest> requests)
{
    return request(std::make_shared<Document>(std::move(document)), frame, std::move(requests));
}

std::uint64_t AsyncFrameRenderQueue::request(std::shared_ptr<const Document> document, FrameIndex frame,
                                             std::vector<FrameRenderTileRequest> requests)
{
    if (!document) return 0;
    std::shared_ptr<State::Job> start;
    std::uint64_t id;
    {
        std::lock_guard lock(state_->mutex);
        if (state_->closed || state_->nextId == 0) return 0;
        id = state_->nextId++;
        auto job = std::make_shared<State::Job>(
            State::Request{id, std::move(document), frame, std::move(requests)});
        if (state_->active) {
            state_->pending = std::move(job);
            state_->active->cancelled.store(true, std::memory_order_release);
        } else {
            start = std::move(job);
            state_->active = start;
        }
    }
    if (start) State::schedule(state_, start);
    return id;
}

void AsyncFrameRenderQueue::cancel()
{
    std::lock_guard lock(state_->mutex);
    state_->pending.reset();
    if (state_->active) state_->active->cancelled.store(true, std::memory_order_release);
}

bool AsyncFrameRenderQueue::busy() const
{
    std::lock_guard lock(state_->mutex);
    return bool(state_->active) || bool(state_->pending);
}

std::optional<AsyncFrameRenderResult> AsyncFrameRenderQueue::takeCompleted()
{
    std::lock_guard lock(state_->mutex);
    auto result = std::move(state_->completed);
    state_->completed.reset();
    return result;
}

} // namespace iiSharedCanvas
