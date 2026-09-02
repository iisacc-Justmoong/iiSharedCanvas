#include <iiSharedCanvas.h>

#include <cstdint>
#include <iostream>
#include <limits>
#include <optional>
#include <string>
#include <variant>

namespace {

int failures = 0;

void expect(bool condition, const std::string &message)
{
    if (!condition) {
        std::cerr << message << '\n';
        ++failures;
    }
}

iiSharedCanvas::VectorPath initialPath()
{
    using namespace iiSharedCanvas;
    VectorPath path;
    path.commands = {
        MoveTo{{2.0, 3.0}},
        LineTo{{12.0, 3.0}},
    };
    path.stroke = StrokeStyle{SolidPaint{0xff102030U}, 2.0};
    return path;
}

iiSharedCanvas::Document makeDocument()
{
    using namespace iiSharedCanvas;
    Document document;
    document.extent = {64, 48};
    document.timeline = {{24, 1}, 1};
    document.assets.emplace_back(
        RasterAsset{"pixels", makeRasterLayer(4, 4, 0x00000000U)});
    document.assets.emplace_back(
        VectorAsset{"shape", {64, 48}, {initialPath()}});
    document.layers.emplace_back(VectorLayer{
        {"shape-layer", "Shape", true, 1.0, {}, RasterBlendMode::SourceOver},
        StaticSource{"shape"},
    });
    return document;
}

} // namespace

