#include "Vector/VectorEditor.h"
#include "File/DocumentFile.h"

#include "Validation/Validation.h"

#include <type_traits>
#include <utility>

namespace iiSharedCanvas {
namespace {

bool samePoint(Point left, Point right) noexcept
{
    return left.x == right.x && left.y == right.y;
}

bool sameCommand(const PathCommand &left, const PathCommand &right) noexcept
{
    if (left.index() != right.index()) {
        return false;
    }

    switch (left.index()) {
    case 0:
        return samePoint(std::get<MoveTo>(left).point,
                         std::get<MoveTo>(right).point);
    case 1:
        return samePoint(std::get<LineTo>(left).point,
                         std::get<LineTo>(right).point);
    case 2: {
        const QuadraticTo &leftCurve = std::get<QuadraticTo>(left);
        const QuadraticTo &rightCurve = std::get<QuadraticTo>(right);
        return samePoint(leftCurve.control, rightCurve.control)
            && samePoint(leftCurve.end, rightCurve.end);
    }
    case 3: {
        const CubicTo &leftCurve = std::get<CubicTo>(left);
        const CubicTo &rightCurve = std::get<CubicTo>(right);
        return samePoint(leftCurve.control1, rightCurve.control1)
            && samePoint(leftCurve.control2, rightCurve.control2)
            && samePoint(leftCurve.end, rightCurve.end);
    }
    case 4:
        return true;
    default:
        return false;
    }
}

bool samePaint(const std::optional<SolidPaint> &left,
               const std::optional<SolidPaint> &right) noexcept
{
    return left.has_value() == right.has_value()
        && (!left || left->argb == right->argb);
}

bool sameStroke(const std::optional<StrokeStyle> &left,
                const std::optional<StrokeStyle> &right) noexcept
{
    return left.has_value() == right.has_value()
        && (!left
            || (left->paint.argb == right->paint.argb
                && left->width == right->width));
}

std::string commandPath(std::size_t pathIndex, std::size_t commandIndex)
{
    return "asset.paths[" + std::to_string(pathIndex) + "].commands["
        + std::to_string(commandIndex) + "]";
}

} // namespace

VectorEditor::VectorEditor(Document &document, const std::string &assetId)
{
    bind(document, assetId);
}

VectorEditor::VectorEditor(DocumentFile &file, const std::string &assetId)
{
    bind(file, assetId);
}

DocumentEditResult VectorEditor::bind(DocumentFile &file, const std::string &assetId)
{
    unbind();
    const auto result = m_documentEditor.bind(file);
    if (!result.ok()) {
        return record(result);
    }
    const Asset *candidate = findAsset(*file.document(), assetId);
    if (!candidate || !std::holds_alternative<VectorAsset>(*candidate)) {
        m_documentEditor.unbind();
        return reject(candidate ? DocumentEditCode::AssetKindMismatch : DocumentEditCode::AssetNotFound,
                      "assets", "a vector asset is required");
    }
    m_assetId = assetId;
    return unchanged();
}

DocumentEditResult VectorEditor::bind(Document &document,
                                      const std::string &assetIdValue)
{
    unbind();
    const DocumentEditResult documentBinding = m_documentEditor.bind(document);
    if (!documentBinding.ok()) {
        return record(documentBinding);
    }

    Asset *candidate = findAsset(document, assetIdValue);
    if (!candidate) {
        m_documentEditor.unbind();
        return reject(DocumentEditCode::AssetNotFound,
                      "assets",
                      "vector asset was not found");
    }
    if (!std::get_if<VectorAsset>(candidate)) {
        m_documentEditor.unbind();
        return reject(DocumentEditCode::AssetKindMismatch,
                      "assets",
                      "asset is not vector content");
    }

    m_assetId = assetIdValue;
    return unchanged();
}

void VectorEditor::unbind() noexcept
{
    m_documentEditor.unbind();
    m_assetId.clear();
    m_lastResult = {};
}

bool VectorEditor::isBound() const noexcept
{
    return m_documentEditor.isBound() && asset() != nullptr;
}

const std::string &VectorEditor::boundAssetId() const noexcept
{
    return m_assetId;
}

const VectorAsset *VectorEditor::asset() const noexcept
{
    const Document *document = m_documentEditor.document();
    return document ? findVectorAsset(*document, m_assetId) : nullptr;
}

std::size_t VectorEditor::pathCount() const noexcept
{
    const VectorAsset *vector = asset();
    return vector ? vector->paths.size() : 0;
}

const VectorPath *VectorEditor::path(std::size_t pathIndex) const noexcept
{
    const VectorAsset *vector = asset();
    return vector && pathIndex < vector->paths.size()
        ? &vector->paths[pathIndex]
        : nullptr;
}

std::uint64_t VectorEditor::revision() const noexcept
{
    return m_documentEditor.revision();
}

const DocumentEditResult &VectorEditor::lastResult() const noexcept
{
    return m_lastResult;
}

DocumentEditResult VectorEditor::setViewport(CanvasExtent viewport)
{
    const VectorAsset *vector = validatedAsset();
    if (!vector) {
        return m_lastResult;
    }
    if (vector->viewport.width == viewport.width
        && vector->viewport.height == viewport.height) {
        return unchanged();
    }
    return record(m_documentEditor.replaceVectorData(
        m_assetId, viewport, vector->paths));
}

DocumentEditResult VectorEditor::createPath(
    Point start,
    std::optional<SolidPaint> fill,
    std::optional<StrokeStyle> stroke,
    std::size_t index)
{
    VectorPath pathValue;
    pathValue.commands.emplace_back(MoveTo{start});
    pathValue.fill = std::move(fill);
    pathValue.stroke = std::move(stroke);
    return insertPath(std::move(pathValue), index);
}

DocumentEditResult VectorEditor::insertPath(VectorPath pathValue,
                                            std::size_t index)
{
    if (!m_documentEditor.isBound()) {
        return reject(DocumentEditCode::NotBound,
                      "assets",
                      "vector editor is not bound");
    }
    return record(m_documentEditor.insertVectorPath(
        m_assetId, std::move(pathValue), index));
}

DocumentEditResult VectorEditor::movePath(std::size_t pathIndex,
                                          std::size_t destinationIndex)
{
    if (!m_documentEditor.isBound()) {
        return reject(DocumentEditCode::NotBound,
                      "assets",
                      "vector editor is not bound");
    }
    return record(m_documentEditor.moveVectorPath(
        m_assetId, pathIndex, destinationIndex));
}

DocumentEditResult VectorEditor::removePath(std::size_t pathIndex)
{
    if (!m_documentEditor.isBound()) {
        return reject(DocumentEditCode::NotBound,
                      "assets",
                      "vector editor is not bound");
    }
    return record(m_documentEditor.removeVectorPath(m_assetId, pathIndex));
}

DocumentEditResult VectorEditor::insertCommand(std::size_t pathIndex,
                                               std::size_t commandIndex,
                                               PathCommand command)
{
    VectorPath pathValue;
    if (!copyPath(pathIndex, pathValue)) {
        return m_lastResult;
    }
    const std::size_t position = commandIndex == AppendDocumentIndex
        ? pathValue.commands.size()
        : commandIndex;
    if (position > pathValue.commands.size()) {
        return reject(DocumentEditCode::IndexOutOfRange,
                      "asset.paths[" + std::to_string(pathIndex) + "].commands",
                      "command insertion index is outside the path");
    }
    pathValue.commands.insert(
        pathValue.commands.begin() + static_cast<std::ptrdiff_t>(position),
        std::move(command));
    return replacePath(pathIndex, std::move(pathValue));
}

DocumentEditResult VectorEditor::replaceCommand(std::size_t pathIndex,
                                                std::size_t commandIndex,
                                                PathCommand command)
{
    VectorPath pathValue;
    if (!copyPath(pathIndex, pathValue)) {
        return m_lastResult;
    }
    if (commandIndex >= pathValue.commands.size()) {
        return reject(DocumentEditCode::IndexOutOfRange,
                      "asset.paths[" + std::to_string(pathIndex) + "].commands",
                      "command index is outside the path");
    }
    if (sameCommand(pathValue.commands[commandIndex], command)) {
        return unchanged();
    }
    pathValue.commands[commandIndex] = std::move(command);
    return replacePath(pathIndex, std::move(pathValue));
}

DocumentEditResult VectorEditor::removeCommand(std::size_t pathIndex,
                                               std::size_t commandIndex)
{
    VectorPath pathValue;
    if (!copyPath(pathIndex, pathValue)) {
        return m_lastResult;
    }
    if (commandIndex >= pathValue.commands.size()) {
        return reject(DocumentEditCode::IndexOutOfRange,
                      "asset.paths[" + std::to_string(pathIndex) + "].commands",
                      "command index is outside the path");
    }
    pathValue.commands.erase(
        pathValue.commands.begin() + static_cast<std::ptrdiff_t>(commandIndex));
    return replacePath(pathIndex, std::move(pathValue));
}

DocumentEditResult VectorEditor::appendMoveTo(std::size_t pathIndex, Point point)
{
    return insertCommand(pathIndex, AppendDocumentIndex, MoveTo{point});
}

DocumentEditResult VectorEditor::appendLineTo(std::size_t pathIndex, Point end)
{
    return insertCommand(pathIndex, AppendDocumentIndex, LineTo{end});
}

DocumentEditResult VectorEditor::appendQuadraticBezierTo(std::size_t pathIndex,
                                                         Point control,
                                                         Point end)
{
    return insertCommand(
        pathIndex, AppendDocumentIndex, QuadraticTo{control, end});
}

DocumentEditResult VectorEditor::appendCubicBezierTo(std::size_t pathIndex,
                                                     Point control1,
                                                     Point control2,
                                                     Point end)
{
    return insertCommand(
        pathIndex, AppendDocumentIndex, CubicTo{control1, control2, end});
}

DocumentEditResult VectorEditor::closePath(std::size_t pathIndex)
{
    VectorPath pathValue;
    if (!copyPath(pathIndex, pathValue)) {
        return m_lastResult;
    }
    if (!pathValue.commands.empty()
        && std::holds_alternative<ClosePath>(pathValue.commands.back())) {
        return unchanged();
    }
    pathValue.commands.emplace_back(ClosePath{});
    return replacePath(pathIndex, std::move(pathValue));
}

DocumentEditResult VectorEditor::openPath(std::size_t pathIndex)
{
    VectorPath pathValue;
    if (!copyPath(pathIndex, pathValue)) {
        return m_lastResult;
    }
    if (pathValue.commands.empty()
        || !std::holds_alternative<ClosePath>(pathValue.commands.back())) {
        return unchanged();
    }
    pathValue.commands.pop_back();
    return replacePath(pathIndex, std::move(pathValue));
}

DocumentEditResult VectorEditor::setAnchorPoint(std::size_t pathIndex,
                                                std::size_t commandIndex,
                                                Point pointValue)
{
    VectorPath pathValue;
    if (!copyPath(pathIndex, pathValue)) {
        return m_lastResult;
    }
    if (commandIndex >= pathValue.commands.size()) {
        return reject(DocumentEditCode::IndexOutOfRange,
                      "asset.paths[" + std::to_string(pathIndex) + "].commands",
                      "command index is outside the path");
    }

    PathCommand &command = pathValue.commands[commandIndex];
    Point *anchor = std::visit([](auto &value) -> Point * {
        using Command = std::decay_t<decltype(value)>;
        if constexpr (std::is_same_v<Command, MoveTo>
                      || std::is_same_v<Command, LineTo>) {
            return &value.point;
        } else if constexpr (std::is_same_v<Command, QuadraticTo>
                             || std::is_same_v<Command, CubicTo>) {
            return &value.end;
        } else {
            return nullptr;
        }
    }, command);
    if (!anchor) {
        return reject(DocumentEditCode::InvalidArgument,
                      commandPath(pathIndex, commandIndex),
                      "ClosePath has no editable anchor point");
    }
    if (samePoint(*anchor, pointValue)) {
        return unchanged();
    }
    *anchor = pointValue;
    return replacePath(pathIndex, std::move(pathValue));
}

DocumentEditResult VectorEditor::setControlPoint(std::size_t pathIndex,
                                                 std::size_t commandIndex,
                                                 std::size_t controlIndex,
                                                 Point pointValue)
{
    VectorPath pathValue;
    if (!copyPath(pathIndex, pathValue)) {
        return m_lastResult;
    }
    if (commandIndex >= pathValue.commands.size()) {
        return reject(DocumentEditCode::IndexOutOfRange,
                      "asset.paths[" + std::to_string(pathIndex) + "].commands",
                      "command index is outside the path");
    }

    PathCommand &command = pathValue.commands[commandIndex];
    Point *control = nullptr;
    if (auto *quadratic = std::get_if<QuadraticTo>(&command)) {
        if (controlIndex == 0) {
            control = &quadratic->control;
        }
    } else if (auto *cubic = std::get_if<CubicTo>(&command)) {
        if (controlIndex == 0) {
            control = &cubic->control1;
        } else if (controlIndex == 1) {
            control = &cubic->control2;
        }
    }
    if (!control) {
        return reject(DocumentEditCode::InvalidArgument,
                      commandPath(pathIndex, commandIndex),
                      "control index is not available on this path command");
    }
    if (samePoint(*control, pointValue)) {
        return unchanged();
    }
    *control = pointValue;
    return replacePath(pathIndex, std::move(pathValue));
}

DocumentEditResult VectorEditor::setPathPaint(
    std::size_t pathIndex,
    std::optional<SolidPaint> fill,
    std::optional<StrokeStyle> stroke)
{
    VectorPath pathValue;
    if (!copyPath(pathIndex, pathValue)) {
        return m_lastResult;
    }
    if (samePaint(pathValue.fill, fill)
        && sameStroke(pathValue.stroke, stroke)) {
        return unchanged();
    }
    pathValue.fill = std::move(fill);
    pathValue.stroke = std::move(stroke);
    return replacePath(pathIndex, std::move(pathValue));
}

const VectorAsset *VectorEditor::validatedAsset()
{
    const Document *document = std::as_const(m_documentEditor).document();
    if (!document) {
        (void)reject(DocumentEditCode::NotBound,
                     "assets",
                     "vector editor is not bound");
        return nullptr;
    }

    const ValidationResult validation = validate(*document);
    if (!validation.ok()) {
        const ValidationIssue &issue = validation.issues.front();
        (void)reject(DocumentEditCode::InvalidDocument,
                     issue.path,
                     issue.message);
        return nullptr;
    }

    const Asset *candidate = findAsset(*document, m_assetId);
    if (!candidate) {
        (void)reject(DocumentEditCode::AssetNotFound,
                     "assets",
                     "vector asset was not found");
        return nullptr;
    }
    const VectorAsset *vector = std::get_if<VectorAsset>(candidate);
    if (!vector) {
        (void)reject(DocumentEditCode::AssetKindMismatch,
                     "assets",
                     "asset is not vector content");
    }
    return vector;
}

bool VectorEditor::copyPath(std::size_t pathIndex, VectorPath &pathValue)
{
    const VectorAsset *vector = validatedAsset();
    if (!vector) {
        return false;
    }
    if (pathIndex >= vector->paths.size()) {
        (void)reject(DocumentEditCode::IndexOutOfRange,
                     "asset.paths",
                     "path index is outside the collection");
        return false;
    }
    pathValue = vector->paths[pathIndex];
    return true;
}

DocumentEditResult VectorEditor::replacePath(std::size_t pathIndex,
                                             VectorPath pathValue)
{
    return record(m_documentEditor.replaceVectorPath(
        m_assetId, pathIndex, std::move(pathValue)));
}

DocumentEditResult VectorEditor::record(DocumentEditResult result)
{
    m_lastResult = std::move(result);
    return m_lastResult;
}

DocumentEditResult VectorEditor::reject(DocumentEditCode code,
                                        std::string pathValue,
                                        std::string message)
{
    m_lastResult = {code, false, std::move(pathValue), std::move(message)};
    return m_lastResult;
}

DocumentEditResult VectorEditor::unchanged()
{
    m_lastResult = {};
    return m_lastResult;
}

} // namespace iiSharedCanvas
