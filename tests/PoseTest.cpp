#include <iiSharedCanvas.h>
#include <QTemporaryDir>
#include <algorithm>
#include <cmath>
#include <iostream>
#include <limits>

namespace {
using namespace iiSharedCanvas;
int failures = 0;
void expect(bool value, const char *message) { if (!value) { std::cerr << message << '\n'; ++failures; } }
Document fixture()
{
    Document doc; doc.extent = {512,512}; doc.timeline.frameCount = 2;
    DocumentEditor editor(doc);
    PoseAsset pose; pose.id = "neutral"; pose.viewport = doc.extent;
    pose.people = {makeNeutralPosePerson("person")};
    pose.people[0].body[0].z = 0.125;
    PoseExpression smile; smile.id = "smile"; smile.name = "Smile"; smile.weight = 0.5;
    smile.deltas = {{PoseGroup::MouthOuterUpper,0,0,-0.02,0}, {PoseGroup::MouthOuterUpper,24,0,-0.02,0}};
    pose.people[0].expressions = {smile};
    expect(editor.insertPoseAsset(pose).changed, "insert complete native pose asset");
    PoseLayer layer; layer.properties = {"pose", "Detailed pose"}; layer.source = StaticSource{"neutral"};
    layer.control.modelId = "test/pose";
    expect(editor.insertPoseLayer(layer).changed, "insert dedicated pose layer");
    return doc;
}
void verify(const Document &doc)
{
    const auto *pose = findPoseAsset(doc,"neutral");
    expect(pose && pose->people.size() == 1, "pose asset survives"); if (!pose) { return; }
    expect(layerRole(doc.layers[0]) == LayerRole::ControlNet && controlNetKind(doc.layers[0]) == ControlNetKind::Pose,
           "pose belongs to ControlNet hierarchy");
    expect(layerKind(doc.layers[0]) == LayerKind::StaticVector, "pose uses static vector identity");
    const auto detailed = renderPoseControlMap(doc,"pose",0);
    expect(detailed.ok() && detailed.nativeAnchorCount == 590 && detailed.emittedAnchorCount == 590,
           "detailed rendering retains every native anchor");
    expect(std::any_of(detailed.pixels.pixels.begin(),detailed.pixels.pixels.end(),[](auto pixel) { return pixel != 0xff000000; }),
           "pose renders visible control pixels");
    PoseRenderOptions standard; standard.profile = PoseRenderProfile::OpenPose;
    const auto projected = renderPoseControlMap(doc,"pose",0,standard);
    expect(projected.ok() && !projected.warnings.empty() && projected.emittedAnchorCount == 130,
           "OpenPose raster projection explicitly reports detail reduction");
    const auto points = exportOpenPoseKeypoints(doc,"pose",0);
    expect(points.ok() && points.people.size() == 1 && points.people[0].body.size() == 25
        && points.people[0].leftHand.size() == 21 && points.people[0].face.size() == 70
        && !points.warnings.empty(), "structured OpenPose export includes body, hands and face");
    const auto &person = pose->people[0];
    const auto mouth = poseAnchors(person,PoseGroup::MouthOuterUpper)[0];
    expect(std::abs(points.people[0].face[48].y - (mouth.y-0.01)*512) < 1e-8,
           "expression targets actually deform exported mouth anchors");
    const auto artwork = renderFrame(doc,0);
    expect(artwork.ok() && std::all_of(artwork.pixels.pixels.begin(),artwork.pixels.pixels.end(),[](auto p) { return p == 0; }),
           "control pose does not contaminate artwork");
    const auto preview = renderFrameLayerTiles(doc,0,0,{{canvasRegion(doc),doc.extent}});
    expect(preview.ok() && preview.role == LayerRole::ControlNet, "pose has explicit per-layer preview");
}
}
int main()
{
    using namespace iiSharedCanvas;
    auto person = makeNeutralPosePerson("person");
    std::size_t count = 0;
    for (unsigned group = 0; group < static_cast<unsigned>(PoseGroup::Count); ++group) { count += poseAnchors(person,static_cast<PoseGroup>(group)).size(); }
    expect(count == 590 && PoseFaceAnchorCount == 523 && PoseEyeAnchorCount == 68 && PoseMouthAnchorCount == 143,
           "full body, fingers and expression-dense face have stable topology");
    expect(poseAnchors(person,PoseGroup::LeftUpperLid).size() == 17 && poseAnchors(person,PoseGroup::MouthInnerUpper).size() == 25,
           "eyes and inner lips remain separately editable");
    expect(person.leftHand[4].x < person.leftHand[0].x && person.rightHand[4].x > person.rightHand[0].x,
           "frontal template mirrors anatomical thumb direction");
    auto doc = fixture(); verify(doc); DocumentEditor editor(doc);
    auto sparse = doc;
    auto &sparsePerson = findPoseAsset(sparse,"neutral")->people[0];
    sparsePerson.body[0].x = 0; sparsePerson.body[0].y = 0;
    sparsePerson.leftHand[4].visibility = PoseVisibility::Missing;
    sparsePerson.rightHand[4].visibility = PoseVisibility::Occluded;
    const auto sparsePoints = exportOpenPoseKeypoints(sparse,"pose",0);
    expect(sparsePoints.ok() && sparsePoints.people[0].body[0].x == 0 && sparsePoints.people[0].body[0].confidence == 1
        && sparsePoints.people[0].leftHand[4].confidence == 0, "origin is valid and missingness is explicit");
    PoseRenderOptions visibleOnly; visibleOnly.includeOccluded = false;
    expect(renderPoseControlMap(sparse,"pose",0,visibleOnly).emittedAnchorCount == 588, "missing and excluded occluded anchors are omitted");
    auto disabled = doc; findPoseLayer(disabled,"pose")->control.enabled = false;
    expect(!renderPoseControlMap(disabled,"pose",0).ok(), "disabled pose control cannot be emitted");
    const auto baseline = encodeIisc(doc); const auto revision = editor.revision();
    auto invalid = *findPoseAsset(doc,"neutral"); invalid.people[0].body[0].x = std::numeric_limits<double>::quiet_NaN();
    expect(!editor.replacePoseAsset("neutral",invalid).ok(), "reject NaN geometry");
    invalid = *findPoseAsset(doc,"neutral"); invalid.people.push_back(invalid.people[0]);
    expect(!editor.replacePoseAsset("neutral",invalid).ok(), "reject duplicate person identity");
    invalid = *findPoseAsset(doc,"neutral"); invalid.people[0].expressions[0].deltas[0].index = 99;
    expect(!editor.replacePoseAsset("neutral",invalid).ok(), "reject invalid expression anchor references");
    expect(editor.revision() == revision && encodeIisc(doc).bytes == baseline.bytes, "rejected pose edits preserve state and revision");
    auto changed = poseAnchors(findPoseAsset(doc,"neutral")->people[0],PoseGroup::LeftUpperLid)[8]; changed.y -= 0.02;
    expect(editor.setPoseAnchor("neutral","person",PoseGroup::LeftUpperLid,8,changed).changed, "edit one dense eye anchor");
    expect(renderPoseControlMap(doc,"pose",0).pixels.pixels != renderPoseControlMap(decodeIisc(baseline.bytes).document,"pose",0).pixels.pixels,
           "eye-anchor changes alter actual pixels");
    changed.locked = true;
    expect(editor.setPoseAnchor("neutral","person",PoseGroup::LeftUpperLid,8,changed).changed, "lock pose anchor");
    changed.x += 0.01;
    expect(!editor.setPoseAnchor("neutral","person",PoseGroup::LeftUpperLid,8,changed).ok(), "locked anchor cannot move");
    expect(editor.setPoseExpressionWeight("neutral","person","smile",1).changed, "adjust expression deformation weight");
    const auto encoded = encodeIisc(doc); auto decoded = decodeIisc(encoded.bytes);
    expect(encoded.ok() && decoded.ok(), "pose snapshot round trip");
    if (decoded.ok()) {
        expect(findPoseAsset(decoded.document,"neutral")->people == findPoseAsset(doc,"neutral")->people, "all anchors, locks, depth and expression fields survive");
        expect(renderPoseControlMap(decoded.document,"pose",0).pixels.pixels == renderPoseControlMap(doc,"pose",0).pixels.pixels, "rendered pose survives snapshot");
    }
    auto legacy = doc; legacy.formatVersion.minor = 7;
    expect(!encodeIisc(legacy).ok(), "old format rejects native poses");
    SerializationLimits limits; limits.maximumPosePeople = 0;
    expect(!encodeIisc(doc,limits).ok() && !decodeIisc(encoded.bytes,limits).ok(), "pose allocation limits apply both ways");
    auto next = *findPoseAsset(doc,"neutral"); next.id = "next"; next.people[0].body[0].x += 0.1;
    expect(editor.insertPoseAsset(next).changed && editor.setKeyframedSource("pose",{{0,"neutral"},{1,"next"}}).changed, "dynamic pose source");
    expect(layerKind(doc.layers[0]) == LayerKind::DynamicVector
        && renderPoseControlMap(doc,"pose",0).pixels.pixels != renderPoseControlMap(doc,"pose",1).pixels.pixels, "current frame selects different joint content");
    expect(!renderPoseControlMap(doc,"pose",2).ok(), "out-of-range pose frame is rejected");
    PoseRenderOptions budget; budget.maximumPixels = 1;
    expect(!renderPoseControlMap(doc,"pose",0,budget).ok(), "bounded conditioning allocation");
    QTemporaryDir directory(QStringLiteral(IISHAREDCANVAS_TEST_OUTPUT_DIR "/pose-XXXXXX"));
    expect(directory.isValid(), "working-file directory");
    if (directory.isValid()) {
        DocumentFile file; const auto path = directory.filePath("pose.iisc").toStdString();
        expect(file.create(path,doc).ok(), "persist dynamic native pose");
        DocumentEditor bound(file);
        expect(bound.setPoseExpressionWeight("neutral","person","smile",0.25).changed, "synchronous expression edit");
        DocumentFile reopened; expect(reopened.open(path).ok(), "reopen pose working file");
        if (reopened.document()) { expect(findPoseAsset(*reopened.document(),"neutral")->people[0].expressions[0].weight == 0.25,
            "committed expression survives reopen"); }
        expect(exportPsd(doc,directory.filePath("pose.psd").toStdString()).code == MediaIoCode::UnsupportedFeature,
               "PSD rejects semantic pose loss");
        auto orphan = doc; orphan.layers.clear(); orphan.frames.clear();
        expect(exportTimelineInterchange(orphan,directory.filePath("timeline").toStdString()).code == MediaIoCode::UnsupportedFeature,
               "orphan pose assets cannot reach foreign vector casts");
    }
    return failures ? 1 : 0;
}
