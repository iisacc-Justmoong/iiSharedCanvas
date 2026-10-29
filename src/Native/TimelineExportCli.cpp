#include "CliSupport.h"
#include <Timeline/TimelineInterchange.h>

#include <algorithm>
#include <exception>
#include <iostream>
#include <system_error>

namespace {
using namespace iiSharedCanvas;
constexpr std::string_view usage =
    "Usage: iisc-export-timeline [--name TEXT] [--] INPUT.iisc OUTPUT_DIR\n"
    "       iisc-export-timeline --help\n"
    "Export a snapshot or working-file timeline to a NEW package directory.\n"
    "The package contains timeline.xml, timeline.fcpxml, PNG/WAV media, manifest.json,\n"
    "and a native source.iisc snapshot. Existing outputs are never overwritten.\n"
    "Source databases are opened read-only and backed up before document decoding.\n"
    "Use -- before dashed paths.\n";
int argumentError(std::string_view message)
{
    std::cerr << "iisc-export-timeline: InvalidArgument: " << message << '\n' << usage;
    return 2;
}
int run(const std::vector<std::string> &arguments)
{
    if (arguments.size() == 1 && (arguments[0] == "--help" || arguments[0] == "-h")) {
        std::cout << usage; return 0;
    }
    TimelineInterchangeOptions options;
    std::vector<std::string> paths;
    bool parseOptions = true, haveName = false;
    for (std::size_t i = 0; i < arguments.size(); ++i) {
        const auto &argument = arguments[i];
        if (parseOptions && argument == "--") parseOptions = false;
        else if (parseOptions && argument == "--name") {
            if (haveName) return argumentError("--name may only be specified once");
            if (++i == arguments.size() || arguments[i].empty()) return argumentError("--name requires a nonempty sequence name");
            haveName = true; options.sequenceName = arguments[i];
        } else if (parseOptions && argument.starts_with('-'))
            return argumentError("unknown option; use --help or -- before dashed paths");
        else paths.push_back(argument);
    }
    if (paths.size() != 2 || !iisc_native_cli::localPath(paths[0]) || !iisc_native_cli::localPath(paths[1]))
        return argumentError("one local input path and one local output directory are required");
    const auto input = std::filesystem::absolute(iisc_native_cli::path(paths[0]));
    auto output = std::filesystem::absolute(iisc_native_cli::path(paths[1])).lexically_normal();
    while (output.filename().empty() && output != output.root_path()) output = output.parent_path();
    std::error_code error;
    if (!std::filesystem::is_regular_file(input, error) || error) {
        std::cerr << "iisc-export-timeline: IoError: input is not a readable regular file\n"; return 1;
    }
    const auto source = std::filesystem::canonical(input, error);
    if (error) { std::cerr << "iisc-export-timeline: IoError: " << error.message() << '\n'; return 1; }
    const auto status = std::filesystem::symlink_status(output, error);
    if (error && error != std::errc::no_such_file_or_directory) {
        std::cerr << "iisc-export-timeline: IoError: " << error.message() << '\n'; return 1;
    }
    if (status.type() != std::filesystem::file_type::not_found) {
        std::cerr << "iisc-export-timeline: AlreadyExists: the destination already exists\n"; return 1;
    }
    error.clear();
    if (!std::filesystem::is_directory(output.parent_path(), error) || error) {
        std::cerr << "iisc-export-timeline: IoError: destination parent directory does not exist\n"; return 1;
    }
    SerializationLimits limits;
    limits.maximumContainerBytes = std::min(options.limits.maxInputBytes, options.limits.maxDecodedBytes);
    limits.maximumCanvasPixels = options.limits.maxPixelsPerFrame;
    limits.maximumTotalRasterPixels = options.limits.maxDecodedBytes / sizeof(std::uint32_t);
    limits.maximumLayers = options.maxLayers;
    Document document;
    const auto text = source.u8string();
    const auto loaded = DocumentFile::readSnapshot(std::string(text.begin(), text.end()), document, limits);
    if (!loaded.ok()) {
        std::cerr << "iisc-export-timeline: " << iisc_native_cli::codeName(loaded.code) << ": " << loaded.message << '\n'; return 1;
    }
    const auto exported = exportTimelineInterchange(document, paths[1], options);
    for (const auto &warning : exported.warnings) std::cerr << "iisc-export-timeline: warning: " << warning << '\n';
    if (!exported.ok()) {
        std::cerr << "iisc-export-timeline: " << iisc_native_cli::codeName(exported.code) << ": " << exported.message << '\n'; return 1;
    }
    std::cout << "Exported timeline package -> " << paths[1] << '\n'; return 0;
}
template<class Arg> int entry(int argc, Arg **argv)
{
    try { return run(iisc_native_cli::arguments(argc, argv)); }
    catch (const std::exception &error) { std::cerr << "iisc-export-timeline: IoError: " << error.what() << '\n'; }
    catch (...) { std::cerr << "iisc-export-timeline: IoError: an unexpected export failure occurred\n"; }
    return 1;
}
}
#ifdef _WIN32
int wmain(int argc, wchar_t **argv) { return entry(argc, argv); }
#else
int main(int argc, char **argv) { return entry(argc, argv); }
#endif
