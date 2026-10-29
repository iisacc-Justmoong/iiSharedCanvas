#include <iiSharedCanvasNative.h>
#include <iiFileProviderDatabase.h>
#include <iiFileProviderNative.h>
#include "LegacyWorkingFileFixture_p.hpp"
#include <algorithm>
#include <cstdlib>
#include <iostream>

using namespace iiSharedCanvas;
namespace sql = iiFileProvider::native::sql;
namespace provider = iiFileProvider::native;
#define CHECK(x) do { if (!(x)) { std::cerr << __LINE__ << ": " #x "\n"; std::exit(1); } } while (false)

namespace {
std::int64_t scalar(sql::Database &db, std::string_view text)
{
    auto statement = db.prepare(text); CHECK(statement);
    auto step = statement->step(); CHECK(step && *step == sql::Step::row);
    auto value = statement->value(0); CHECK(value);
    return std::get<std::int64_t>(*value);
}
sql::Blob unhex(std::string_view text)
{
    const auto digit = [](char c) { return c <= '9' ? c - '0' : (c <= 'F' ? c - 'A' + 10 : c - 'a' + 10); };
    sql::Blob bytes;
    for (std::size_t i = 0; i < text.size(); i += 2)
        bytes.push_back(std::uint8_t((digit(text[i]) << 4) | digit(text[i + 1])));
    return bytes;
}
void legacy(const std::filesystem::path &path)
{
    auto db = sql::Database::open(path, {.mode=sql::OpenMode::create}); CHECK(db);
    CHECK(db->execute("PRAGMA application_id=1229542211"));
    CHECK(db->execute("PRAGMA user_version=1"));
    CHECK(db->execute("CREATE TABLE canvas_state (singleton INTEGER PRIMARY KEY CHECK(singleton=1), revision INTEGER NOT NULL CHECK(revision>=0))"));
    CHECK(db->execute("CREATE TABLE canvas_records (kind INTEGER NOT NULL, id TEXT NOT NULL, position INTEGER NOT NULL CHECK(position>=0), data BLOB NOT NULL, digest BLOB NOT NULL, UNIQUE(kind,id))"));
    CHECK(db->execute("INSERT INTO canvas_state VALUES(1,0)"));
    for (const auto &record : loadLegacyWorkingRecords(
             std::filesystem::u8path(IISHAREDCANVAS_TEST_RESOURCE_DIR) / "fixtures/legacy-working-v1.tsv")) {
        auto insert = db->prepare("INSERT INTO canvas_records VALUES(?,?,?,?,?)"); CHECK(insert);
        CHECK(insert->bind(1, std::int64_t(record.kind))); CHECK(insert->bind(2, std::string(record.id)));
        CHECK(insert->bind(3, std::int64_t(record.position))); CHECK(insert->bind(4, unhex(record.data)));
        CHECK(insert->bind(5, unhex(record.digest))); CHECK(insert->step());
    }
}
} // namespace

