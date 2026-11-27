#include "Document/ShapeCombinationPlan.h"

#include <algorithm>
#include <cmath>
#include <limits>
#include <utility>
#include <type_traits>
#include <vector>

namespace iiSharedCanvas {
namespace {
constexpr std::size_t MaximumShapeNodes=4096;
constexpr std::uint32_t MaximumShapeDepth=256;
// A transform remains a sequence of original finite binary values: it is not
// composed here, which preserves the exact expression supplied by the caller.
bool finite(const AffineTransform &t) noexcept
{
    return std::isfinite(t.m11) && std::isfinite(t.m12) && std::isfinite(t.m21) &&
        std::isfinite(t.m22) && std::isfinite(t.translationX) && std::isfinite(t.translationY);
}
bool filledPath(const VectorPath &path)
{
    if (!path.fill || path.stroke || path.commands.size()<2 ||
        !std::holds_alternative<MoveTo>(path.commands.front()) ||
        !std::holds_alternative<ClosePath>(path.commands.back())) return false;
    const auto finitePoint=[](Point point) { return std::isfinite(point.x) && std::isfinite(point.y); };
    bool open=false;
    for (const auto &command:path.commands) {
        if (command.valueless_by_exception()) return false;
        if (std::holds_alternative<MoveTo>(command)) {
            if (open) return false;
            open=true;
        } else if (std::holds_alternative<ClosePath>(command)) {
            if (!open) return false;
            open=false;
        } else if (!open) return false;
        const bool valid=std::visit([&](const auto &value) {
            using T=std::decay_t<decltype(value)>;
            if constexpr (std::is_same_v<T,MoveTo> || std::is_same_v<T,LineTo>) return finitePoint(value.point);
            else if constexpr (std::is_same_v<T,QuadraticTo>) return finitePoint(value.control) && finitePoint(value.end);
            else if constexpr (std::is_same_v<T,CubicTo>) return finitePoint(value.control1) && finitePoint(value.control2) && finitePoint(value.end);
            else return true;
        },command);
        if (!valid) return false;
    }
    return !open;
}
} // namespace

std::string validateShapeExpression(const VectorAsset &asset)
{
    if (!asset.shape) return {};
    const auto &shape=*asset.shape;
    if (shape.nodes.empty() || shape.nodes.size()>MaximumShapeNodes || shape.root!=shape.nodes.size()-1)
        return "shape expression must have a bounded postorder root";
    std::vector<std::uint32_t> depth(shape.nodes.size()), uses(shape.nodes.size());
    for (std::size_t i=0;i<shape.nodes.size();++i) {
        const auto &node=shape.nodes[i];
        if (static_cast<unsigned>(node.kind)>static_cast<unsigned>(ShapeExpressionKind::ExclusiveOr))
            return "unknown shape expression operation";
        if (node.kind==ShapeExpressionKind::Path) {
            if (node.first>=asset.paths.size() || !filledPath(asset.paths[node.first]))
                return "shape operands must refer to closed filled paths without strokes";
            depth[i]=1; continue;
        }
        if (node.first>=i) return "shape expression children must precede their parent";
        ++uses[node.first]; depth[i]=depth[node.first]+1;
        if (node.kind==ShapeExpressionKind::Transform) {
            if (!finite(node.transform)) return "shape transform must be finite";
        } else {
            if (node.second>=i) return "shape expression children must precede their parent";
            ++uses[node.second]; depth[i]=std::max(depth[i],depth[node.second]+1);
        }
        if (depth[i]>MaximumShapeDepth) return "shape expression nesting exceeds its bound";
    }
    for (std::size_t i=0;i+1<uses.size();++i)
        if (uses[i]!=1) return "shape expression must be one non-sharing tree";
    if (uses.back()) return "shape expression root cannot be a child";
    return {};
}

ShapeCombinationResult ShapeCombinationPlan::operator()(std::span<const ShapeOperand> operands,
    ShapeBooleanOperation operation,std::string assetId,CanvasExtent viewport,std::uint32_t color) const
{
    if (operands.size()<2 || assetId.empty() || viewport.width<=0 || viewport.height<=0 ||
        static_cast<unsigned>(operation)>static_cast<unsigned>(ShapeBooleanOperation::ExclusiveOr))
        return {{},"invalid shape combination request"};
    VectorAsset output; output.id=std::move(assetId); output.viewport=viewport;
    ShapeExpression expression; expression.color=color;
    const auto append=[&](ShapeExpressionNode node) {
        if (expression.nodes.size()>=MaximumShapeNodes) return false;
        expression.nodes.push_back(std::move(node)); return true;
    };
    bool first=true; std::uint32_t accumulated=0;
    for (const auto &operand:operands) {
        if (!operand.asset || !finite(operand.transform) || !validateShapeExpression(*operand.asset).empty() ||
            operand.asset->paths.empty()) return {{},"invalid vector shape operand"};
        const auto pathOffset=output.paths.size(), nodeOffset=expression.nodes.size();
        if (operand.asset->paths.size()>MaximumShapeNodes || pathOffset>MaximumShapeNodes-operand.asset->paths.size())
            return {{},"shape path index exceeds its bound"};
        for (const auto &path:operand.asset->paths) {
            if (!filledPath(path)) return {{},"shape operands require closed fills without strokes"};
            output.paths.push_back(path);
        }
        std::uint32_t root=0;
        if (operand.asset->shape) {
            for (auto node:operand.asset->shape->nodes) {
                if (node.kind==ShapeExpressionKind::Path) node.first+=static_cast<std::uint32_t>(pathOffset);
                else {
                    node.first+=static_cast<std::uint32_t>(nodeOffset);
                    if (node.kind!=ShapeExpressionKind::Transform) node.second+=static_cast<std::uint32_t>(nodeOffset);
                }
                if (!append(std::move(node))) return {{},"shape expression exceeds its node budget"};
            }
            root=static_cast<std::uint32_t>(nodeOffset+operand.asset->shape->root);
        } else {
            for (std::size_t i=0;i<operand.asset->paths.size();++i) {
                ShapeExpressionNode leaf; leaf.first=static_cast<std::uint32_t>(pathOffset+i);
                if (!append(leaf)) return {{},"shape expression exceeds its node budget"};
                const auto index=static_cast<std::uint32_t>(expression.nodes.size()-1);
                if (!i) root=index;
                else {
                    ShapeExpressionNode join; join.kind=ShapeExpressionKind::Union; join.first=root; join.second=index;
                    if (!append(join)) return {{},"shape expression exceeds its node budget"};
                    root=static_cast<std::uint32_t>(expression.nodes.size()-1);
                }
            }
        }
        ShapeExpressionNode transformed; transformed.kind=ShapeExpressionKind::Transform;
        transformed.first=root; transformed.transform=operand.transform;
        if (!append(transformed)) return {{},"shape expression exceeds its node budget"};
        root=static_cast<std::uint32_t>(expression.nodes.size()-1);
        if (first) { accumulated=root; first=false; }
        else {
            ShapeExpressionNode join; join.first=accumulated; join.second=root;
            switch (operation) {
            case ShapeBooleanOperation::Union: join.kind=ShapeExpressionKind::Union; break;
            case ShapeBooleanOperation::Subtract: join.kind=ShapeExpressionKind::Subtract; break;
            case ShapeBooleanOperation::Intersect: join.kind=ShapeExpressionKind::Intersect; break;
            case ShapeBooleanOperation::ExclusiveOr: join.kind=ShapeExpressionKind::ExclusiveOr; break;
            }
            if (!append(join)) return {{},"shape expression exceeds its node budget"};
            accumulated=static_cast<std::uint32_t>(expression.nodes.size()-1);
        }
    }
    expression.root=accumulated; output.shape=std::move(expression);
    if (auto error=validateShapeExpression(output); !error.empty()) return {{},std::move(error)};
    return {std::move(output),{}};
}
} // namespace iiSharedCanvas
