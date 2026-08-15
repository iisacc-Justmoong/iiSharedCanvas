#include <iiSharedCanvas.h>

#include <type_traits>
#include <variant>

#include <QQuickPaintedItem>

static_assert(std::is_same_v<decltype(iiSharedCanvas::RasterAsset::pixels), RasterLayer>);
static_assert(std::is_same_v<decltype(iiSharedCanvas::Layer::transform), AffineTransform>);
static_assert(std::is_same_v<decltype(iiSharedCanvas::Layer::blendMode), RasterBlendMode>);
static_assert(std::variant_size_v<iiSharedCanvas::Asset> == 2);
static_assert(std::variant_size_v<iiSharedCanvas::LayerSource> == 2);
static_assert(std::is_default_constructible_v<iiSharedCanvas::BitmapEditor>);
static_assert(std::is_base_of_v<QQuickPaintedItem, iiSharedCanvas::BitmapItem>);

int main()
{
    return iiSharedCanvas::CurrentFormatMajor == 1
        && iiSharedCanvas::CurrentFormatMinor == 0
        ? 0
        : 1;
}
