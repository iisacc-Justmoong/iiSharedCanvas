#pragma once

#include <File/DocumentFile.h>
#include <Media/MediaIo.h>

#include <filesystem>
#include <string>
#include <string_view>
#include <vector>

namespace iisc_native_cli {

inline std::vector<std::string> arguments(int argc, char **argv)
{
    return {argv + 1, argv + argc};
}
#ifdef _WIN32
inline std::vector<std::string> arguments(int argc, wchar_t **argv)
{
    std::vector<std::string> result;
    for (int i = 1; i < argc; ++i) {
        const auto value = std::filesystem::path(argv[i]).u8string();
        result.emplace_back(value.begin(), value.end());
    }
    return result;
}
#endif
inline bool localPath(std::string_view text)
{
    return !text.empty() && text.find('\0') == text.npos && text.find("://") == text.npos;
}
inline std::filesystem::path path(const std::string &text)
{
    return std::filesystem::path(std::u8string(text.begin(), text.end()));
}
inline bool extensionIs(const std::string &text, std::string_view expected)
{
    const auto extension = path(text).extension().u8string();
    if (extension.size() != expected.size()) return false;
    for (std::size_t i = 0; i < extension.size(); ++i) {
        auto ch = extension[i];
        if (ch >= u8'A' && ch <= u8'Z') ch = char8_t(ch + (u8'a' - u8'A'));
        if (ch != static_cast<char8_t>(expected[i])) return false;
    }
    return true;
}
inline const char *codeName(iiSharedCanvas::MediaIoCode code)
{
    using C = iiSharedCanvas::MediaIoCode;
    switch (code) {
    case C::None: return "None";
    case C::InvalidArgument: return "InvalidArgument";
    case C::UnsupportedFormat: return "UnsupportedFormat";
    case C::UnsupportedFeature: return "UnsupportedFeature";
    case C::DependencyUnavailable: return "DependencyUnavailable";
    case C::InvalidData: return "InvalidData";
    case C::LimitExceeded: return "LimitExceeded";
    case C::AlreadyExists: return "AlreadyExists";
    case C::IoError: return "IoError";
    case C::Cancelled: return "Cancelled";
    case C::TimedOut: return "TimedOut";
    }
    return "UnknownError";
}
inline const char *codeName(iiSharedCanvas::DocumentFileCode code)
{
    using C = iiSharedCanvas::DocumentFileCode;
    switch (code) {
    case C::None: return "None";
    case C::NotOpen: return "NotOpen";
    case C::AlreadyOpen: return "AlreadyOpen";
    case C::InvalidPath: return "InvalidPath";
    case C::AlreadyExists: return "AlreadyExists";
    case C::InvalidDocument: return "InvalidDocument";
    case C::UnsupportedFormat: return "UnsupportedFormat";
    case C::CorruptFile: return "CorruptFile";
    case C::LimitExceeded: return "LimitExceeded";
    case C::EditRejected: return "EditRejected";
    case C::Conflict: return "Conflict";
    case C::IoError: return "IoError";
    case C::InvalidArgument: return "InvalidArgument";
    }
    return "UnknownError";
}
} // namespace iisc_native_cli