int main()
{
    using namespace iiSharedCanvas;

    VectorEditor unbound;
    const DocumentEditResult unboundEdit = unbound.appendLineTo(0, {1.0, 1.0});
    expect(!unboundEdit.ok()
               && unboundEdit.code == DocumentEditCode::NotBound
               && !unboundEdit.changed,
           "an unbound vector editor must reject geometry edits");

    Document document = makeDocument();
    VectorEditor editor(document, "shape");
    expect(editor.isBound()
               && editor.boundAssetId() == "shape"
               && editor.asset() == findVectorAsset(document, "shape")
               && editor.pathCount() == 1
               && editor.path(0) != nullptr,
           "the vector editor must bind to one vector asset by stable id");

    VectorEditor wrongKind;
    const DocumentEditResult wrongKindBind = wrongKind.bind(document, "pixels");
    expect(!wrongKindBind.ok()
               && wrongKindBind.code == DocumentEditCode::AssetKindMismatch
               && !wrongKind.isBound(),
           "binding must reject a raster asset without retaining a stale binding");

    Document invalidDocument = makeDocument();
    invalidDocument.extent.width = 0;
    VectorEditor invalidBinding;
    const DocumentEditResult invalidBind = invalidBinding.bind(invalidDocument, "shape");
    expect(!invalidBind.ok()
               && invalidBind.code == DocumentEditCode::InvalidDocument
               && !invalidBinding.isBound(),
           "binding must fail closed when the complete document is invalid");

    const std::uint64_t beforeCreate = editor.revision();
    const DocumentEditResult created = editor.createPath(
        {4.0, 5.0},
        std::nullopt,
        StrokeStyle{SolidPaint{0xffaabbccU}, 1.5});
    expect(created.ok() && created.changed
               && editor.revision() == beforeCreate + 1
               && editor.pathCount() == 2
               && editor.path(1)->commands.size() == 1
               && std::get<MoveTo>(editor.path(1)->commands.front()).point.x == 4.0,
           "createPath must atomically create a styled path beginning with MoveTo");

    expect(editor.appendLineTo(1, {18.0, 5.0}).changed,
           "linear segments must be appendable");
    expect(editor.appendQuadraticBezierTo(1, {22.0, 9.0}, {18.0, 14.0}).changed,
           "quadratic Bezier segments must be appendable");
    expect(editor.appendCubicBezierTo(
               1, {14.0, 18.0}, {8.0, 18.0}, {4.0, 14.0}).changed,
           "cubic Bezier segments must be appendable");

    const VectorPath *mixedPath = editor.path(1);
    expect(mixedPath
               && mixedPath->commands.size() == 4
               && std::holds_alternative<LineTo>(mixedPath->commands[1])
               && std::holds_alternative<QuadraticTo>(mixedPath->commands[2])
               && std::holds_alternative<CubicTo>(mixedPath->commands[3]),
           "linear, quadratic, and cubic commands must coexist in native path order");

    expect(editor.setAnchorPoint(1, 1, {19.0, 6.0}).changed
               && std::get<LineTo>(editor.path(1)->commands[1]).point.y == 6.0,
           "linear endpoints must be editable without changing command type");
    expect(editor.setAnchorPoint(1, 3, {3.0, 13.0}).changed
               && std::get<CubicTo>(editor.path(1)->commands[3]).end.x == 3.0,
           "anchor editing must update the terminal point of a Bezier segment");
    expect(editor.setControlPoint(1, 2, 0, {21.0, 10.0}).changed
               && std::get<QuadraticTo>(editor.path(1)->commands[2]).control.y == 10.0,
           "quadratic Bezier control points must be editable");
    expect(editor.setControlPoint(1, 3, 1, {7.0, 17.0}).changed
               && std::get<CubicTo>(editor.path(1)->commands[3]).control2.x == 7.0,
           "both cubic Bezier control points must be addressable");

    const std::uint64_t beforeInvalidControl = editor.revision();
    const DocumentEditResult invalidControl = editor.setControlPoint(
        1, 1, 0, {0.0, 0.0});
    expect(!invalidControl.ok()
               && invalidControl.code == DocumentEditCode::InvalidArgument
               && editor.revision() == beforeInvalidControl,
           "a linear command must reject control-point editing without mutation");

    expect(editor.insertCommand(1, 1, LineTo{{9.0, 6.0}}).changed
               && editor.path(1)->commands.size() == 5,
           "commands must be insertable at an explicit native path index");
    expect(editor.replaceCommand(
               1, 1, QuadraticTo{{8.0, 7.0}, {9.0, 6.0}}).changed
               && std::holds_alternative<QuadraticTo>(editor.path(1)->commands[1]),
           "one path command must be replaceable without rebuilding the asset");
    expect(editor.removeCommand(1, 1).changed
               && editor.path(1)->commands.size() == 4,
           "one non-required path command must be removable");

    expect(editor.closePath(1).changed
               && std::holds_alternative<ClosePath>(editor.path(1)->commands.back()),
           "a path must support explicit close commands");
    const std::uint64_t beforeDuplicateClose = editor.revision();
    expect(editor.closePath(1).ok()
               && !editor.lastResult().changed
               && editor.revision() == beforeDuplicateClose,
           "closing an already closed path must be an idempotent no-op");
    expect(editor.openPath(1).changed
               && !std::holds_alternative<ClosePath>(editor.path(1)->commands.back()),
           "the trailing close command must be removable through openPath");

    expect(editor.setPathPaint(
               1,
               SolidPaint{0xff55aa77U},
               StrokeStyle{SolidPaint{0xff001122U}, 3.25}).changed
               && editor.path(1)->fill->argb == 0xff55aa77U
               && editor.path(1)->stroke->width == 3.25,
           "fill and stroke must be editable as one validated path paint operation");

    const std::uint64_t beforeInvalidPaint = editor.revision();
    const DocumentEditResult invalidPaint = editor.setPathPaint(
        1, std::nullopt, std::nullopt);
    expect(!invalidPaint.ok()
               && invalidPaint.code == DocumentEditCode::ValidationRejected
               && editor.path(1)->fill
               && editor.path(1)->stroke
               && editor.revision() == beforeInvalidPaint,
           "removing every paint must roll back the path and preserve revision");

    const std::size_t beforeRejectedCommandCount = editor.path(1)->commands.size();
    const std::uint64_t beforeInvalidCoordinate = editor.revision();
    const DocumentEditResult invalidCoordinate = editor.appendLineTo(
        1, {std::numeric_limits<double>::quiet_NaN(), 8.0});
    expect(!invalidCoordinate.ok()
               && invalidCoordinate.code == DocumentEditCode::ValidationRejected
               && editor.path(1)->commands.size() == beforeRejectedCommandCount
               && editor.revision() == beforeInvalidCoordinate,
           "non-finite linear geometry must be rejected atomically");

    const std::uint64_t beforeRemovingMove = editor.revision();
    const DocumentEditResult removedRequiredMove = editor.removeCommand(1, 0);
    expect(!removedRequiredMove.ok()
               && removedRequiredMove.code == DocumentEditCode::ValidationRejected
               && std::holds_alternative<MoveTo>(editor.path(1)->commands.front())
               && editor.revision() == beforeRemovingMove,
           "removing the required initial MoveTo must roll back the complete edit");

    expect(editor.setViewport({80, 60}).changed
               && editor.asset()->viewport.width == 80
               && editor.asset()->viewport.height == 60,
           "the bound vector viewport must be atomically editable");
    expect(editor.movePath(1, 0).changed
               && editor.path(0)->fill
               && editor.path(0)->fill->argb == 0xff55aa77U,
           "path paint order must be editable within the bound asset");
    expect(editor.removePath(1).changed && editor.pathCount() == 1,
           "paths must be removable without touching another asset");

    const std::uint64_t beforeInvalidPath = editor.revision();
    const DocumentEditResult invalidPath = editor.createPath(
        {0.0, 0.0}, std::nullopt, std::nullopt);
    expect(!invalidPath.ok()
               && invalidPath.code == DocumentEditCode::ValidationRejected
               && editor.pathCount() == 1
               && editor.revision() == beforeInvalidPath,
           "a newly created path without fill or stroke must fail without partial insertion");
    expect(validate(document).ok(),
           "all accepted vector edits must preserve complete document validity");

    document.extent.height = 0;
    const std::size_t beforeExternallyInvalidEdit = editor.path(0)->commands.size();
    const DocumentEditResult externallyInvalid = editor.appendLineTo(0, {2.0, 2.0});
    expect(!externallyInvalid.ok()
               && externallyInvalid.code == DocumentEditCode::InvalidDocument
               && editor.path(0)->commands.size() == beforeExternallyInvalidEdit,
           "the editor must detect external aggregate invalidation before mutation");

    editor.unbind();
    expect(!editor.isBound()
               && editor.boundAssetId().empty()
               && editor.revision() == 0,
           "unbind must clear identity and editor-local revision state");

    return failures == 0 ? 0 : 1;
}
