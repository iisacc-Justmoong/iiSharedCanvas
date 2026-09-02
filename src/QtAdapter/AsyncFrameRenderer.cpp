#include "QtAdapter/AsyncFrameRenderer.h"

#include <QFutureWatcher>
#include <QPromise>
#include <QThreadPool>

#include <algorithm>
#include <atomic>
#include <cstddef>
#include <memory>
#include <optional>
#include <utility>
#include <vector>

namespace iiSharedCanvas {

namespace {

struct AsyncFrameRenderOutput {
    FrameLayerBatchRenderResult layers;
    FrameTileRenderResult frame;
};

struct ParallelLayerState {
    std::shared_ptr<const Document> document;
    FrameIndex frame = 0;
    std::vector<FrameRenderTileRequest> requests;
    std::vector<std::optional<FrameLayerTileRenderResult>> layers;
    std::atomic_size_t nextLayer{0};
    std::atomic_size_t remainingWorkers{0};
    std::shared_ptr<QPromise<AsyncFrameRenderOutput>> promise;
};

void finishParallelLayers(const std::shared_ptr<ParallelLayerState> &state)
{
    if (!state->promise->isCanceled()) {
        FrameLayerBatchRenderResult batch;
        batch.requests = state->requests;
        batch.layers.reserve(state->layers.size());
        for (std::optional<FrameLayerTileRenderResult> &layer : state->layers) {
            if (!layer) {
                batch.status = FrameRenderStatus::InvalidDocument;
                batch.message = "parallel layer rendering ended without a layer result";
                break;
            }
            if (batch.ok() && !layer->ok()) {
                batch.status = layer->status;
                batch.message = layer->message;
            }
            batch.layers.push_back(std::move(*layer));
        }

        AsyncFrameRenderOutput output;
        output.layers = std::move(batch);
        output.frame = composeFrameLayers(output.layers);
        state->promise->addResult(std::move(output));
    }
    state->promise->finish();
}

void renderParallelLayers(const std::shared_ptr<ParallelLayerState> &state)
{
    while (!state->promise->isCanceled()) {
        const std::size_t layerIndex = state->nextLayer.fetch_add(
            1, std::memory_order_relaxed);
        if (layerIndex >= state->layers.size()) {
            break;
        }
        state->layers[layerIndex] = renderFrameLayerTiles(
            *state->document, state->frame, layerIndex, state->requests);
    }

    if (state->remainingWorkers.fetch_sub(1, std::memory_order_acq_rel) == 1) {
        finishParallelLayers(state);
    }
}

} // namespace

struct AsyncFrameRenderer::Private {
    struct Request {
        qulonglong id = 0;
        std::shared_ptr<const Document> document;
        FrameIndex frame = 0;
        std::vector<FrameRenderTileRequest> tiles;
    };

