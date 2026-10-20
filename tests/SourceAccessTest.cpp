#include <Document/Document.h>
#include <File/DocumentFile.h>
#include <iiFileProviderPresentation.h>
#include <QCoreApplication>
#include <QTemporaryDir>
#include <future>
#include <mutex>
#include <chrono>
#include <iostream>
#include <atomic>
#include <sqlite3.h>
namespace {
struct Observation {
    std::string path;
    std::atomic_bool armed{false}, sourceHeld{false}, beganWhileHeld{false};
    std::atomic_int begins{0};
    std::promise<void> statement;
    const sqlite3_io_methods *original = nullptr;
    sqlite3_io_methods methods{};
};
Observation *observation=nullptr;
sqlite3_vfs *originalVfs=nullptr;
thread_local bool writerThread=false;
int observeLock(sqlite3_file *file,int level) {
    auto &seen=*observation;
    if(seen.armed && level==SQLITE_LOCK_RESERVED) {
        seen.beganWhileHeld.store(seen.sourceHeld.load());
        if(seen.begins.fetch_add(1)==0)seen.statement.set_value();
    }
    return seen.original->xLock(file,level);
}
int observeOpen(sqlite3_vfs *,const char *path,sqlite3_file *file,int flags,int *outFlags) {
    const auto result=originalVfs->xOpen(originalVfs,path,file,flags,outFlags);
    if(result==SQLITE_OK && writerThread && path && observation->path==path
        && (flags&SQLITE_OPEN_MAIN_DB)) {
        auto &seen=*observation;
        seen.original=file->pMethods;
        seen.methods=*file->pMethods;
        seen.methods.xLock=observeLock;
        file->pMethods=&seen.methods;
    }
    return result;
}
}
using namespace std::chrono_literals;
using namespace iiSharedCanvas;
int main(int argc,char **argv) {
    QCoreApplication app(argc,argv);
    QTemporaryDir directory(QStringLiteral(IISHAREDCANVAS_TEST_OUTPUT_DIR)+"/.source-transaction-XXXXXX");
    if(!directory.isValid()){std::cerr<<directory.errorString().toStdString();return 2;}
    const auto path=directory.filePath("working.iisc").toStdString();
    Document document;document.extent={16,16};document.timeline.frameCount=24;
    DocumentFile created;auto result=created.create(path,document);
    if(!result.ok()){std::cerr<<result.message;return 2;}created.close();
    Observation seen;seen.path=path;observation=&seen;
    originalVfs=sqlite3_vfs_find(nullptr);
    if(!originalVfs)return 2;
    auto vfs=*originalVfs;vfs.pNext=nullptr;vfs.zName="iiSharedCanvasSourceAccessTest";vfs.xOpen=observeOpen;
    if(sqlite3_vfs_register(&vfs,1)!=SQLITE_OK)return 2;
    struct RestoreVfs {
        sqlite3_vfs *vfs;
        ~RestoreVfs(){sqlite3_vfs_register(originalVfs,1);sqlite3_vfs_unregister(vfs);}
    } restore{&vfs};
    auto statementStarted=seen.statement.get_future();
    std::promise<bool> opened;auto ready=opened.get_future();
    std::promise<void> begin,callback;auto start=begin.get_future();auto callbackEntered=callback.get_future();
    auto writing=std::async(std::launch::async,[&]{
        writerThread=true;
        DocumentFile writer;auto open=writer.open(path);opened.set_value(open.ok());if(!open.ok()){std::cerr<<"open: "<<open.message<<'\n';return open;}
        start.wait();seen.armed=true;return writer.edit([&](Document &draft){draft.timeline.frameCount=48;callback.set_value();return true;});
    });
    if(!ready.get()){writing.get();return 2;}
    bool callbackBeforeIO=false;
    {
        iiFileProvider::native::SourceAccess access(std::filesystem::u8path(path));std::lock_guard lock(access);
        seen.sourceHeld=true;
        begin.set_value();callbackBeforeIO=callbackEntered.wait_for(2s)==std::future_status::ready;
        statementStarted.wait_for(500ms);
        seen.sourceHeld=false;
    }
    const auto edited=writing.get();
    DocumentFile reopened;const auto read=reopened.open(path);
    const bool persisted=read.ok()&&reopened.document()->timeline.frameCount==48&&reopened.revision()==1;
    reopened.close();
    observation=nullptr;
    if(!callbackBeforeIO||seen.beganWhileHeld||seen.begins!=1||!edited.ok()||!persisted) {
        std::cerr<<"callbackBeforeIO="<<callbackBeforeIO<<" beganWhileHeld="<<seen.beganWhileHeld<<" begins="<<seen.begins<<" persisted="<<persisted<<" error="<<edited.message<<'\n';return 1;
    }
    return 0;
}
