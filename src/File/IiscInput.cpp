#include <iiFileProviderDevice.h>
#include "File/IiscInput_p.hpp"

#include <iiFileProvider.h>
#include <QDir>
#include <QFileInfo>
#include <algorithm>
#include <limits>
#include <utility>

namespace iiSharedCanvas::detail {
namespace {
void backupWorkingFile(const QString &path, const QString &backup, SerializationLimits limits)
{
    std::uint64_t encodedBytes = std::uint64_t(QFileInfo(path).size());
    for (const auto suffix : {"-wal", "-journal", "-shm"}) {
        const QFileInfo sidecar(path + suffix);
        if (!sidecar.exists() && !sidecar.isSymLink()) { continue; }
        if (sidecar.isSymLink() || !sidecar.isFile() || sidecar.size() < 0) {
            throw IiscInputFailure(DocumentFileCode::CorruptFile, "SQLite sidecars must be regular local files, not symbolic links");
        }
        if (encodedBytes > limits.maximumContainerBytes
            || std::uint64_t(sidecar.size()) > limits.maximumContainerBytes - encodedBytes) {
            throw IiscInputFailure(DocumentFileCode::LimitExceeded, "SQLite source and sidecars exceed the input byte budget");
        }
        encodedBytes += std::uint64_t(sidecar.size());
    }
    iiFileProvider::Database source(path.toStdString(), true);
    source.execute("PRAGMA query_only=ON; PRAGMA trusted_schema=OFF");
    iiFileProvider::Transaction transaction(&source, false);
    iiFileProvider::File::create(backup, {});
    iiFileProvider::Database destination(backup.toStdString());
    source.backupTo(destination, limits.maximumContainerBytes);
    transaction.commit();
    if (QFileInfo(backup).size() < 0 || std::uint64_t(QFileInfo(backup).size()) > limits.maximumContainerBytes) {
        throw IiscInputFailure(DocumentFileCode::LimitExceeded, "private SQLite backup exceeds its byte budget");
    }
}
} // namespace

Document loadIiscDocumentReadOnly(const std::string &path, const std::string &temporaryParent, SerializationLimits limits)
{
    const auto nativePath = iiFileProvider::File::pathString(std::filesystem::path(std::u8string(path.begin(),path.end())));
    const auto parent = iiFileProvider::File::pathString(std::filesystem::path(std::u8string(temporaryParent.begin(),temporaryParent.end())));
    if (QFileInfo(nativePath).size() <= 0) { throw IiscInputFailure(DocumentFileCode::CorruptFile, "native input is empty"); }
    if (std::uint64_t(QFileInfo(nativePath).size()) > limits.maximumContainerBytes) {
        throw IiscInputFailure(DocumentFileCode::LimitExceeded, "native input exceeds its byte budget");
    }
    const auto prefix = iiFileProvider::File::readPrefix(nativePath, 16);
    if (prefix == QByteArray("SQLite format 3\0", 16)) {
        iiFileProvider::TemporaryDirectory directory(QDir(parent).filePath(".iisc-input-XXXXXX"));
        if (!directory.isValid()) { throw IiscInputFailure(DocumentFileCode::IoError, "cannot create a private snapshot directory beside the destination"); }
        const auto backup = directory.filePath("source.iisc");
        backupWorkingFile(nativePath, backup, limits);
        DocumentFile reader;
        const auto opened = reader.open(backup.toStdString(), limits);
        if (!opened.ok() || !reader.document()) { throw IiscInputFailure(opened.code, opened.message); }
        return *reader.document();
    }
    if (!prefix.startsWith(QByteArray("IISC\r\n\x1a\n", 8))) {
        throw IiscInputFailure(DocumentFileCode::UnsupportedFormat, "input is not a native iisc snapshot or working file");
    }
    if (limits.maximumContainerBytes >= std::uint64_t(std::numeric_limits<qsizetype>::max())) {
        throw IiscInputFailure(DocumentFileCode::LimitExceeded, "native read limit is not addressable");
    }
    const auto bytes = iiFileProvider::File::read(nativePath, qint64(limits.maximumContainerBytes));
    auto decoded = decodeIisc({reinterpret_cast<const std::uint8_t *>(bytes.constData()), std::size_t(bytes.size())}, limits);
    if (!decoded.ok()) {
        throw IiscInputFailure(decoded.error.code == IiscErrorCode::LimitExceeded ? DocumentFileCode::LimitExceeded
            : decoded.error.code == IiscErrorCode::UnsupportedVersion ? DocumentFileCode::UnsupportedFormat
            : DocumentFileCode::CorruptFile, decoded.error.message);
    }
    return std::move(decoded.document);
}
} // namespace iiSharedCanvas::detail
