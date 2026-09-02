#include <iiSharedCanvas.h>

#include <cstdint>
#include <optional>
#include <string>
#include <type_traits>
#include <variant>
#include <vector>

#include <QQuickItem>
#include <QQuickPaintedItem>

static_assert(std::is_same_v<decltype(iiSharedCanvas::RasterAsset::pixels), RasterLayer>);
static_assert(std::is_same_v<decltype(iiSharedCanvas::RasterAsset::id), std::string>);
static_assert(std::is_same_v<decltype(iiSharedCanvas::RasterChunk::column), std::int32_t>);
static_assert(std::is_same_v<decltype(iiSharedCanvas::RasterChunk::row), std::int32_t>);
static_assert(std::is_same_v<decltype(iiSharedCanvas::RasterChunk::pixels), RasterLayer>);
static_assert(std::is_same_v<decltype(iiSharedCanvas::ChunkedRasterAsset::chunks),
                             std::vector<iiSharedCanvas::RasterChunk>>);
static_assert(std::is_same_v<decltype(iiSharedCanvas::CameraRawSensorImage::samples),
                             std::vector<std::uint32_t>>);
static_assert(std::is_same_v<decltype(iiSharedCanvas::CameraRawSensorImage::activeArea),
                             std::optional<iiSharedCanvas::CameraRawRegion>>);
static_assert(std::is_same_v<decltype(iiSharedCanvas::CameraRawSensorImage::cfaPattern),
                             std::optional<iiSharedCanvas::CameraRawCfaPattern>>);
static_assert(std::is_same_v<decltype(iiSharedCanvas::CameraRawData::image),
                             iiSharedCanvas::CameraRawSensorImage>);
static_assert(std::is_default_constructible_v<iiSharedCanvas::CameraRawData>);
static_assert(std::is_same_v<decltype(iiSharedCanvas::StableDiffusionSamplingPass::seed),
                             std::optional<std::uint64_t>>);
static_assert(std::is_same_v<decltype(iiSharedCanvas::StableDiffusionSamplingPass::steps),
                             std::optional<std::uint32_t>>);
static_assert(std::is_same_v<decltype(iiSharedCanvas::StableDiffusionSamplingPass::cfgScale),
                             std::optional<double>>);
static_assert(std::is_same_v<decltype(iiSharedCanvas::StableDiffusionMetadata::samplingPasses),
                             std::vector<iiSharedCanvas::StableDiffusionSamplingPass>>);
static_assert(std::is_same_v<decltype(iiSharedCanvas::StableDiffusionMetadata::comfyUi),
                             iiSharedCanvas::ComfyUiMetadata>);
static_assert(std::is_same_v<decltype(iiSharedCanvas::Document::stableDiffusionMetadata),
                             std::optional<iiSharedCanvas::StableDiffusionMetadata>>);
static_assert(std::is_default_constructible_v<iiSharedCanvas::StableDiffusionMetadata>);
static_assert(std::is_same_v<decltype(iiSharedCanvas::Automatic1111Infotext::parameters),
                             std::vector<iiSharedCanvas::StableDiffusionMetadataEntry>>);
static_assert(std::is_same_v<decltype(iiSharedCanvas::Automatic1111ParseResult::metadata),
                             iiSharedCanvas::StableDiffusionMetadata>);
static_assert(std::is_default_constructible_v<iiSharedCanvas::Automatic1111ParseResult>);
static_assert(std::is_same_v<decltype(iiSharedCanvas::VectorAsset::id), std::string>);
static_assert(std::is_same_v<decltype(iiSharedCanvas::VectorAsset::viewport),
                             iiSharedCanvas::CanvasExtent>);
static_assert(std::is_same_v<decltype(iiSharedCanvas::VectorAsset::paths),
                             std::vector<iiSharedCanvas::VectorPath>>);
static_assert(std::is_same_v<decltype(iiSharedCanvas::VectorPath::commands),
                             std::vector<iiSharedCanvas::PathCommand>>);
static_assert(std::is_same_v<decltype(iiSharedCanvas::VectorPath::fill),
                             std::optional<iiSharedCanvas::SolidPaint>>);
static_assert(std::is_same_v<decltype(iiSharedCanvas::VectorPath::stroke),
                             std::optional<iiSharedCanvas::StrokeStyle>>);
static_assert(std::variant_size_v<iiSharedCanvas::PathCommand> == 5);
static_assert(std::is_same_v<decltype(iiSharedCanvas::SolidPaint::argb), std::uint32_t>);
static_assert(std::is_same_v<decltype(iiSharedCanvas::StrokeStyle::width), double>);
static_assert(std::is_same_v<decltype(iiSharedCanvas::LayerProperties::transform),
                             AffineTransform>);
