#include <iiSharedCanvasNative.h>
#include <iiFileProviderNative.h>

#include <filesystem>
#include <iostream>
#include <string>
#include <string_view>
#include <vector>

using namespace iiSharedCanvas;
namespace fs = std::filesystem;
namespace provider = iiFileProvider::native;

namespace {
int failures = 0;
void check(bool value, const char *message)
{
    if (!value) { std::cerr << message << '\n'; ++failures; }
}
std::string text(const fs::path &path)
{
    const auto value = path.u8string();
    return {value.begin(), value.end()};
}
Document fixture()
{
    Document document;
    document.formatVersion = {1, 19};
    document.extent = {2, 2};
    document.assets.emplace_back(RasterAsset{"pixels", makeRasterLayer(2, 2, 0x80ab1234U)});
    document.layers.emplace_back(StaticBitmapLayer{{"layer", "레이어"}, StaticSource{"pixels"}});
    return document;
}
void expectCurrent(const std::string &path, std::string_view authorship = {})
{
    Document document;
    const auto result = DocumentFile::readSnapshot(path, document);
    check(result.ok() && document.formatVersion.major == CurrentFormatMajor
              && document.formatVersion.minor == CurrentFormatMinor,
          "upgraded document has the current model version");
    if (result.ok()) {
        const auto frame = renderFrame(document, 0);
        check(frame.ok() && frame.pixels.pixels.front() == 0x80ab1234U,
              "upgraded artwork is preserved");
        check(layerProperties(document.layers.front()).name == "레이어", "layer name is preserved");
        if (!authorship.empty())
            check(document.authorship.dump() == authorship, "upgrade preserves original authorship without stamping a new edit");
    }
}
}

