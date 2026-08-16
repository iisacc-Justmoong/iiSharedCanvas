#include "Bitmap/BitmapEditor.h"

#include <Render/DirtyRegion.h>

#include <algorithm>
#include <cmath>
#include <limits>
#include <utility>

namespace iiSharedCanvas {
namespace {

bool hasValidPixelStorage(const RasterLayer &layer)
{
    if (layer.width <= 0 || layer.height <= 0) {
        return false;
    }

    const auto width = static_cast<std::size_t>(layer.width);
    const auto height = static_cast<std::size_t>(layer.height);
    return width <= std::numeric_limits<std::size_t>::max() / height
        && width * height == layer.pixels.size();
}

bool isFinitePoint(DocumentPoint point)
{
    return std::isfinite(point.x) && std::isfinite(point.y);
}

bool inUnitRange(double value)
{
    return std::isfinite(value) && value >= 0.0 && value <= 1.0;
}

bool contains(DevicePixelRect outer, DevicePixelRect inner)
{
    if (inner.width <= 0 || inner.height <= 0) {
        return false;
    }

    const std::int64_t outerRight = static_cast<std::int64_t>(outer.origin.x) + outer.width;
    const std::int64_t outerBottom = static_cast<std::int64_t>(outer.origin.y) + outer.height;
    const std::int64_t innerRight = static_cast<std::int64_t>(inner.origin.x) + inner.width;
    const std::int64_t innerBottom = static_cast<std::int64_t>(inner.origin.y) + inner.height;
    return inner.origin.x >= outer.origin.x
        && inner.origin.y >= outer.origin.y
        && innerRight <= outerRight
        && innerBottom <= outerBottom;
}

} // namespace

BitmapEditor::BitmapEditor(Document &document, const std::string &assetId)
{
    bind(document, assetId);
}

bool BitmapEditor::bind(Document &document, const std::string &assetIdValue)
{
    Asset *candidate = findAsset(document, assetIdValue);
    auto *raster = candidate ? std::get_if<RasterAsset>(candidate) : nullptr;
    if (!raster) {
        setError(candidate ? "asset is not raster content" : "raster asset was not found");
        return false;
    }
    if (!hasValidPixelStorage(raster->pixels)) {
        setError("raster asset has invalid dimensions or pixel storage");
        return false;
    }

    m_document = &document;
    m_assetId = assetIdValue;
    resetRasterDabStream(m_dabStream);
    m_undoHistory.clear();
    m_redoHistory.clear();
    m_dirtyBounds = {};
    m_revision = 0;
    m_nextStrokeSeed = 1;
    m_strokeTime = 0.0;
    m_strokeActive = false;
    m_strokeChanged = false;
    clearError();
    return true;
}

void BitmapEditor::unbind() noexcept
{
    m_document = nullptr;
    m_assetId.clear();
    resetRasterDabStream(m_dabStream);
    m_undoHistory.clear();
    m_redoHistory.clear();
    m_dirtyBounds = {};
    m_revision = 0;
    m_strokeTime = 0.0;
    m_strokeActive = false;
    m_strokeChanged = false;
    clearError();
}

bool BitmapEditor::isBound() const noexcept
{
    const RasterAsset *asset = rasterAsset();
    return asset && hasValidPixelStorage(asset->pixels);
}

const std::string &BitmapEditor::boundAssetId() const noexcept
{
    return m_assetId;
}

const RasterLayer *BitmapEditor::pixels() const noexcept
{
    const RasterAsset *asset = rasterAsset();
    return asset && hasValidPixelStorage(asset->pixels) ? &asset->pixels : nullptr;
}

int BitmapEditor::width() const noexcept
{
    const RasterLayer *layer = pixels();
    return layer ? layer->width : 0;
}

int BitmapEditor::height() const noexcept
{
    const RasterLayer *layer = pixels();
    return layer ? layer->height : 0;
}

std::optional<std::uint32_t> BitmapEditor::pixelAt(int x, int y) const noexcept
{
    const RasterLayer *layer = pixels();
    if (!layer || x < 0 || y < 0 || x >= layer->width || y >= layer->height) {
        return std::nullopt;
    }
    return rasterLayerPixelAt(*layer, {x, y});
}

const BitmapBrush &BitmapEditor::brush() const noexcept
{
    return m_brush;
}

bool BitmapEditor::setBrush(const BitmapBrush &value)
{
    if (m_strokeActive) {
        setError("brush settings cannot change during an active stroke");
        return false;
    }
    if (!std::isfinite(value.size) || value.size <= 0.0 || value.size > 4096.0
        || !inUnitRange(value.opacity)
        || !inUnitRange(value.flow)
        || !inUnitRange(value.hardness)
        || !std::isfinite(value.spacing)
        || value.spacing < 0.0
        || value.spacing > 4096.0
        || !std::isfinite(value.spacingRatio)
        || value.spacingRatio < 0.0
        || value.spacingRatio > 4.0) {
        setError("brush values are outside the supported finite range");
        return false;
    }

    m_brush = value;
    clearError();
    return true;
}

bool BitmapEditor::setPixel(int x, int y, std::uint32_t argb)
{
    if (!requireBound()) {
        return false;
    }
    if (m_strokeActive) {
        setError("a direct pixel edit cannot run during a brush stroke");
        return false;
    }

    RasterLayer *layer = mutablePixels();
    if (!layer || x < 0 || y < 0 || x >= layer->width || y >= layer->height) {
        setError("pixel coordinate is outside the bitmap");
        return false;
    }

    const std::size_t index = static_cast<std::size_t>(y)
        * static_cast<std::size_t>(layer->width) + static_cast<std::size_t>(x);
    if (layer->pixels[index] == argb) {
        clearError();
        return true;
    }

    recordSnapshot();
    layer->pixels[index] = argb;
    noteChange({{x, y}, 1, 1});
    clearError();
    return true;
}

bool BitmapEditor::clear(std::uint32_t argb)
{
    if (!requireBound()) {
        return false;
    }
    if (m_strokeActive) {
        setError("clear cannot run during a brush stroke");
        return false;
    }

    RasterLayer *layer = mutablePixels();
    if (!layer) {
        return false;
    }
    if (std::all_of(layer->pixels.begin(), layer->pixels.end(),
                    [argb](std::uint32_t pixel) { return pixel == argb; })) {
        clearError();
        return true;
    }

    recordSnapshot();
    std::fill(layer->pixels.begin(), layer->pixels.end(), argb);
    noteChange(bitmapBounds());
    clearError();
    return true;
}

bool BitmapEditor::replacePatch(DevicePixelRect bounds,
                                const std::vector<std::uint32_t> &argbPixels)
{
    if (!requireBound()) {
        return false;
    }
    if (m_strokeActive) {
        setError("a pixel patch cannot be replaced during a brush stroke");
        return false;
    }
    if (!contains(bitmapBounds(), bounds)) {
        setError("pixel patch bounds are outside the bitmap");
        return false;
    }

    const auto patchWidth = static_cast<std::size_t>(bounds.width);
    const auto patchHeight = static_cast<std::size_t>(bounds.height);
    if (patchWidth > std::numeric_limits<std::size_t>::max() / patchHeight
        || patchWidth * patchHeight != argbPixels.size()) {
        setError("pixel patch size does not match its bounds");
        return false;
    }

    RasterLayer *layer = mutablePixels();
    bool changed = false;
    for (int y = 0; y < bounds.height && !changed; ++y) {
        const std::size_t destinationStart = static_cast<std::size_t>(bounds.origin.y + y)
            * static_cast<std::size_t>(layer->width)
            + static_cast<std::size_t>(bounds.origin.x);
        const std::size_t sourceStart = static_cast<std::size_t>(y) * patchWidth;
        changed = !std::equal(argbPixels.begin() + static_cast<std::ptrdiff_t>(sourceStart),
                              argbPixels.begin() + static_cast<std::ptrdiff_t>(sourceStart + patchWidth),
                              layer->pixels.begin() + static_cast<std::ptrdiff_t>(destinationStart));
    }
    if (!changed) {
        clearError();
        return true;
    }

    recordSnapshot();
    for (int y = 0; y < bounds.height; ++y) {
        const std::size_t destinationStart = static_cast<std::size_t>(bounds.origin.y + y)
            * static_cast<std::size_t>(layer->width)
            + static_cast<std::size_t>(bounds.origin.x);
        const std::size_t sourceStart = static_cast<std::size_t>(y) * patchWidth;
        std::copy_n(argbPixels.begin() + static_cast<std::ptrdiff_t>(sourceStart),
                    patchWidth,
                    layer->pixels.begin() + static_cast<std::ptrdiff_t>(destinationStart));
    }
    noteChange(bounds);
    clearError();
    return true;
}

bool BitmapEditor::replacePixels(const RasterLayer &replacement)
{
    if (!requireBound()) {
        return false;
    }
    if (m_strokeActive) {
        setError("the bitmap cannot be replaced during a brush stroke");
        return false;
    }
    if (!hasValidPixelStorage(replacement)) {
        setError("replacement raster has invalid dimensions or pixel storage");
        return false;
    }

    RasterLayer *layer = mutablePixels();
    if (layer->width == replacement.width
        && layer->height == replacement.height
        && layer->pixels == replacement.pixels) {
        clearError();
        return true;
    }

    const DevicePixelRect priorBounds = bitmapBounds();
    recordSnapshot();
    *layer = replacement;
    noteChange(uniteDevicePixelRects(priorBounds, bitmapBounds()));
    clearError();
    return true;
}

bool BitmapEditor::beginStroke(DocumentPoint point, double pressure)
{
    if (!requireBound()) {
        return false;
    }
    if (m_strokeActive) {
        setError("a brush stroke is already active");
        return false;
    }
    if (!validPoint(point, pressure)) {
        return false;
    }

    recordSnapshot(false);
    resetRasterDabStream(m_dabStream);
    m_strokeActive = true;
    m_strokeChanged = false;
    m_strokeTime = 0.0;
    return appendStrokePoint(point, pressure, false);
}

bool BitmapEditor::continueStroke(DocumentPoint point, double pressure)
{
    if (!m_strokeActive) {
        setError("no brush stroke is active");
        return false;
    }
    if (!validPoint(point, pressure)) {
        return false;
    }
    return appendStrokePoint(point, pressure, false);
}

bool BitmapEditor::endStroke(DocumentPoint point, double pressure)
{
    if (!m_strokeActive) {
        setError("no brush stroke is active");
        return false;
    }
    if (!validPoint(point, pressure)) {
        return false;
    }

    const bool appended = appendStrokePoint(point, pressure, true);
    m_strokeActive = false;
    resetRasterDabStream(m_dabStream);

    RasterLayer *layer = mutablePixels();
    if (!m_strokeChanged || (layer && !m_undoHistory.empty()
                             && layer->width == m_undoHistory.back().width
                             && layer->height == m_undoHistory.back().height
                             && layer->pixels == m_undoHistory.back().pixels)) {
        if (!m_undoHistory.empty()) {
            m_undoHistory.pop_back();
        }
    } else {
        m_redoHistory.clear();
        ++m_nextStrokeSeed;
    }
    m_strokeChanged = false;
    return appended;
}

void BitmapEditor::cancelStroke()
{
    if (!m_strokeActive) {
        return;
    }

    RasterLayer *layer = mutablePixels();
    if (layer && !m_undoHistory.empty()) {
        const bool changed = layer->width != m_undoHistory.back().width
            || layer->height != m_undoHistory.back().height
            || layer->pixels != m_undoHistory.back().pixels;
        *layer = std::move(m_undoHistory.back());
        m_undoHistory.pop_back();
        if (changed) {
            noteChange(bitmapBounds());
        }
    }
    resetRasterDabStream(m_dabStream);
    m_strokeActive = false;
    m_strokeChanged = false;
    clearError();
}

bool BitmapEditor::strokeActive() const noexcept
{
    return m_strokeActive;
}

bool BitmapEditor::canUndo() const noexcept
{
    return !m_strokeActive && !m_undoHistory.empty() && isBound();
}

bool BitmapEditor::canRedo() const noexcept
{
    return !m_strokeActive && !m_redoHistory.empty() && isBound();
}

bool BitmapEditor::undo()
{
    if (!canUndo()) {
        setError("no bitmap edit is available to undo");
        return false;
    }

    RasterLayer *layer = mutablePixels();
    const DevicePixelRect priorBounds = bitmapBounds();
    m_redoHistory.push_back(*layer);
    *layer = std::move(m_undoHistory.back());
    m_undoHistory.pop_back();
    noteChange(uniteDevicePixelRects(priorBounds, bitmapBounds()));
    clearError();
    return true;
}

bool BitmapEditor::redo()
{
    if (!canRedo()) {
        setError("no bitmap edit is available to redo");
        return false;
    }

    RasterLayer *layer = mutablePixels();
    const DevicePixelRect priorBounds = bitmapBounds();
    m_undoHistory.push_back(*layer);
    if (m_undoHistory.size() > HistoryLimit) {
        m_undoHistory.erase(m_undoHistory.begin());
    }
    *layer = std::move(m_redoHistory.back());
    m_redoHistory.pop_back();
    noteChange(uniteDevicePixelRects(priorBounds, bitmapBounds()));
    clearError();
    return true;
}

std::uint64_t BitmapEditor::revision() const noexcept
{
    return m_revision;
}

DevicePixelRect BitmapEditor::dirtyBounds() const noexcept
{
    return m_dirtyBounds;
}

void BitmapEditor::clearDirtyBounds() noexcept
{
    m_dirtyBounds = {};
}

const std::string &BitmapEditor::lastError() const noexcept
{
    return m_lastError;
}

RasterAsset *BitmapEditor::rasterAsset() noexcept
{
    if (!m_document || m_assetId.empty()) {
        return nullptr;
    }
    Asset *asset = findAsset(*m_document, m_assetId);
    return asset ? std::get_if<RasterAsset>(asset) : nullptr;
}

const RasterAsset *BitmapEditor::rasterAsset() const noexcept
{
    if (!m_document || m_assetId.empty()) {
        return nullptr;
    }
    const Asset *asset = findAsset(std::as_const(*m_document), m_assetId);
    return asset ? std::get_if<RasterAsset>(asset) : nullptr;
}

RasterLayer *BitmapEditor::mutablePixels() noexcept
{
    RasterAsset *asset = rasterAsset();
    return asset ? &asset->pixels : nullptr;
}

bool BitmapEditor::requireBound()
{
    if (!isBound()) {
        setError("no valid raster asset is bound");
        return false;
    }
    return true;
}

bool BitmapEditor::validPoint(DocumentPoint point, double pressure)
{
    if (!isFinitePoint(point) || !inUnitRange(pressure)) {
        setError("stroke position and pressure must be finite and pressure must be within zero and one");
        return false;
    }
    return true;
}

BrushState BitmapEditor::brushState() const
{
    Rasterizer rasterizer;
    rasterizer.radius = static_cast<Types::Pixel>(std::max(1.0, std::ceil(m_brush.size * 0.5)));
    rasterizer.brushSize = m_brush.size;
    rasterizer.argb = m_brush.eraser ? 0xff000000U : m_brush.argb;
    rasterizer.spacing = m_brush.spacing;
    rasterizer.spacingRatio = m_brush.spacingRatio;
    rasterizer.spacingEnabled = m_brush.spacingEnabled;
    rasterizer.opacity = m_brush.opacity;
    rasterizer.opacityEnabled = m_brush.opacityEnabled;
    rasterizer.flow = m_brush.flow;
    rasterizer.flowEnabled = m_brush.flowEnabled;
    rasterizer.hardness = m_brush.hardness;
    rasterizer.hardnessEnabled = m_brush.hardnessEnabled;
    rasterizer.blendMode = m_brush.eraser
        ? RasterBlendMode::DestinationOut
        : RasterBlendMode::SourceOver;

    BrushDynamics dynamics;
    dynamics.pressureToSize = 1.0;
    dynamics.pressureToFlow = 1.0;
    dynamics.pressureToOpacity = 1.0;
    dynamics.pressureToOpacityEnabled = m_brush.pressureToOpacityEnabled;
    return {std::move(rasterizer), std::move(dynamics), BrushMaterial{}, m_nextStrokeSeed};
}

DevicePixelRect BitmapEditor::bitmapBounds() const noexcept
{
    const RasterLayer *layer = pixels();
    return layer ? DevicePixelRect{{0, 0}, layer->width, layer->height} : DevicePixelRect{};
}

void BitmapEditor::recordSnapshot(bool clearRedo)
{
    const RasterLayer *layer = pixels();
    if (!layer) {
        return;
    }
    m_undoHistory.push_back(*layer);
    if (m_undoHistory.size() > HistoryLimit) {
        m_undoHistory.erase(m_undoHistory.begin());
    }
    if (clearRedo) {
        m_redoHistory.clear();
    }
}

void BitmapEditor::noteChange(DevicePixelRect bounds)
{
    if (isEmpty(bounds)) {
        return;
    }
    m_dirtyBounds = uniteDevicePixelRects(m_dirtyBounds, bounds);
    ++m_revision;
}

bool BitmapEditor::appendStrokePoint(DocumentPoint point, double pressure, bool finish)
{
    RasterLayer *layer = mutablePixels();
    if (!layer) {
        setError("bound raster asset became unavailable");
        return false;
    }

    m_strokeTime += 1.0 / 120.0;
    StrokePoint strokePoint;
    strokePoint.position = point;
    strokePoint.pressure = pressure;
    strokePoint.time = m_strokeTime;

    const BrushState state = brushState();
    const std::vector<BrushDab> dabs = appendRasterDabs(m_dabStream,
                                                        strokePoint,
                                                        state,
                                                        finish);
    if (dabs.empty()) {
        clearError();
        return true;
    }

    const RasterProjection projection{};
    const std::vector<RasterSample> samples = projectBrushDabs(dabs,
                                                               state.rasterizer,
                                                               projection,
                                                               state.material);
    if (!samples.empty()) {
        paintRasterSamples(*layer, samples);
        const DevicePixelRect projectedBounds = deviceBoundsForBrushDabsUnion(
            dabs, state.rasterizer, projection);
        const DevicePixelRect clippedBounds = intersectDevicePixelRects(bitmapBounds(),
                                                                         projectedBounds);
        if (!isEmpty(clippedBounds)) {
            m_strokeChanged = true;
            noteChange(clippedBounds);
        }
    }
    clearError();
    return true;
}

void BitmapEditor::setError(std::string message)
{
    m_lastError = std::move(message);
}

void BitmapEditor::clearError() noexcept
{
    m_lastError.clear();
}

} // namespace iiSharedCanvas
