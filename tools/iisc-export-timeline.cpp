#include "IiscInput_p.hpp"

#include <Timeline/TimelineInterchange.h>

#include <QDir>
#include <QFileInfo>
#include <QGuiApplication>
#include <QStringList>

#include <exception>
#include <iostream>
#include <string_view>

namespace {
using namespace iiSharedCanvas;
using namespace iisc_tools;

constexpr std::string_view usage =
    "Usage: iisc-export-timeline [--name TEXT] [--] INPUT.iisc OUTPUT_DIR\n"
    "       iisc-export-timeline --help\n"
    "Export a native snapshot or working-file timeline to a NEW package directory.\n"
    "The package contains timeline.xml, timeline.fcpxml, PNG/WAV media, manifest.json,\n"
    "and a native source.iisc snapshot. Existing outputs are never overwritten.\n"
    "Source databases are opened read-only and backed up before document decoding.\n"
    "Use -- before dashed paths.\n";

int argumentError(std::string_view message)
{
    std::cerr << "iisc-export-timeline: InvalidArgument: " << message << '\n' << usage;
    return 2;
}

int convert(const QStringList &arguments, int &guiArgc, char **guiArgv)
{
    if (arguments.size() == 1 && (arguments.front() == "--help" || arguments.front() == "-h")) {
        std::cout << usage;
        return 0;
    }
    TimelineInterchangeOptions options;
    QStringList paths;
    bool parseOptions = true;
    bool haveName = false;
    for (qsizetype index = 0; index < arguments.size(); ++index) {
        const auto &argument = arguments[index];
        if (parseOptions && argument == "--") { parseOptions = false; }
        else if (parseOptions && argument == "--name") {
            if (haveName) { return argumentError("--name may only be specified once"); }
            if (++index == arguments.size() || arguments[index].isEmpty()) {
                return argumentError("--name requires a nonempty sequence name");
            }
            haveName = true;
            options.sequenceName = arguments[index].toStdString();
        } else if (parseOptions && argument.startsWith('-')) {
            return argumentError("unknown option; use --help or -- before dashed paths");
        } else { paths.push_back(argument); }
    }
    if (paths.size() != 2 || paths[0].isEmpty() || paths[1].isEmpty()) {
        return argumentError("exactly one input and one output directory are required");
    }
    if (paths[0].contains("://") || paths[1].contains("://")) {
        return argumentError("only local filesystem paths are supported");
    }
    const QFileInfo input(paths[0]);
    const QFileInfo output(QDir::cleanPath(paths[1]));
    if (!input.isFile() || !input.isReadable()) {
        throw Failure(MediaIoCode::IoError, "input is not a readable regular file");
    }
    if (output.exists() || output.isSymLink()) {
        throw Failure(MediaIoCode::AlreadyExists, "the destination already exists; a new package directory is required");
    }
    if (!output.dir().exists()) {
        throw Failure(MediaIoCode::IoError, "the destination parent directory does not exist");
    }
    // Parse only our arguments. Qt receives argv[0] and never consumes paths or flags.
    if (qEnvironmentVariableIsEmpty("QT_QPA_PLATFORM")) { qputenv("QT_QPA_PLATFORM", "offscreen"); }
    QGuiApplication application(guiArgc, guiArgv);
    const auto document = loadDocument(input.canonicalFilePath(), output.absolutePath(), options.limits, options.maxLayers);
    const auto result = exportTimelineInterchange(document, output.absoluteFilePath().toStdString(), options);
    for (const auto &warning : result.warnings) {
        std::cerr << "iisc-export-timeline: warning: " << warning << '\n';
    }
    if (!result.ok()) { throw Failure(result.code, result.message); }
    std::cout << "Exported timeline package -> " << paths[1].toStdString() << '\n';
    return 0;
}
}

int main(int argc, char **argv)
{
    try {
        QStringList arguments;
        for (int index = 1; index < argc; ++index) { arguments.push_back(QString::fromLocal8Bit(argv[index])); }
        int guiArgc = 1;
        char *guiArgv[] = {argv[0], nullptr};
        return convert(arguments, guiArgc, guiArgv);
    } catch (const Failure &failure) {
        std::cerr << "iisc-export-timeline: " << codeName(failure.code) << ": " << failure.what() << '\n';
    } catch (const std::exception &failure) {
        std::cerr << "iisc-export-timeline: IoError: " << failure.what() << '\n';
    } catch (...) {
        std::cerr << "iisc-export-timeline: IoError: an unexpected export failure occurred\n";
    }
    return 1;
}
