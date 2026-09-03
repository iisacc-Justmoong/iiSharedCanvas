#include <iiSharedCanvas.h>

#include <QDir>
#include <QFile>
#include <QGuiApplication>
#include <QTemporaryDir>

#include <algorithm>
#include <array>
#include <iostream>
#include <string>

namespace {
int failures = 0;
void expect(bool condition, const std::string &message)
{
    if (!condition) { ++failures; std::cerr << message << '\n'; }
}
}

int main(int argc, char **argv)
{
    QGuiApplication application(argc, argv);
    using namespace iiSharedCanvas;
    QDir().mkpath(IISHAREDCANVAS_TEST_OUTPUT_DIR);
    QTemporaryDir directory(QStringLiteral(IISHAREDCANVAS_TEST_OUTPUT_DIR "/layered-dispatch-XXXXXX"));
    expect(directory.isValid(), "create layered dispatch test directory");

    const auto formats = layeredDocumentFormats();
    for (const std::string name : {"psd", "ora"}) {
        expect(std::count_if(formats.begin(), formats.end(), [&](const auto &format) {
            return format.name == name && format.canRead && format.canWrite == (name == "psd");
        }) == 1, "advertise implemented layered read/write capabilities exactly once: " + name);
    }
    expect(formats.size() == 2, "do not advertise unimplemented layered formats");
    expect(decodeLayeredDocument({}).result.code == MediaIoCode::InvalidArgument,
           "empty input is not a layered document");
    const std::array<std::uint8_t, 8> unknown{'u', 'n', 'k', 'n', 'o', 'w', 'n', '!'};
    auto failed = decodeLayeredDocument(unknown);
    expect(failed.result.code == MediaIoCode::UnsupportedFormat && failed.format.empty()
           && failed.document.layers.empty() && failed.document.assets.empty(),
           "unknown content fails without a partially imported document");

    LayeredDocumentImportOptions options;
    options.idPrefix.clear();
    expect(decodeLayeredDocument(unknown, options).result.code == MediaIoCode::InvalidArgument,
           "empty id prefix is rejected");
    options.idPrefix = std::string("bad\0prefix", 10);
    expect(decodeLayeredDocument(unknown, options).result.code == MediaIoCode::InvalidArgument,
           "embedded NUL in the id prefix is rejected");
    options.idPrefix = std::string(1, char(0xff));
    expect(decodeLayeredDocument(unknown, options).result.code == MediaIoCode::InvalidArgument,
           "noncanonical UTF-8 in the id prefix is rejected");
    options.idPrefix.assign(1025, 'a');
    expect(decodeLayeredDocument(unknown, options).result.code == MediaIoCode::InvalidArgument,
           "id prefix has a documented size bound");
    options = {};
    options.limits.maxInputBytes = unknown.size() - 1;
    expect(decodeLayeredDocument(unknown, options).result.code == MediaIoCode::LimitExceeded,
           "input budget is checked before dispatch");

    const std::array<std::uint8_t, 4> truncatedPsd{'8', 'B', 'P', 'S'};
    auto psd = decodeLayeredDocument(truncatedPsd);
    expect(!psd.ok() && psd.format == "psd" && psd.document.assets.empty(),
           "identified but truncated PSD cannot become a composite bitmap");
    const std::array<std::uint8_t, 4> truncatedZip{'P', 'K', 3, 4};
    auto ora = decodeLayeredDocument(truncatedZip);
    expect(!ora.ok() && ora.format == "ora" && ora.document.assets.empty(),
           "identified but truncated ORA cannot become a partial document");

    expect(importLayeredDocument("").result.code == MediaIoCode::InvalidArgument,
           "empty file path rejected");
    expect(importLayeredDocument("https://example.invalid/canvas.ora").result.code == MediaIoCode::InvalidArgument,
           "network resources rejected without fetching");
    expect(importLayeredDocument(directory.filePath("missing.ora").toStdString()).result.code == MediaIoCode::IoError,
           "missing source reported as I/O failure");
    const auto fakePath = directory.filePath("misleading.psd");
    QFile fake(fakePath);
    expect(fake.open(QIODevice::WriteOnly) && fake.write("not a PSD") == 9,
           "write independently constructed invalid fixture");
    fake.close();
    expect(importLayeredDocument(fakePath.toStdString()).result.code == MediaIoCode::UnsupportedFormat,
           "a suffix cannot override content identification");

    // Optional local fixtures let maintainers verify independently produced
    // documents without introducing network access into CTest or the library.
    for (int index = 1; index < argc; ++index) {
        auto external = importLayeredDocument(argv[index]);
        expect(external.ok(), "external layered fixture: " + external.result.message);
        if (!external.ok()) { continue; }
        const auto encoded = encodeIisc(external.document);
        const auto restored = decodeIisc(encoded.bytes);
        expect(encoded.ok() && restored.ok()
               && encodeIisc(restored.document).bytes == encoded.bytes,
               "external fixture preserves every field through canonical serialization");
        const auto before = renderFrame(external.document, 0);
        const auto after = renderFrame(restored.document, 0);
        expect(before.ok() && after.ok() && before.pixels.pixels == after.pixels.pixels,
               "external fixture preserves the rendered frame through serialization");
        DocumentFile file;
        const auto output = directory.filePath(QString::number(index) + ".iisc").toStdString();
        expect(file.create(output, external.document).ok(), "external fixture creates a native working file");
        DocumentFile reopened;
        expect(reopened.open(output).ok() && reopened.document()
               && encodeIisc(*reopened.document()).bytes == encoded.bytes,
               "external fixture reopens with exact native layer data");
        std::cout << external.format << ' ' << external.document.extent.width << 'x'
                  << external.document.extent.height << ": " << external.document.layers.size() << " layers\n";
        for (const auto &layer : external.document.layers) {
            const auto &properties = layerProperties(layer);
            std::cout << properties.id << " name=" << properties.name << " offset="
                      << properties.transform.translationX << ',' << properties.transform.translationY << '\n';
        }
    }
    return failures == 0 ? 0 : 1;
}
