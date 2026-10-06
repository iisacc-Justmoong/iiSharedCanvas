#include <iiSharedCanvas.h>
#include <QCoreApplication>
#include <QDir>
#include <algorithm>
#include <filesystem>
#include <iostream>
#include <type_traits>

using namespace iiSharedCanvas;
static_assert(std::is_same_v<decltype(StaticBitmapLayer{}.content), StaticBitmapContent>);
static_assert(std::is_same_v<decltype(StaticVectorLayer{}.content), StaticVectorContent>);
static_assert(std::is_same_v<decltype(DynamicBitmapLayer{}.content), DynamicBitmapContent>);
static_assert(std::is_same_v<decltype(DynamicVectorLayer{}.content), DynamicVectorContent>);
static_assert(!std::is_same_v<StaticBitmapContent, StaticVectorContent>);
static_assert(!std::is_same_v<StaticBitmapContent, DynamicBitmapContent>);
static_assert(!std::is_same_v<StaticVectorContent, DynamicVectorContent>);
static_assert(!std::is_same_v<DynamicBitmapContent, DynamicVectorContent>);
static_assert(!std::is_constructible_v<StaticBitmapContent, KeyframedSource>);
static_assert(!std::is_constructible_v<DynamicVectorContent, StaticSource>);
static_assert(!std::is_constructible_v<StaticBitmapContent, StaticVectorContent>);
static_assert(!std::is_constructible_v<DynamicBitmapContent, DynamicVectorContent>);
static_assert(static_cast<unsigned>(LayerKind::DynamicVector) == 3);
namespace {
int failures = 0;
void expect(bool value, const char *message) { if (!value) { std::cerr << message << '\n'; ++failures; } }
Document fixture() {
    Document d; d.extent = {2,2}; d.timeline.frameCount = 2;
    d.assets.emplace_back(RasterAsset{"r0",makeRasterLayer(2,2,0xffff0000U)});
    d.assets.emplace_back(RasterAsset{"r1",makeRasterLayer(2,2,0xff0000ffU)});
    VectorPath path; path.commands={MoveTo{{0,0}},LineTo{{2,0}},LineTo{{2,2}},LineTo{{0,2}},ClosePath{}};
    path.fill=SolidPaint{0xffff0000U};
    d.assets.emplace_back(VectorAsset{"v0",{2,2},{path}});
    path.fill=SolidPaint{0xff0000ffU}; d.assets.emplace_back(VectorAsset{"v1",{2,2},{path}});
    d.layers.emplace_back(StaticBitmapLayer{{"sb"},StaticBitmapContent{"r0"}});
    d.layers.emplace_back(StaticVectorLayer{{"sv"},StaticVectorContent{"v0"}});
    d.layers.emplace_back(DynamicBitmapLayer{{"db"},DynamicBitmapContent{KeyframedSource{{0,1}}}});
    d.layers.emplace_back(DynamicVectorLayer{{"dv"},DynamicVectorContent{KeyframedSource{{0,1}}}});
    d.frames={{0,{{"db","r0"},{"dv","v0"}}},{1,{{"db","r1"},{"dv","v1"}}}};
    return d;
}
void verify(const Document &d) {
    expect(validate(d).ok(),"four explicit content types validate");
    expect(findStaticBitmapLayer(d,"sb") && findStaticVectorLayer(d,"sv")
        && findDynamicBitmapLayer(d,"db") && findDynamicVectorLayer(d,"dv"),"typed lookup distinguishes all four alternatives");
    const LayerKind kinds[]={LayerKind::StaticBitmap,LayerKind::StaticVector,LayerKind::DynamicBitmap,LayerKind::DynamicVector};
    for(std::size_t i=0;i<4;++i) {
        expect(layerKind(d.layers[i])==kinds[i],"runtime identity is the explicit layer/content type");
        auto isolated=d; for(std::size_t j=0;j<4;++j) layerProperties(isolated.layers[j]).visible=i==j;
        for(FrameIndex frame=0;frame<2;++frame) {
            const auto result=renderFrame(isolated,frame);
            expect(result.ok() && result.pixels.pixels[0]==(i<2||frame==0?0xffff0000U:0xff0000ffU),
                   "each content has exactly its named static/dynamic bitmap/vector behavior");
        }
    }
}
}
int main(int argc,char **argv) {
    QCoreApplication app(argc,argv); QDir().mkpath(IISHAREDCANVAS_TEST_OUTPUT_DIR);
    auto d=fixture(); verify(d);
    auto detached=layerSource(d.layers.front());
    std::get<StaticSource>(detached).assetId="v0";
    expect(findStaticBitmapLayer(d,"sb")->content.assetId=="r0",
           "a detached generic source does not mutate typed content");
    auto bytes=encodeIisc(d); auto decoded=decodeIisc(bytes.bytes);
    expect(bytes.ok() && decoded.ok() && encodeIisc(decoded.document).bytes==bytes.bytes,"four content identities persist canonically");
    if(decoded.ok()) verify(decoded.document);
    for(std::uint16_t version=0;version<19;++version) {
        auto legacy=fixture(); legacy.formatVersion.minor=version;
        const auto original=encodeIisc(legacy); const auto restored=decodeIisc(original.bytes);
        expect(original.ok() && restored.ok() && encodeIisc(restored.document).bytes==original.bytes,
               "every legacy format migrates to typed memory without changing canonical legacy bytes");
        if(restored.ok()) verify(restored.document);
    }
    {
        auto one=fixture(); one.layers.resize(1); one.frames.clear();
        layerProperties(one.layers.front()).id="four-type-corruption-static-bitmap";
        auto encoded=encodeIisc(one);
        const std::string marker="four-type-corruption-static-bitmap";
        const auto id=std::search(encoded.bytes.begin(),encoded.bytes.end(),marker.begin(),marker.end());
        expect(id!=encoded.bytes.end(),"find independent corruption fixture layer id");
        if(id!=encoded.bytes.end()) {
            // id, empty name u32, visible, opacity, six affine doubles, blend,
            // no-artboard flag, then the explicit 1.19 identity byte.
            const auto offset=std::size_t(id-encoded.bytes.begin())+marker.size()+4+1+8+48+1+1;
            encoded.bytes[offset]=static_cast<std::uint8_t>(LayerKind::DynamicBitmap);
            std::uint32_t crc = 0xffffffffU;
            for (std::size_t i = IiscHeaderSize; i < encoded.bytes.size(); ++i) {
                crc ^= encoded.bytes[i];
                for (int bit = 0; bit < 8; ++bit)
                    crc = (crc >> 1U) ^ (0xedb88320U & (0U - (crc & 1U)));
            }
            crc ^= 0xffffffffU;
            for (unsigned i = 0; i < 4; ++i) encoded.bytes[24+i] = std::uint8_t(crc >> (8*i));
            expect(decodeIisc(encoded.bytes).error.code == IiscErrorCode::InvalidData,"a dynamic type with static content is rejected, never silently inferred");
        }
    }
    DocumentEditor editor(d);
    expect(editor.setStaticSource("db","r1").changed && findStaticBitmapLayer(d,"db") && !findDynamicBitmapLayer(d,"db"),
           "explicit source conversion replaces the concrete bitmap layer and content type together");
    expect(editor.setKeyframedSource("sv",{{0,"v0"},{1,"v1"}}).changed && findDynamicVectorLayer(d,"sv"),
           "explicit static-to-dynamic conversion replaces the concrete vector type");
    const auto prior=encodeIisc(d).bytes; const auto revision=editor.revision();
    expect(!editor.setStaticSource("dv","r0").ok() && encodeIisc(d).bytes==prior && editor.revision()==revision,
           "failed conversion restores the complete original concrete type and content");
    auto invalid=fixture(); findStaticBitmapLayer(invalid,"sb")->content.assetId="v0";
    expect(!validate(invalid).ok(),"static bitmap cannot own vector content even through raw aggregates");
    invalid=fixture(); findDynamicVectorLayer(invalid,"dv")->content.frameIndices={0};
    expect(!validate(invalid).ok(),"dynamic vector cannot discard its frame ownership contract");
    const auto path=std::string(IISHAREDCANVAS_TEST_OUTPUT_DIR "/four-layer-types.iisc"); std::filesystem::remove(path);
    DocumentFile file; expect(file.create(path,fixture()).ok(),"create four-type working document");
    DocumentEditor bound(file); expect(bound.setStaticSource("db","r1").changed,"persist concrete type conversion");
    file.close(); expect(file.open(path).ok() && findStaticBitmapLayer(*file.document(),"db"),"reopening preserves explicit converted type");
    return failures?1:0;
}
