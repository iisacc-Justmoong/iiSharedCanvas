#include <File/DocumentFile.h>
#include <Layered/LayeredDocumentCodec.h>

#include <QFileInfo>
#include <QGuiApplication>
#include <QStringList>

#include <exception>
#include <iostream>
#include <string_view>

namespace {
using namespace iiSharedCanvas;

constexpr std::string_view usage =
    "Usage: iisc-import [--id-prefix PREFIX] [--] INPUT OUTPUT.iisc\n"
    "       iisc-import --help\n"
    "Import supported PSD or OpenRaster layers into a new .iisc working file.\n"
    "Existing output files are never overwritten. Use -- before dashed paths.\n";

const char *codeName(MediaIoCode code)
{
    switch (code) {
    case MediaIoCode::None: return "None";
    case MediaIoCode::InvalidArgument: return "InvalidArgument";
    case MediaIoCode::UnsupportedFormat: return "UnsupportedFormat";
    case MediaIoCode::UnsupportedFeature: return "UnsupportedFeature";
    case MediaIoCode::DependencyUnavailable: return "DependencyUnavailable";
    case MediaIoCode::InvalidData: return "InvalidData";
    case MediaIoCode::LimitExceeded: return "LimitExceeded";
    case MediaIoCode::AlreadyExists: return "AlreadyExists";
    case MediaIoCode::IoError: return "IoError";
    case MediaIoCode::Cancelled: return "Cancelled";
    case MediaIoCode::TimedOut: return "TimedOut";
    }
    return "UnknownError";
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

int argumentError(std::string_view message)
{
    std::cerr << "iisc-import: InvalidArgument: " << message << '\n' << usage;
    return 2;
}

int convert(const QStringList &arguments, int &guiArgc, char **guiArgv)
{
    if (arguments.size() == 1 && (arguments.front() == "--help" || arguments.front() == "-h")) {
        std::cout << usage;
        return 0;
    }

    LayeredDocumentImportOptions options;
    QStringList paths;
    bool parseOptions = true;
    bool prefixSeen = false;
    for (qsizetype index = 0; index < arguments.size(); ++index) {
        const auto &argument = arguments[index];
        if (parseOptions && argument == "--") {
            parseOptions = false;
        } else if (parseOptions && (argument == "--id-prefix" || argument.startsWith("--id-prefix="))) {
            if (prefixSeen) { return argumentError("--id-prefix may only be specified once"); }
            prefixSeen = true;
            QString prefix;
            if (argument == "--id-prefix") {
                if (index + 1 >= arguments.size() || arguments[index + 1].startsWith('-')) {
                    return argumentError("--id-prefix requires a value; use --id-prefix=VALUE for a dashed prefix");
                }
                prefix = arguments[++index];
            } else {
                prefix = argument.mid(qsizetype(std::string_view("--id-prefix=").size()));
            }
            if (prefix.isEmpty()) { return argumentError("the ID prefix must not be empty"); }
            options.idPrefix = prefix.toStdString();
        } else if (parseOptions && argument.startsWith('-')) {
            return argumentError("unknown option; use --help, or -- before dashed file paths");
        } else {
            paths.push_back(argument);
        }
    }
    if (paths.size() != 2 || paths[0].isEmpty() || paths[1].isEmpty()) {
        return argumentError("exactly one input and one output path are required");
    }
    const QFileInfo output(paths[1]);
    if (output.suffix().compare("iisc", Qt::CaseInsensitive) != 0) {
        return argumentError("the output path must have the .iisc extension");
    }
    if (output.exists() || output.isSymLink()) {
        std::cerr << "iisc-import: AlreadyExists: the output path already exists: "
                  << paths[1].toStdString() << '\n';
        return 1;
    }

    // Only the program name is passed to Qt. Qt-specific switches must not eat
    // positional file names or change the utility's documented option grammar.
    if (qEnvironmentVariableIsEmpty("QT_QPA_PLATFORM")) { qputenv("QT_QPA_PLATFORM", "offscreen"); }
    QGuiApplication application(guiArgc, guiArgv);
    const auto imported = importLayeredDocument(paths[0].toStdString(), options);
    for (const auto &warning : imported.result.warnings) {
        std::cerr << "iisc-import: warning: " << warning << '\n';
    }
    if (!imported.ok()) {
        std::cerr << "iisc-import: " << codeName(imported.result.code) << ": "
                  << imported.result.message << '\n';
        return 1;
    }

    DocumentFile destination;
    const auto created = destination.create(paths[1].toStdString(), imported.document);
    if (!created.ok()) {
        std::cerr << "iisc-import: " << codeName(created.code) << ": " << created.message << '\n';
        return 1;
    }
    // create() commits synchronously. Closing is not a delayed save operation.
    destination.close();
    std::cout << "Imported " << imported.format << ": " << imported.document.layers.size()
              << " layers -> " << paths[1].toStdString() << '\n';
    return 0;
}
} // namespace

int main(int argc, char **argv)
{
    try {
        QStringList arguments;
        for (int index = 1; index < argc; ++index) {
            arguments.push_back(QString::fromLocal8Bit(argv[index]));
        }
        int guiArgc = 1;
        char *guiArgv[] = {argv[0], nullptr};
        return convert(arguments, guiArgc, guiArgv);
    } catch (const std::exception &error) {
        std::cerr << "iisc-import: IoError: " << error.what() << '\n';
        return 1;
    } catch (...) {
        std::cerr << "iisc-import: IoError: an unexpected conversion failure occurred\n";
        return 1;
    }
}
