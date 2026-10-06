#include "ControlNet/Pose.h"
#include "Document/Document.h"
#include "Render/FrameRenderer.h"
#include "Validation/Validation.h"
#include <algorithm>
#include <cmath>
#include <numbers>
#include <stdexcept>
#include <unordered_set>

namespace iiSharedCanvas {
std::span<PoseAnchor> poseAnchors(PosePerson &person, PoseGroup group) noexcept
{
    switch (group) {
    case PoseGroup::Body: return person.body;
    case PoseGroup::LeftHand: return person.leftHand;
    case PoseGroup::RightHand: return person.rightHand;
    case PoseGroup::Outline: return person.face.outline;
    case PoseGroup::LeftBrow: return person.face.leftBrow;
    case PoseGroup::RightBrow: return person.face.rightBrow;
    case PoseGroup::NoseBridge: return person.face.noseBridge;
    case PoseGroup::NoseContour: return person.face.noseContour;
    case PoseGroup::LeftUpperLid: return person.face.leftEye.upperLid;
    case PoseGroup::LeftLowerLid: return person.face.leftEye.lowerLid;
    case PoseGroup::LeftCrease: return person.face.leftEye.crease;
    case PoseGroup::LeftIris: return person.face.leftEye.iris;
    case PoseGroup::LeftPupil: return person.face.leftEye.pupil;
    case PoseGroup::RightUpperLid: return person.face.rightEye.upperLid;
    case PoseGroup::RightLowerLid: return person.face.rightEye.lowerLid;
    case PoseGroup::RightCrease: return person.face.rightEye.crease;
    case PoseGroup::RightIris: return person.face.rightEye.iris;
    case PoseGroup::RightPupil: return person.face.rightEye.pupil;
    case PoseGroup::MouthOuterUpper: return person.face.mouth.outerUpper;
    case PoseGroup::MouthOuterLower: return person.face.mouth.outerLower;
    case PoseGroup::MouthInnerUpper: return person.face.mouth.innerUpper;
    case PoseGroup::MouthInnerLower: return person.face.mouth.innerLower;
    case PoseGroup::UpperTeeth: return person.face.mouth.upperTeeth;
    case PoseGroup::LowerTeeth: return person.face.mouth.lowerTeeth;
    case PoseGroup::Tongue: return person.face.mouth.tongue;
    case PoseGroup::LeftCheek: return person.face.leftCheek;
    case PoseGroup::RightCheek: return person.face.rightCheek;
    case PoseGroup::Forehead: return person.face.forehead;
    case PoseGroup::LeftNasolabialFold: return person.face.leftNasolabialFold;
    case PoseGroup::RightNasolabialFold: return person.face.rightNasolabialFold;
    case PoseGroup::LeftUnderEye: return person.face.leftUnderEye;
    case PoseGroup::RightUnderEye: return person.face.rightUnderEye;
    default: return {};
    }
}

std::span<const PoseAnchor> poseAnchors(const PosePerson &person, PoseGroup group) noexcept
{
    switch (group) {
    case PoseGroup::Body: return person.body;
    case PoseGroup::LeftHand: return person.leftHand;
    case PoseGroup::RightHand: return person.rightHand;
    case PoseGroup::Outline: return person.face.outline;
    case PoseGroup::LeftBrow: return person.face.leftBrow;
    case PoseGroup::RightBrow: return person.face.rightBrow;
    case PoseGroup::NoseBridge: return person.face.noseBridge;
    case PoseGroup::NoseContour: return person.face.noseContour;
    case PoseGroup::LeftUpperLid: return person.face.leftEye.upperLid;
    case PoseGroup::LeftLowerLid: return person.face.leftEye.lowerLid;
    case PoseGroup::LeftCrease: return person.face.leftEye.crease;
    case PoseGroup::LeftIris: return person.face.leftEye.iris;
    case PoseGroup::LeftPupil: return person.face.leftEye.pupil;
    case PoseGroup::RightUpperLid: return person.face.rightEye.upperLid;
    case PoseGroup::RightLowerLid: return person.face.rightEye.lowerLid;
    case PoseGroup::RightCrease: return person.face.rightEye.crease;
    case PoseGroup::RightIris: return person.face.rightEye.iris;
    case PoseGroup::RightPupil: return person.face.rightEye.pupil;
    case PoseGroup::MouthOuterUpper: return person.face.mouth.outerUpper;
    case PoseGroup::MouthOuterLower: return person.face.mouth.outerLower;
    case PoseGroup::MouthInnerUpper: return person.face.mouth.innerUpper;
    case PoseGroup::MouthInnerLower: return person.face.mouth.innerLower;
    case PoseGroup::UpperTeeth: return person.face.mouth.upperTeeth;
    case PoseGroup::LowerTeeth: return person.face.mouth.lowerTeeth;
    case PoseGroup::Tongue: return person.face.mouth.tongue;
    case PoseGroup::LeftCheek: return person.face.leftCheek;
    case PoseGroup::RightCheek: return person.face.rightCheek;
    case PoseGroup::Forehead: return person.face.forehead;
    case PoseGroup::LeftNasolabialFold: return person.face.leftNasolabialFold;
    case PoseGroup::RightNasolabialFold: return person.face.rightNasolabialFold;
    case PoseGroup::LeftUnderEye: return person.face.leftUnderEye;
    case PoseGroup::RightUnderEye: return person.face.rightUnderEye;
    default: return {};
    }
}

namespace {
PoseAnchor anchor(double x, double y) { return {x,y,{},1,PoseVisibility::Visible,false}; }
template<std::size_t N> void arc(std::array<PoseAnchor,N> &points, double cx, double cy, double rx, double ry)
{
    for (std::size_t i=0; i<N; ++i) {
        const double t = double(i)/(N-1);
        points[i] = anchor(cx-rx+2*rx*t,cy+ry*std::sin(std::numbers::pi*t));
    }
}
void eye(PoseEye &value,double x)
{
    arc(value.upperLid,x,.16,.024,-.009); arc(value.lowerLid,x,.16,.024,.007);
    arc(value.crease,x,.147,.025,-.009);
    for (std::size_t i=0;i<value.iris.size();++i) {
        const auto angle=2*std::numbers::pi*i/value.iris.size();
        value.iris[i]=anchor(x+.006*std::cos(angle),.16+.006*std::sin(angle));
    }
    value.pupil[0]=anchor(x,.16);
}
bool visible(const PoseAnchor &a,const PoseRenderOptions &options)
{
    return a.visibility != PoseVisibility::Missing && a.confidence > 0 && a.confidence >= options.minimumConfidence
        && (options.includeOccluded || a.visibility != PoseVisibility::Occluded);
}
bool validOptions(const PoseRenderOptions &o)
{
    return static_cast<unsigned>(o.profile)<=static_cast<unsigned>(PoseRenderProfile::OpenPose)
        && std::isfinite(o.minimumConfidence) && o.minimumConfidence>=0 && o.minimumConfidence<=1
        && std::isfinite(o.pointRadius) && o.pointRadius>0 && o.pointRadius<=64
        && std::isfinite(o.lineWidth) && o.lineWidth>0 && o.lineWidth<=64;
}
std::array<PoseAnchor,70> face70(const PosePerson &p)
{
    std::array<PoseAnchor,70> result; const auto &f=p.face;
    for(int i=0;i<17;++i) result[i]=f.outline[i*2];
    for(int i=0;i<5;++i) { result[17+i]=f.rightBrow[i*4]; result[22+i]=f.leftBrow[i*4]; result[31+i]=f.noseContour[i*4]; }
    for(int i=0;i<4;++i) result[27+i]=f.noseBridge[std::array{0,3,5,8}[i]];
    for(int side=0;side<2;++side) {
        const auto &e=side?f.leftEye:f.rightEye; const int start=36+side*6;
        result[start]=e.upperLid[0]; result[start+1]=e.upperLid[5]; result[start+2]=e.upperLid[11];
        result[start+3]=e.upperLid[16]; result[start+4]=e.lowerLid[11]; result[start+5]=e.lowerLid[5];
    }
    for(int i=0;i<7;++i) result[48+i]=f.mouth.outerUpper[i*4];
    for(int i=0;i<5;++i) result[55+i]=f.mouth.outerLower[20-i*4];
    for(int i=0;i<5;++i) result[60+i]=f.mouth.innerUpper[i*6];
    for(int i=0;i<3;++i) result[65+i]=f.mouth.innerLower[18-i*6];
    result[68]=f.rightEye.pupil[0]; result[69]=f.leftEye.pupil[0]; return result;
}
constexpr std::array<int,18> coco{0,1,2,3,4,5,6,7,9,10,11,12,13,14,15,16,17,18};
constexpr std::array<std::array<int,2>,17> limbs{{{1,2},{1,5},{2,3},{3,4},{5,6},{6,7},{1,8},{8,9},{9,10},{1,11},{11,12},{12,13},{1,0},{0,14},{14,16},{0,15},{15,17}}};
constexpr std::array<std::uint32_t,18> palette{0xffff0000,0xffff5500,0xffffaa00,0xffffff00,0xffaaff00,0xff55ff00,0xff00ff00,0xff00ff55,0xff00ffaa,0xff00ffff,0xff00aaff,0xff0055ff,0xff0000ff,0xff5500ff,0xffaa00ff,0xffff00ff,0xffff00aa,0xffff0055};
const PoseAsset *resolved(const Document &doc,const std::string &id, std::uint32_t frame,std::string &message)
{
    const auto validation=validate(doc);
    if(!validation.ok()) { message=validation.issues.front().message; return nullptr; }
    const auto *base=findLayer(doc,id); const auto *layer=base?std::get_if<PoseLayer>(base):nullptr;
    if(!layer || !layer->control.enabled) { message="pose layer is missing or disabled"; return nullptr; }
    const auto *asset=resolveAssetAt(doc,*base,frame); const auto *pose=asset?std::get_if<PoseAsset>(asset):nullptr;
    if(!pose) message="pose frame is outside the source range or timeline";
    return pose;
}
}

PosePerson makeNeutralPosePerson(std::string id)
{
    PosePerson p; p.id=std::move(id); p.name="Neutral pose";
    constexpr std::array<std::array<double,2>,25> body{{{.5,.19},{.5,.28},{.39,.3},{.32,.44},{.28,.58},{.61,.3},{.68,.44},{.72,.58},{.5,.57},{.44,.58},{.43,.76},{.42,.92},{.56,.58},{.57,.76},{.58,.92},{.465,.16},{.535,.16},{.42,.18},{.58,.18},{.565,.97},{.6,.97},{.58,.94},{.435,.97},{.4,.97},{.42,.94}}};
    for(std::size_t i=0;i<body.size();++i) p.body[i]=anchor(body[i][0],body[i][1]);
    for(int side=0;side<2;++side) {
        auto &hand=side?p.leftHand:p.rightHand; const double x=side?.72:.28;
        hand[0]=anchor(x,.58);
        for(int finger=0;finger<5;++finger) for(int joint=0;joint<4;++joint) {
            const double spread=(finger-2)*.013;
            hand[1+finger*4+joint]=anchor(x+spread*(side?1:-1)*(1+joint*.22),.602+joint*(finger==0?.012:.019));
        }
    }
    arc(p.face.outline,.5,.13,.082,.13); arc(p.face.rightBrow,.465,.135,.025,-.008); arc(p.face.leftBrow,.535,.135,.025,-.008);
    for(std::size_t i=0;i<9;++i) p.face.noseBridge[i]=anchor(.5,.15+i*.006);
    arc(p.face.noseContour,.5,.198,.019,.008); eye(p.face.leftEye,.535); eye(p.face.rightEye,.465);
    arc(p.face.mouth.outerUpper,.5,.226,.035,-.01); arc(p.face.mouth.outerLower,.5,.226,.035,.014);
    arc(p.face.mouth.innerUpper,.5,.226,.026,-.004); arc(p.face.mouth.innerLower,.5,.226,.026,.007);
    arc(p.face.mouth.upperTeeth,.5,.226,.022,-.001); arc(p.face.mouth.lowerTeeth,.5,.23,.022,.001); arc(p.face.mouth.tongue,.5,.232,.014,.001);
    arc(p.face.leftCheek,.548,.2,.027,.007); arc(p.face.rightCheek,.452,.2,.027,.007); arc(p.face.forehead,.5,.11,.054,-.014);
    arc(p.face.leftNasolabialFold,.536,.214,.013,.015); arc(p.face.rightNasolabialFold,.464,.214,.013,.015);
    arc(p.face.leftUnderEye,.535,.178,.025,.006); arc(p.face.rightUnderEye,.465,.178,.025,.006);
    return p;
}

PosePerson evaluatePosePerson(const PosePerson &person)
{
    PosePerson result=person;
    for(const auto &expression:person.expressions) for(const auto &delta:expression.deltas) {
        auto points=poseAnchors(result,delta.group);
        if(delta.index>=points.size()) throw std::invalid_argument("expression references an unknown pose anchor");
        auto &a=points[delta.index]; a.x+=expression.weight*delta.dx; a.y+=expression.weight*delta.dy;
        if(delta.dz!=0 && expression.weight!=0) a.z=a.z.value_or(0)+expression.weight*delta.dz;
    }
    return result;
}

std::string validatePoseAsset(const PoseAsset &asset)
{
    if(asset.viewport.width<=0 || asset.viewport.height<=0 || asset.viewport.width>32768 || asset.viewport.height>32768 || asset.people.size()>64)
        return "pose viewport must be within 1..32768 per axis and contain at most 64 people";
    std::unordered_set<std::string> ids;
    const auto coordinate=[](double v) { return std::isfinite(v) && std::abs(v)<=16; };
    for(const auto &person:asset.people) {
        if(person.id.empty() || !ids.insert(person.id).second || person.expressions.size()>128) return "pose people require unique nonempty ids and at most 128 expression targets";
        for(unsigned group=0;group<static_cast<unsigned>(PoseGroup::Count);++group) for(const auto &a:poseAnchors(person,static_cast<PoseGroup>(group))) {
            if(!coordinate(a.x)||!coordinate(a.y)||(a.z&&!coordinate(*a.z))||!std::isfinite(a.confidence)||a.confidence<0||a.confidence>1
                ||static_cast<unsigned>(a.visibility)>static_cast<unsigned>(PoseVisibility::Occluded)) return "pose anchor coordinates, confidence or visibility are invalid";
        }
        std::unordered_set<std::string> expressions;
        for(const auto &e:person.expressions) {
            if(e.id.empty()||!expressions.insert(e.id).second||!std::isfinite(e.weight)||e.weight<0||e.weight>1||e.deltas.size()>PosePersonAnchorCount) return "expression ids, weights or delta count are invalid";
            std::unordered_set<std::uint64_t> addresses;
            for(const auto &d:e.deltas) if(d.index>=poseAnchors(person,d.group).size()||!coordinate(d.dx)||!coordinate(d.dy)||!coordinate(d.dz)
                ||!addresses.insert(std::uint64_t(d.group)*1024+d.index).second) return "expression delta has invalid or duplicate anchor address";
        }
        const auto evaluated=evaluatePosePerson(person);
        for(unsigned group=0;group<static_cast<unsigned>(PoseGroup::Count);++group) for(const auto &a:poseAnchors(evaluated,static_cast<PoseGroup>(group)))
            if(!coordinate(a.x)||!coordinate(a.y)||(a.z&&!coordinate(*a.z))) return "combined expressions exceed coordinate bounds";
    }
    return {};
}

PoseAsset *findPoseAsset(Document &doc,const std::string &id) noexcept { auto *a=findAsset(doc,id); return a?std::get_if<PoseAsset>(a):nullptr; }
const PoseAsset *findPoseAsset(const Document &doc,const std::string &id) noexcept { const auto *a=findAsset(doc,id); return a?std::get_if<PoseAsset>(a):nullptr; }
PoseLayer *findPoseLayer(Document &doc,const std::string &id) noexcept { auto *l=findLayer(doc,id); return l?std::get_if<PoseLayer>(l):nullptr; }
const PoseLayer *findPoseLayer(const Document &doc,const std::string &id) noexcept { const auto *l=findLayer(doc,id); return l?std::get_if<PoseLayer>(l):nullptr; }

VectorAsset poseVectorPreview(const PoseAsset &asset,const PoseRenderOptions &options)
{
    if(!validatePoseAsset(asset).empty() || !validOptions(options)) throw std::invalid_argument("poseVectorPreview requires a validated pose and render options");
    VectorAsset out; out.id="pose-preview"; out.viewport=asset.viewport;
    const auto point=[&](const PoseAnchor &a) { return Point{a.x*asset.viewport.width,a.y*asset.viewport.height}; };
    const auto dot=[&](const PoseAnchor &a,std::uint32_t color) {
        if(!visible(a,options)) return;
        const auto center=point(a); VectorPath path; path.fill=SolidPaint{color};
        for(int i=0;i<12;++i) { const auto angle=2*std::numbers::pi*i/12; const Point p{center.x+options.pointRadius*std::cos(angle),center.y+options.pointRadius*std::sin(angle)};
            if(i==0) path.commands.emplace_back(MoveTo{p}); else path.commands.emplace_back(LineTo{p}); }
        path.commands.emplace_back(ClosePath{}); out.paths.push_back(std::move(path));
    };
    const auto line=[&](const PoseAnchor &a,const PoseAnchor &b,std::uint32_t color) {
        if(!visible(a,options)||!visible(b,options)) return;
        VectorPath path; path.stroke=StrokeStyle{SolidPaint{color},options.lineWidth}; path.commands={MoveTo{point(a)},LineTo{point(b)}}; out.paths.push_back(std::move(path));
    };
    for(const auto &authored:asset.people) {
        if(!authored.enabled) continue; const auto p=evaluatePosePerson(authored);
        for(std::size_t i=0;i<limbs.size();++i) line(p.body[coco[limbs[i][0]]],p.body[coco[limbs[i][1]]],palette[i]);
        if(options.profile==PoseRenderProfile::NativeDetailed) {
            for(const auto pair:std::array<std::array<int,2>,9>{{{1,8},{8,9},{8,12},{14,19},{19,20},{14,21},{11,22},{22,23},{11,24}}}) line(p.body[pair[0]],p.body[pair[1]],0xff00ff88);
            for(std::size_t i=0;i<p.body.size();++i) dot(p.body[i],palette[i%palette.size()]);
        } else for(std::size_t i=0;i<coco.size();++i) dot(p.body[coco[i]],palette[i]);
        for(const auto *hand:{&p.leftHand,&p.rightHand}) {
            for(int finger=0;finger<5;++finger) for(int joint=0;joint<4;++joint) {
                const int index=1+finger*4+joint; line((*hand)[joint?index-1:0],(*hand)[index],palette[finger*3]);
            }
            for(const auto &a:*hand) dot(a,0xff0000ff);
        }
        if(options.profile==PoseRenderProfile::NativeDetailed) {
            for(unsigned group=static_cast<unsigned>(PoseGroup::Outline);group<static_cast<unsigned>(PoseGroup::Count);++group) {
                const auto points=poseAnchors(p,static_cast<PoseGroup>(group));
                for(std::size_t i=0;i<points.size();++i) { dot(points[i],0xffffffff); if(i) line(points[i-1],points[i],0xffffffff); }
                if(group==static_cast<unsigned>(PoseGroup::LeftIris)||group==static_cast<unsigned>(PoseGroup::RightIris)) line(points.back(),points.front(),0xffffffff);
            }
        } else {
            const auto face=face70(p); for(const auto &a:face) dot(a,0xffffffff);
            for(const auto range:std::array<std::array<int,3>,9>{{{0,16,0},{17,21,0},{22,26,0},{27,30,0},{31,35,0},{36,41,1},{42,47,1},{48,59,1},{60,67,1}}}) {
                for(int i=range[0]+1;i<=range[1];++i) line(face[i-1],face[i],0xffffffff);
                if(range[2]) line(face[range[1]],face[range[0]],0xffffffff);
            }
        }
    }
    return out;
}

PoseControlMapResult renderPoseControlMap(const Document &doc,const std::string &id,std::uint32_t frame,const PoseRenderOptions &options)
{
    PoseControlMapResult result; const auto *asset=resolved(doc,id,frame,result.message); if(!asset) return result;
    if(!validOptions(options)||std::uint64_t(asset->viewport.width)*asset->viewport.height>options.maximumPixels) { result.message="invalid pose render options or pixel budget exceeded"; return result; }
    Document preview; preview.extent=asset->viewport;
    preview.assets={RasterAsset{"background",makeRasterLayer(asset->viewport.width,asset->viewport.height,0xff000000)},poseVectorPreview(*asset,options)};
    preview.layers={StaticBitmapLayer{{"background"},StaticSource{"background"}},StaticVectorLayer{{"pose"},StaticSource{"pose-preview"}}};
    const auto rendered=renderFrame(preview,0); if(!rendered.ok()) { result.message=rendered.message; return result; } result.pixels=rendered.pixels;
    for(const auto &authored:asset->people) {
        if(!authored.enabled) continue; const auto p=evaluatePosePerson(authored); result.nativeAnchorCount+=PosePersonAnchorCount;
        if(options.profile==PoseRenderProfile::NativeDetailed) {
            for(unsigned g=0;g<static_cast<unsigned>(PoseGroup::Count);++g) for(const auto &a:poseAnchors(p,static_cast<PoseGroup>(g))) result.emittedAnchorCount+=visible(a,options);
        } else {
            for(auto i:coco) result.emittedAnchorCount+=visible(p.body[i],options);
            for(const auto &a:p.leftHand) result.emittedAnchorCount+=visible(a,options);
            for(const auto &a:p.rightHand) result.emittedAnchorCount+=visible(a,options);
            for(const auto &a:face70(p)) result.emittedAnchorCount+=visible(a,options);
        }
    }
    if(options.profile==PoseRenderProfile::OpenPose) result.warnings.push_back("Projection uses COCO18 body, 21+21 hands and 70 face points; dense eye/mouth details and foot joints are omitted from this raster, retained in native assets. Renderer is not pixel-identical to a model annotator.");
    return result;
}

OpenPoseKeypointsResult exportOpenPoseKeypoints(const Document &doc,const std::string &id,std::uint32_t frame)
{
    OpenPoseKeypointsResult result; const auto *asset=resolved(doc,id,frame,result.message); if(!asset) return result;
    const auto convert=[&](const PoseAnchor &a) { return a.visibility==PoseVisibility::Missing?PoseKeypoint{}:PoseKeypoint{a.x*asset->viewport.width,a.y*asset->viewport.height,a.confidence}; };
    for(const auto &authored:asset->people) {
        if(!authored.enabled) continue; const auto p=evaluatePosePerson(authored); OpenPosePerson out; out.id=p.id;
        for(std::size_t i=0;i<25;++i) out.body[i]=convert(p.body[i]);
        for(std::size_t i=0;i<21;++i) { out.leftHand[i]=convert(p.leftHand[i]); out.rightHand[i]=convert(p.rightHand[i]); }
        const auto face=face70(p); for(std::size_t i=0;i<70;++i) out.face[i]=convert(face[i]); result.people.push_back(std::move(out));
    }
    result.warnings.push_back("BODY_25/21+21/70 projection samples dense face contours; depth, locks, crease/iris/teeth/tongue detail and expression definitions remain in native assets only.");
    return result;
}
} // namespace iiSharedCanvas
