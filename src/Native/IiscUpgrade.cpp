#include "File/IiscUpgrade.h"
#include "File/IiscInput_p.hpp"
#include "Native/Media_p.hpp"

#include <iiFileProviderNative.h>
#include <iiFileProviderPresentation.h>

#include <filesystem>
#include <new>
#include <string_view>
#include <utility>

namespace iiSharedCanvas {
namespace {
namespace fs = std::filesystem;
namespace provider = iiFileProvider::native;
using detail::IiscInputFailure;

std::string pathText(const fs::path &path)
{
    const auto value = path.u8string();
    return {value.begin(), value.end()};
}
fs::path localPath(const std::string &path)
{
    if (path.empty() || !media_detail::validText(path) || path.find("://") != path.npos)
        throw IiscInputFailure(DocumentFileCode::InvalidPath, "a local UTF-8 file path is required");
    return fs::absolute(fs::path(std::u8string(path.begin(), path.end())));
}
void require(const DocumentFileResult &result)
{
    if (!result.ok()) throw IiscInputFailure(result.code, result.message);
}
void require(const IiscError &error)
{
    if (error.code == IiscErrorCode::None) return;
    throw IiscInputFailure(error.code == IiscErrorCode::UnsupportedVersion ? DocumentFileCode::UnsupportedFormat
        : error.code == IiscErrorCode::LimitExceeded ? DocumentFileCode::LimitExceeded
        : error.code == IiscErrorCode::InvalidDocument ? DocumentFileCode::InvalidDocument
        : DocumentFileCode::CorruptFile, error.message);
}
bool current(const Document &document)
{
    return document.formatVersion.major == CurrentFormatMajor && document.formatVersion.minor == CurrentFormatMinor;
}
DocumentFileCode storageCode(provider::FileCode code)
{
    switch (code) {
    case provider::FileCode::InvalidPath: return DocumentFileCode::InvalidPath;
    case provider::FileCode::AlreadyExists: return DocumentFileCode::AlreadyExists;
    case provider::FileCode::Conflict: return DocumentFileCode::Conflict;
    case provider::FileCode::LimitExceeded: return DocumentFileCode::LimitExceeded;
    case provider::FileCode::IoFailure: return DocumentFileCode::IoError;
    }
    return DocumentFileCode::IoError;
}
bool occupied(const fs::path &path)
{
    std::error_code error;
    const auto status = fs::symlink_status(path, error);
    if (error && error != std::errc::no_such_file_or_directory) throw fs::filesystem_error("destination status", path, error);
    return fs::exists(status) || fs::is_symlink(status);
}
}

IiscUpgradeResult upgradeIiscFile(const std::string &inputPath, const std::string &outputPath, SerializationLimits limits)
{
    IiscUpgradeResult result;
    try {
        const auto input = localPath(inputPath);
        std::error_code statusError;
        const auto sourceStatus = fs::symlink_status(input, statusError);
        if (statusError && statusError != std::errc::no_such_file_or_directory)
            throw fs::filesystem_error("input status", input, statusError);
        if (fs::is_symlink(sourceStatus) || !fs::is_regular_file(sourceStatus))
            throw IiscInputFailure(DocumentFileCode::InvalidPath, "input must be a regular local file, not a symbolic link");
        const bool inPlace = outputPath.empty();
        const auto output = inPlace ? input : localPath(outputPath);
        result.outputPath = pathText(output);
        if (!inPlace && occupied(output))
            throw IiscInputFailure(DocumentFileCode::AlreadyExists, "destination already exists");
        if (!fs::is_directory(output.parent_path()))
            throw IiscInputFailure(DocumentFileCode::IoError, "destination parent directory does not exist");
        if (fs::file_size(input) > limits.maximumContainerBytes)
            throw IiscInputFailure(DocumentFileCode::LimitExceeded, "input exceeds the native container byte budget");

        auto reader = provider::File::openRead(input);
        const auto prefix = reader->read(16);
        reader.reset();
        if (prefix == std::string("SQLite format 3\0", 16)) {
            auto source = detail::loadIiscDocumentReadOnly(pathText(input), pathText(output.parent_path()), limits);
            result.sourceVersion = source.formatVersion;
            if (inPlace) {
                if (current(source)) return result;
                DocumentFile file;
                require(file.open(pathText(input), limits));
                result.sourceVersion = file.document()->formatVersion;
                const auto upgraded = file.upgradeFormat();
                require(upgraded);
                result.changed = upgraded.changed;
            } else {
                source.formatVersion = {CurrentFormatMajor, CurrentFormatMinor};
                provider::StagedFile staged(output);
                (void)provider::File::remove(staged.path());
                DocumentFile file;
                require(file.create(pathText(staged.path()), source, limits));
                file.close();
                provider::Presentation::cancelPending(staged.path());
                if (fs::file_size(staged.path()) > limits.maximumContainerBytes)
                    throw IiscInputFailure(DocumentFileCode::LimitExceeded, "upgraded working file exceeds the container byte budget");
                // Native StagedFile::publish may replace an existing path. Use
                // the provider's exclusive create so a destination appearing
                // after the initial check is never overwritten.
                const auto bytes = provider::File::read(staged.path(), limits.maximumContainerBytes);
                provider::File::create(output, bytes);
                result.changed = true;
            }
            return result;
        }

        const auto original = provider::File::read(input, limits.maximumContainerBytes);
        const auto decoded = decodeIisc({reinterpret_cast<const std::uint8_t *>(original.data()), original.size()}, limits);
        require(decoded.error);
        result.sourceVersion = decoded.document.formatVersion;
        if (inPlace && current(decoded.document)) return result;
        auto upgraded = decoded.document;
        upgraded.formatVersion = {CurrentFormatMajor, CurrentFormatMinor};
        const auto encoded = encodeIisc(upgraded, limits);
        require(encoded.error);
        const std::string_view replacement(reinterpret_cast<const char *>(encoded.bytes.data()), encoded.bytes.size());
        if (inPlace) provider::File::update(input, original, replacement);
        else provider::File::create(output, replacement);
        result.changed = true;
    } catch (const IiscInputFailure &failure) {
        result.code = failure.code; result.message = failure.what();
    } catch (const provider::FileError &failure) {
        result.code = storageCode(failure.code()); result.message = failure.what();
    } catch (const std::bad_alloc &) {
        result.code = DocumentFileCode::LimitExceeded; result.message = "not enough memory to upgrade document";
    } catch (const std::exception &failure) {
        result.code = DocumentFileCode::IoError; result.message = failure.what();
    } catch (...) {
        result.code = DocumentFileCode::IoError; result.message = "an unexpected native upgrade failure occurred";
    }
    return result;
}
} // namespace iiSharedCanvas
