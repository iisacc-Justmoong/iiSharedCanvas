#include "CliSupport.h"
#include <Layered/LayeredDocumentCodec.h>

#include <algorithm>
#include <exception>
#include <iostream>
#include <system_error>

namespace {
using namespace iiSharedCanvas;
constexpr std::string_view usage =
    "Usage: iisc-export-psd [--overwrite] [--] INPUT.iisc OUTPUT.psd\n"
    "       iisc-export-psd --help\n"
    "Export native snapshot or working-file canvas layers at frame zero to PSD.\n"
    "Existing outputs are preserved unless --overwrite is specified.\n"
    "Source databases are opened read-only and backed up before document decoding.\n"
    "Use -- before dashed paths.\n";

int argumentError(std::string_view message)
{
    std::cerr << "iisc-export-psd: InvalidArgument: " << message << '\n' << usage;
    return 2;
}
int run(const std::vector<std::string> &arguments)
{
    if (arguments.size() == 1 && (arguments[0] == "--help" || arguments[0] == "-h")) {
        std::cout << usage;
        return 0;
    }
    PsdExportOptions options;
    std::vector<std::string> paths;
    bool parseOptions = true;
    for (const auto &argument : arguments) {
        if (parseOptions && argument == "--") parseOptions = false;
        else if (parseOptions && argument == "--overwrite") {
            if (options.overwrite) return argumentError("--overwrite may only be specified once");
            options.overwrite = true;
        } else if (parseOptions && argument.starts_with('-')) {
            return argumentError("unknown option; use --help or -- before dashed paths");
        } else paths.push_back(argument);
    }
    if (paths.size() != 2 || !iisc_native_cli::localPath(paths[0]) || !iisc_native_cli::localPath(paths[1]))
        return argumentError("one local input path and one local output path are required");
    if (!iisc_native_cli::extensionIs(paths[1], ".psd"))
        return argumentError("the destination must have the .psd extension");

    const auto input = std::filesystem::absolute(iisc_native_cli::path(paths[0]));
    const auto output = std::filesystem::absolute(iisc_native_cli::path(paths[1]));
    std::error_code ec;
    if (!std::filesystem::is_regular_file(input, ec) || ec) {
        std::cerr << "iisc-export-psd: IoError: input is not a readable regular file\n"; return 1;
    }
    const auto source = std::filesystem::canonical(input, ec);
    if (ec) { std::cerr << "iisc-export-psd: IoError: " << ec.message() << '\n'; return 1; }
    ec.clear();
    const auto status = std::filesystem::symlink_status(output, ec);
    if (ec && ec != std::errc::no_such_file_or_directory) {
        std::cerr << "iisc-export-psd: IoError: " << ec.message() << '\n'; return 1;
    }
    if (std::filesystem::is_symlink(status)) {
        if (!options.overwrite) std::cerr << "iisc-export-psd: AlreadyExists: destination already exists\n";
        else std::cerr << "iisc-export-psd: InvalidArgument: destination must not be a symbolic link\n";
        return 1;
    }
    if (std::filesystem::exists(status)) {
        if (!options.overwrite) { std::cerr << "iisc-export-psd: AlreadyExists: destination already exists\n"; return 1; }
        if (!std::filesystem::is_regular_file(status)) {
            std::cerr << "iisc-export-psd: InvalidArgument: destination must be a regular file\n"; return 1;
        }
        ec.clear();
        const bool alias = std::filesystem::equivalent(input, output, ec);
        if (ec || alias) {
            std::cerr << "iisc-export-psd: InvalidArgument: destination must not alias the source\n"; return 1;
        }
    }
    ec.clear();
    if (!std::filesystem::is_directory(output.parent_path(), ec) || ec) {
        std::cerr << "iisc-export-psd: IoError: destination parent directory does not exist\n"; return 1;
    }
    SerializationLimits limits;
    limits.maximumContainerBytes = std::min(options.limits.maxInputBytes, options.limits.maxDecodedBytes);
    limits.maximumCanvasPixels = options.limits.maxPixelsPerFrame;
    limits.maximumTotalRasterPixels = options.limits.maxDecodedBytes / sizeof(std::uint32_t);
    limits.maximumLayers = options.maxLayers;
    Document document;
    const auto sourceText = source.u8string();
    const auto loaded = DocumentFile::readSnapshot(std::string(sourceText.begin(), sourceText.end()), document, limits);
    if (!loaded.ok()) {
        std::cerr << "iisc-export-psd: " << iisc_native_cli::codeName(loaded.code) << ": " << loaded.message << '\n';
        return 1;
    }
    const auto exported = exportPsd(document, paths[1], options);
    for (const auto &warning : exported.warnings) std::cerr << "iisc-export-psd: warning: " << warning << '\n';
    if (!exported.ok()) {
        std::cerr << "iisc-export-psd: " << iisc_native_cli::codeName(exported.code) << ": " << exported.message << '\n';
        return 1;
    }
    std::cout << "Exported psd frame 0 -> " << paths[1] << '\n';
    return 0;
}
template<class Arg> int entry(int argc, Arg **argv)
{
    try { return run(iisc_native_cli::arguments(argc, argv)); }
    catch (const std::exception &error) { std::cerr << "iisc-export-psd: IoError: " << error.what() << '\n'; }
    catch (...) { std::cerr << "iisc-export-psd: IoError: an unexpected export failure occurred\n"; }
    return 1;
}
}
#ifdef _WIN32
int wmain(int argc, wchar_t **argv) { return entry(argc, argv); }
#else
int main(int argc, char **argv) { return entry(argc, argv); }
#endif
