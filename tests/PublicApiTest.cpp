#include <iiSharedCanvas.h>

#include <cstdint>
#include <optional>
#include <string>
#include <type_traits>
#include <variant>
#include <vector>

#include <QQuickItem>
#include <QQuickPaintedItem>

static_assert(std::is_aggregate_v<iiSharedCanvas::Document>);
static_assert(std::is_aggregate_v<iiSharedCanvas::MediaLimits>);
static_assert(std::is_aggregate_v<iiSharedCanvas::BitmapImportOptions>);
static_assert(std::is_aggregate_v<iiSharedCanvas::VectorImportOptions>);
static_assert(std::is_aggregate_v<iiSharedCanvas::VideoImportOptions>);
static_assert(std::is_aggregate_v<iiSharedCanvas::LayeredDocumentImportOptions>);
static_assert(std::is_aggregate_v<iiSharedCanvas::LayeredDocumentImportResult>);
static_assert(std::is_aggregate_v<iiSharedCanvas::PsdExportOptions>);
static_assert(std::is_aggregate_v<iiSharedCanvas::TimelineInterchangeOptions>);
static_assert(std::is_same_v<decltype(iiSharedCanvas::encodePsd(std::declval<const iiSharedCanvas::Document &>())),
                             iiSharedCanvas::MediaBytesResult>);
static_assert(std::is_same_v<decltype(iiSharedCanvas::exportPsd(std::declval<const iiSharedCanvas::Document &>(),
                                                             std::declval<const std::string &>())),
                             iiSharedCanvas::MediaIoResult>);
static_assert(std::is_same_v<decltype(iiSharedCanvas::LayeredDocumentImportResult::document),
                             iiSharedCanvas::Document>);
static_assert(std::is_same_v<decltype(iiSharedCanvas::LayeredDocumentImportResult::format),
                             std::string>);
static_assert(std::is_same_v<decltype(std::declval<const iiSharedCanvas::LayeredDocumentImportResult &>().ok()), bool>);
static_assert(std::is_same_v<decltype(iiSharedCanvas::importLayeredDocument(std::declval<const std::string &>())),
                             iiSharedCanvas::LayeredDocumentImportResult>);
static_assert(std::is_same_v<decltype(iiSharedCanvas::BitmapImportResult::asset), iiSharedCanvas::RasterAsset>);
static_assert(std::is_same_v<decltype(iiSharedCanvas::VectorImportResult::asset), iiSharedCanvas::VectorAsset>);
static_assert(std::is_same_v<decltype(iiSharedCanvas::MediaDocumentResult::document), iiSharedCanvas::Document>);
static_assert(std::is_same_v<decltype(iiSharedCanvas::VideoInfo::frameRate), iiSharedCanvas::FrameRate>);
static_assert(std::is_same_v<decltype(std::declval<const iiSharedCanvas::MediaBytesResult &>().ok()), bool>);
static_assert(!std::is_copy_constructible_v<iiSharedCanvas::DocumentFile>);
static_assert(!std::is_move_constructible_v<iiSharedCanvas::DocumentFile>);
static_assert(std::is_same_v<decltype(std::declval<iiSharedCanvas::DocumentFile &>().document()),
                             const iiSharedCanvas::Document *>);
static_assert(std::is_constructible_v<iiSharedCanvas::DocumentEditor, iiSharedCanvas::DocumentFile &>);
static_assert(std::is_constructible_v<iiSharedCanvas::BitmapEditor,
                                      iiSharedCanvas::DocumentFile &, const std::string &>);
static_assert(std::is_constructible_v<iiSharedCanvas::ChunkedBitmapEditor,
                                      iiSharedCanvas::DocumentFile &, const std::string &>);
static_assert(std::is_constructible_v<iiSharedCanvas::VectorEditor,
                                      iiSharedCanvas::DocumentFile &, const std::string &>);

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
static_assert(std::is_same_v<decltype(iiSharedCanvas::StableDiffusionGenerationParameters::parameters),
                             std::vector<iiSharedCanvas::StableDiffusionMetadataEntry>>);
static_assert(std::is_same_v<decltype(iiSharedCanvas::StableDiffusionGenerationParametersParseResult::metadata),
                             iiSharedCanvas::StableDiffusionMetadata>);
static_assert(std::is_same_v<decltype(
                                 iiSharedCanvas::StableDiffusionGenerationParametersParseResult::generationParameters),
                             iiSharedCanvas::StableDiffusionGenerationParameters>);
static_assert(std::is_default_constructible_v<iiSharedCanvas::StableDiffusionGenerationParametersParseResult>);
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
static_assert(std::is_same_v<decltype(iiSharedCanvas::LayerFrameRange::firstFrame),
                             iiSharedCanvas::FrameIndex>);
static_assert(std::is_same_v<decltype(iiSharedCanvas::LayerFrameRange::lastFrame),
                             iiSharedCanvas::FrameIndex>);
static_assert(std::is_same_v<decltype(iiSharedCanvas::LayerProperties::frameRange),
                             std::optional<iiSharedCanvas::LayerFrameRange>>);
static_assert(std::is_same_v<decltype(iiSharedCanvas::BitmapLayer::properties),
                             iiSharedCanvas::LayerProperties>);
static_assert(std::is_same_v<decltype(iiSharedCanvas::BitmapLayer::source),
                             iiSharedCanvas::LayerSource>);
static_assert(std::is_same_v<decltype(iiSharedCanvas::VectorLayer::properties),
                             iiSharedCanvas::LayerProperties>);
static_assert(std::is_same_v<decltype(iiSharedCanvas::VectorLayer::source),
                             iiSharedCanvas::LayerSource>);
