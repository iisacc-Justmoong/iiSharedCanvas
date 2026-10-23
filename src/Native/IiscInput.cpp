#include "File/IiscInput_p.hpp"
#include "Native/Storage_p.hpp"
#include <iiFileProviderNative.h>
#include <iiFileProviderPresentation.h>
#include <mutex>

namespace iiSharedCanvas::detail {
namespace {
namespace provider=iiFileProvider::native;
std::string utf8(const std::filesystem::path& path){const auto text=path.u8string();return {text.begin(),text.end()};}
void backupWorkingFile(const std::filesystem::path& path,const std::filesystem::path& backup,SerializationLimits limits){
    provider::SourceAccess access(path);std::lock_guard sourceLock(access);
    auto total=std::filesystem::file_size(path);
    for(const auto suffix:{"-wal","-journal","-shm"}){
        auto sidecar=path;sidecar+=suffix;const auto status=std::filesystem::symlink_status(sidecar);
        if(!std::filesystem::exists(status))continue;
        if(std::filesystem::is_symlink(status)||!std::filesystem::is_regular_file(status))
            throw IiscInputFailure(DocumentFileCode::CorruptFile,"SQLite sidecars must be regular local files");
        const auto bytes=std::filesystem::file_size(sidecar);
        if(total>limits.maximumContainerBytes || bytes>limits.maximumContainerBytes-total)
            throw IiscInputFailure(DocumentFileCode::LimitExceeded,"SQLite source and sidecars exceed input budget");
        total+=bytes;
    }
    NativeDatabase source(utf8(path),true);source.execute("PRAGMA query_only=ON; PRAGMA trusted_schema=OFF");
    NativeTransaction transaction(&source,false);provider::File::create(backup,{});
    NativeDatabase destination(utf8(backup));source.backupTo(destination,limits.maximumContainerBytes);transaction.commit();
}
}
Document loadIiscDocumentReadOnly(const std::string& path,const std::string& parent,SerializationLimits limits){
    const auto input=std::filesystem::u8path(path);const auto size=std::filesystem::file_size(input);
    if(!size)throw IiscInputFailure(DocumentFileCode::CorruptFile,"Native input is empty");
    if(size>limits.maximumContainerBytes)throw IiscInputFailure(DocumentFileCode::LimitExceeded,"Native input exceeds byte budget");
    auto reader=provider::File::openRead(input);const auto prefix=reader->read(16);
    if(prefix==std::string("SQLite format 3\0",16)){
        reader.reset();provider::TemporaryDirectory temporary(std::filesystem::u8path(parent));
        const auto backup=temporary.path()/"source.iisc";backupWorkingFile(input,backup,limits);
        DocumentFile file;const auto opened=file.open(utf8(backup),limits);
        if(!opened.ok() || !file.document())throw IiscInputFailure(opened.code,opened.message);
        return *file.document();
    }
    if(!prefix.starts_with(std::string("IISC\r\n\x1a\n",8)))
        throw IiscInputFailure(DocumentFileCode::UnsupportedFormat,"Input is not a native canvas snapshot or working file");
    const auto bytes=provider::File::read(input,limits.maximumContainerBytes);
    auto decoded=decodeIisc({reinterpret_cast<const std::uint8_t*>(bytes.data()),bytes.size()},limits);
    if(!decoded.ok())throw IiscInputFailure(decoded.error.code==IiscErrorCode::LimitExceeded?DocumentFileCode::LimitExceeded:
        decoded.error.code==IiscErrorCode::UnsupportedVersion?DocumentFileCode::UnsupportedFormat:DocumentFileCode::CorruptFile,decoded.error.message);
    return std::move(decoded.document);
}
}