    QFutureWatcher<AsyncFrameRenderOutput> watcher;
    std::optional<Request> pending;
    FrameTileRenderResult lastResult;
    FrameLayerBatchRenderResult lastLayerResult;
    qulonglong nextRequest = 1;
    qulonglong runningRequest = 0;
    qulonglong lastCompletedRequest = 0;
    bool busy = false;
    bool discardRunning = false;
};

AsyncFrameRenderer::AsyncFrameRenderer(QObject *parent)
    : QObject(parent), d(std::make_unique<Private>())
{
    connect(&d->watcher, &QFutureWatcher<AsyncFrameRenderOutput>::finished,
            this, &AsyncFrameRenderer::finishCurrent);
}

AsyncFrameRenderer::~AsyncFrameRenderer()
{
    cancel();
    d->watcher.disconnect(this);
}

qulonglong AsyncFrameRenderer::request(
    Document document,
    FrameIndex frame,
    std::vector<FrameRenderTileRequest> requests)
{
    return request(std::make_shared<Document>(std::move(document)),
                   frame,
                   std::move(requests));
}

qulonglong AsyncFrameRenderer::request(
    std::shared_ptr<const Document> document,
    FrameIndex frame,
    std::vector<FrameRenderTileRequest> requests)
{
    if (!document) {
        return 0;
    }
    const qulonglong requestId = d->nextRequest++;
    d->pending = Private::Request{
        requestId,
        std::move(document),
        frame,
        std::move(requests),
    };
    d->discardRunning = false;
    setBusy(true);
    if (!d->watcher.isRunning()) {
        startNext();
    }
    return requestId;
}

void AsyncFrameRenderer::cancel()
{
    d->pending.reset();
    d->discardRunning = true;
    if (d->watcher.isRunning()) {
        d->watcher.cancel();
    } else {
        setBusy(false);
    }
}

bool AsyncFrameRenderer::busy() const noexcept
{
    return d->busy;
}

qulonglong AsyncFrameRenderer::lastCompletedRequest() const noexcept
{
    return d->lastCompletedRequest;
}

const FrameTileRenderResult &AsyncFrameRenderer::lastResult() const noexcept
{
    return d->lastResult;
}

const FrameLayerBatchRenderResult &AsyncFrameRenderer::lastLayerResult() const noexcept
{
    return d->lastLayerResult;
}

FrameTileRenderResult AsyncFrameRenderer::takeResult() noexcept
{
    FrameTileRenderResult result = std::move(d->lastResult);
    d->lastResult = {};
    return result;
}

FrameLayerBatchRenderResult AsyncFrameRenderer::takeLayerResult() noexcept
{
    FrameLayerBatchRenderResult result = std::move(d->lastLayerResult);
    d->lastLayerResult = {};
    return result;
}

void AsyncFrameRenderer::startNext()
{
    if (!d->pending) {
        setBusy(false);
        return;
    }

    Private::Request request = std::move(*d->pending);
    d->pending.reset();
    d->runningRequest = request.id;

    auto promise = std::make_shared<QPromise<AsyncFrameRenderOutput>>();
    promise->start();
    QFuture<AsyncFrameRenderOutput> future = promise->future();
    d->watcher.setFuture(future);

    const std::size_t layerCount = request.document->layers.size();
    if (layerCount == 0) {
        QThreadPool::globalInstance()->start(
            [promise = std::move(promise), request = std::move(request)]() mutable {
                if (!promise->isCanceled()) {
                    AsyncFrameRenderOutput output;
                    output.layers = renderFrameLayers(
                        *request.document, request.frame, request.tiles);
                    output.frame = composeFrameLayers(output.layers);
                    promise->addResult(std::move(output));
                }
                promise->finish();
            });
        return;
    }

    auto state = std::make_shared<ParallelLayerState>();
    state->document = std::move(request.document);
    state->frame = request.frame;
    state->requests = std::move(request.tiles);
    state->layers.resize(layerCount);
    state->promise = std::move(promise);

    const std::size_t maximumWorkers = static_cast<std::size_t>(
        std::max(1, QThreadPool::globalInstance()->maxThreadCount()));
    const std::size_t workerCount = std::min(layerCount, maximumWorkers);
    state->remainingWorkers.store(workerCount, std::memory_order_relaxed);
    for (std::size_t worker = 0; worker < workerCount; ++worker) {
        QThreadPool::globalInstance()->start([state] {
            renderParallelLayers(state);
        });
    }
}

void AsyncFrameRenderer::finishCurrent()
{
    const qulonglong completedId = d->runningRequest;
    const bool hasResult = d->watcher.future().resultCount() > 0;
    AsyncFrameRenderOutput output;
    if (hasResult) {
        output = d->watcher.result();
    }

    if (d->pending) {
        startNext();
        return;
    }

    setBusy(false);
    if (d->discardRunning || !hasResult) {
        return;
    }
    d->lastLayerResult = std::move(output.layers);
    d->lastResult = std::move(output.frame);
    d->lastCompletedRequest = completedId;
    emit finished(completedId);
}

void AsyncFrameRenderer::setBusy(bool value)
{
    if (d->busy == value) {
        return;
    }
    d->busy = value;
    emit busyChanged();
}

} // namespace iiSharedCanvas
