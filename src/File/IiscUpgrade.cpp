#include "File/IiscUpgrade.h"
#include "File/IiscInput_p.hpp"

#include <iiFileProvider.h>
#include <QFileInfo>
#include <QDir>

#include <limits>
#include <utility>

namespace iiSharedCanvas {
namespace {
using detail::IiscInputFailure;
QString nativePath(const std::string &path)
{
    return iiFileProvider::File::absolutePath(iiFileProvider::File::pathString(
        std::filesystem::path(std::u8string(path.begin(),path.end()))));
}
void require(const DocumentFileResult &result)
{
    if (!result.ok()) { throw IiscInputFailure(result.code, result.message); }
}
void require(const IiscError &error)
{
    if (error.code == IiscErrorCode::None) { return; }
    throw IiscInputFailure(error.code == IiscErrorCode::UnsupportedVersion ? DocumentFileCode::UnsupportedFormat
        : error.code == IiscErrorCode::LimitExceeded ? DocumentFileCode::LimitExceeded
        : error.code == IiscErrorCode::InvalidDocument ? DocumentFileCode::InvalidDocument
        : DocumentFileCode::CorruptFile, error.message);
}
bool isCurrent(const Document &document)
{
    return document.formatVersion.major == CurrentFormatMajor && document.formatVersion.minor == CurrentFormatMinor;
}
DocumentFileCode storageCode(iiFileProvider::FileCode code)
{
    switch (code) {
    case iiFileProvider::FileCode::InvalidPath: return DocumentFileCode::InvalidPath;
    case iiFileProvider::FileCode::AlreadyExists: return DocumentFileCode::AlreadyExists;
    case iiFileProvider::FileCode::Conflict: return DocumentFileCode::Conflict;
    case iiFileProvider::FileCode::LimitExceeded: return DocumentFileCode::LimitExceeded;
    case iiFileProvider::FileCode::CorruptFile: return DocumentFileCode::CorruptFile;
    default: return DocumentFileCode::IoError;
    }
}
} // namespace

IiscUpgradeResult upgradeIiscFile(const std::string &inputPath, const std::string &outputPath, SerializationLimits limits)
{
    IiscUpgradeResult result;
    try {
        const QFileInfo input(nativePath(inputPath));
        if (input.isSymLink() || !input.isFile() || !input.isReadable()) {
            throw IiscInputFailure(DocumentFileCode::InvalidPath, "input must be a readable regular local file, not a symbolic link");
        }
        const bool inPlace = outputPath.empty();
        const QFileInfo output(inPlace ? input.absoluteFilePath() : nativePath(outputPath));
        result.outputPath = output.absoluteFilePath().toUtf8().toStdString();
        if (!inPlace && (output.exists() || output.isSymLink())) {
            throw IiscInputFailure(DocumentFileCode::AlreadyExists, "the destination already exists; omit the output for an in-place upgrade");
        }
        if (!output.dir().exists()) { throw IiscInputFailure(DocumentFileCode::IoError, "the destination parent directory does not exist"); }
        if (limits.maximumContainerBytes >= std::uint64_t(std::numeric_limits<qsizetype>::max())
            || input.size() < 0 || std::uint64_t(input.size()) > limits.maximumContainerBytes) {
            throw IiscInputFailure(DocumentFileCode::LimitExceeded, "input exceeds the native container byte budget");
        }
        const auto prefix = iiFileProvider::File::readPrefix(input.absoluteFilePath(), 16);
        if (prefix == QByteArray("SQLite format 3\0", 16)) {
            const auto source = detail::loadIiscDocumentReadOnly(input.absoluteFilePath().toStdString(), output.absolutePath().toStdString(), limits);
            result.sourceVersion = source.formatVersion;
            if (inPlace) {
                if (isCurrent(source)) { return result; }
                DocumentFile file;
                require(file.open(input.absoluteFilePath().toStdString(), limits));
                result.sourceVersion = file.document()->formatVersion;
                const auto upgraded = file.upgradeFormat();
                require(upgraded);
                result.changed = upgraded.changed;
            } else {
                auto upgraded = source;
                upgraded.formatVersion = {CurrentFormatMajor, CurrentFormatMinor};
                iiFileProvider::StagedFile staged(output.absoluteFilePath());
                iiFileProvider::File::remove(staged.path());
                DocumentFile file;
                require(file.create(staged.path().toStdString(), upgraded, limits));
                file.close();
                if (QFileInfo(staged.path()).size() < 0 || std::uint64_t(QFileInfo(staged.path()).size()) > limits.maximumContainerBytes) {
                    throw IiscInputFailure(DocumentFileCode::LimitExceeded, "upgraded working file exceeds the container byte budget");
                }
                const auto published = staged.publish(false);
                if (!published.succeeded) {
                    throw IiscInputFailure(QFileInfo::exists(output.absoluteFilePath()) ? DocumentFileCode::AlreadyExists : DocumentFileCode::IoError, published.message);
                }
                result.changed = true;
            }
            return result;
        }
        const auto original = iiFileProvider::File::read(input.absoluteFilePath(), qint64(limits.maximumContainerBytes));
        const auto decoded = decodeIisc({reinterpret_cast<const std::uint8_t *>(original.constData()), std::size_t(original.size())}, limits);
        require(decoded.error);
        result.sourceVersion = decoded.document.formatVersion;
        if (inPlace && isCurrent(decoded.document)) { return result; }
        auto upgraded = decoded.document;
        upgraded.formatVersion = {CurrentFormatMajor, CurrentFormatMinor};
        const auto encoded = encodeIisc(upgraded, limits);
        require(encoded.error);
        const QByteArray replacement(reinterpret_cast<const char *>(encoded.bytes.data()), qsizetype(encoded.bytes.size()));
        if (inPlace) { iiFileProvider::File::update(input.absoluteFilePath(), original, replacement); }
        else { iiFileProvider::File::create(output.absoluteFilePath(), replacement); }
        result.changed = true;
        return result;
    } catch (const IiscInputFailure &failure) {
        result.code = failure.code; result.message = failure.what();
    } catch (const iiFileProvider::FileError &failure) {
        result.code = storageCode(failure.code()); result.message = failure.what();
    } catch (const std::exception &failure) {
        result.code = DocumentFileCode::IoError; result.message = failure.what();
    } catch (...) {
        result.code = DocumentFileCode::IoError; result.message = "an unexpected native upgrade failure occurred";
    }
    return result;
}
} // namespace iiSharedCanvas
