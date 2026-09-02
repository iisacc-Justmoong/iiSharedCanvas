#pragma once

#include "Document/Document.h"
#include "Export.h"
#include "Render/FrameRenderer.h"

#include <QObject>
#include <QtTypes>

#include <memory>
#include <vector>

namespace iiSharedCanvas {

class IISHAREDCANVAS_EXPORT AsyncFrameRenderer final : public QObject {
    Q_OBJECT
    Q_PROPERTY(bool busy READ busy NOTIFY busyChanged)
    Q_PROPERTY(qulonglong lastCompletedRequest READ lastCompletedRequest NOTIFY finished)

public:
    explicit AsyncFrameRenderer(QObject *parent = nullptr);
    ~AsyncFrameRenderer() override;

    qulonglong request(Document document,
                       FrameIndex frame,
                       std::vector<FrameRenderTileRequest> requests);
    qulonglong request(std::shared_ptr<const Document> document,
                       FrameIndex frame,
                       std::vector<FrameRenderTileRequest> requests);
    void cancel();
    [[nodiscard]] bool busy() const noexcept;
    [[nodiscard]] qulonglong lastCompletedRequest() const noexcept;
    [[nodiscard]] const FrameTileRenderResult &lastResult() const noexcept;
    [[nodiscard]] const FrameLayerBatchRenderResult &lastLayerResult() const noexcept;
    FrameTileRenderResult takeResult() noexcept;
    FrameLayerBatchRenderResult takeLayerResult() noexcept;

signals:
    void busyChanged();
    void finished(qulonglong requestId);

private:
    struct Private;
    std::unique_ptr<Private> d;

    void startNext();
    void finishCurrent();
    void setBusy(bool busy);
};

} // namespace iiSharedCanvas