static_assert(std::is_same_v<decltype(iiSharedCanvas::LayerProperties::blendMode),
                             RasterBlendMode>);
static_assert(std::is_same_v<decltype(iiSharedCanvas::LayerProperties::id), std::string>);
static_assert(std::is_same_v<decltype(iiSharedCanvas::LayerProperties::name), std::string>);
static_assert(std::is_same_v<decltype(iiSharedCanvas::LayerProperties::visible), bool>);
static_assert(std::is_same_v<decltype(iiSharedCanvas::LayerProperties::opacity), double>);
static_assert(std::is_same_v<decltype(iiSharedCanvas::BitmapLayer::properties),
                             iiSharedCanvas::LayerProperties>);
static_assert(std::is_same_v<decltype(iiSharedCanvas::BitmapLayer::source),
                             iiSharedCanvas::LayerSource>);
static_assert(std::is_same_v<decltype(iiSharedCanvas::VectorLayer::properties),
                             iiSharedCanvas::LayerProperties>);
static_assert(std::is_same_v<decltype(iiSharedCanvas::VectorLayer::source),
                             iiSharedCanvas::LayerSource>);
static_assert(std::is_same_v<decltype(iiSharedCanvas::KeyframedSource::keyframes),
                             std::vector<iiSharedCanvas::Keyframe>>);
static_assert(std::is_same_v<decltype(iiSharedCanvas::Document::assets),
                             std::vector<iiSharedCanvas::Asset>>);
static_assert(std::is_same_v<decltype(iiSharedCanvas::Document::layers),
                             std::vector<iiSharedCanvas::Layer>>);
static_assert(std::is_same_v<decltype(iiSharedCanvas::Document::canvasMode),
                             iiSharedCanvas::CanvasMode>);
static_assert(std::is_same_v<decltype(iiSharedCanvas::Document::infiniteCanvas),
                             iiSharedCanvas::InfiniteCanvas>);
static_assert(std::variant_size_v<iiSharedCanvas::Asset> == 3);
static_assert(std::variant_size_v<iiSharedCanvas::Layer> == 2);
static_assert(std::is_same_v<std::variant_alternative_t<0, iiSharedCanvas::Layer>,
                             iiSharedCanvas::BitmapLayer>);
static_assert(std::is_same_v<std::variant_alternative_t<1, iiSharedCanvas::Layer>,
                             iiSharedCanvas::VectorLayer>);
static_assert(std::variant_size_v<iiSharedCanvas::LayerSource> == 2);
static_assert(std::is_default_constructible_v<iiSharedCanvas::BitmapEditor>);
static_assert(std::is_default_constructible_v<iiSharedCanvas::ChunkedBitmapEditor>);
static_assert(std::is_default_constructible_v<iiSharedCanvas::DocumentEditor>);
static_assert(std::is_default_constructible_v<iiSharedCanvas::AsyncFrameRenderer>);
static_assert(std::is_constructible_v<iiSharedCanvas::DocumentEditor,
                                      iiSharedCanvas::Document &>);
static_assert(std::is_same_v<decltype(iiSharedCanvas::DocumentEditResult::changed), bool>);
static_assert(std::is_default_constructible_v<iiSharedCanvas::FrameRenderResult>);
static_assert(std::is_default_constructible_v<iiSharedCanvas::FrameTileRenderResult>);
static_assert(std::is_default_constructible_v<iiSharedCanvas::FrameLayerTileRenderResult>);
static_assert(std::is_default_constructible_v<iiSharedCanvas::FrameLayerBatchRenderResult>);
static_assert(std::is_same_v<decltype(iiSharedCanvas::FrameLayerBatchRenderResult::layers),
                             std::vector<iiSharedCanvas::FrameLayerTileRenderResult>>);
static_assert(std::is_default_constructible_v<iiSharedCanvas::IiscEncodeResult>);
static_assert(std::is_default_constructible_v<iiSharedCanvas::IiscDecodeResult>);
static_assert(std::is_base_of_v<QQuickPaintedItem, iiSharedCanvas::BitmapItem>);
static_assert(std::is_base_of_v<QQuickItem, iiSharedCanvas::CanvasItem>);
static_assert(!std::is_base_of_v<QQuickPaintedItem, iiSharedCanvas::CanvasItem>);

int main()
{
    return iiSharedCanvas::CurrentFormatMajor == 1
        && iiSharedCanvas::CurrentFormatMinor == 2
        ? 0
        : 1;
}
