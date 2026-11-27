#include <iiSharedCanvas>

#include <array>
#include <filesystem>
#include <iostream>
#include <memory>
#include <string_view>

using namespace iiSharedCanvas;
namespace {
int failures = 0;
void expect(bool value, std::string_view message)
{
    if (!value) { ++failures; std::cerr << message << '\n'; }
}

Document sample()
{
    Document d; d.extent = {2, 2};
    d.assets.emplace_back(RasterAsset{"background", makeRasterLayer(2, 2, 0xff808080)});
    d.assets.emplace_back(RasterAsset{"paint", makeRasterLayer(2, 2, 0xffff0000)});
    d.layers.emplace_back(StaticBitmapLayer{{"base", "Base"}, StaticSource{"background"}});
    d.layers.emplace_back(StaticBitmapLayer{{"top", "Top"}, StaticSource{"paint"}});
    layerProperties(d.layers.back()).blendMode = RasterBlendMode::Multiply;
    return d;
}

SelectionLayerRelations relations(const Document &document)
{
    SelectionLayerRelations state;
    auto mask = TiledMask::fromRaster({{0, 0}, {2, 2}}, {0, 0},
                                    RasterMask{{2, 2}, {0, 64, 128, 255}});
    expect(mask.ok(), "prepare a selection mask");
    expect(state.selections.insert({"selection", "Selection", std::make_shared<TiledMask>(mask.mask)}),
           "insert a named selection");
    LayerGroup group; group.id = "group"; group.name = "Group"; group.mode = LayerGroupMode::PassThrough;
    expect(state.groups.insert(group) && state.groups.assign("top", "group"), "create a group and membership");
    expect(state.links.connect("base", "top"), "connect layers without applying a transform");
    expect(state.validate(document).empty(), "valid relations");
    return state;
}

void persistentRelations()
{
    auto d = sample(); DocumentHistory history(d); auto state = relations(d);
    expect(history.setSelectionLayerRelations(state).changed, "publish relations in history");
    const auto *channel = d.relations.selections.find("selection");
    expect(channel && channel->mask && channel->mask->coverage(1, 0) == 64, "selection coverage remains exact");
    const auto bytes = encodeIisc(d);
    expect(bytes.ok(), "encode selection and layer relations");
    if (!bytes.ok()) return;
    SerializationLimits noChannels; noChannels.maximumSelectionChannels=0;
    expect(!encodeIisc(d,noChannels).ok() && !decodeIisc(bytes.bytes,noChannels).ok(),
           "configured channel limits apply to both encoding and decoding");
    const auto restored = decodeIisc(bytes.bytes);
    const auto *saved = restored.ok() ? restored.document.relations.selections.find("selection") : nullptr;
    expect(saved && saved->mask->coverage(0, 1) == 128 && restored.document.relations.links.connected("base", "top"),
           "restore named selection and layer link");
    expect(history.undo().changed && !d.relations.selections.find("selection"), "undo all relation changes");
    expect(history.redo().changed && d.relations.selections.find("selection"), "redo all relation changes");
    auto legacy = d; legacy.formatVersion.minor = 23; // Relationships first appeared in 1.24.
    expect(!encodeIisc(legacy).ok(), "old document version must not discard relationships");
    const auto root = std::filesystem::path(IISHAREDCANVAS_TEST_OUTPUT_DIR) / "selection-layer-relations";
    std::filesystem::create_directories(root);
    const auto from = root / "relations.iisc", to = root / "moved.iisc";
    std::filesystem::remove(from); std::filesystem::remove(to);
    DocumentFile file; const auto created = file.create(from.string(), d);
    expect(created.ok(), "create a working file with relations");
    if (!created.ok()) return;
    file.close(); std::filesystem::rename(from, to);
    expect(file.open(to.string()).ok() && file.document()->relations.selections.find("selection"),
           "relations survive working-file relocation and reopening");
}

void compositionAndLinks()
{
    auto d = sample(); DocumentHistory history(d); auto state = relations(d);
    expect(history.setSelectionLayerRelations(state).changed, "publish pass-through group");
    const auto pass = renderFrame(d, 0);
    auto grouped = d.relations;
    expect(grouped.groups.setMode("group", LayerGroupMode::Isolated) &&
           history.setSelectionLayerRelations(grouped).changed, "switch group to isolated composition");
    const auto isolated = renderFrame(d, 0);
    expect(pass.ok() && isolated.ok() && pass.pixels.pixels[0] != isolated.pixels.pixels[0],
           "pass-through and isolation use different backdrops");
    expect(history.undo().changed && renderFrame(d, 0).pixels.pixels == pass.pixels.pixels,
           "undo restores group composition");
    AffineTransform transform; transform.translationX = 1;
    expect(history.setLayerTransform("top", transform).changed &&
           layerProperties(d.layers.front()).transform.translationX == 0,
           "a graph edge never propagates an edit automatically");
    LayerQueryIndex index(d);
    const auto found = index.query(d, LayerQuery{.nameContains = "Top"});
    expect(found.ok() && found.layerIds == std::vector<std::string>{"top"}, "query index returns matching IDs");
    expect(history.setLayerVisible("top", false).changed && !index.query(d, {}).ok(),
           "query rejects a stale index");
    expect(index.refresh(d).ok() && index.query(d, LayerQuery{.visible = false}).layerIds == std::vector<std::string>{"top"},
           "refresh indexes changed layer properties");
}

void locks()
{
    auto d = sample(); DocumentHistory history(d); auto state = relations(d);
    expect(state.locks.setGroup("group", LayerLock::Pixels | LayerLock::Transform), "set group locks");
    expect(history.setSelectionLayerRelations(state).changed, "publish group locks");
    const auto before = encodeIisc(d).bytes; const auto count = history.statistics().undoEntries;
    AffineTransform t; t.translationX = 1;
    const std::array<std::uint32_t, 1> pixel{0xffabcdef};
    const std::array patch{BitmapPatch{{{0, 0}, 1, 1}, pixel}};
    expect(!history.setLayerTransform("top", t).ok() && !history.replaceBitmapPatches("paint", patch).ok() &&
           encodeIisc(d).bytes == before && history.statistics().undoEntries == count,
           "inherited locks reject transform and pixel changes atomically");
    expect(history.undo().changed && history.setLayerTransform("top", t).changed,
           "undoing lock changes permits edits again");
    auto invalid = relations(d);
    LayerGroup second; second.id = "other"; second.parentId = "group";
    expect(invalid.groups.insert(second) && !invalid.groups.setParent("group", "other"), "reject group cycles");
    expect(!invalid.groups.descendsFrom("missing","missing"), "missing groups are not their own descendants");
    expect(!invalid.links.connect("base", "base"), "reject self links");
}
void removalAndTransparency()
{
    auto d=sample(); DocumentHistory history(d); auto state=relations(d);
    expect(history.setSelectionLayerRelations(state).changed,"install relationships before deletion");
    expect(history.removeLayer("top").changed && !d.relations.links.connected("base","top") &&
           d.relations.groups.groupOf("top").empty(),"layer removal cleans its relationships");
    expect(history.undo().changed && d.relations.links.connected("base","top") &&
           d.relations.groups.groupOf("top")=="group","undo restores the layer and relationships atomically");
    expect(history.redo().changed && history.undo().changed,"removal redo and undo remain reversible");
    expect(history.removeLayerWithAsset("top").changed && !d.relations.links.connected("base","top") &&
           history.undo().changed && d.relations.links.connected("base","top"),
           "deleting a layer with its asset restores its relationships on undo");

    state=d.relations; state.locks.setLayer("top",LayerLock::Transparency);
    expect(history.setSelectionLayerRelations(state).changed,"apply transparency lock");
    const std::array<std::uint32_t,1> opaque{0xff123456}, transparent{0x00123456};
    const std::array sameAlpha{BitmapPatch{{{0,0},1,1},opaque}};
    const std::array changedAlpha{BitmapPatch{{{0,0},1,1},transparent}};
    expect(history.replaceBitmapPatches("paint",sameAlpha).changed,"transparency lock permits preserving alpha");
    const auto revision=d.authorship.revision();
    expect(!history.replaceBitmapPatches("paint",changedAlpha).ok() && d.authorship.revision()==revision,
           "transparency lock rejects an alpha change without stamping history");
    state=d.relations; state.locks.setLayer("top",LayerLock::Structure);
    expect(history.setSelectionLayerRelations(state).changed && !history.removeLayer("top").ok(),
           "structure lock prevents removing the layer");
}
void fileHistoryAndEdits()
{
    auto before=sample(); auto after=before; after.relations=relations(after);
    const auto delta=encodeIiscEdit(before,after);
    expect(delta.ok(),"encode relationship edit transaction");
    auto changed=before;
    expect(delta.ok() && applyIiscEdit(changed,delta.bytes).code==IiscErrorCode::None &&
           changed.relations==after.relations,"apply relationship edit transaction");
    changed=before;
    expect(delta.ok() && applyIiscEdit(changed,delta.bytes,{},[](const Document &){return false;}).code!=IiscErrorCode::None &&
           changed.relations.empty(),"rejected relationship edit rolls back");
    const auto root=std::filesystem::path(IISHAREDCANVAS_TEST_OUTPUT_DIR)/"selection-layer-history";
    std::filesystem::create_directories(root); const auto path=root/"history.iisc"; std::filesystem::remove(path);
    DocumentFile file; expect(file.create(path.string(),before).ok(),"create relationship history file");
    if (!file.isOpen()) return;
    DocumentHistory history(file);
    expect(history.setSelectionLayerRelations(after.relations).changed && file.document()->relations==after.relations,
           "file-backed relationship command persists");
    expect(history.undo().changed && file.document()->relations.empty() && history.redo().changed,
           "file-backed relationship command can be undone and redone");
    expect(history.removeLayerWithAsset("top").changed && !file.document()->relations.links.connected("base","top") &&
           history.undo().changed && file.document()->relations.links.connected("base","top"),
           "file-backed layer-and-asset deletion restores its relationships");
    file.close();
    expect(file.open(path.string()).ok() && file.document()->relations==after.relations,
           "file-backed redo survives reopening");
}
void sparseMasksAndNestedGroups()
{
    const CanvasRegion huge{{-1000,-1000},{1000000,1000000}};
    auto selected=TiledMask::rectangle(huge,{-1000,-1000},{999000,999000},false);
    expect(selected.ok() && selected.mask.tile_count()==0 && selected.mask.coverage(0,0)==255,
           "large uniform selections use implicit coverage");
    auto copy=std::make_shared<TiledMask>(selected.mask); SelectionChannelStore store;
    expect(store.insert({"huge","Huge",copy}),"save large sparse selection");
    *copy=TiledMask{};
    expect(store.find("huge")->mask->coverage(0,0)==255,"channel detaches caller-owned mutable wrapper");
    const auto snapshot=store.find("huge")->mask->snapshot();
    expect(TiledMask::fromSnapshot(snapshot).mask.equivalent(*store.find("huge")->mask),"sparse snapshot round trip");
    auto invalid=snapshot; invalid.tiles.push_back({-1,0,1,{}});
    expect(!TiledMask::fromSnapshot(invalid).ok(),"reject malformed sparse tile coordinates");
    auto d=sample(); auto state=relations(d);
    LayerGroup child; child.id="child"; child.parentId="group"; child.visible=false;
    expect(state.groups.insert(child) && state.groups.assign("top","child"),"prepare nested hidden group");
    DocumentHistory history(d);
    expect(history.setSelectionLayerRelations(state).changed,"publish nested group");
    const auto hidden=renderFrame(d,0);
    expect(hidden.ok() && hidden.pixels.pixels[0]==0xff808080,"hidden group excludes descendants from composition");
    LayerQueryIndex index(d);
    expect(index.query(d,LayerQuery{.visible=false}).layerIds==std::vector<std::string>{"top"},
           "layer query includes inherited group visibility");
    auto nestedLock=d.relations; nestedLock.locks.setGroup("group",LayerLock::Transform);
    expect(history.setSelectionLayerRelations(nestedLock).changed &&
           hasLayerLock(d.relations.locks.effective(d,"top"),LayerLock::Transform),
           "lock inheritance crosses multiple group levels");
    AffineTransform moved; moved.translationX=1;
    expect(!history.setLayerTransform("top",moved).ok() && history.undo().changed,
           "inherited ancestor lock blocks edits and is undoable");
    const auto output=std::filesystem::path(IISHAREDCANVAS_TEST_OUTPUT_DIR)/"group-export.psd";
    expect(exportPsd(d,output.string()).code==MediaIoCode::UnsupportedFeature,
           "PSD export rejects groups it cannot preserve");
    expect(exportTimelineInterchange(d,(output.parent_path()/"group-timeline").string()).code==MediaIoCode::UnsupportedFeature,
           "timeline interchange rejects groups it cannot preserve");
    DocumentEditor editor(d);
    expect(editor.renameLayer("top","renamed").changed && d.relations.groups.groupOf("renamed")=="child" &&
           d.relations.links.connected("base","renamed"),"layer ID changes retarget relationships");
    auto replacement=*findLayer(d,"renamed"); layerProperties(replacement).id="replacement";
    expect(editor.replaceLayer("renamed",std::move(replacement)).changed &&
           d.relations.groups.groupOf("replacement")=="child" && d.relations.links.connected("base","replacement"),
           "layer replacement retargets membership and links when its ID changes");
}
void fullLock()
{
    auto d=sample(); auto state=relations(d); state.locks.setGroup("group",LayerLock::All);
    DocumentHistory history(d); expect(history.setSelectionLayerRelations(state).changed,"publish full group lock");
    const auto revision=d.authorship.revision();
    expect(!history.setLayerVisible("top",false).ok() && !history.setLayerName("top","Other").ok() &&
           !history.removeLayerWithAsset("top").ok() && d.authorship.revision()==revision,
           "full inherited lock protects metadata and structure");
    expect(history.undo().changed,"undo can remove a full group lock");
}
void sharedAssetLock()
{
    auto d=sample();
    d.layers.emplace_back(StaticBitmapLayer{{"other","Other"},StaticSource{"paint"}});
    auto state=relations(d); state.locks.setLayer("top",LayerLock::Pixels);
    DocumentHistory history(d); expect(history.setSelectionLayerRelations(state).changed,"lock one reference to a shared asset");
    const auto revision=d.authorship.revision();
    BitmapEditor direct(d,"paint");
    expect(!direct.setPixel(0,0,0xff012345) && d.authorship.revision()==revision,
           "direct bitmap edits cannot bypass a locked shared-asset reference");
    DocumentEditor editor(d);
    expect(!editor.replaceRasterPixels("paint",makeRasterLayer(2,2,0xff012345)).ok() &&
           d.authorship.revision()==revision,"document editor enforces the same shared-asset lock");
    state=d.relations; state.locks.setLayer("top",LayerLock::Structure);
    expect(history.setSelectionLayerRelations(state).changed && !editor.renameAsset("paint","renamed-asset").ok(),
           "a structural lock protects shared asset identities used by the layer");
}
void groupedArtboardDuplication()
{
    auto d=sample(); d.artboards.push_back({"board","Board",{{0,0},{2,2}}});
    for (auto &layer:d.layers) layerProperties(layer).artboardId="board";
    DocumentHistory history(d); expect(history.setSelectionLayerRelations(relations(d)).changed,"prepare a grouped artboard");
    const auto revision=d.authorship.revision(); DocumentEditor editor(d);
    expect(!editor.duplicateArtboard("board","copy",{0,0}).ok() && d.artboards.size()==1 &&
           d.authorship.revision()==revision,"artboard duplication must not silently drop group composition");
}
void manyGroups()
{
    auto d=sample(); d.layers.clear(); SelectionLayerRelations state;
    constexpr int count=2000;
    LayerGroup empty; empty.id="empty";
    expect(state.groups.insert(empty),"prepare an empty group");
    for (int i=0;i<count;++i) {
        const auto suffix=std::to_string(i);
        LayerGroup group; group.id="group-"+suffix; group.name=group.id;
        const auto layer="layer-"+suffix;
        expect(state.groups.insert(group) && state.groups.assign(layer,group.id),"prepare independent groups");
        if (i>0) expect(state.links.connect("layer-"+std::to_string(i-1),layer),"prepare connected layer chain");
        d.layers.emplace_back(StaticBitmapLayer{{layer,layer},StaticSource{"background"}});
    }
    expect(state.validate(d).empty(),"validate thousands of independent groups");
    expect(state.links.component("layer-0").size()==count,"resolve a large connected component");
    auto branch=state.groups;
    expect(branch.erase("empty") && branch.find("group-1999") && state.groups.find("empty"),
           "group index copies detach on structural mutation and keep positions valid");
    d.relations=std::move(state); LayerQueryIndex index(d);
    const auto result=index.query(d,{});
    expect(result.ok() && result.layerIds.size()==count && result.layerIds.front()=="layer-0" &&
           result.layerIds.back()=="layer-1999","query a large group hierarchy in document order");
}
} // namespace

int main() { persistentRelations(); compositionAndLinks(); locks(); removalAndTransparency(); fileHistoryAndEdits();
             sparseMasksAndNestedGroups(); fullLock(); sharedAssetLock(); groupedArtboardDuplication();
             manyGroups(); return failures ? 1 : 0; }
