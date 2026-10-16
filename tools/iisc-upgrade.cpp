#include <File/IiscUpgrade.h>

#include <algorithm>
#include <exception>
#include <iostream>
#include <string>
#include <string_view>
#include <vector>

namespace {
using namespace iiSharedCanvas;
constexpr std::string_view usage =
    "Usage: iisc-upgrade [--] INPUT.iisc [OUTPUT.iisc]\n"
    "       iisc-upgrade --help\n"
    "Upgrade a native IISC snapshot or SQLite working file to the current model.\n"
    "One path updates the input in place; a second path keeps the source intact.\n"
    "The storage kind and all document content/authorship are preserved.\n"
    "Current inputs are unchanged. Existing output paths are never overwritten.\n"
    "Use -- before dashed paths. No desktop session is required.\n";

int argumentError(std::string_view message)
{
    std::cerr << "iisc-upgrade: InvalidArgument: " << message << '\n' << usage;
    return 2;
}
const char *codeName(DocumentFileCode code)
{
    switch (code) {
    case DocumentFileCode::None: return "None";
    case DocumentFileCode::NotOpen: return "NotOpen";
    case DocumentFileCode::AlreadyOpen: return "AlreadyOpen";
    case DocumentFileCode::InvalidPath: return "InvalidPath";
    case DocumentFileCode::AlreadyExists: return "AlreadyExists";
    case DocumentFileCode::InvalidDocument: return "InvalidDocument";
    case DocumentFileCode::UnsupportedFormat: return "UnsupportedFormat";
    case DocumentFileCode::CorruptFile: return "CorruptFile";
    case DocumentFileCode::LimitExceeded: return "LimitExceeded";
    case DocumentFileCode::EditRejected: return "EditRejected";
    case DocumentFileCode::Conflict: return "Conflict";
    case DocumentFileCode::IoError: return "IoError";
    }
    return "UnknownError";
}
int upgrade(const std::vector<std::string> &arguments)
{
    if (arguments.size() == 1 && (arguments[0] == "--help" || arguments[0] == "-h")) {
        std::cout << usage << "Current model: IISC " << CurrentFormatMajor << '.' << CurrentFormatMinor << '\n';
        return 0;
    }
    std::vector<std::string> paths;
    bool parseOptions = true;
    for (const auto &argument : arguments) {
        if (parseOptions && argument == "--") { parseOptions = false; }
        else if (parseOptions && argument.starts_with('-')) {
            return argumentError("unknown option; use --help or -- before dashed paths");
        } else { paths.push_back(argument); }
    }
    if (paths.empty() || paths.size() > 2 || paths[0].empty() || (paths.size() == 2 && paths[1].empty())) {
        return argumentError("one input path and an optional output path are required");
    }
    if (paths.size() == 2) {
        auto suffix = paths[1].size() >= 5 ? paths[1].substr(paths[1].size()-5) : std::string{};
        std::transform(suffix.begin(),suffix.end(),suffix.begin(),[](unsigned char ch) {
            return char(ch >= 'A' && ch <= 'Z' ? ch + ('a'-'A') : ch);
        });
        if (suffix != ".iisc") { return argumentError("the destination must have the .iisc extension"); }
    }
    for (const auto &path : paths) {
        if (path.find("://") != std::string::npos) { return argumentError("only local filesystem paths are supported"); }
    }
    const auto result = upgradeIiscFile(paths[0], paths.size() == 2 ? paths[1] : std::string{});
    if (!result.ok()) {
        std::cerr << "iisc-upgrade: " << codeName(result.code) << ": " << result.message << '\n';
        return 1;
    }
    if (result.changed) {
        std::cout << "Upgraded IISC " << result.sourceVersion.major << '.' << result.sourceVersion.minor
                  << " -> " << CurrentFormatMajor << '.' << CurrentFormatMinor << " -> " << result.outputPath << '\n';
    } else {
        std::cout << "Already current IISC " << CurrentFormatMajor << '.' << CurrentFormatMinor << ": " << result.outputPath << '\n';
    }
    return 0;
}
} // namespace
int main(int argc, char **argv)
{
    try {
        return upgrade({argv + 1, argv + argc});
    } catch (const std::exception &failure) {
        std::cerr << "iisc-upgrade: IoError: " << failure.what() << '\n';
    } catch (...) {
        std::cerr << "iisc-upgrade: IoError: an unexpected upgrade failure occurred\n";
    }
    return 1;
}
