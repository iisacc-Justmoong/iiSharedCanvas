#include "QtAdapter/AsyncFrameRenderer.h"

#include <QFutureWatcher>
#include <QPromise>
#include <QThreadPool>

#include <optional>
#include <utility>

namespace iiSharedCanvas {

struct AsyncFrameRenderer::Private {
    struct Request {
        qulonglong id = 0;
        std::shared_ptr<const Document> document;
        FrameIndex frame = 0;
        std::vector<FrameRenderTileRequest> tiles;
    };

    QFutureWatcher<FrameTileRenderResult> watcher;
    std::optional<Request> pending;
    FrameTileRenderResult lastResult;
    qulonglong nextRequest = 1;
    qulonglong runningRequest = 0;
    qulonglong lastCompletedRequest = 0;
    bool busy = false;
    bool discardRunning = false;
};

AsyncFrameRenderer::AsyncFrameRenderer(QObject *parent)
    : QObject(parent), d(std::make_unique<Private>())
{
    connect(&d->watcher, &QFutureWatcher<FrameTileRenderResult>::finished,
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

FrameTileRenderResult AsyncFrameRenderer::takeResult() noexcept
{
    FrameTileRenderResult result = std::move(d->lastResult);
    d->lastResult = {};
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

    QPromise<FrameTileRenderResult> promise;
    promise.start();
    QFuture<FrameTileRenderResult> future = promise.future();
    d->watcher.setFuture(future);
    QThreadPool::globalInstance()->start(
        [promise = std::move(promise), request = std::move(request)]() mutable {
            if (!promise.isCanceled()) {
                promise.addResult(renderFrameTiles(*request.document,
                                                   request.frame,
                                                   request.tiles));
            }
            promise.finish();
        });
}

void AsyncFrameRenderer::finishCurrent()
{
    const qulonglong completedId = d->runningRequest;
    const bool hasResult = d->watcher.future().resultCount() > 0;
    FrameTileRenderResult result;
    if (hasResult) {
        result = d->watcher.result();
    }

    if (d->pending) {
        startNext();
        return;
    }

    setBusy(false);
    if (d->discardRunning || !hasResult) {
        return;
    }
    d->lastResult = std::move(result);
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
