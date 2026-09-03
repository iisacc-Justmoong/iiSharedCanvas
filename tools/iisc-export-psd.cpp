#include "IiscInput_p.hpp"

#include <Layered/LayeredDocumentCodec.h>

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

int convert(const QStringList &arguments, int &guiArgc, char **guiArgv)
{
    if (arguments.size() == 1 && (arguments.front() == "--help" || arguments.front() == "-h")) {
        std::cout << usage;
        return 0;
    }
    PsdExportOptions options;
    QStringList paths;
    bool parseOptions = true;
    for (const auto &argument : arguments) {
        if (parseOptions && argument == "--") { parseOptions = false; }
        else if (parseOptions && argument == "--overwrite") {
            if (options.overwrite) { return argumentError("--overwrite may only be specified once"); }
            options.overwrite = true;
        } else if (parseOptions && argument.startsWith('-')) {
            return argumentError("unknown option; use --help or -- before dashed paths");
        } else { paths.push_back(argument); }
    }
    if (paths.size() != 2 || paths[0].isEmpty() || paths[1].isEmpty()) {
        return argumentError("exactly one input and one output path are required");
    }
    if (QFileInfo(paths[1]).suffix().compare("psd", Qt::CaseInsensitive) != 0) {
        return argumentError("the destination must have the .psd extension");
    }
    if (paths[0].contains("://") || paths[1].contains("://")) {
        return argumentError("only local filesystem paths are supported");
    }
    const QFileInfo input(paths[0]);
    const QFileInfo output(paths[1]);
    if (!input.isFile() || !input.isReadable()) {
        throw Failure(MediaIoCode::IoError, "input is not a readable regular file");
    }
    if (output.exists() || output.isSymLink()) {
        if (!options.overwrite) { throw Failure(MediaIoCode::AlreadyExists, "the destination already exists"); }
        if (output.isSymLink() || !output.isFile()
            || output.canonicalFilePath() == input.canonicalFilePath()) {
            throw Failure(MediaIoCode::InvalidArgument, "the destination must not be the source, a source alias, or a special file");
        }
    }
    if (!output.dir().exists()) { throw Failure(MediaIoCode::IoError, "the destination parent directory does not exist"); }
    // Qt must not consume the utility's paths/options or require a desktop session.
    if (qEnvironmentVariableIsEmpty("QT_QPA_PLATFORM")) { qputenv("QT_QPA_PLATFORM", "offscreen"); }
    QGuiApplication application(guiArgc, guiArgv);
    const auto document = loadDocument(input.canonicalFilePath(), output.absolutePath(), options.limits, options.maxLayers);
    const auto result = exportPsd(document, output.absoluteFilePath().toStdString(), options);
    for (const auto &warning : result.warnings) {
        std::cerr << "iisc-export-psd: warning: " << warning << '\n';
    }
    if (!result.ok()) { throw Failure(result.code, result.message); }
    std::cout << "Exported psd frame 0 -> " << paths[1].toStdString() << '\n';
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
        std::cerr << "iisc-export-psd: " << codeName(failure.code) << ": " << failure.what() << '\n';
    } catch (const std::exception &failure) {
        std::cerr << "iisc-export-psd: IoError: " << failure.what() << '\n';
    } catch (...) {
        std::cerr << "iisc-export-psd: IoError: an unexpected export failure occurred\n";
    }
    return 1;
}
