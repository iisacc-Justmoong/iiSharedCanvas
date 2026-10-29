#include <iiSharedCanvasNative.h>
#include <iiSDKCore/Runtime/Runtime.h>

#include <iostream>
#include <memory>
#include <stdexcept>
#include <utility>

namespace {
// This adapter belongs to the consumer: neither SDK needs to include the other.
class RuntimeExecutor final : public iiSharedCanvas::FrameRenderExecutor {
public:
    explicit RuntimeExecutor(std::shared_ptr<ii::runtime::Runtime> runtime)
        : runtime_(std::move(runtime)), workers_(runtime_->stats().workers) {}
    void post(std::function<void()> task) override
    {
        if (!runtime_->post(std::move(task))) throw std::runtime_error("runtime rejected render work");
    }
    std::size_t maxConcurrency() const noexcept override { return workers_; }
private:
    std::shared_ptr<ii::runtime::Runtime> runtime_;
    std::size_t workers_;
};
}

int main()
{
    using namespace iiSharedCanvas;
    auto runtime = std::make_shared<ii::runtime::Runtime>(ii::runtime::RuntimeOptions{2, 16});
    auto executor = std::make_shared<RuntimeExecutor>(runtime);
    Document document; document.extent = {8, 8};
    document.assets.emplace_back(RasterAsset{"base", makeRasterLayer(8, 8, 0xff123456U)});
    StaticBitmapLayer layer; layer.properties.id = "layer"; layer.content = StaticSource{"base"};
    document.layers.emplace_back(std::move(layer));
    const std::vector<FrameRenderTileRequest> requests{{{{0, 0}, {8, 8}}, {8, 8}}};
    AsyncFrameRenderQueue queue(executor);
    const auto id = queue.request(document, 0, requests);
    const auto idle = runtime->wait_idle();
    const auto result = queue.takeCompleted();
    const auto stats = runtime->stats();
    if (!id || !idle || queue.busy() || !result || result->requestId != id
        || result->status != AsyncFrameRenderStatus::Rendered || !result->frame.ok()
        || result->frame.tiles.size() != 1 || result->frame.tiles[0].pixels.pixels.front() != 0xff123456U
        || stats.executed < 2) {
        std::cerr << "iiSDKCore Runtime did not complete the native frame render\n";
        return 1;
    }
    return 0;
}
