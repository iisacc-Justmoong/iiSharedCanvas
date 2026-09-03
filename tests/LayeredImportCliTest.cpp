#include <Document/Document.h>
#include <File/DocumentFile.h>
#include <Validation/Validation.h>

#include <QDir>
#include <QFile>
#include <QGuiApplication>
#include <QProcess>
#include <QProcessEnvironment>
#include <QTemporaryDir>

#include <array>
#include <cstdint>
#include <iostream>
#include <string>
#include <vector>

namespace {
using namespace iiSharedCanvas;
using Bytes = std::vector<std::uint8_t>;
int failures = 0;

void expect(bool condition, const std::string &message)
{
    if (!condition) {
        std::cerr << message << '\n';
        ++failures;
    }
}

void u16(Bytes &out, std::uint16_t value)
{
    out.push_back(std::uint8_t(value >> 8));
    out.push_back(std::uint8_t(value));
}

void u32(Bytes &out, std::uint32_t value)
{
    u16(out, std::uint16_t(value >> 16));
    u16(out, std::uint16_t(value));
}

void append(Bytes &out, const Bytes &data) { out.insert(out.end(), data.begin(), data.end()); }
void text(Bytes &out, const std::string &data) { out.insert(out.end(), data.begin(), data.end()); }
void block(Bytes &out, const Bytes &data) { u32(out, std::uint32_t(data.size())); append(out, data); }

// Independent one-pixel raw PSD records; the black merged preview is deliberately
// different from the actual layers so a composite-only conversion cannot pass.
Bytes psd(bool unsupportedBlend = false, bool metadata = false)
{
    Bytes records, channels;
    // PSD binary layer records, unlike OpenRaster XML, are bottom-to-top.
    const std::array<std::string, 2> names{"Bottom", "Top"};
    const std::array<std::uint32_t, 2> colors{0xff112233U, 0x80aabbccU};
    for (std::size_t index = 0; index < names.size(); ++index) {
        u32(records, 0); u32(records, std::uint32_t(index == 1));
        u32(records, 1); u32(records, std::uint32_t(index == 1 ? 2 : 1));
        u16(records, 4);
        for (int channel : {0, 1, 2, -1}) {
            u16(records, std::uint16_t(channel)); u32(records, 3);
            u16(channels, 0);
            channels.push_back(std::uint8_t(colors[index] >> (channel == -1 ? 24 : 16 - channel * 8)));
        }
        text(records, "8BIM"); text(records, unsupportedBlend ? "diff" : "norm");
        records.insert(records.end(), {255, 0, 0, 0});
        Bytes extra;
        u32(extra, 0); u32(extra, 0);
        extra.push_back(std::uint8_t(names[index].size())); text(extra, names[index]);
        while (extra.size() % 4 != 0) { extra.push_back(0); }
        block(records, extra);
    }
    Bytes layerInfo;
    u16(layerInfo, 2); append(layerInfo, records); append(layerInfo, channels);
    if (layerInfo.size() % 2 != 0) { layerInfo.push_back(0); }
    Bytes layerAndMask;
    block(layerAndMask, layerInfo); u32(layerAndMask, 0);
    Bytes resources;
    if (metadata) {
        text(resources, "8BIM"); u16(resources, 1005);
        u16(resources, 0); // Empty resource Pascal name, padded to two bytes.
        Bytes resolution;
        u32(resolution, 72U << 16); u16(resolution, 1); u16(resolution, 1);
        u32(resolution, 72U << 16); u16(resolution, 1); u16(resolution, 1);
        block(resources, resolution);
    }
    Bytes out;
    text(out, "8BPS"); u16(out, 1); out.insert(out.end(), 6, 0);
    u16(out, 3); u32(out, 1); u32(out, 2); u16(out, 8); u16(out, 3);
    u32(out, 0); block(out, resources); block(out, layerAndMask);
    u16(out, 0); out.insert(out.end(), 6, 0);
    return out;
}

QByteArray byteArray(const Bytes &bytes)
{
    return QByteArray(reinterpret_cast<const char *>(bytes.data()), qsizetype(bytes.size()));
}

void writeFile(const QString &path, const QByteArray &data)
{
    QFile file(path);
    expect(file.open(QIODevice::WriteOnly | QIODevice::NewOnly) && file.write(data) == data.size(),
           "create CLI fixture: " + path.toStdString());
}

QByteArray readFile(const QString &path)
{
    QFile file(path);
    expect(file.open(QIODevice::ReadOnly), "read CLI fixture: " + path.toStdString());
    return file.readAll();
}

struct ProcessResult {
    int code = -1;
    QByteArray output;
    QByteArray error;
};

ProcessResult run(const QStringList &arguments, const QString &directory, bool withoutPlatform = false)
{
    QProcess process;
    process.setWorkingDirectory(directory);
    auto environment = QProcessEnvironment::systemEnvironment();
    if (withoutPlatform) { environment.remove("QT_QPA_PLATFORM"); }
    else { environment.insert("QT_QPA_PLATFORM", "offscreen"); }
    process.setProcessEnvironment(environment);
    process.start(QStringLiteral(IISHAREDCANVAS_IMPORT_EXECUTABLE), arguments);
    const bool started = process.waitForStarted(10000);
    const bool finished = started && process.waitForFinished(30000);
    expect(started && finished, "CLI process must start and finish: " + process.errorString().toStdString());
    if (!finished) {
        process.kill();
        process.waitForFinished(1000);
    }
    expect(process.exitStatus() == QProcess::NormalExit, "CLI must not crash");
    return {finished ? process.exitCode() : -1, process.readAllStandardOutput(), process.readAllStandardError()};
}

void expectFailure(const ProcessResult &result, const std::string &message)
{
    expect(result.code != 0 && !result.error.isEmpty() && result.output.isEmpty(),
           message + ": " + result.error.toStdString());
}

QString forwardedArgument(const QString &argument)
{
#if defined(Q_OS_UNIX)
    // QProcess encodes every Unix argv entry as a filename. On macOS this
    // normalizes Unicode to NFD; compare the bytes the child actually receives.
    return QString::fromLocal8Bit(QFile::encodeName(argument));
#else
    return argument;
#endif
}

void testSuccessfulConversion(const QString &directory)
{
    const QString source = QDir(directory).filePath(QStringLiteral("원본 레이어 with spaces.data"));
    const QString destination = QDir(directory).filePath(QStringLiteral("변환 결과 with spaces.iisc"));
    const auto bytes = byteArray(psd(false, true));
    writeFile(source, bytes);
    const auto result = run({"--id-prefix", QStringLiteral("가져오기"), source, destination}, directory, true);
    expect(result.code == 0, "two-layer CLI import succeeds: " + result.error.toStdString());
    expect(result.output.contains("psd") && result.output.contains("2")
               && result.output.contains(forwardedArgument(destination).toUtf8()),
           "success reports source format, layer count and exact output path");
    expect(result.error.contains("warning:") && result.error.contains("image resources"),
           "non-persisted metadata warnings are visible on stderr");
    expect(readFile(source) == bytes, "successful conversion must not modify the source");
    expect(readFile(destination).startsWith(QByteArray("SQLite format 3\0", 16)),
           "CLI creates a synchronously committed working file, not a legacy snapshot");
    DocumentFile reader;
    expect(reader.open(destination.toStdString()).ok(), "CLI output reopens in another process lifetime");
    const auto *document = reader.document();
    if (document && document->layers.size() == 2) {
        expect(validate(*document).ok() && document->assets.size() == 2,
               "CLI output has a validated document and two separately editable assets");
        expect(layerProperties(document->layers[0]).name == "Bottom"
                   && layerProperties(document->layers[1]).name == "Top",
               "CLI preserves layer names and bottom-to-top order");
        const auto prefix = forwardedArgument(QStringLiteral("가져오기")).toStdString();
        expect(layerProperties(document->layers[0]).id == prefix + "-layer-0"
                   && layerProperties(document->layers[1]).id == prefix + "-layer-1",
               "CLI forwards the exact Unicode ID prefix");
        const auto *bottom = findRasterAsset(*document, std::get<StaticSource>(layerSource(document->layers[0])).assetId);
        const auto *top = findRasterAsset(*document, std::get<StaticSource>(layerSource(document->layers[1])).assetId);
        expect(bottom && top && bottom->pixels.pixels == std::vector<std::uint32_t>{0xff112233U}
                   && top->pixels.pixels == std::vector<std::uint32_t>{0x80aabbccU}
                   && layerProperties(document->layers[1]).transform.translationX == 1,
               "CLI preserves actual layer pixels, alpha and offsets instead of merged preview");
    } else {
        expect(false, "CLI output contains exactly two layers");
    }
    reader.close();
    const auto committed = readFile(destination);
    const auto collision = run({source, destination}, directory);
    expectFailure(collision, "CLI rejects an existing working file");
    expect(collision.error.contains("AlreadyExists") && readFile(destination) == committed,
           "collision identifies AlreadyExists and leaves the original output byte-exact");
}

void testFailuresAndArguments(const QString &directory)
{
    const QString source = QDir(directory).filePath("input.psd");
    const auto sourceBytes = byteArray(psd());
    writeFile(source, sourceBytes);
    for (const QStringList &arguments : std::vector<QStringList>{
             {}, {source}, {source, "out.iisc", "extra"}, {"--unknown", source, "out.iisc"},
             {"--id-prefix"}, {"--id-prefix", "--", source, "out.iisc"},
             {"--id-prefix", "", source, "out.iisc"},
             {"--id-prefix", "one", "--id-prefix", "two", source, "out.iisc"},
             {source, "out.png"}}) {
        expectFailure(run(arguments, directory), "invalid CLI arguments fail before publication");
        expect(!QFile::exists(QDir(directory).filePath("out.iisc"))
                   && !QFile::exists(QDir(directory).filePath("out.png")),
               "invalid arguments do not publish an output");
    }
    for (const QString &option : {QStringLiteral("--help"), QStringLiteral("-h")}) {
        const auto result = run({option}, directory);
        expect(result.code == 0 && result.output.contains("iisc-import")
                   && result.output.contains("--id-prefix"), "help documents invocation and succeeds");
    }
    const QString unknown = QDir(directory).filePath("unknown.data");
    writeFile(unknown, "not a layered format");
    auto rejected = run({unknown, "unknown.iisc"}, directory);
    expectFailure(rejected, "unknown formats do not become flattened pseudo-layers");
    expect(rejected.error.contains("UnsupportedFormat")
               && !QFile::exists(QDir(directory).filePath("unknown.iisc")),
           "unknown formats report an explicit error without output");
    const QString semantic = QDir(directory).filePath("unsupported.psd");
    writeFile(semantic, byteArray(psd(true)));
    rejected = run({semantic, "unsupported.iisc"}, directory);
    expectFailure(rejected, "unsupported semantics fail closed");
    expect(rejected.error.contains("UnsupportedFeature")
               && !QFile::exists(QDir(directory).filePath("unsupported.iisc")),
           "unsupported semantics never leave a partial working file");
    const QString same = QDir(directory).filePath("same.iisc");
    writeFile(same, sourceBytes);
    rejected = run({same, same}, directory);
    expectFailure(rejected, "source and destination cannot be the same file");
    expect(rejected.error.contains("AlreadyExists") && readFile(same) == sourceBytes,
           "same-file attempt leaves source unchanged");
    expect(readFile(source) == sourceBytes, "failure paths never mutate the input");

    writeFile(QDir(directory).filePath("-source.psd"), sourceBytes);
    const auto dashed = run({"--id-prefix=-ids", "--", "-source.psd", "-output.iisc"}, directory);
    expect(dashed.code == 0, "-- protects dashed positional paths: " + dashed.error.toStdString());
    DocumentFile reader;
    expect(reader.open(QDir(directory).filePath("-output.iisc").toStdString()).ok()
               && reader.document() && layerProperties(reader.document()->layers.front()).id == "-ids-layer-0",
           "equals option syntax preserves a dashed ID prefix");
}
} // namespace

int main(int argc, char **argv)
{
    if (qEnvironmentVariableIsEmpty("QT_QPA_PLATFORM")) { qputenv("QT_QPA_PLATFORM", "offscreen"); }
    QGuiApplication application(argc, argv);
    QDir().mkpath(IISHAREDCANVAS_TEST_OUTPUT_DIR);
    QTemporaryDir directory(QStringLiteral(IISHAREDCANVAS_TEST_OUTPUT_DIR "/layered-cli-XXXXXX"));
    expect(directory.isValid(), "create scoped CLI test directory");
    if (directory.isValid()) {
        testSuccessfulConversion(directory.path());
        testFailuresAndArguments(directory.path());
    }
    return failures == 0 ? 0 : 1;
}
