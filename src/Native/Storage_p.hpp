#pragma once
#include "File/DocumentFile.h"
#include <iiFileProviderDatabase.h>
#include <limits>
#include <optional>
#include <stdexcept>

namespace iiSharedCanvas::detail {
namespace sql=iiFileProvider::native::sql;
struct NativeStorageFailure final:std::runtime_error {
    NativeStorageFailure(DocumentFileCode value,std::string message):std::runtime_error(std::move(message)),code(value){}
    DocumentFileCode code;
};
template<class T> T sqlValue(sql::Result<T> result) {
    if(!result) throw NativeStorageFailure(result.error().code==sql::Code::busy?DocumentFileCode::Conflict:
        result.error().code==sql::Code::limit_exceeded?DocumentFileCode::LimitExceeded:DocumentFileCode::IoError,result.error().message);
    return std::move(*result);
}
inline void sqlValue(sql::Result<void> result) {
    if(!result) throw NativeStorageFailure(result.error().code==sql::Code::busy?DocumentFileCode::Conflict:
        result.error().code==sql::Code::limit_exceeded?DocumentFileCode::LimitExceeded:DocumentFileCode::IoError,result.error().message);
}
class NativeStatement;
class NativeTransaction;
// This private adapter translates the document's existing, typed schema calls to
// the provider's Result API. SQLite handles and execution remain in FileProvider.
class NativeDatabase final {
public:
    explicit NativeDatabase(const std::string& path,bool readonly=false)
        :value(sqlValue(sql::Database::open(std::filesystem::u8path(path),
            {.mode=readonly?sql::OpenMode::read_only:sql::OpenMode::read_write,
             .maximum_value_bytes=std::numeric_limits<int>::max()}))){}
    void execute(const char* text) {
        // Only fixed, private schema/PRAGMA literals enter this method. Values
        // and identifiers from files are always bound through NativeStatement.
        std::string_view remaining(text);
        while(!remaining.empty()) {
            const auto end=remaining.find(';'); auto part=remaining.substr(0,end);
            if(part.find_first_not_of(" \t\r\n")!=part.npos) sqlValue(value.execute(part));
            if(end==remaining.npos) break;
            remaining.remove_prefix(end+1);
        }
    }
    std::int64_t scalar(const char*);
    void configureDurable(){sqlValue(value.configure_durable());}
    bool hasMoved()const{return sqlValue(value.has_moved());}
    bool inTransaction()const noexcept{return value.in_transaction();}
    std::uint64_t patchBlob(const char* table,const char* column,std::int64_t id,std::span<const std::uint8_t> bytes){return sqlValue(value.patch_blob(table,column,id,bytes));}
    void backupTo(NativeDatabase& target,std::uint64_t limit,int timeout=30000){sqlValue(value.backup_to(target.value,limit,timeout));}
    sql::Database value;
};
class NativeStatement final {
public:
    NativeStatement(NativeDatabase* database,const char* query):value(sqlValue(database->value.prepare(query))){}
    void integer(int index,std::int64_t data){sqlValue(value.bind(index,data));}
    void text(int index,const std::string& data){sqlValue(value.bind(index,data));}
    void bytes(int index,const void* data,std::size_t size){
        if(size && !data) throw NativeStorageFailure(DocumentFileCode::InvalidDocument,"Missing record bytes");
        const auto* p=static_cast<const std::uint8_t*>(data);sqlValue(value.bind(index,size?sql::Blob(p,p+size):sql::Blob{}));
    }
    bool row(){cached.clear();cached.resize(value.columns());return sqlValue(value.step())==sql::Step::row;}
    void done(){if(row())throw NativeStorageFailure(DocumentFileCode::CorruptFile,"Unexpected additional database row");}
    std::int64_t integer(int column)const{return typed<std::int64_t>(column);}
    std::string text(int column)const{return typed<std::string>(column);}
    std::span<const std::uint8_t> bytes(int column)const{return typed<sql::Blob>(column);}
private:
    template<class T> const T& typed(int column)const{
        if(column<0 || column>=value.columns())throw NativeStorageFailure(DocumentFileCode::CorruptFile,"Invalid record column");
        if(cached.size()!=std::size_t(value.columns()))cached.resize(value.columns());
        auto& item=cached[column];if(!item)item=sqlValue(value.value(column));
        if(!std::holds_alternative<T>(*item))throw NativeStorageFailure(DocumentFileCode::CorruptFile,"Record field has an invalid storage type");
        return std::get<T>(*item);
    }
    sql::Statement value;
    mutable std::vector<std::optional<sql::Value>> cached;
};
inline std::int64_t NativeDatabase::scalar(const char* query){
    NativeStatement statement(this,query);
    if(!statement.row())throw NativeStorageFailure(DocumentFileCode::CorruptFile,"Missing scalar record");
    const auto result=statement.integer(0);statement.done();return result;
}
class NativeTransaction final{
public:
    NativeTransaction(NativeDatabase* database,bool writing):value(sqlValue(sql::Transaction::begin(database->value,writing))){}
    void commit(){sqlValue(value.commit());}
private:sql::Transaction value;
};
}
