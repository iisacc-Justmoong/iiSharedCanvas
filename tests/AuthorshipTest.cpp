#include <iiSharedCanvas.h>
#include <File/DocumentFile.h>
#include <iiFileProvider.h>
#include <QCoreApplication>
#include <QDir>
#include <QTemporaryDir>
#include <cstdlib>
#include <iostream>
using namespace iiSharedCanvas;
static void check(bool value, const char *message) {
    if (!value) { std::cerr << message << '\n'; std::exit(1); }
}
int main(int argc, char **argv) {
    QCoreApplication app(argc, argv);
    const auto author = iiFileProvider::FileAuthor::fromIisaccAccount(
        QJsonObject{{"sub", "canvas-author"}, {"email", "author@example.com"},
            {"displayName", "Canvas Author"}, {"userId", "@canvas_author"},
            {"societyCloudMembership", "Free"}, {"avatarUrl", QJsonValue::Null}},
        QUrl("https://iisacc.com"), QDateTime::currentDateTimeUtc());
    check(author.has_value(), "test account must be valid");
    Document document; document.extent = {64, 64};
    DocumentEditor editor(document);
    check(editor.setFileAuthor(*author).changed, "author must attach immediately");
    const auto first = document.authorship.dump();
    check(editor.setFrameCount(12).changed, "timeline edit must apply");
    check(first != document.authorship.dump() && document.authorship.revision() == 2,
          "accepted edit must synchronously dump authorship once");
    const auto before = document.authorship.dump();
    check(!editor.setFrameCount(12).changed && !editor.setFrameCount(0).ok(), "no-op/rejected edit");
    check(before == document.authorship.dump(), "no-op/rejected edits must preserve metadata");
    auto encoded = encodeIisc(document);
    check(encoded.ok(), "native encoding");
    auto decoded = decodeIisc(encoded.bytes);
    check(decoded.ok() && decoded.document.authorship.dump() == before, "native author round trip");
    check(!decoded.document.authorship.hasActiveAuthor(), "loaded author must not impersonate editor");
    QTemporaryDir dir(QDir::currentPath() + "/authorship-XXXXXX");
    check(dir.isValid(), "working directory");
    DocumentFile file;
    check(file.create((dir.path()+"/author.iisc").toStdString(), document).ok(), "working file creation");
    auto current = [&] {
        DocumentFile reader; check(reader.open(file.filePath()).ok(), "second connection open");
        return reader.document()->authorship.dump();
    };
    DocumentEditor bound(file);
    check(!bound.setFileAuthor(*author).changed, "same author only selects runtime context");
    check(bound.setFrameCount(24).changed && current() == file.document()->authorship.dump(),
          "authorship must commit to disk before editor returns");
    check(!file.document()->authorship.toJson()["lastAuthor"].isNull(), "reselecting the same profile must retain active attribution");
    check(bound.insertRasterAsset("bitmap",makeRasterLayer(64,64)).changed, "raster insertion");
    BitmapEditor pixels(file,"bitmap");
    auto pixelRevision = file.document()->authorship.revision();
    check(pixels.setPixel(2,3,0xffaabbcc), "pixel mutation");
    check(file.document()->authorship.revision()==pixelRevision+1 && current()==file.document()->authorship.dump(),
          "pixel metadata must reach a second connection immediately");
    check(pixels.setPixel(2,3,0xffaabbcc) && file.document()->authorship.revision()==pixelRevision+1, "identical pixels must not stamp authorship");
    auto revision = file.document()->authorship.revision();
    check(file.edit([](Document &draft) { draft.extent.width = 80; return true; }).changed,
          "raw transaction must apply");
    check(file.document()->authorship.revision() == revision + 1 && current() == file.document()->authorship.dump(),
          "raw transaction must synchronously stamp metadata");
    const auto stable = current();
    check(!file.edit([](Document &draft) { draft.extent.width = 99; return false; }).ok(), "rejected transaction");
    check(stable == current() && stable == file.document()->authorship.dump(), "transaction rollback includes metadata");
    Document legacy; legacy.extent = {16, 16}; legacy.formatVersion.minor = 4;
    auto old = encodeIisc(legacy); check(old.ok(), "legacy encoding");
    auto restored = decodeIisc(old.bytes); check(restored.ok() && restored.document.authorship.isEmpty(), "legacy read");
    DocumentEditor migrate(restored.document); check(migrate.setFrameCount(3).changed, "legacy edit");
    check(restored.document.formatVersion.minor == CurrentFormatMinor && restored.document.authorship.revision() == 1,
          "only a real legacy edit upgrades authorship format");
    return 0;
}