int run(const std::vector<std::string> &arguments)
{
    if (arguments.size() == 2 && arguments[0] == "--create") {
        const auto encoded = encodeIisc(fixture());
        if (!encoded.ok()) return 1;
        const auto &path = arguments[1];
        provider::File::create(fs::path(std::u8string(path.begin(), path.end())),
            std::string_view(reinterpret_cast<const char *>(encoded.bytes.data()), encoded.bytes.size()));
        return 0;
    }
    if (arguments.size() == 2 && (arguments[0] == "--create-working" || arguments[0] == "--create-working-alt")) {
        auto document = fixture();
        if (arguments[0] == "--create-working-alt") layerProperties(document.layers.front()).name = "WAL 변경";
        DocumentFile file;
        const auto created = file.create(arguments[1], document);
        if (!created.ok()) return 1;
        file.close();
        return 0;
    }
    if (arguments.size() == 2 && arguments[0] == "--verify") {
        expectCurrent(arguments[1]);
        return failures ? 1 : 0;
    }
    if (arguments.size() == 2 && arguments[0] == "--verify-import") {
        expectCurrent(arguments[1]);
        Document imported;
        const auto loaded = DocumentFile::readSnapshot(arguments[1], imported);
        check(loaded.ok() && !imported.layers.empty() && !imported.assets.empty()
                  && layerProperties(imported.layers.front()).id == "custom-layer-0"
                  && assetId(imported.assets.front()) == "custom-asset-0",
              "layered CLI preserves the requested ID prefix");
        return failures ? 1 : 0;
    }
    fs::create_directories(IISHAREDCANVAS_TEST_OUTPUT_DIR);
    provider::TemporaryDirectory temporary{fs::path(IISHAREDCANVAS_TEST_OUTPUT_DIR)};
    auto originalDocument = fixture();
    constexpr auto captured = "2026-10-28T00:00:00.000Z";
    const auto author = provider::FileAuthor::fromIisaccAccount(
        {{"sub", "upgrade-test-author"}, {"email", "upgrade-author@example.com"},
         {"displayName", "Upgrade Author"}}, "https://iisacc.com", captured);
    check(author.has_value() && originalDocument.authorship.setAuthor(*author, captured),
          "upgrade fixture has a valid original author");
    const auto originalAuthorship = originalDocument.authorship.dump();
    check(!originalAuthorship.empty(), "upgrade fixture retains original authorship metadata");
    const auto encoded = encodeIisc(originalDocument);
    check(encoded.ok(), "legacy snapshot fixture encodes");
    const std::string bytes(reinterpret_cast<const char *>(encoded.bytes.data()), encoded.bytes.size());
    const auto snapshot = temporary.path() / "source 한글.iisc";
    provider::File::create(snapshot, bytes);
    const auto copy = temporary.path() / "copy.iisc";
    const auto copied = upgradeIiscFile(text(snapshot), text(copy));
    check(copied.ok() && copied.changed && copied.sourceVersion.minor == 19,
          "legacy snapshot upgrades to an explicit destination");
    check(provider::File::read(snapshot) == bytes, "explicit upgrade preserves source snapshot");
    expectCurrent(text(copy), originalAuthorship);
    check(upgradeIiscFile(text(snapshot), text(copy)).code == DocumentFileCode::AlreadyExists,
          "explicit upgrade cannot overwrite its destination");
    const auto inplace = upgradeIiscFile(text(snapshot));
    check(inplace.ok() && inplace.changed, "legacy snapshot upgrades in place");
    expectCurrent(text(snapshot), originalAuthorship);
    const auto upgradedBytes = provider::File::read(snapshot);
    const auto writeTime = fs::last_write_time(snapshot);
    const auto noop = upgradeIiscFile(text(snapshot));
    check(noop.ok() && !noop.changed && provider::File::read(snapshot) == upgradedBytes
              && fs::last_write_time(snapshot) == writeTime,
          "current in-place snapshot upgrade is a no-op");

    const auto working = temporary.path() / "working.iisc";
    {
        DocumentFile owner;
        check(owner.create(text(working), originalDocument).ok(), "legacy working fixture creates");
        owner.close();
    }
    const auto workingBytes = provider::File::read(working);
    const auto workingCopy = temporary.path() / "working-copy.iisc";
    const auto copiedWorking = upgradeIiscFile(text(working), text(workingCopy));
    check(copiedWorking.ok() && copiedWorking.changed, "working document upgrades to a new file");
    check(provider::File::read(working) == workingBytes, "explicit working upgrade leaves source bytes intact");
    const auto workingCopyReader = provider::File::openRead(workingCopy);
    check(workingCopyReader->read(16) == std::string("SQLite format 3\0", 16),
          "working destination preserves SQLite storage kind");
    expectCurrent(text(workingCopy), originalAuthorship);
    const auto upgradedWorking = upgradeIiscFile(text(working));
    check(upgradedWorking.ok() && upgradedWorking.changed, "working document upgrades in place");
    expectCurrent(text(working), originalAuthorship);
    const auto workingNoop = upgradeIiscFile(text(working));
    check(workingNoop.ok() && !workingNoop.changed, "current working document is a no-op");

    const auto bad = temporary.path() / "bad.iisc";
    provider::File::create(bad, "not an IISC document");
    check(!upgradeIiscFile(text(bad)).ok() && provider::File::read(bad) == "not an IISC document",
          "invalid input is rejected without mutation");
    SerializationLimits limits; limits.maximumContainerBytes = 8;
    check(upgradeIiscFile(text(snapshot), {}, limits).code == DocumentFileCode::LimitExceeded,
          "input size limit is enforced");
    check(upgradeIiscFile("https://example.invalid/a.iisc").code == DocumentFileCode::InvalidPath,
          "remote input is rejected");
    std::error_code error;
    const auto link = temporary.path() / "link.iisc";
    fs::create_symlink(snapshot, link, error);
    if (!error) check(upgradeIiscFile(text(link)).code == DocumentFileCode::InvalidPath,
                      "symbolic-link source is rejected");
    return failures ? 1 : 0;
}

#ifdef _WIN32
int wmain(int argc, wchar_t **argv)
{
    std::vector<std::string> arguments;
    for (int i = 1; i < argc; ++i) {
        const auto value = fs::path(argv[i]).u8string();
        arguments.emplace_back(value.begin(), value.end());
    }
    return run(arguments);
}
#else
int main(int argc, char **argv)
{
    return run({argv + 1, argv + argc});
}
#endif
