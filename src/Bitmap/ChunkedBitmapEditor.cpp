#include "Bitmap/ChunkedBitmapEditor.h"

#include <Layer/RasterLayer.h>
#include <Render/DirtyRegion.h>

#include <algorithm>
#include <cmath>
#include <limits>
#include <map>
#include <utility>

namespace iiSharedCanvas {
namespace {

bool inUnitRange(double value) noexcept
{
    return std::isfinite(value) && value >= 0.0 && value <= 1.0;
}

std::int32_t floorDivision(std::int32_t value, std::int32_t divisor) noexcept
{
    const std::int32_t quotient = value / divisor;
    const std::int32_t remainder = value % divisor;
    return remainder < 0 ? quotient - 1 : quotient;
}

bool sameChunks(const std::vector<RasterChunk> &first,
                const std::vector<RasterChunk> &second) noexcept
{
    if (first.size() != second.size()) {
        return false;
    }
    for (std::size_t index = 0; index < first.size(); ++index) {
        const RasterChunk &left = first[index];
        const RasterChunk &right = second[index];
        if (left.column != right.column
            || left.row != right.row
            || left.pixels.width != right.pixels.width
            || left.pixels.height != right.pixels.height
            || left.pixels.pixels != right.pixels.pixels) {
            return false;
        }
    }
    return true;
}

bool chunkLess(const RasterChunk &chunk,
               const std::pair<std::int32_t, std::int32_t> &coordinate) noexcept
{
    return chunk.row < coordinate.second
        || (chunk.row == coordinate.second && chunk.column < coordinate.first);
}

} // namespace

ChunkedBitmapEditor::ChunkedBitmapEditor(Document &document, const std::string &assetId)
{
    bind(document, assetId);
}

bool ChunkedBitmapEditor::bind(Document &document, const std::string &assetId)
{
    ChunkedRasterAsset *candidate = findChunkedRasterAsset(document, assetId);
    if (!candidate || document.canvasMode != CanvasMode::Infinite) {
        setError(candidate ? "chunked raster requires an infinite canvas"
                           : "chunked raster asset was not found");
        return false;
    }

    m_document = &document;
    m_assetId = assetId;
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

void ChunkedBitmapEditor::unbind() noexcept
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

bool ChunkedBitmapEditor::isBound() const noexcept
{
    return m_document
        && m_document->canvasMode == CanvasMode::Infinite
        && asset() != nullptr;
}

const std::string &ChunkedBitmapEditor::boundAssetId() const noexcept
{
    return m_assetId;
}

const BitmapBrush &ChunkedBitmapEditor::brush() const noexcept
{
    return m_brush;
}

bool ChunkedBitmapEditor::setBrush(const BitmapBrush &value)
{
    if (m_strokeActive) {
        setError("brush settings cannot change during an active stroke");
        return false;
    }
    if (!std::isfinite(value.size) || value.size <= 0.0 || value.size > 4096.0
        || !inUnitRange(value.opacity)
        || !inUnitRange(value.flow)
        || !inUnitRange(value.hardness)
        || !std::isfinite(value.spacing) || value.spacing < 0.0 || value.spacing > 4096.0
        || !std::isfinite(value.spacingRatio)
        || value.spacingRatio < 0.0 || value.spacingRatio > 4.0) {
        setError("brush values are outside the supported finite range");
        return false;
    }
    m_brush = value;
    clearError();
    return true;
}

std::optional<std::uint32_t> ChunkedBitmapEditor::pixelAt(std::int32_t x,
                                                           std::int32_t y) const noexcept
{
    const ChunkedRasterAsset *bound = asset();
    if (!bound || !contains(x, y)) {
        return std::nullopt;
    }
    const std::int32_t size = m_document->infiniteCanvas.chunkSize;
    const std::int32_t column = floorDivision(x, size);
    const std::int32_t row = floorDivision(y, size);
    const RasterChunk *chunk = findRasterChunk(*bound, column, row);
    if (!chunk) {
        return 0x00000000U;
    }
    return rasterLayerPixelAt(chunk->pixels,
                              {x - column * size, y - row * size});
}

bool ChunkedBitmapEditor::clear()
{
    if (!requireBound() || m_strokeActive) {
        if (m_strokeActive) {
            setError("clear cannot run during a brush stroke");
        }
        return false;
    }
    ChunkedRasterAsset *bound = asset();
    if (bound->chunks.empty()) {
        clearError();
        return true;
    }
    recordSnapshot();
    bound->chunks.clear();
    const CanvasOrigin origin = canvasOrigin(*m_document);
    noteChange({{origin.x, origin.y}, m_document->extent.width, m_document->extent.height});
    clearError();
    return true;
}

bool ChunkedBitmapEditor::replaceRegion(CanvasOrigin origin, const RasterLayer &pixels)
{
    if (!requireBound() || m_strokeActive) {
        if (m_strokeActive) {
            setError("the chunked bitmap cannot be replaced during a brush stroke");
        }
        return false;
    }
    if (origin.x != canvasOrigin(*m_document).x
        || origin.y != canvasOrigin(*m_document).y
        || pixels.width != m_document->extent.width
        || pixels.height != m_document->extent.height
        || pixels.width <= 0 || pixels.height <= 0
        || static_cast<std::uint64_t>(pixels.width) * pixels.height != pixels.pixels.size()) {
        setError("replacement raster must exactly cover the allocated infinite canvas region");
        return false;
    }

    ChunkedRasterAsset replacement{m_assetId, {}};
    const std::int32_t chunkSize = m_document->infiniteCanvas.chunkSize;
    for (std::int32_t localY = 0; localY < pixels.height; ++localY) {
        for (std::int32_t localX = 0; localX < pixels.width; ++localX) {
            const std::uint32_t argb = rasterLayerPixelAt(pixels, {localX, localY});
            if (argb == 0x00000000U) {
                continue;
            }
            const std::int32_t worldX = origin.x + localX;
            const std::int32_t worldY = origin.y + localY;
            const std::int32_t column = floorDivision(worldX, chunkSize);
            const std::int32_t row = floorDivision(worldY, chunkSize);
            auto position = std::lower_bound(replacement.chunks.begin(),
                                             replacement.chunks.end(),
                                             std::pair{column, row},
                                             chunkLess);
            if (position == replacement.chunks.end()
                || position->column != column || position->row != row) {
                position = replacement.chunks.insert(
                    position,
                    RasterChunk{column, row,
                                makeRasterLayer(chunkSize, chunkSize, 0x00000000U)});
            }
            position->pixels.pixels[static_cast<std::size_t>(worldY - row * chunkSize)
                                        * static_cast<std::size_t>(chunkSize)
                                    + static_cast<std::size_t>(worldX - column * chunkSize)] = argb;
        }
    }

    ChunkedRasterAsset *bound = asset();
    if (sameChunks(bound->chunks, replacement.chunks)) {
        clearError();
        return true;
    }
    recordSnapshot();
    bound->chunks = std::move(replacement.chunks);
    noteChange({{origin.x, origin.y}, pixels.width, pixels.height});
    clearError();
    return true;
}

bool ChunkedBitmapEditor::beginStroke(DocumentPoint point, double pressure)
{
    if (!requireBound() || m_strokeActive || !validPoint(point, pressure)) {
        if (m_strokeActive) {
            setError("a brush stroke is already active");
        }
        return false;
    }
    recordSnapshot(false);
    resetRasterDabStream(m_dabStream);
    m_strokeActive = true;
    m_strokeChanged = false;
    m_strokeTime = 0.0;
    return appendStrokePoint(point, pressure, false);
}

bool ChunkedBitmapEditor::continueStroke(DocumentPoint point, double pressure)
{
    if (!m_strokeActive) {
        setError("no brush stroke is active");
        return false;
    }
    return validPoint(point, pressure) && appendStrokePoint(point, pressure, false);
}

bool ChunkedBitmapEditor::endStroke(DocumentPoint point, double pressure)
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
    ChunkedRasterAsset *bound = asset();
    if (!m_strokeChanged || (bound && !m_undoHistory.empty()
                             && sameChunks(bound->chunks, m_undoHistory.back()))) {
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

void ChunkedBitmapEditor::cancelStroke()
{
    if (!m_strokeActive) {
        return;
    }
    ChunkedRasterAsset *bound = asset();
    if (bound && !m_undoHistory.empty()) {
        bound->chunks = std::move(m_undoHistory.back());
        m_undoHistory.pop_back();
        const CanvasOrigin origin = canvasOrigin(*m_document);
        noteChange({{origin.x, origin.y}, m_document->extent.width, m_document->extent.height});
    }
    resetRasterDabStream(m_dabStream);
    m_strokeActive = false;
    m_strokeChanged = false;
    clearError();
}

bool ChunkedBitmapEditor::strokeActive() const noexcept
{
    return m_strokeActive;
}

bool ChunkedBitmapEditor::canUndo() const noexcept
{
    return !m_strokeActive && !m_undoHistory.empty() && isBound();
}

bool ChunkedBitmapEditor::canRedo() const noexcept
{
    return !m_strokeActive && !m_redoHistory.empty() && isBound();
}

bool ChunkedBitmapEditor::undo()
{
    if (!canUndo()) {
        setError("no chunked bitmap edit is available to undo");
        return false;
    }
    ChunkedRasterAsset *bound = asset();
    m_redoHistory.push_back(bound->chunks);
    bound->chunks = std::move(m_undoHistory.back());
    m_undoHistory.pop_back();
    const CanvasOrigin origin = canvasOrigin(*m_document);
    noteChange({{origin.x, origin.y}, m_document->extent.width, m_document->extent.height});
    clearError();
    return true;
}

bool ChunkedBitmapEditor::redo()
{
    if (!canRedo()) {
        setError("no chunked bitmap edit is available to redo");
        return false;
    }
    ChunkedRasterAsset *bound = asset();
    m_undoHistory.push_back(bound->chunks);
    if (m_undoHistory.size() > HistoryLimit) {
        m_undoHistory.erase(m_undoHistory.begin());
    }
    bound->chunks = std::move(m_redoHistory.back());
    m_redoHistory.pop_back();
    const CanvasOrigin origin = canvasOrigin(*m_document);
    noteChange({{origin.x, origin.y}, m_document->extent.width, m_document->extent.height});
    clearError();
    return true;
}

std::uint64_t ChunkedBitmapEditor::revision() const noexcept
{
    return m_revision;
}

DevicePixelRect ChunkedBitmapEditor::dirtyBounds() const noexcept
{
    return m_dirtyBounds;
}

void ChunkedBitmapEditor::clearDirtyBounds() noexcept
{
    m_dirtyBounds = {};
}

const std::string &ChunkedBitmapEditor::lastError() const noexcept
{
    return m_lastError;
}

ChunkedRasterAsset *ChunkedBitmapEditor::asset() noexcept
{
    return m_document ? findChunkedRasterAsset(*m_document, m_assetId) : nullptr;
}

const ChunkedRasterAsset *ChunkedBitmapEditor::asset() const noexcept
{
    return m_document
        ? findChunkedRasterAsset(std::as_const(*m_document), m_assetId)
        : nullptr;
}

bool ChunkedBitmapEditor::requireBound()
{
    if (!isBound()) {
        setError("no valid chunked raster asset is bound");
        return false;
    }
    return true;
}

bool ChunkedBitmapEditor::validPoint(DocumentPoint point, double pressure)
{
    if (!std::isfinite(point.x) || !std::isfinite(point.y) || !inUnitRange(pressure)
        || point.x < std::numeric_limits<std::int32_t>::min()
        || point.x > std::numeric_limits<std::int32_t>::max()
        || point.y < std::numeric_limits<std::int32_t>::min()
        || point.y > std::numeric_limits<std::int32_t>::max()) {
        setError("stroke position must use finite supported coordinates and unit pressure");
        return false;
    }
    return true;
}

BrushState ChunkedBitmapEditor::brushState() const
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

bool ChunkedBitmapEditor::contains(std::int32_t x, std::int32_t y) const noexcept
{
    if (!m_document) {
        return false;
    }
    const CanvasOrigin origin = canvasOrigin(*m_document);
    return x >= origin.x && y >= origin.y
        && static_cast<std::int64_t>(x) < static_cast<std::int64_t>(origin.x) + m_document->extent.width
        && static_cast<std::int64_t>(y) < static_cast<std::int64_t>(origin.y) + m_document->extent.height;
}

RasterChunk &ChunkedBitmapEditor::ensureChunk(std::int32_t column, std::int32_t row)
{
    ChunkedRasterAsset *bound = asset();
    auto position = std::lower_bound(bound->chunks.begin(), bound->chunks.end(),
                                     std::pair{column, row}, chunkLess);
    if (position == bound->chunks.end()
        || position->column != column || position->row != row) {
        const std::int32_t size = m_document->infiniteCanvas.chunkSize;
        position = bound->chunks.insert(
            position,
            RasterChunk{column, row, makeRasterLayer(size, size, 0x00000000U)});
    }
    return *position;
}

void ChunkedBitmapEditor::recordSnapshot(bool clearRedo)
{
    const ChunkedRasterAsset *bound = asset();
    if (!bound) {
        return;
    }
    m_undoHistory.push_back(bound->chunks);
    if (m_undoHistory.size() > HistoryLimit) {
        m_undoHistory.erase(m_undoHistory.begin());
    }
    if (clearRedo) {
        m_redoHistory.clear();
    }
}

void ChunkedBitmapEditor::noteChange(DevicePixelRect bounds)
{
    if (isEmpty(bounds)) {
        return;
    }
    m_dirtyBounds = uniteDevicePixelRects(m_dirtyBounds, bounds);
    ++m_revision;
}

bool ChunkedBitmapEditor::appendStrokePoint(DocumentPoint point,
                                             double pressure,
                                             bool finish)
{
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

    const std::vector<RasterSample> samples = projectBrushDabs(
        dabs, state.rasterizer, RasterProjection{}, state.material);
    const std::int32_t chunkSize = m_document->infiniteCanvas.chunkSize;
    std::map<std::pair<std::int32_t, std::int32_t>, std::vector<RasterSample>> byChunk;
    DevicePixelRect changedBounds{};
    for (RasterSample sample : samples) {
        if (!contains(sample.position.x, sample.position.y)) {
            continue;
        }
        const std::int32_t column = floorDivision(sample.position.x, chunkSize);
        const std::int32_t row = floorDivision(sample.position.y, chunkSize);
        changedBounds = uniteDevicePixelRects(changedBounds,
                                               {{sample.position.x, sample.position.y}, 1, 1});
        sample.position.x -= column * chunkSize;
        sample.position.y -= row * chunkSize;
        byChunk[{column, row}].push_back(sample);
    }
    for (auto &[coordinate, localSamples] : byChunk) {
        RasterChunk &chunk = ensureChunk(coordinate.first, coordinate.second);
        paintRasterSamples(chunk.pixels, localSamples);
    }
    if (!byChunk.empty()) {
        m_strokeChanged = true;
        noteChange(changedBounds);
    }
    clearError();
    return true;
}

void ChunkedBitmapEditor::setError(std::string message)
{
    m_lastError = std::move(message);
}

void ChunkedBitmapEditor::clearError() noexcept
{
    m_lastError.clear();
}

} // namespace iiSharedCanvas
