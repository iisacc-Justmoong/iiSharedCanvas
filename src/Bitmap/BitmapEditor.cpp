#include "Bitmap/BitmapEditor.h"
#include "Bitmap/BitmapEditJournal_p.h"
#include "Bitmap/BrushEngineState_p.hpp"
#include "Bitmap/StrokeSample_p.hpp"
#include "File/DocumentFile.h"

#include <Render/DirtyRegion.h>

#include <algorithm>
#include <cmath>
#include <limits>
#include <stdexcept>
#include <utility>

namespace iiSharedCanvas
{
namespace
{

bool hasValidPixelStorage(const RasterLayer &layer)
{
    if (layer.width <= 0 || layer.height <= 0)
    {
        return false;
    }

    const auto width = static_cast<std::size_t>(layer.width);
    const auto height = static_cast<std::size_t>(layer.height);
    return width <= std::numeric_limits<std::size_t>::max() / height &&
           width * height == layer.pixels.size();
}

bool isFinitePoint(DocumentPoint point) { return std::isfinite(point.x) && std::isfinite(point.y); }

bool inUnitRange(double value) { return std::isfinite(value) && value >= 0.0 && value <= 1.0; }

bool contains(DevicePixelRect outer, DevicePixelRect inner)
{
    if (inner.width <= 0 || inner.height <= 0)
    {
        return false;
    }

    const std::int64_t outerRight = static_cast<std::int64_t>(outer.origin.x) + outer.width;
    const std::int64_t outerBottom = static_cast<std::int64_t>(outer.origin.y) + outer.height;
    const std::int64_t innerRight = static_cast<std::int64_t>(inner.origin.x) + inner.width;
    const std::int64_t innerBottom = static_cast<std::int64_t>(inner.origin.y) + inner.height;
    return inner.origin.x >= outer.origin.x && inner.origin.y >= outer.origin.y &&
           innerRight <= outerRight && innerBottom <= outerBottom;
}

} // namespace

BitmapEditor::BitmapEditor(Document &document, const std::string &assetId,
                           RasterHistoryLimits limits)
    : m_historyLimits(limits)
{
    bind(document, assetId);
}

BitmapEditor::BitmapEditor(DocumentFile &file, const std::string &assetId,
                           RasterHistoryLimits limits)
    : m_historyLimits(limits)
{
    bind(file, assetId);
}

bool BitmapEditor::bind(DocumentFile &file, const std::string &assetId)
{
    if (!file.isOpen())
    {
        setError("no working file is open");
        return false;
    }
    if (!bind(*file.boundDocument(), assetId))
    {
        return false;
    }
    m_file = &file;
    m_fileGeneration = file.bindingGeneration();
    return true;
}

bool BitmapEditor::bind(Document &document, const std::string &assetIdValue)
{
    if (pixelPreviewActive() && isBound())
    {
        m_historyResult = {RasterHistoryCode::InvalidArgument};
        setError("resolve the pixel preview before changing its binding");
        return false;
    }

    if (isSemanticMaskAsset(document, assetIdValue))
    {
        setError("semantic identity masks require exact categorical edits through DocumentEditor");
        return false;
    }
    Asset *candidate = findAsset(document, assetIdValue);
    auto *raster = candidate ? std::get_if<RasterAsset>(candidate) : nullptr;
    if (!raster)
    {
        setError(candidate ? "asset is not raster content" : "raster asset was not found");
        return false;
    }
    if (!hasValidPixelStorage(raster->pixels))
    {
        setError("raster asset has invalid dimensions or pixel storage");
        return false;
    }

    m_document = &document;
    m_file = nullptr;
    m_fileGeneration = 0;
    m_assetId = assetIdValue;
    resetRasterDabStream(m_dabStream);
    m_journal = std::make_shared<detail::BitmapEditJournal>(m_historyLimits);
    m_historyResult = {};
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
    if (pixelPreviewActive() && isBound())
    {
        m_historyResult = {RasterHistoryCode::InvalidArgument};
        return;
    }

    m_document = nullptr;
    m_file = nullptr;
    m_fileGeneration = 0;
    m_assetId.clear();
    resetRasterDabStream(m_dabStream);
    m_journal.reset();
    m_historyResult = {};
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

const std::string &BitmapEditor::boundAssetId() const noexcept { return m_assetId; }

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
    if (!layer || x < 0 || y < 0 || x >= layer->width || y >= layer->height)
    {
        return std::nullopt;
    }
    return rasterLayerPixelAt(*layer, {x, y});
}

const BitmapBrush &BitmapEditor::brush() const noexcept { return m_brush; }

bool BitmapEditor::setBrush(const BitmapBrush &value)
{
    if (value.engineState)
    {
        const auto errors = detail::validateEngineState(*value.engineState);
        if (!errors.empty())
        {
            setError(errors.front());
            return false;
        }
    }
    if (m_strokeActive)
    {
        setError("brush settings cannot change during an active stroke");
        return false;
    }
    if (!std::isfinite(value.size) || value.size <= 0.0 || value.size > 4096.0 ||
        !inUnitRange(value.opacity) || !inUnitRange(value.flow) || !inUnitRange(value.hardness) ||
        !std::isfinite(value.spacing) || value.spacing < 0.0 || value.spacing > 4096.0 ||
        !std::isfinite(value.spacingRatio) || value.spacingRatio < 0.0 || value.spacingRatio > 4.0)
    {
        setError("brush values are outside the supported finite range");
        return false;
    }

    m_brush = value;
    clearError();
    return true;
}

bool BitmapEditor::setPixel(int x, int y, std::uint32_t argb)
{
    const BitmapPatch patch{{{x, y}, 1, 1}, std::span(&argb, 1)};
    return replacePatches(std::span(&patch, 1));
}
bool BitmapEditor::clear(std::uint32_t argb)
{
    if (m_file)
        return editFile([&](auto &editor) { return editor.clear(argb); });
    if (!requireBound() || m_strokeActive || pixelPreviewActive())
    {
        setError("clear requires an idle bound bitmap");
        return false;
    }
    auto &edits = journal();
    return finishEdit(edits.clear(*m_document, m_assetId, argb));
}
bool BitmapEditor::replacePatch(DevicePixelRect bounds, const std::vector<std::uint32_t> &pixels)
{
    const BitmapPatch patch{bounds, pixels};
    return replacePatches(std::span(&patch, 1));
}
bool BitmapEditor::replacePatches(std::span<const BitmapPatch> patches)
{
    if (m_file)
        return editFile([&](auto &editor) { return editor.replacePatches(patches); });
    if (!requireBound() || m_strokeActive || pixelPreviewActive())
    {
        m_historyResult = {RasterHistoryCode::InvalidArgument};
        setError("pixel patches require an idle bound bitmap");
        return false;
    }
    auto &edits = journal();
    return finishEdit(edits.replace(*m_document, m_assetId, patches));
}
bool BitmapEditor::replacePixels(const RasterLayer &pixels)
{
    if (m_file)
        return editFile([&](auto &editor) { return editor.replacePixels(pixels); });
    if (!requireBound() || m_strokeActive || pixelPreviewActive())
    {
        setError("replacement requires an idle bound bitmap");
        return false;
    }
    auto &edits = journal();
    return finishEdit(edits.replacePixels(*m_document, m_assetId, pixels));
}

bool BitmapEditor::beginStroke(DocumentPoint point, double pressure)
{
    return beginStrokeInput(detail::legacyStrokeSample(point, pressure, 1.0 / 120.0));
}

bool BitmapEditor::continueStroke(DocumentPoint point, double pressure)
{
    return continueStrokeInput(
        detail::legacyStrokeSample(point, pressure, m_strokeTime + 1.0 / 120.0));
}

bool BitmapEditor::endStroke(DocumentPoint point, double pressure)
{
    return endStrokeInput(detail::legacyStrokeSample(point, pressure, m_strokeTime + 1.0 / 120.0));
}

bool BitmapEditor::beginStrokeInput(const StrokePoint &sample)
{
    if (m_file)
        return editFile([&](auto &editor) { return editor.beginStrokeInput(sample); });
    if (!requireBound() || m_strokeActive || pixelPreviewActive() || !validInput(sample))
    {
        if (m_strokeActive || pixelPreviewActive())
            setError("an edit preview or brush stroke is already active");
        return false;
    }
    auto &edits = journal();
    edits.begin();
    const auto priorTime = m_strokeTime;
    resetRasterDabStream(m_dabStream);
    m_strokeActive = true;
    m_strokeChanged = false;
    m_strokeTime = 0;
    if (appendStrokePoint(sample, false))
        return true;
    const auto failure = m_historyResult;
    const auto message = m_lastError;
    (void)edits.cancel(*m_document, m_assetId);
    m_historyResult = failure;
    m_strokeActive = false;
    m_strokeTime = priorTime;
    resetRasterDabStream(m_dabStream);
    setError(message);
    return false;
}

bool BitmapEditor::continueStrokeInput(const StrokePoint &sample)
{
    if (m_file)
    {
        return editFile([&](BitmapEditor &editor) { return editor.continueStrokeInput(sample); });
    }
    if (!m_strokeActive)
    {
        setError("no brush stroke is active");
        return false;
    }
    if (!validInput(sample))
    {
        return false;
    }
    return appendStrokePoint(sample, false);
}

bool BitmapEditor::endStrokeInput(const StrokePoint &sample)
{
    if (m_file)
        return editFile([&](auto &editor) { return editor.endStrokeInput(sample); });
    if (!m_strokeActive)
    {
        setError("no brush stroke is active");
        return false;
    }
    if (!validInput(sample) || !appendStrokePoint(sample, true))
        return false;
    if (m_strokeChanged)
        ++m_nextStrokeSeed;
    auto &edits = journal();
    m_historyResult = edits.previewAdmission();
    edits.finish();
    m_strokeActive = false;
    m_strokeChanged = false;
    resetRasterDabStream(m_dabStream);
    return true;
}

bool BitmapEditor::advanceStrokeTime(double seconds)
{
    if (m_file)
        return editFile([&](BitmapEditor &editor) { return editor.advanceStrokeTime(seconds); });
    if (!m_strokeActive)
    {
        setError("no brush stroke is active");
        return false;
    }
    if (!requireBound())
        return false;
    const auto *raster = m_brush.engineState ? &m_brush.engineState->rasterizer : nullptr;
    if (!raster || (!raster->airbrushEnabled && !detail::clockCatchUp(*raster)))
    {
        setError("active brush has no time-based placement or catch-up");
        return false;
    }
    auto sample = m_dabStream.previousPoint;
    sample.time = seconds;
    if (detail::clockCatchUp(*raster))
        sample.position = m_dabStream.smoothingTarget;
    return validInput(sample) && appendStrokePoint(sample, false);
}

bool BitmapEditor::rebaseStrokeTime(double seconds)
{
    if (m_file)
        return editFile([&](BitmapEditor &editor) { return editor.rebaseStrokeTime(seconds); });
    if (!m_strokeActive)
    {
        setError("no brush stroke is active");
        return false;
    }
    if (!requireBound())
        return false;
    if (!detail::rebaseStrokeClock(m_dabStream, seconds))
    {
        setError("stroke clock must use finite monotonic seconds");
        return false;
    }
    m_strokeTime = seconds;
    clearError();
    return true;
}

void BitmapEditor::cancelStroke()
{
    if (m_file)
    {
        (void)editFile(
            [](auto &editor)
            {
                editor.cancelStroke();
                return !editor.strokeActive();
            });
        return;
    }
    if (!m_strokeActive)
        return;
    auto &edits = journal();
    if (!finishEdit(edits.cancel(*m_document, m_assetId)))
        return;
    resetRasterDabStream(m_dabStream);
    m_strokeActive = false;
    m_strokeChanged = false;
}

bool BitmapEditor::strokeActive() const noexcept { return m_strokeActive; }

bool BitmapEditor::canUndo() const noexcept
{
    return !m_strokeActive && !pixelPreviewActive() && m_journal && m_journal->canUndo() &&
           isBound();
}
bool BitmapEditor::canRedo() const noexcept
{
    return !m_strokeActive && !pixelPreviewActive() && m_journal && m_journal->canRedo() &&
           isBound();
}
bool BitmapEditor::undo()
{
    if (m_file)
        return editFile([](auto &editor) { return editor.undo(); });
    if (!canUndo())
    {
        setError("no bitmap edit is available to undo");
        return false;
    }
    auto &edits = journal();
    return finishEdit(edits.step(*m_document, m_assetId, false));
}
bool BitmapEditor::redo()
{
    if (m_file)
        return editFile([](auto &editor) { return editor.redo(); });
    if (!canRedo())
    {
        setError("no bitmap edit is available to redo");
        return false;
    }
    auto &edits = journal();
    return finishEdit(edits.step(*m_document, m_assetId, true));
}
RasterHistoryStatistics BitmapEditor::historyStatistics() const noexcept
{
    return m_journal ? m_journal->statistics() : RasterHistoryStatistics{};
}
std::size_t BitmapEditor::previewStorageBytes() const noexcept
{
    return m_journal ? m_journal->previewBytes() : 0;
}
bool BitmapEditor::pixelPreviewActive() const noexcept
{
    return m_journal && m_journal->pixelPreviewActive();
}
std::optional<std::uint32_t> BitmapEditor::pixelPreviewSourceAt(int x, int y) const noexcept
{
    const auto current = pixelAt(x, y);
    if (!current || !pixelPreviewActive())
        return current;
    return m_journal->sourcePixel(*m_document, m_assetId, {x, y});
}
bool BitmapEditor::beginPixelPreview(std::string_view label)
{
    if (m_file)
        return editFile([&](auto &editor) { return editor.beginPixelPreview(label); });
    if (!requireBound() || m_strokeActive || pixelPreviewActive())
    {
        m_historyResult = {RasterHistoryCode::InvalidArgument};
        setError("pixel preview requires an idle bound bitmap");
        return false;
    }
    auto &edits = journal();
    return finishEdit(edits.beginPixels(*m_document, m_assetId, label));
}
bool BitmapEditor::replacePixelPreview(std::span<const BitmapPatch> patches)
{
    if (m_file)
        return editFile([&](auto &editor) { return editor.replacePixelPreview(patches); });
    if (!requireBound() || !pixelPreviewActive())
    {
        m_historyResult = {RasterHistoryCode::InvalidArgument};
        setError("no pixel preview is active");
        return false;
    }
    auto &edits = journal();
    return finishEdit(edits.replacePixelPreview(*m_document, m_assetId, patches));
}
bool BitmapEditor::commitPixelPreview()
{
    if (m_file)
        return editFile([](auto &editor) { return editor.commitPixelPreview(); });
    if (!requireBound() || !pixelPreviewActive())
    {
        m_historyResult = {RasterHistoryCode::InvalidArgument};
        setError("no pixel preview is active");
        return false;
    }
    auto &edits = journal();
    return finishEdit(edits.commitPixels(*m_document, m_assetId));
}
bool BitmapEditor::cancelPixelPreview()
{
    if (m_file)
        return editFile([](auto &editor) { return editor.cancelPixelPreview(); });
    if (!requireBound() || !pixelPreviewActive())
    {
        m_historyResult = {RasterHistoryCode::InvalidArgument};
        setError("no pixel preview is active");
        return false;
    }
    auto &edits = journal();
    return finishEdit(edits.cancel(*m_document, m_assetId));
}
detail::BitmapEditJournal &BitmapEditor::journal()
{
    if (!m_journal)
        m_journal = std::make_shared<detail::BitmapEditJournal>(m_historyLimits);
    else if (m_journal.use_count() != 1)
        m_journal = std::make_shared<detail::BitmapEditJournal>(*m_journal);
    return *m_journal;
}
bool BitmapEditor::finishEdit(RasterHistoryResult result)
{
    m_historyResult = result;
    if (!result.ok())
    {
        setError(std::string(result.message()));
        return false;
    }
    if (m_journal && !isEmpty(m_journal->dirty))
        noteChange(m_journal->dirty);
    clearError();
    return true;
}

std::uint64_t BitmapEditor::revision() const noexcept { return m_revision; }

DevicePixelRect BitmapEditor::dirtyBounds() const noexcept { return m_dirtyBounds; }

void BitmapEditor::clearDirtyBounds() noexcept { m_dirtyBounds = {}; }

const std::string &BitmapEditor::lastError() const noexcept { return m_lastError; }

RasterAsset *BitmapEditor::rasterAsset() noexcept
{
    if (!m_document || m_assetId.empty() ||
        (m_file && (!m_file->isOpen() || m_fileGeneration != m_file->bindingGeneration())))
    {
        return nullptr;
    }
    if (isSemanticMaskAsset(*m_document, m_assetId))
    {
        return nullptr;
    }
    Asset *asset = findAsset(*m_document, m_assetId);
    return asset ? std::get_if<RasterAsset>(asset) : nullptr;
}

const RasterAsset *BitmapEditor::rasterAsset() const noexcept
{
    if (!m_document || m_assetId.empty() ||
        (m_file && (!m_file->isOpen() || m_fileGeneration != m_file->bindingGeneration())))
    {
        return nullptr;
    }
    if (isSemanticMaskAsset(*m_document, m_assetId))
    {
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
    if (!isBound())
    {
        setError("no valid raster asset is bound");
        return false;
    }
    return true;
}

bool BitmapEditor::validPoint(DocumentPoint point, double pressure)
{
    if (!isFinitePoint(point) || !inUnitRange(pressure))
    {
        setError(
            "stroke position and pressure must be finite and pressure must be within zero and one");
        return false;
    }
    return true;
}

bool BitmapEditor::validInput(const StrokePoint &sample)
{
    if (!validPoint(sample.position, sample.pressure))
        return false;
    if (!detail::validStrokeSample(sample) || (m_strokeActive && sample.time < m_strokeTime))
    {
        setError("stroke sample must be finite, normalized and use monotonic seconds");
        return false;
    }
    return true;
}

BrushState BitmapEditor::brushState() const
{
    Rasterizer rasterizer = m_brush.engineState ? m_brush.engineState->rasterizer : Rasterizer{};
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
    rasterizer.blendMode = m_brush.eraser        ? RasterBlendMode::DestinationOut
                           : m_brush.engineState ? m_brush.engineState->rasterizer.blendMode
                                                 : RasterBlendMode::SourceOver;

    BrushDynamics dynamics;
    dynamics.pressureToSize = 1.0;
    dynamics.pressureToFlow = 1.0;
    dynamics.pressureToOpacity = 1.0;
    dynamics.pressureToOpacityEnabled = m_brush.pressureToOpacityEnabled;
    return {std::move(rasterizer),
            m_brush.engineState ? m_brush.engineState->dynamics : std::move(dynamics),
            m_brush.engineState ? m_brush.engineState->material : BrushMaterial{},
            m_nextStrokeSeed};
}

DevicePixelRect BitmapEditor::bitmapBounds() const noexcept
{
    const RasterLayer *layer = pixels();
    return layer ? DevicePixelRect{{0, 0}, layer->width, layer->height} : DevicePixelRect{};
}

void BitmapEditor::noteChange(DevicePixelRect bounds)
{
    if (isEmpty(bounds))
    {
        return;
    }
    m_dirtyBounds = uniteDevicePixelRects(m_dirtyBounds, bounds);
    ++m_revision;
}

bool BitmapEditor::appendStrokePoint(const StrokePoint &sample, bool finish)
{
    if (!requireBound())
        return false;
    const BrushState state = brushState();
    auto nextStream = m_dabStream;
    auto &edits = journal();
    const auto bounds = bitmapBounds();
    const detail::BitmapEditSource source{edits, *m_document, m_assetId, CanvasOrigin{}};
    const RasterSourceSampler sampler{&source,
                                      &detail::sampleBitmapEditSource,
                                      {source.origin.x, source.origin.y},
                                      bounds.width,
                                      bounds.height};
    try
    {
        auto dabs = appendRasterDabs(nextStream, sample, state, finish);
        auto samples =
            projectBrushDabs(dabs, state.rasterizer, RasterProjection{}, sampler, state.material);
        std::erase_if(samples,
                      [&](const auto &value)
                      {
                          return value.position.x < bounds.origin.x ||
                                 value.position.y < bounds.origin.y ||
                                 std::int64_t(value.position.x) >=
                                     std::int64_t(bounds.origin.x) + bounds.width ||
                                 std::int64_t(value.position.y) >=
                                     std::int64_t(bounds.origin.y) + bounds.height;
                      });
        if (!samples.empty() &&
            !finishEdit(edits.preview(*m_document, m_assetId, samples, state.rasterizer.blendMode)))
            return false;
        m_dabStream = std::move(nextStream);
        m_strokeTime = sample.time;
        m_strokeChanged = edits.strokeChanged();
    }
    catch (const std::bad_alloc &)
    {
        m_historyResult = {RasterHistoryCode::BudgetExceeded};
        setError(std::string(m_historyResult.message()));
        return false;
    }
    catch (const std::logic_error &error)
    {
        setError(error.what());
        return false;
    }
    clearError();
    return true;
}

void BitmapEditor::setError(std::string message) { m_lastError = std::move(message); }

bool BitmapEditor::editFile(const std::function<bool(BitmapEditor &)> &edit)
{
#ifdef IISHAREDCANVAS_NATIVE_API
    static_assert(std::is_nothrow_move_assignable_v<BitmapEditor>);
    if (!m_file->isOpen() || m_fileGeneration != m_file->bindingGeneration())
    {
        m_historyResult = {RasterHistoryCode::Conflict};
        setError("the working-file binding is no longer valid");
        return false;
    }
    DocumentFile *file = m_file;
    if (!file->beginBitmapEdit(m_fileGeneration))
    {
        const auto code = file->lastResult().code;
        m_historyResult = {code == DocumentFileCode::Conflict ? RasterHistoryCode::Conflict :
                            code == DocumentFileCode::LimitExceeded ? RasterHistoryCode::BudgetExceeded :
                            RasterHistoryCode::WriteRejected};
        try { setError(file->lastResult().message); } catch (...) { clearError(); }
        return false;
    }
    bool accepted = false, fallback = false;
    {
        auto finish = [&](DocumentFile *owner) noexcept { owner->endBitmapEdit(accepted); };
        std::unique_ptr<DocumentFile, decltype(finish)> guard(file, finish);
        try
        {
            BitmapEditor working = *this;
            working.m_file = nullptr;
            working.clearError();
            auto &edits = working.journal(); // Detach COW state before exposing the port.
            detail::BitmapCommitPort port{file,
                [](void *context, const detail::PreparedBitmapChange &change,
                   RasterHistoryCode &rejection, bool &useDraftFallback)
                {
                    auto *owner = static_cast<DocumentFile *>(context);
                    if (owner->commitBitmapEdit(change, useDraftFallback)) return true;
                    if (!useDraftFallback)
                    {
                        const auto code = owner->lastResult().code;
                        rejection = code == DocumentFileCode::Conflict ? RasterHistoryCode::Conflict :
                                    code == DocumentFileCode::LimitExceeded ? RasterHistoryCode::BudgetExceeded :
                                    RasterHistoryCode::WriteRejected;
                    }
                    return false;
                }};
            edits.setCommitPort(&port);
            bool ok = false;
            try { ok = edit(working); }
            catch (...) { edits.setCommitPort(nullptr); throw; }
            edits.setCommitPort(nullptr);
            fallback = port.useDraftFallback;
            if (ok)
            {
                working.m_file = file;
                *this = std::move(working); // Statically checked noexcept after durable commit.
                accepted = true;
            }
            else if (!fallback)
            {
                m_historyResult = working.m_historyResult.ok()
                                      ? RasterHistoryResult{RasterHistoryCode::WriteRejected}
                                      : working.m_historyResult;
                try { setError(working.lastError().empty() ? file->lastResult().message : working.lastError()); }
                catch (...) { clearError(); }
            }
        }
        catch (const std::bad_alloc &)
        {
            m_historyResult = {RasterHistoryCode::BudgetExceeded};
            clearError();
        }
        catch (const std::exception &error)
        {
            m_historyResult = {RasterHistoryCode::WriteRejected};
            try { setError(error.what()); } catch (...) { clearError(); }
        }
        catch (...)
        {
            m_historyResult = {RasterHistoryCode::WriteRejected};
            clearError();
        }
    }
    if (fallback) return editFileDraft(edit);
    return accepted;
#else
    return editFileDraft(edit);
#endif
}

bool BitmapEditor::editFileDraft(const std::function<bool(BitmapEditor &)> &edit)
{
    if (!m_file->isOpen() || m_fileGeneration != m_file->bindingGeneration())
    {
        m_historyResult = {RasterHistoryCode::Conflict};
        setError("the working-file binding is no longer valid");
        return false;
    }
    BitmapEditor working = *this;
    working.m_file = nullptr;
    working.clearError();
    const auto result = m_file->edit(
        [&](Document &draft)
        {
            working.m_document = &draft;
            return edit(working);
        });
    if (!result.ok())
    {
        m_historyResult = working.m_historyResult.ok()
                              ? RasterHistoryResult{RasterHistoryCode::WriteRejected}
                              : working.m_historyResult;
        setError(working.lastError().empty() ? result.message : working.lastError());
        return false;
    }
    working.m_document = m_document;
    working.m_file = m_file;
    *this = std::move(working);
    return true;
}

void BitmapEditor::clearError() noexcept { m_lastError.clear(); }

} // namespace iiSharedCanvas
