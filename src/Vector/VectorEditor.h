#pragma once

#include "Document/DocumentEditor.h"
#include "iiSharedCanvas/Export.h"

#include <cstddef>
#include <cstdint>
#include <optional>
#include <string>

namespace iiSharedCanvas {

class IISHAREDCANVAS_EXPORT VectorEditor final {
public:
    VectorEditor() = default;
    VectorEditor(Document &document, const std::string &assetId);
    VectorEditor(DocumentFile &file, const std::string &assetId);

    DocumentEditResult bind(Document &document, const std::string &assetId);
    DocumentEditResult bind(DocumentFile &file, const std::string &assetId);
    void unbind() noexcept;

    [[nodiscard]] bool isBound() const noexcept;
    [[nodiscard]] const std::string &boundAssetId() const noexcept;
    [[nodiscard]] const VectorAsset *asset() const noexcept;
    [[nodiscard]] std::size_t pathCount() const noexcept;
    [[nodiscard]] const VectorPath *path(std::size_t pathIndex) const noexcept;
    [[nodiscard]] std::uint64_t revision() const noexcept;
    [[nodiscard]] const DocumentEditResult &lastResult() const noexcept;

    DocumentEditResult setViewport(CanvasExtent viewport);
    DocumentEditResult createPath(
        Point start,
        std::optional<SolidPaint> fill,
        std::optional<StrokeStyle> stroke,
        std::size_t index = AppendDocumentIndex);
    DocumentEditResult insertPath(
        VectorPath path,
        std::size_t index = AppendDocumentIndex);
    DocumentEditResult movePath(std::size_t pathIndex,
                                std::size_t destinationIndex);
    DocumentEditResult removePath(std::size_t pathIndex);

    DocumentEditResult insertCommand(std::size_t pathIndex,
                                     std::size_t commandIndex,
                                     PathCommand command);
    DocumentEditResult replaceCommand(std::size_t pathIndex,
                                      std::size_t commandIndex,
                                      PathCommand command);
    DocumentEditResult removeCommand(std::size_t pathIndex,
                                     std::size_t commandIndex);

    DocumentEditResult appendMoveTo(std::size_t pathIndex, Point point);
    DocumentEditResult appendLineTo(std::size_t pathIndex, Point end);
    DocumentEditResult appendQuadraticBezierTo(std::size_t pathIndex,
                                               Point control,
                                               Point end);
    DocumentEditResult appendCubicBezierTo(std::size_t pathIndex,
                                           Point control1,
                                           Point control2,
                                           Point end);
    DocumentEditResult closePath(std::size_t pathIndex);
    DocumentEditResult openPath(std::size_t pathIndex);

    DocumentEditResult setAnchorPoint(std::size_t pathIndex,
                                      std::size_t commandIndex,
                                      Point point);
    DocumentEditResult setControlPoint(std::size_t pathIndex,
                                       std::size_t commandIndex,
                                       std::size_t controlIndex,
                                       Point point);
    DocumentEditResult setPathPaint(std::size_t pathIndex,
                                    std::optional<SolidPaint> fill,
                                    std::optional<StrokeStyle> stroke);

private:
    [[nodiscard]] const VectorAsset *validatedAsset();
    [[nodiscard]] bool copyPath(std::size_t pathIndex, VectorPath &path);
    [[nodiscard]] DocumentEditResult replacePath(std::size_t pathIndex,
                                                 VectorPath path);
    [[nodiscard]] DocumentEditResult record(DocumentEditResult result);
    [[nodiscard]] DocumentEditResult reject(DocumentEditCode code,
                                            std::string path,
                                            std::string message);
    [[nodiscard]] DocumentEditResult unchanged();

    DocumentEditor m_documentEditor;
    std::string m_assetId;
    DocumentEditResult m_lastResult;
};

} // namespace iiSharedCanvas
