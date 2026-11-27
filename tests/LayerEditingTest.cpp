#include <iiSharedCanvas>
#include <Document/LayerLayout.h>
#include <Document/ShapeCombinationPlan.h>
#include <Document/LayerTransactions.h>

#include <cmath>
#include <filesystem>
#include <iostream>
#include <limits>
#include <string_view>
#include <vector>

using namespace iiSharedCanvas;
namespace {
int failures=0;
void expect(bool condition,std::string_view message)
{
    if (!condition) { ++failures; std::cerr << message << '\n'; }
}
Document document()
{
    Document value; value.extent={32,32};
    value.assets.emplace_back(RasterAsset{"pixels",makeRasterLayer(2,2,0xffffffffU)});
    for (const std::string id:{"a","b","c","d","e"})
        value.layers.emplace_back(StaticBitmapLayer{{id,id},StaticSource{"pixels"}});
    return value;
}
void arrangement()
{
    auto value=document();
    const std::vector<std::string> selected{"b","d"};
    const auto up=LayerArrangementPlanner{}(value,selected,LayerArrangement::Forward);
    expect(up.ok() && up.order==std::vector<std::string>({"a","c","b","e","d"}),
           "forward moves every selected layer one step while retaining relative order");
    const auto down=LayerArrangementPlanner{}(value,selected,LayerArrangement::Backward);
    expect(down.ok() && down.order==std::vector<std::string>({"b","a","d","c","e"}),
           "backward preserves selected ordering");
    const auto top=LayerArrangementPlanner{}(value,selected,LayerArrangement::Front);
    expect(top.ok() && top.order==std::vector<std::string>({"a","c","e","b","d"}),
           "front preserves relative ordering of both sets");
    const auto bottom=LayerArrangementPlanner{}(value,selected,LayerArrangement::Back);
    expect(bottom.ok() && bottom.order==std::vector<std::string>({"b","d","a","c","e"}),
           "back preserves relative ordering of both sets");
    expect(!LayerArrangementPlanner{}(value,std::vector<std::string>{"missing"},LayerArrangement::Front).ok(),
           "unknown identifiers fail without a partial plan");
    expect(!LayerArrangementPlanner{}(value,std::vector<std::string>{"b","b"},LayerArrangement::Front).ok(),
           "duplicate identifiers are rejected");
    auto locked=value; locked.relations.locks.setLayer("c",LayerLock::Structure);
    expect(!LayerArrangementPlanner{}(locked,std::vector<std::string>{"b"},LayerArrangement::Forward).ok(),
           "structural locks also protect layers crossed by an arrangement");
    auto grouped=value; grouped.relations.groups.insert({"g","Group","",LayerGroupMode::PassThrough,true});
    grouped.relations.groups.assign("b","g"); grouped.relations.groups.assign("c","g");
    const auto groupFront=LayerArrangementPlanner{}(grouped,std::vector<std::string>{"b"},LayerArrangement::Front);
    expect(groupFront.ok() && groupFront.order==std::vector<std::string>({"a","c","b","d","e"}),
           "front moves a layer within its direct group");
    const auto overGroup=LayerArrangementPlanner{}(grouped,std::vector<std::string>{"a"},LayerArrangement::Forward);
    expect(overGroup.ok() && overGroup.order==std::vector<std::string>({"b","c","a","d","e"}),
           "a root-layer step crosses a child group as one contiguous block");
    auto groups=grouped; groups.relations.groups.insert({"h","Other","",LayerGroupMode::PassThrough,true});
    groups.relations.groups.assign("d","h"); groups.relations.groups.assign("e","h");
    const auto across=LayerArrangementPlanner{}(groups,std::vector<std::string>{"b","d"},LayerArrangement::Front);
    expect(across.ok() && across.order==std::vector<std::string>({"a","c","b","e","d"}),
           "selections in different groups are arranged independently");
    auto nested=value; nested.relations.groups.insert({"outer","Outer","",LayerGroupMode::PassThrough,true});
    nested.relations.groups.insert({"inner","Inner","outer",LayerGroupMode::Isolated,true});
    nested.relations.groups.assign("b","inner"); nested.relations.groups.assign("c","inner");
    nested.relations.groups.assign("d","outer");
    const auto deep=LayerArrangementPlanner{}(nested,std::vector<std::string>{"a","b"},LayerArrangement::Front);
    expect(deep.ok() && deep.order==std::vector<std::string>({"c","b","d","e","a"}),
           "nested and root selections move independently without splitting descendants");
    const auto nestedStep=LayerArrangementPlanner{}(nested,std::vector<std::string>{"d"},LayerArrangement::Backward);
    expect(nestedStep.ok() && nestedStep.order==std::vector<std::string>({"a","d","b","c","e"}),
           "a direct member crosses a nested group as one block");
    nested.relations.locks.setLayer("c",LayerLock::Structure);
    expect(!LayerArrangementPlanner{}(nested,std::vector<std::string>{"d"},LayerArrangement::Backward).ok(),
           "crossing a child group respects structural locks on every displaced descendant");
    DocumentHistory history(value);
    expect(history.applyLayerOrder(top.order).changed &&
           layerProperties(value.layers.back()).id=="d" && history.undo().changed &&
           layerProperties(value.layers[1]).id=="b" && history.redo().changed &&
           layerProperties(value.layers.back()).id=="d",
           "an arrangement is one reversible document history command");
}
void layout()
{
    const std::vector<LayerGeometry> geometry{{"a",{0,0,4,2}},{"b",{8,3,2,5}},{"c",{20,1,8,1}}};
    const auto left=LayerAlignmentSolver{}(geometry,{10,20,20,10},LayerAlignment::Left);
    expect(left.ok() && left.offsets.size()==3 && left.offsets[0].x==10 && left.offsets[1].x==2 &&
           left.offsets[2].x==-10 && left.offsets[0].y==0,"left alignment uses the explicit world-space reference");
    const auto bottom=LayerAlignmentSolver{}(geometry,{10,20,20,10},LayerAlignment::Bottom);
    expect(bottom.ok() && bottom.offsets[0].y==28 && bottom.offsets[1].y==22 && bottom.offsets[2].y==28,
           "bottom alignment uses each individual boundary");
    const auto centers=LayerDistributionSolver{}(geometry,LayerAxis::Horizontal,LayerDistribution::Centers);
    const auto gaps=LayerDistributionSolver{}(geometry,LayerAxis::Horizontal,LayerDistribution::Gaps);
    expect(centers.ok() && centers.offsets[1].x==4 && centers.offsets[0].x==0 && centers.offsets[2].x==0,
           "center distribution fixes outer centers");
    expect(gaps.ok() && gaps.offsets[1].x==3 && gaps.offsets[0].x==0 && gaps.offsets[2].x==0,
           "gap distribution accounts for unequal widths");
    auto invalid=geometry; invalid[0].bounds.width=std::numeric_limits<double>::infinity();
    expect(!LayerAlignmentSolver{}(invalid,{0,0,1,1},LayerAlignment::Left).ok() &&
           !LayerDistributionSolver{}(invalid,LayerAxis::Horizontal,LayerDistribution::Gaps).ok(),
           "non-finite bounds fail before producing offsets");
    auto value=document(); DocumentHistory history(value);
    expect(history.translateLayers(left.offsets).changed &&
           layerProperties(value.layers[0]).transform.translationX==10 &&
           layerProperties(value.layers[1]).transform.translationX==2 && history.undo().changed &&
           layerProperties(value.layers[0]).transform.translationX==0 && history.redo().changed &&
           layerProperties(value.layers[1]).transform.translationX==2,
           "all planned translations form one reversible history command");
    auto locked=document(); locked.relations.locks.setLayer("b",LayerLock::Transform);
    DocumentHistory protectedHistory(locked);
    expect(!protectedHistory.translateLayers(left.offsets).ok() &&
           layerProperties(locked.layers[0]).transform.translationX==0 && !protectedHistory.canUndo(),
           "one locked member rejects the entire layout without a partial edit");
}
void durableLayout()
{
    const auto path=std::filesystem::path(IISHAREDCANVAS_TEST_OUTPUT_DIR)/"layer-layout.iisc";
    std::filesystem::create_directories(path.parent_path()); std::filesystem::remove(path);
    DocumentFile file; expect(file.create(path.string(),document()).ok(),"create a working file for layout history");
    if (!file.document()) return;
    {
        DocumentHistory history(file);
        const auto plan=LayerArrangementPlanner{}(*file.document(),std::vector<std::string>{"b"},LayerArrangement::Forward);
        const std::vector<LayerOffset> offsets{{"a",3,4},{"b",-2,5}};
        expect(plan.ok() && history.applyLayerOrder(plan.order).changed && history.translateLayers(offsets).changed &&
               history.undo().changed && history.undo().changed && history.redo().changed && history.redo().changed,
               "file-bound ordering and translations are separate reversible operations");
    }
    file.close(); expect(file.open(path.string()).ok(),"reopen the layout working file");
    if (file.document()) {
        expect(layerProperties(file.document()->layers[2]).id=="b" &&
               layerProperties(*findLayer(*file.document(),"a")).transform.translationX==3 &&
               layerProperties(*findLayer(*file.document(),"b")).transform.translationY==5,
               "ordering and translations survive working-file reopen");
    }
    file.close();
}
VectorAsset rectangle(std::string id,double left,double right)
{
    VectorPath path; path.fill=SolidPaint{0xffffffffU};
    path.commands={MoveTo{{left,0}},LineTo{{right,0}},LineTo{{right,2}},LineTo{{left,2}},ClosePath{}};
    return {std::move(id),{8,2},{std::move(path)}};
}
void legacyVectorUpgrade()
{
    for (bool explicitUpgrade:{false,true}) {
        Document old; old.formatVersion.minor=25; old.extent={8,2};
        old.assets.push_back(rectangle("legacy-vector",0,4));
        old.layers.emplace_back(StaticVectorLayer{{"legacy-layer","Old"},StaticSource{"legacy-vector"}});
        const auto path=std::filesystem::path(IISHAREDCANVAS_TEST_OUTPUT_DIR)/
            (explicitUpgrade ? "explicit-vector-upgrade.iisc" : "edited-vector-upgrade.iisc");
        std::filesystem::create_directories(path.parent_path()); std::filesystem::remove(path);
        DocumentFile file; const auto created=file.create(path.string(),old);
        expect(created.ok(),"create an older vector working file");
        if (!created.ok()) continue;
        if (explicitUpgrade) {
            expect(file.upgradeFormat().ok(),"explicitly upgrade an older vector working file");
        } else {
            DocumentHistory history(file);
            expect(history.setLayerName("legacy-layer","Edited").changed,
                   "editing an older vector document publishes a format upgrade");
        }
        file.close(); const auto reopened=file.open(path.string());
        expect(reopened.ok() && file.document() && findVectorAsset(*file.document(),"legacy-vector") &&
               file.document()->formatVersion.minor==CurrentFormatMinor,
               "untouched legacy vector records remain readable after a format upgrade");
        file.close();
    }
}
void combinations()
{
    auto left=rectangle("left",0,4),right=rectangle("right",2,6);
    const std::vector<ShapeOperand> inputs{{&left,{}},{&right,{}}};
    for (const auto &[operation,expected]:std::vector<std::pair<ShapeBooleanOperation,std::vector<int>>>{
        {ShapeBooleanOperation::Union,{1,1,1,1,1,1,0,0}},
        {ShapeBooleanOperation::Subtract,{1,1,0,0,0,0,0,0}},
        {ShapeBooleanOperation::Intersect,{0,0,1,1,0,0,0,0}},
        {ShapeBooleanOperation::ExclusiveOr,{1,1,0,0,1,1,0,0}}}) {
        auto result=ShapeCombinationPlan{}(inputs,operation,"combined",{8,2},0xff336699U);
        expect(result.ok() && result.asset.shape.has_value(),"shape plan owns an exact expression");
        if (!result.ok()) continue;
        Document d; d.extent={8,2};
        DocumentHistory history(d);
        expect(history.insertLayerWithAsset(result.asset,StaticVectorLayer{{"shape","Shape"},StaticSource{"combined"}}).changed,
               "compound vector is inserted as one history operation");
        const auto image=renderFrame(d,0);
        expect(image.ok(),"compound vector renders");
        if (image.ok()) for (std::size_t x=0;x<expected.size();++x)
            expect(bool(image.pixels.pixels[x]>>24)==bool(expected[x]),"boolean sample matches the selected operation");
        const auto encoded=encodeIisc(d); const auto decoded=encoded.ok() ? decodeIisc(encoded.bytes) : IiscDecodeResult{};
        expect(encoded.ok() && decoded.ok() && findVectorAsset(decoded.document,"combined") &&
               findVectorAsset(decoded.document,"combined")->shape.has_value(),
               "the exact expression survives IISC serialization");
        auto legacy=d; legacy.formatVersion.minor=25;
        expect(!encodeIisc(legacy).ok(),"a pre-expression format rejects the compound shape");
        expect(history.undo().changed && d.layers.empty() && history.redo().changed &&
               renderFrame(d,0).pixels.pixels==image.pixels.pixels,
               "undo and redo retain both the expression and its appearance");
    }
    VectorPath curve; curve.fill=SolidPaint{0xffffffffU};
    curve.commands={MoveTo{{0,0}},CubicTo{{.125,.875},{1.125,1.875},{2,1}},LineTo{{0,0}},ClosePath{}};
    VectorAsset curved{"curved",{8,2},{curve}};
    AffineTransform transform; transform.translationX=1.125;
    const std::vector<ShapeOperand> curvedInputs{{&curved,transform},{&right,{}}};
    auto exact=ShapeCombinationPlan{}(curvedInputs,ShapeBooleanOperation::Union,"exact",{8,2},0xffffffffU);
    expect(exact.ok() && std::get<CubicTo>(exact.asset.paths[0].commands[1]).control1==Point{.125,.875} &&
           exact.asset.shape->nodes[1].transform.translationX==1.125,
           "source Bezier control points and transforms remain uncut exact input values");
    auto open=left;
    open.paths[0].commands={MoveTo{{0,0}},LineTo{{1,0}},MoveTo{{2,0}},LineTo{{3,0}},ClosePath{}};
    const std::vector<ShapeOperand> openInputs{{&open,{}},{&right,{}}};
    expect(!ShapeCombinationPlan{}(openInputs,ShapeBooleanOperation::Union,"invalid",{8,2},0xffffffffU).ok(),
           "every contour of a compound operand must be closed");
    if (exact.ok()) {
        Document source; source.extent={8,2}; source.assets.push_back(exact.asset);
        source.layers.emplace_back(StaticVectorLayer{{"shape","Shape"},StaticSource{"exact"}});
        const auto path=std::filesystem::path(IISHAREDCANVAS_TEST_OUTPUT_DIR)/"layer-shape.iisc";
        std::filesystem::create_directories(path.parent_path()); std::filesystem::remove(path);
        DocumentFile file; expect(file.create(path.string(),source).ok(),"create a compound shape working file");
        if (file.document()) {
            auto changed=curve; std::get<CubicTo>(changed.commands[1]).control1={.375,.625};
            DocumentHistory history(file);
            expect(history.replaceVectorPath("exact",0,changed).changed && history.undo().changed && history.redo().changed,
                   "compound operand editing is file-bound and reversible");
        }
        file.close(); expect(file.open(path.string()).ok(),"reopen a compound shape working file");
        if (file.document()) {
            const auto *saved=findVectorAsset(*file.document(),"exact");
            expect(saved && saved->shape &&
                   std::get<CubicTo>(saved->paths[0].commands[1]).control1==Point{.375,.625} &&
                   saved->shape->nodes[1].transform.translationX==1.125,
                   "reopen preserves exact operand edits, expression and transform");
        }
        file.close();
    }
}

Document animated()
{
    Document d; d.extent={2,1}; d.timeline.frameCount=2;
    d.assets.emplace_back(RasterAsset{"a0",makeRasterLayer(2,1,0xffff0000U)});
    d.assets.emplace_back(RasterAsset{"a1",makeRasterLayer(2,1,0xff00ff00U)});
    d.assets.emplace_back(RasterAsset{"top",makeRasterLayer(2,1,0x800000ffU)});
    d.assets.emplace_back(RasterAsset{"hidden",makeRasterLayer(2,1,0xffff00ffU)});
    DynamicBitmapContent content; content.frameIndices={0,1};
    d.layers.emplace_back(DynamicBitmapLayer{{"base","Base"},content});
    d.layers.emplace_back(StaticBitmapLayer{{"top-layer","Top"},StaticSource{"top"}});
    LayerProperties hidden{"hidden-layer","Hidden"}; hidden.visible=false;
    d.layers.emplace_back(StaticBitmapLayer{hidden,StaticSource{"hidden"}});
    d.frames={{0,{{"base","a0"}}},{1,{{"base","a1"}}}};
    return d;
}
void transactions()
{
    auto d=animated();
    const std::vector<std::string> selected{"base","top-layer"};
    const auto frame0=renderFrame(d,0),frame1=renderFrame(d,1);
    auto verified=prepareVerifiedLayerMerge(d,selected,"verified","verified-asset");
    expect(verified.ok(),"the external merge controller verifies the actual candidate at every frame");
    if (verified.ok()) {
        auto candidate=d; DocumentHistory verifiedHistory(candidate);
        expect(verifiedHistory.applyLayerTransaction(std::move(verified.plan)).changed &&
               renderFrame(candidate,0).pixels.pixels==frame0.pixels.pixels &&
               renderFrame(candidate,1).pixels.pixels==frame1.pixels.pixels,
               "a verified merge preserves the real rendered frames");
    }
    auto backdrop=animated();
    backdrop.layers.insert(backdrop.layers.begin(),StaticBitmapLayer{{"backdrop","Backdrop"},StaticSource{"top"}});
    auto upper=prepareVerifiedLayerMerge(backdrop,selected,"upper","upper-asset");
    expect(upper.ok(),"a contiguous selection above an unselected backdrop can merge");
    if (upper.ok()) {
        const auto before=renderFrame(backdrop,0);
        auto candidate=backdrop; DocumentHistory upperHistory(candidate);
        expect(upperHistory.applyLayerTransaction(std::move(upper.plan)).changed &&
               renderFrame(candidate,0).pixels.pixels==before.pixels.pixels,
               "verified upper-layer merge preserves the real backdrop composition");
    }
    auto impossible=prepareVerifiedLayerMerge(backdrop,std::vector<std::string>{"base","hidden-layer"},
                                               "invalid","invalid-asset");
    expect(!impossible.ok() && backdrop.layers.size()==4,
           "the external merge controller rejects intervening layers without mutation");
    const std::vector<LayerMergeFrame> evidence{{frame0,frame0,frame0},{frame1,frame1,frame1}};
    auto merged=LayerMergeTransaction{}(d,selected,evidence,"merged","merge-asset");
    expect(merged.ok(),"merge prepares an externally verified frame-preserving transaction");
    if (merged.ok()) {
        DocumentHistory history(d);
        expect(history.applyLayerTransaction(std::move(merged.plan)).changed && d.layers.size()==2 &&
               !findAsset(d,"a0") && !findAsset(d,"a1") && !findAsset(d,"top") && findAsset(d,"hidden"),
               "merge removes only exclusive input assets and preserves unselected hidden data");
        expect(renderFrame(d,0).pixels.pixels==frame0.pixels.pixels && renderFrame(d,1).pixels.pixels==frame1.pixels.pixels,
               "merge preserves each frame's visible composition");
        expect(history.undo().changed && d.layers.size()==3 && findAsset(d,"a0") && findAsset(d,"a1") &&
               history.redo().changed && d.layers.size()==2,
               "merge restores all removed layers and assets with one undo and redo");
    }
    auto shared=animated();
    std::get<StaticBitmapLayer>(shared.layers[2]).content.assetId="top";
    auto sharedPlan=LayerMergeTransaction{}(shared,selected,evidence,"shared-merged","shared-asset");
    expect(sharedPlan.ok(),"prepare a merge with a shared source asset");
    if (sharedPlan.ok()) {
        DocumentHistory sharedHistory(shared);
        expect(sharedHistory.applyLayerTransaction(std::move(sharedPlan.plan)).changed && findAsset(shared,"top") &&
               !findAsset(shared,"a0"),"a source shared by an unselected layer is retained");
    }
    auto limited=animated(); DocumentHistoryLimits limits; limits.maximumBytes=1;
    DocumentHistory tiny(limited,limits);
    auto overBudget=LayerMergeTransaction{}(limited,selected,evidence,"budget","budget-asset");
    expect(overBudget.ok() && tiny.applyLayerTransaction(std::move(overBudget.plan)).code==DocumentHistoryCode::BudgetExceeded &&
           limited.layers.size()==3 && findAsset(limited,"a0") && !tiny.canUndo(),
           "a history budget failure leaves every layer and asset unchanged");
    auto stale=animated(); DocumentHistory staleHistory(stale);
    auto stalePlan=LayerMergeTransaction{}(stale,selected,evidence,"stale","stale-asset");
    expect(stalePlan.ok() && staleHistory.setLayerName("base","Changed").changed &&
           staleHistory.applyLayerTransaction(std::move(stalePlan.plan)).code==DocumentHistoryCode::Conflict &&
           stale.layers.size()==3 && staleHistory.statistics().undoEntries==1,
           "a stale plan cannot overwrite a later history edit");
    auto invalid=animated(); auto unverified=evidence; unverified[0].after.pixels.pixels[0]^=1;
    expect(!LayerMergeTransaction{}(invalid,selected,unverified,"merged","merge-asset").ok(),
           "merge rejects a frame whose composition proof differs");
    expect(!LayerMergeTransaction{}(invalid,std::vector<std::string>{"base","hidden-layer"},evidence,
                                    "merged","merge-asset").ok(),
           "intervening unselected layers are rejected");
    invalid.relations.locks.setLayer("top-layer",LayerLock::Structure);
    expect(!LayerMergeTransaction{}(invalid,selected,evidence,"merged","merge-asset").ok(),
           "merge respects inherited structural locks");
    auto flat=animated();
    const std::vector<FrameRenderResult> frames{renderFrame(flat,0),renderFrame(flat,1)};
    auto flattened=DocumentFlattenTransaction{}(flat,frames,"flat","flat-asset");
    expect(flattened.ok(),"flatten prepares all timeline frames");
    if (flattened.ok()) {
        DocumentHistory history(flat);
        expect(history.applyLayerTransaction(std::move(flattened.plan)).changed && flat.layers.size()==1 &&
               !findAsset(flat,"hidden") && !findAsset(flat,"a0") && !findAsset(flat,"top"),
               "flatten removes hidden layers and unreferenced source assets");
        expect(renderFrame(flat,0).pixels.pixels==frames[0].pixels.pixels &&
               renderFrame(flat,1).pixels.pixels==frames[1].pixels.pixels,
               "flatten preserves every frame");
        expect(history.undo().changed && flat.layers.size()==3 && findAsset(flat,"hidden") &&
               history.redo().changed && flat.layers.size()==1,
               "flatten is one reversible document transaction");
    }
    // Exercise the same transaction against the actual durable working-file
    // owner, including Undo/Redo and a close/reopen after the edit session.
    const auto path=std::filesystem::path(IISHAREDCANVAS_TEST_OUTPUT_DIR)/"layer-edit-transaction.iisc";
    std::filesystem::create_directories(path.parent_path()); std::filesystem::remove(path);
    DocumentFile file; auto original=animated();
    const auto created=file.create(path.string(),original);
    expect(created.ok(),"create a working file for a flatten transaction");
    if (created.ok()) {
        const std::vector<FrameRenderResult> savedFrames{renderFrame(*file.document(),0),renderFrame(*file.document(),1)};
        auto plan=DocumentFlattenTransaction{}(*file.document(),savedFrames,"durable","durable-asset");
        {
            DocumentHistory history(file);
            expect(plan.ok() && history.applyLayerTransaction(std::move(plan.plan)).changed &&
                   history.undo().changed && file.document()->layers.size()==3 && history.redo().changed,
                   "file-bound flatten commits and replays atomically");
        }
        file.close(); const auto reopened=file.open(path.string());
        expect(reopened.ok() && file.document()->layers.size()==1 && !findAsset(*file.document(),"hidden") &&
               renderFrame(*file.document(),1).pixels.pixels==savedFrames[1].pixels.pixels,
               "flattened frames and removed hidden source survive reopening");
        file.close();
    }
}

}
int main()
{
    arrangement(); layout(); durableLayout(); legacyVectorUpgrade(); combinations(); transactions(); return failures ? 1 : 0;
}