int main(int argc, char **argv)
{
    if (argc == 3 && (std::string_view(argv[1]) == "--legacy-fixture" || std::string_view(argv[1]) == "--bad-fixture")) {
        legacy(std::filesystem::path(argv[2]));
        if (std::string_view(argv[1]) == "--bad-fixture") {
            auto db=sql::Database::open(std::filesystem::path(argv[2])); CHECK(db);
            CHECK(db->execute("UPDATE canvas_records SET digest=zeroblob(32)"));
        }
        return 0;
    }
    const std::filesystem::path parent(IISHAREDCANVAS_TEST_OUTPUT_DIR);
    std::filesystem::create_directories(parent);
    provider::TemporaryDirectory temporary{parent};
    const auto path = temporary.path()/"tiles.iisc";
    Document document; document.extent={130,70};
    auto paint = makeRasterLayer(130,70);
    paint.pixels[0]=0x00112233; paint.pixels[64]=0xffabcdef;
    paint.pixels[69*130+129]=0x80765432;
    document.assets.emplace_back(RasterAsset{"paint",std::move(paint)});
    document.assets.emplace_back(RasterAsset{"mask",makeRasterLayer(65,65,0x7f112233)});
    document.assets.emplace_back(VideoAsset{"video",{24,1},{makeRasterLayer(2,2,0xff123456)}});
    const auto encoded=encodeIisc(document); CHECK(encoded.ok());
    DocumentFile file; auto created=file.create(path.string(),document);
    if (!created.ok()) std::cerr<<created.message<<'\n';
    CHECK(created.ok() && file.storageVersion()==2);
    CHECK(encodeIisc(*file.document()).bytes==encoded.bytes);
    {
        auto db=sql::Database::open(path,{.mode=sql::OpenMode::read_only}); CHECK(db);
        CHECK(scalar(*db,"PRAGMA user_version")==2);
        CHECK(scalar(*db,"SELECT count(*) FROM canvas_tiles")==7);
        CHECK(scalar(*db,"SELECT tile_count FROM canvas_tile_state")==7);
        CHECK(scalar(*db,"SELECT length(data) FROM canvas_records WHERE id='paint' AND kind=1")<100);
        CHECK(scalar(*db,"SELECT count(*) FROM canvas_tiles WHERE asset='video'")==0);
    }
    const auto beforeNoop=provider::File::read(path,1024*1024);
    CHECK(file.edit([](Document &){return true;}).ok());
    CHECK(file.revision()==0 && provider::File::read(path,1024*1024)==beforeNoop);
    CHECK(file.edit([](Document &d){std::get<RasterAsset>(d.assets[0]).pixels.pixels[1]=0xff987654;return true;}).ok());
    CHECK(file.revision()==1 && file.lastWriteStatistics().payloadBytesWritten<1024);
    DocumentFileInfo info; CHECK(DocumentFile::readStorageInfo(path.string(),info).ok());
    CHECK(info.storageVersion==2 && info.revision==1);
    auto preserved=info; CHECK(!DocumentFile::readStorageInfo((temporary.path()/"absent").string(),info).ok());
    CHECK(info.storageVersion==preserved.storageVersion && info.revision==preserved.revision);
    const auto changed=encodeIisc(*file.document()).bytes;
    Document snapshot; CHECK(DocumentFile::readSnapshot(path.string(),snapshot).ok());
    CHECK(encodeIisc(snapshot).bytes==changed);
#ifndef _WIN32
    // macOS exposes its normal TMPDIR through /var -> /private/var. Exercise
    // the same ancestor symlink entirely inside the test output directory.
    const auto actualTemp=temporary.path()/"real-temp";
    const auto aliasTemp=temporary.path()/"temp-alias";
    std::filesystem::create_directory(actualTemp);
    std::filesystem::create_directory_symlink(actualTemp,aliasTemp);
    const char *oldTemp=std::getenv("TMPDIR");
    const bool hadTemp=oldTemp!=nullptr;
    const std::string previousTemp=hadTemp ? oldTemp : "";
    CHECK(::setenv("TMPDIR",aliasTemp.c_str(),1)==0);
    Document aliasedSnapshot;
    const auto aliasedResult=DocumentFile::readSnapshot(path.string(),aliasedSnapshot);
    if (hadTemp) CHECK(::setenv("TMPDIR",previousTemp.c_str(),1)==0);
    else CHECK(::unsetenv("TMPDIR")==0);
    CHECK(aliasedResult.ok() && encodeIisc(aliasedSnapshot).bytes==changed);
#endif
    DocumentFile stale; CHECK(stale.open(path.string()).ok());
    CHECK(file.edit([](Document &d){d.assets.erase(d.assets.begin()+1);return true;}).ok());
    CHECK(stale.upgradeStorage().code==DocumentFileCode::Conflict); stale.close();
    file.close(); CHECK(file.storageVersion()==0); CHECK(file.open(path.string()).ok());
    CHECK(std::get<RasterAsset>(file.document()->assets[0]).pixels.pixels[0]==0x00112233);
    CHECK(std::get<RasterAsset>(file.document()->assets[0]).pixels.pixels[1]==0xff987654);
    file.close();
    {
        auto db=sql::Database::open(path,{.mode=sql::OpenMode::read_only}); CHECK(db);
        CHECK(scalar(*db,"SELECT count(*) FROM canvas_tiles")==3);
    }
    unsigned corruption=0;
    const auto poison=[&](std::string_view statement, DocumentFileCode expected=DocumentFileCode::CorruptFile) {
        const auto copy=temporary.path()/("corrupt-"+std::to_string(corruption++)+".iisc");
        std::filesystem::copy_file(path,copy);
        {auto db=sql::Database::open(copy); CHECK(db); CHECK(db->execute(statement));}
        const auto before=provider::File::read(copy,1024*1024);
        DocumentFile corrupt; const auto opened=corrupt.open(copy.string());
        if (opened.code!=expected) std::cerr<<"poison result "<<int(opened.code)<<": "<<opened.message<<'\n';
        CHECK(opened.code==expected && !corrupt.isOpen());
        CHECK(provider::File::read(copy,1024*1024)==before);
    };
    poison("DELETE FROM canvas_tiles WHERE asset='paint' AND column=0 AND row=0");
    poison("UPDATE canvas_tiles SET row=1 WHERE asset='paint' AND column=0 AND row=0");
    poison("UPDATE canvas_tiles SET asset='unreferenced'");
    poison("UPDATE canvas_tiles SET data=zeroblob(65536) WHERE column=0 AND row=0");
    poison("UPDATE canvas_tiles SET digest=zeroblob(32)");
    poison("UPDATE canvas_tiles SET data=zeroblob(16384),digest=X'4AD2F52A7D6DED03A896FA72D94C1DE3F0F60ED7E6ECF51230F6E94CBAD9B414' WHERE asset='paint' AND column=0 AND row=0");
    poison("UPDATE canvas_tiles SET data=CAST(zeroblob(1284)||X'3254768001000000'||zeroblob(15092) AS BLOB),digest=X'B5FE4F9FB98EA875403B56AC0216B01CA9F17DDE0EC386D868570B061FD43E73' WHERE asset='paint' AND column=2 AND row=1");
    poison("UPDATE canvas_tile_state SET tile_count=0");
    poison("PRAGMA user_version=999",DocumentFileCode::UnsupportedFormat);
    SerializationLimits tiny; tiny.maximumContainerBytes=64;
    CHECK(file.open(path.string(),tiny).code==DocumentFileCode::LimitExceeded);

    const auto old=temporary.path()/"legacy.iisc"; legacy(old);
    const auto legacyBytes=provider::File::read(old,1024*1024);
    CHECK(file.open(old.string()).ok() && file.storageVersion()==1);
    const auto legacyDocument=encodeIisc(*file.document()).bytes;
    const auto authorship=file.document()->authorship.dump();
    CHECK(file.edit([](Document &){return true;}).ok() && file.storageVersion()==1);
    CHECK(!file.edit([](Document &d){d.extent.width=-1;return false;}).ok());
    CHECK(file.storageVersion()==1 && provider::File::read(old,1024*1024)==legacyBytes);
    const auto generation=file.bindingGeneration();
    auto upgraded=file.upgradeStorage(); if(!upgraded.ok())std::cerr<<upgraded.message<<'\n';
    CHECK(upgraded.ok() && upgraded.changed && file.storageVersion()==2 && file.revision()==1);
    CHECK(file.bindingGeneration()==generation && file.document()->authorship.dump()==authorship);
    CHECK(encodeIisc(*file.document()).bytes==legacyDocument);
    {
        auto db=sql::Database::open(old,{.mode=sql::OpenMode::read_only}); CHECK(db);
        auto query=db->prepare("SELECT hex(digest) FROM canvas_tiles WHERE asset='paint' AND column=0 AND row=0"); CHECK(query);
        CHECK(query->step()); auto digest=query->value(0); CHECK(digest);
        // Independent Python hashlib + struct.pack oracle, not the C++ codec.
        CHECK(std::get<std::string>(*digest)=="9391CA65F16C301C589F1845AF6CC8FB9F31B8E2BBDF08251D4B4204D3F06003");
    }
    CHECK(file.upgradeStorage().ok() && !file.lastResult().changed && file.revision()==1);
    file.close(); CHECK(file.open(old.string()).ok() && file.storageVersion()==2);
    CHECK(encodeIisc(*file.document()).bytes==legacyDocument); file.close();
    const auto automatic=temporary.path()/"automatic.iisc"; legacy(automatic);
    CHECK(file.open(automatic.string()).ok());
    CHECK(file.edit([](Document &d){std::get<RasterAsset>(d.assets[0]).pixels.pixels[1]=0xff778899;return true;}).ok());
    CHECK(file.storageVersion()==2 && file.revision()==1); file.close();
    CHECK(file.open(automatic.string()).ok());
    CHECK(std::get<RasterAsset>(file.document()->assets[0]).pixels.pixels[1]==0xff778899); file.close();

    const auto direct=temporary.path()/"direct-bitmap-migration.iisc"; legacy(direct);
    const auto directBytes=provider::File::read(direct,1024*1024);
    CHECK(file.open(direct.string()).ok() && file.storageVersion()==1);
    CHECK(file.document()->formatVersion.minor==CurrentFormatMinor);
    BitmapEditor directEditor(file,"paint");
    CHECK(directEditor.setPixel(0,0,0xff123456));
    CHECK(file.revision()==0 && file.storageVersion()==1);
    CHECK(!directEditor.setPixel(-1,0,0xff778899));
    CHECK(provider::File::read(direct,1024*1024)==directBytes);
    CHECK(directEditor.setPixel(0,0,0xff778899));
    CHECK(file.storageVersion()==2 && file.revision()==1);
    CHECK(directEditor.undo() && directEditor.redo());
    file.close(); CHECK(file.open(direct.string()).ok());
    CHECK(std::get<RasterAsset>(file.document()->assets[0]).pixels.pixels[0]==0xff778899);
    file.close();

    const auto limited=temporary.path()/"limited.iisc"; legacy(limited);
    const auto limitedBytes=provider::File::read(limited,1024*1024);
    SerializationLimits budget; budget.maximumContainerBytes=1024;
    CHECK(file.open(limited.string(),budget).ok());
    CHECK(file.upgradeStorage().code==DocumentFileCode::LimitExceeded);
    CHECK(file.isOpen() && file.storageVersion()==1 && file.revision()==0);
    CHECK(provider::File::read(limited,1024*1024)==limitedBytes);
    CHECK(file.edit([](Document &d){std::get<RasterAsset>(d.assets[0]).pixels.pixels[0]=0xff778899;return true;}).code==DocumentFileCode::LimitExceeded);
    CHECK(file.storageVersion()==1 && file.revision()==0);
    CHECK(std::get<RasterAsset>(file.document()->assets[0]).pixels.pixels[0]==0xff123456);
    CHECK(provider::File::read(limited,1024*1024)==limitedBytes); file.close();

    const auto limitedDirect=temporary.path()/"limited-direct.iisc"; legacy(limitedDirect);
    const auto limitedDirectBytes=provider::File::read(limitedDirect,1024*1024);
    CHECK(file.open(limitedDirect.string(),budget).ok());
    BitmapEditor limitedEditor(file,"paint");
    CHECK(!limitedEditor.setPixel(0,0,0xff778899));
    CHECK(file.isOpen() && file.storageVersion()==1 && file.revision()==0);
    CHECK(limitedEditor.pixelAt(0,0)==0xff123456 && !limitedEditor.canUndo());
    CHECK(provider::File::read(limitedDirect,1024*1024)==limitedDirectBytes); file.close();

    // The v2 intermediate has one 16 KiB tile, but the final all-zero image
    // has none. The final representation fits the same 1024-byte limit.
    const auto clearedDirect=temporary.path()/"cleared-direct.iisc"; legacy(clearedDirect);
    CHECK(file.open(clearedDirect.string(),budget).ok());
    BitmapEditor clearEditor(file,"paint");
    CHECK(clearEditor.clear(0));
    CHECK(file.storageVersion()==2 && file.revision()==1);
    file.close(); CHECK(file.open(clearedDirect.string(),budget).ok());
    CHECK(std::ranges::all_of(std::get<RasterAsset>(file.document()->assets[0]).pixels.pixels,
                              [](auto pixel){return pixel==0;}));
    file.close();

    Document chunks; chunks.extent={64,64}; chunks.canvasMode=CanvasMode::Infinite;
    chunks.infiniteCanvas.chunkSize=32;
    chunks.assets.emplace_back(ChunkedRasterAsset{"chunks",{{-1,-1,makeRasterLayer(32,32)},
        {0,0,makeRasterLayer(32,32,0x00112233)}}});
    const auto chunkPath=temporary.path()/"chunks.iisc";
    CHECK(file.create(chunkPath.string(),chunks).ok()); file.close();
    CHECK(file.open(chunkPath.string()).ok());
    CHECK(encodeIisc(*file.document()).bytes==encodeIisc(chunks).bytes);
    CHECK(std::get<ChunkedRasterAsset>(file.document()->assets[0]).chunks.size()==2);
    CHECK(file.edit([](Document &d){d.infiniteCanvas.chunkSize=64;
        for(auto &c:std::get<ChunkedRasterAsset>(d.assets[0]).chunks)c.pixels=makeRasterLayer(64,64,0xffabcdef);
        return true;}).ok());
    const auto resized=encodeIisc(*file.document()).bytes; file.close();
    CHECK(file.open(chunkPath.string()).ok() && encodeIisc(*file.document()).bytes==resized);
    std::cout<<"WORKING_TILES_OK storage=2 legacy_read=true explicit_upgrade=true automatic_upgrade=true "
        "hidden_rgb=true zero_chunks=true corruption_checks="<<corruption<<" bounded_small_write=true migration_budget_rollback=true\n";
}
