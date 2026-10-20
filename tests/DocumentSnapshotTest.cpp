#include <iiSharedCanvas.h>
#include <QCoreApplication>
#include <QFile>
#include <QTemporaryDir>
#include <sqlite3.h>
#include <filesystem>
#include <iostream>

namespace {
using namespace iiSharedCanvas;
int failures=0;
void expect(bool value,const std::string &message) {
    if(!value){++failures;std::cerr<<message<<'\n';}
}
Document artwork(std::uint32_t color) {
    Document d;d.extent={5001,2};d.timeline={{30000,1001},4};
    d.assets.emplace_back(RasterAsset{"pixels",makeRasterLayer(5001,2,color)});
    d.layers.emplace_back(StaticBitmapLayer{{"layer","Original"},StaticSource{"pixels"}});
    d.audioAssets.push_back({"sound",48000,1,{1,-2,3,-4}});
    return d;
}
QByteArray bytes(const QString &path) {
    QFile file(path);return file.open(QIODevice::ReadOnly)?file.readAll():QByteArray{};
}
auto encoded(const Document &d){return encodeIisc(d).bytes;}
bool writeSnapshot(const QString &path,const Document &d) {
    const auto data=encodeIisc(d);if(!data.ok())return false;
    QFile file(path);return file.open(QIODevice::WriteOnly|QIODevice::NewOnly)
        && file.write(reinterpret_cast<const char *>(data.bytes.data()),qint64(data.bytes.size()))==qint64(data.bytes.size());
}
bool execute(sqlite3 *db,const std::string &sql) {
    char *error=nullptr;const auto code=sqlite3_exec(db,sql.c_str(),nullptr,nullptr,&error);
    if(code!=SQLITE_OK){expect(false,error?error:"SQLite fixture command failed");sqlite3_free(error);return false;}
    return true;
}
}
int main(int argc,char **argv) {
    QCoreApplication application(argc,argv);
    QTemporaryDir root(IISHAREDCANVAS_TEST_OUTPUT_DIR "/snapshot-read-XXXXXX");
    expect(root.isValid(),"private fixture directory exists");if(!root.isValid())return 1;
    auto original=artwork(0x80ff0000U);
    for(const auto minor:{std::uint16_t(19),CurrentFormatMinor}) {
        original.formatVersion.minor=minor;
        const auto path=root.filePath(QString("original-%1 # 한글.iisc").arg(minor));
        expect(writeSnapshot(path,original),"legacy fixture writes");
        const auto native=std::filesystem::path(path.toStdU16String());
        const auto before=bytes(path);const auto modified=std::filesystem::last_write_time(native);
        Document snapshot;const auto result=DocumentFile::readSnapshot(path.toStdString(),snapshot);
        expect(result.ok(),"read-only legacy snapshot succeeds: "+result.message);
        expect(encoded(snapshot)==encoded(original),"all typed data, original extent and format are preserved");
        expect(snapshot.extent.width==5001,"full-resolution snapshot is not capped to preview bounds");
        expect(bytes(path)==before&&std::filesystem::last_write_time(native)==modified,"legacy source is unchanged");
        expect(!QFile::exists(path+"-wal")&&!QFile::exists(path+"-shm"),"legacy source has no database sidecars");
        SerializationLimits limits;limits.maximumContainerBytes=8;
        const auto prior=encoded(snapshot);
        expect(DocumentFile::readSnapshot(path.toStdString(),snapshot,limits).code==DocumentFileCode::LimitExceeded,"byte budget is enforced");
        expect(encoded(snapshot)==prior,"a failed read preserves its output");
    }
    original=artwork(0xffff0000U);const auto working=root.filePath("working.iisc");
    DocumentFile owner;expect(owner.create(working.toStdString(),original).ok(),"authoring fixture opens");
    Document detached;const auto revision=owner.revision(),generation=owner.bindingGeneration();
    expect(DocumentFile::readSnapshot(working.toStdString(),detached).ok(),"an open working file can be read without another source authoring owner");
    const auto authoring=encoded(*owner.document());
    std::get<RasterAsset>(detached.assets.front()).pixels.pixels.front()=0;
    expect(encoded(*owner.document())==authoring&&owner.revision()==revision&&owner.bindingGeneration()==generation,"detached edits do not change the live owner");
    owner.close();
    const auto replacement=root.filePath("replacement.iisc");const auto blue=artwork(0xff0000ffU);
    expect(owner.create(replacement.toStdString(),blue).ok(),"replacement records exist");
    const auto expected=encoded(*owner.document());owner.close();
    sqlite3 *db=nullptr;expect(sqlite3_open(working.toUtf8().constData(),&db)==SQLITE_OK,"WAL fixture opens");
    if(db) {
        auto attach=replacement;attach.replace("'","''");
        execute(db,"PRAGMA journal_mode=WAL");execute(db,"PRAGMA wal_autocheckpoint=0");
        execute(db,"ATTACH DATABASE '"+attach.toStdString()+"' AS changed");
        execute(db,"BEGIN IMMEDIATE");execute(db,"DELETE FROM main.canvas_records");
        execute(db,"INSERT INTO main.canvas_records(kind,id,position,data,digest) SELECT kind,id,position,data,digest FROM changed.canvas_records");
        execute(db,"UPDATE main.canvas_state SET revision=revision+1");execute(db,"COMMIT");
        const auto before=bytes(working),wal=bytes(working+"-wal");expect(!wal.isEmpty(),"committed source records reside in WAL");
        expect(DocumentFile::readSnapshot(working.toStdString(),detached).ok(),"WAL snapshot reads");
        expect(encoded(detached)==expected,"latest committed WAL records are read, not stale main-file pixels");
        expect(bytes(working)==before&&bytes(working+"-wal")==wal,"read preserves source database and committed WAL bytes");
        sqlite3_close(db);
    }
    const auto before=bytes(working);QFile::setPermissions(working,QFileDevice::ReadOwner);
    expect(DocumentFile::readSnapshot(working.toStdString(),detached).ok(),"read-only working source succeeds");
    expect(bytes(working)==before,"read-only source remains unchanged");
    QFile::setPermissions(working,QFileDevice::ReadOwner|QFileDevice::WriteOwner);
    const auto prior=encoded(detached);
    for(const auto &path:{std::string{},std::string("bad\0path",8),std::string("https://example.invalid/image.iisc"),root.filePath("missing.iisc").toStdString()}) {
        expect(!DocumentFile::readSnapshot(path,detached).ok(),"invalid/missing source is rejected");
        expect(encoded(detached)==prior,"invalid input cannot replace the detached result");
    }
    const auto corrupt=root.filePath("corrupt.iisc");QFile file(corrupt);expect(file.open(QIODevice::WriteOnly),"corrupt fixture opens");file.write("not a native canvas");file.close();
    expect(DocumentFile::readSnapshot(corrupt.toStdString(),detached).code==DocumentFileCode::UnsupportedFormat,"foreign files are rejected");
    expect(encoded(detached)==prior,"foreign input preserves prior output");
    std::cout<<"Document snapshot assertions: "<<(failures?"FAILED":"passed")<<'\n';return failures?1:0;
}
