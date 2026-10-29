#include "CliSupport.h"
#include <Layered/LayeredDocumentCodec.h>

#include <exception>
#include <iostream>
#include <system_error>

namespace {
using namespace iiSharedCanvas;
constexpr std::string_view usage =
    "Usage: iisc-import [--id-prefix PREFIX] [--] INPUT OUTPUT.iisc\n"
    "       iisc-import --help\n"
    "Import supported PSD or OpenRaster layers into a new .iisc working file.\n"
    "Existing output files are never overwritten. Use -- before dashed paths.\n";

int argumentError(std::string_view message)
{
    std::cerr << "iisc-import: InvalidArgument: " << message << '\n' << usage;
    return 2;
}
int run(const std::vector<std::string> &arguments)
{
    if (arguments.size() == 1 && (arguments[0] == "--help" || arguments[0] == "-h")) {
        std::cout << usage;
        return 0;
    }
    LayeredDocumentImportOptions options;
    std::vector<std::string> paths;
    bool parseOptions = true, prefixSeen = false;
    for (std::size_t index = 0; index < arguments.size(); ++index) {
        const auto &argument = arguments[index];
        if (parseOptions && argument == "--") parseOptions = false;
        else if (parseOptions && (argument == "--id-prefix" || argument.starts_with("--id-prefix="))) {
            if (prefixSeen) return argumentError("--id-prefix may only be specified once");
            prefixSeen = true;
            if (argument == "--id-prefix") {
                if (index + 1 >= arguments.size() || arguments[index + 1].starts_with('-'))
                    return argumentError("--id-prefix requires a value; use --id-prefix=VALUE for a dashed prefix");
                options.idPrefix = arguments[++index];
            } else options.idPrefix = argument.substr(std::string_view("--id-prefix=").size());
            if (options.idPrefix.empty()) return argumentError("the ID prefix must not be empty");
        } else if (parseOptions && argument.starts_with('-')) {
            return argumentError("unknown option; use --help or -- before dashed file paths");
        } else paths.push_back(argument);
    }
    if (paths.size() != 2 || !iisc_native_cli::localPath(paths[0]) || !iisc_native_cli::localPath(paths[1]))
        return argumentError("one local input path and one local output path are required");
    if (!iisc_native_cli::extensionIs(paths[1], ".iisc"))
        return argumentError("the output path must have the .iisc extension");
    std::error_code ec;
    const auto output = std::filesystem::absolute(iisc_native_cli::path(paths[1]), ec);
    if (ec) { std::cerr << "iisc-import: IoError: " << ec.message() << '\n'; return 1; }
    const auto outputStatus = std::filesystem::symlink_status(output, ec);
    if (ec && ec != std::errc::no_such_file_or_directory) {
        std::cerr << "iisc-import: IoError: " << ec.message() << '\n'; return 1;
    }
    if (std::filesystem::exists(outputStatus) || std::filesystem::is_symlink(outputStatus)) {
        std::cerr << "iisc-import: AlreadyExists: output path already exists\n"; return 1;
    }
    ec.clear();
    if (!std::filesystem::is_directory(output.parent_path(), ec) || ec) {
        std::cerr << "iisc-import: IoError: destination parent directory does not exist\n"; return 1;
    }
    const auto imported = importLayeredDocument(paths[0], options);
    for (const auto &warning : imported.result.warnings) std::cerr << "iisc-import: warning: " << warning << '\n';
    if (!imported.ok()) {
        std::cerr << "iisc-import: " << iisc_native_cli::codeName(imported.result.code)
                  << ": " << imported.result.message << '\n'; return 1;
    }
    DocumentFile destination;
    const auto created = destination.create(paths[1], imported.document);
    if (!created.ok()) {
        std::cerr << "iisc-import: " << iisc_native_cli::codeName(created.code) << ": " << created.message << '\n';
        return 1;
    }
    destination.close();
    std::cout << "Imported " << imported.format << ": " << imported.document.layers.size() << " layers -> " << paths[1] << '\n';
    return 0;
}
template<class Arg> int entry(int argc, Arg **argv)
{
    try { return run(iisc_native_cli::arguments(argc, argv)); }
    catch (const std::exception &error) { std::cerr << "iisc-import: IoError: " << error.what() << '\n'; }
    catch (...) { std::cerr << "iisc-import: IoError: an unexpected conversion failure occurred\n"; }
    return 1;
}
}
#ifdef _WIN32
int wmain(int argc, wchar_t **argv) { return entry(argc, argv); }
#else
int main(int argc, char **argv) { return entry(argc, argv); }
#endif