static_assert(std::is_same_v<decltype(iiSharedCanvas::KeyframedSource::frameIndices),
                             std::vector<iiSharedCanvas::FrameIndex>>);
static_assert(std::is_same_v<decltype(iiSharedCanvas::Keyframe::layerId),
                             std::string>);
static_assert(std::is_same_v<decltype(iiSharedCanvas::Keyframe::assetId),
                             std::string>);
static_assert(std::is_same_v<decltype(iiSharedCanvas::Frame::index),
                             iiSharedCanvas::FrameIndex>);
static_assert(std::is_same_v<decltype(iiSharedCanvas::Frame::keyframes),
                             std::vector<iiSharedCanvas::Keyframe>>);
static_assert(std::is_same_v<decltype(iiSharedCanvas::KeyframePlacement::frame),
                             iiSharedCanvas::FrameIndex>);
static_assert(std::is_same_v<decltype(iiSharedCanvas::KeyframePlacement::assetId),
                             std::string>);
static_assert(std::is_same_v<decltype(iiSharedCanvas::KeyframePlacement::frame),
                             iiSharedCanvas::FrameIndex>);
static_assert(std::is_same_v<decltype(iiSharedCanvas::KeyframePlacement::assetId),
                             std::string>);
static_assert(std::is_same_v<decltype(iiSharedCanvas::Document::assets),
                             std::vector<iiSharedCanvas::Asset>>);
static_assert(std::is_same_v<decltype(iiSharedCanvas::Document::layers),
                             std::vector<iiSharedCanvas::Layer>>);
static_assert(std::is_same_v<decltype(iiSharedCanvas::Document::frames),
                             std::vector<iiSharedCanvas::Frame>>);
static_assert(std::is_same_v<decltype(iiSharedCanvas::AssetReference::frameIndex),
                             std::optional<std::size_t>>);
static_assert(std::is_same_v<decltype(iiSharedCanvas::AssetReference::keyframeIndex),
                             std::optional<std::size_t>>);
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
static_assert(std::is_default_constructible_v<iiSharedCanvas::VectorEditor>);
static_assert(std::is_default_constructible_v<iiSharedCanvas::DocumentEditor>);
static_assert(std::is_default_constructible_v<iiSharedCanvas::AsyncFrameRenderer>);
static_assert(std::is_default_constructible_v<iiSharedCanvas::TimelineProject>);
static_assert(std::is_same_v<decltype(iiSharedCanvas::TimelineProject::mediaSources),
                             std::vector<iiSharedCanvas::TimelineMediaSource>>);
static_assert(std::is_same_v<decltype(iiSharedCanvas::TimelineProject::sequences),
                             std::vector<iiSharedCanvas::TimelineSequence>>);
static_assert(std::is_same_v<decltype(iiSharedCanvas::TimelineProject::renderProfiles),
                             std::vector<iiSharedCanvas::TimelineRenderProfile>>);
static_assert(std::is_same_v<decltype(iiSharedCanvas::TimelineSequence::editingFrameRate),
                             iiSharedCanvas::TimelineFrameRate>);
static_assert(std::is_same_v<decltype(iiSharedCanvas::TimelineClipProperties::source),
                             iiSharedCanvas::TimelineClipSource>);
static_assert(std::is_same_v<decltype(iiSharedCanvas::TimelineGeneratedReference::timeBase),
                             iiSharedCanvas::TimelineTimeBase>);
static_assert(std::is_same_v<decltype(iiSharedCanvas::TimelineRenderProfile::container),
                             iiSharedCanvas::TimelineContainerDescriptor>);
static_assert(std::is_same_v<decltype(iiSharedCanvas::TimelineRenderProfile::video),
                             std::optional<iiSharedCanvas::TimelineVideoOutput>>);
static_assert(std::variant_size_v<iiSharedCanvas::TimelineMediaStream> == 4);
static_assert(std::variant_size_v<iiSharedCanvas::TimelineTrack> == 4);
static_assert(std::variant_size_v<iiSharedCanvas::TimelineClip> == 4);
static_assert(std::is_same_v<
              std::variant_alternative_t<3, iiSharedCanvas::TimelineMediaStream>,
              iiSharedCanvas::TimelineDataStream>);
static_assert(std::is_same_v<
              std::variant_alternative_t<3, iiSharedCanvas::TimelineTrack>,
              iiSharedCanvas::TimelineDataTrack>);
static_assert(std::is_same_v<
              std::variant_alternative_t<3, iiSharedCanvas::TimelineClip>,
              iiSharedCanvas::TimelineDataClip>);
static_assert(std::is_constructible_v<iiSharedCanvas::TimelineEditor,
                                      iiSharedCanvas::TimelineProject &>);
static_assert(!std::is_copy_constructible_v<iiSharedCanvas::TimelineEditor>);
static_assert(!std::is_copy_assignable_v<iiSharedCanvas::TimelineEditor>);
static_assert(!std::is_move_constructible_v<iiSharedCanvas::TimelineEditor>);
static_assert(!std::is_move_assignable_v<iiSharedCanvas::TimelineEditor>);
static_assert(std::is_same_v<decltype(iiSharedCanvas::TimelineEditResult::changed), bool>);
static_assert(std::is_constructible_v<iiSharedCanvas::DocumentEditor,
                                      iiSharedCanvas::Document &>);
static_assert(std::is_constructible_v<iiSharedCanvas::VectorEditor,
                                      iiSharedCanvas::Document &,
                                      const std::string &>);
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
        && iiSharedCanvas::CurrentFormatMinor == 3
        ? 0
        : 1;
}
