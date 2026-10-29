#include <iiSharedCanvasNative.h>
#include <charconv>
#include <filesystem>
#include <iostream>
#include <string_view>
#include <vector>

namespace {
using namespace iiSharedCanvas;

std::string quote(std::string_view value)
{
    constexpr char hex[] = "0123456789abcdef";
    std::string result = "\"";
    for (unsigned char c : value) {
        if (c == '"' || c == '\\') { result += '\\'; result += char(c); }
        else if (c < 32) { result += "\\u00"; result += hex[c >> 4]; result += hex[c & 15]; }
        else result += char(c);
    }
    return result + '"';
}

int run(const std::vector<std::string> &arguments)
{
    if (arguments.size() == 2 && arguments[1] == "--help") {
        std::cout << "iisc-working inspect|verify|upgrade-storage PATH [--dry-run] [--maximum-bytes N] [--snapshot-timeout-ms N]\n"
                     "JSON output; inspect/verify/dry-run preserve the source.\n";
        return 0;
    }
    const std::string command = arguments.size() > 1 ? arguments[1] : "";
    const std::string path = arguments.size() > 2 ? arguments[2] : "";
    bool dryRun = false;
    bool customSnapshotTimeout = false;
    int snapshotTimeout = DefaultNativeSnapshotTimeoutMs;
    SerializationLimits limits;
    DocumentFileResult result;
    DocumentFileInfo info;
    bool verified = false, wouldChange = false;
    const auto invalid = [&](std::string message) { result = {DocumentFileCode::InvalidPath, false, std::move(message)}; };
    if (path.empty() || (command != "inspect" && command != "verify" && command != "upgrade-storage"))
        invalid("expected inspect, verify or upgrade-storage and a UTF-8 local path");
    for (std::size_t i = 3; i < arguments.size() && result.ok(); ++i) {
        if (arguments[i] == "--dry-run" && command == "upgrade-storage") dryRun = true;
        else if (arguments[i] == "--maximum-bytes" && i + 1 < arguments.size()) {
            const auto &text = arguments[++i]; std::uint64_t value = 0;
            const auto parsed = std::from_chars(text.data(), text.data() + text.size(), value);
            if (parsed.ec != std::errc{} || parsed.ptr != text.data() + text.size() || !value)
                invalid("maximum-bytes must be a positive integer");
            else limits.maximumContainerBytes = value;
        } else if (arguments[i] == "--snapshot-timeout-ms" && i + 1 < arguments.size()) {
            const auto &text = arguments[++i]; int value = 0;
            const auto parsed = std::from_chars(text.data(), text.data() + text.size(), value);
            if (parsed.ec != std::errc{} || parsed.ptr != text.data() + text.size() || value <= 0)
                invalid("snapshot-timeout-ms must be a positive integer within the int range");
            else { snapshotTimeout = value; customSnapshotTimeout = true; }
        } else invalid("unknown or incomplete option");
    }
    if (result.ok() && customSnapshotTimeout && command != "verify" && !dryRun)
        invalid("snapshot-timeout-ms requires verify or upgrade-storage --dry-run");
    if (result.ok()) result = DocumentFile::readStorageInfo(path, info);
    if (result.ok() && (command == "verify" || dryRun)) {
        Document snapshot;
        result = DocumentFile::readSnapshot(path, snapshot, limits, snapshotTimeout);
        if (result.ok()) {
            DocumentFileInfo after;
            result = DocumentFile::readStorageInfo(path, after);
            if (result.ok() && (after.revision != info.revision || after.storageVersion != info.storageVersion))
                result = {DocumentFileCode::Conflict, false, "working file changed during verification"};
            verified = result.ok();
        }
    }
    if (result.ok() && command == "upgrade-storage") {
        wouldChange = info.storageVersion < 2;
        if (!dryRun) {
            DocumentFile file;
            result = file.open(path, limits);
            if (result.ok()) result = file.upgradeStorage();
            if (result.ok()) { info = {file.revision(), file.storageVersion()}; verified = true; }
        }
    }
    std::cout << "{\"schemaVersion\":1,\"ok\":" << (result.ok() ? "true" : "false")
              << ",\"command\":" << quote(command) << ",\"path\":" << quote(path)
              << ",\"storageVersion\":" << info.storageVersion << ",\"revision\":" << info.revision
              << ",\"verified\":" << (verified ? "true" : "false")
              << ",\"dryRun\":" << (dryRun ? "true" : "false")
              << ",\"wouldChange\":" << (wouldChange ? "true" : "false")
              << ",\"changed\":" << (result.changed ? "true" : "false")
              << ",\"code\":" << int(result.code) << ",\"message\":" << quote(result.message) << "}\n";
    return result.ok() ? 0 : (result.code == DocumentFileCode::InvalidPath ? 2 : 1);
}
} // namespace

#ifdef _WIN32
int wmain(int argc, wchar_t **argv)
{
    std::vector<std::string> arguments;
    for (int i = 0; i < argc; ++i) {
        const auto value = std::filesystem::path(argv[i]).u8string();
        arguments.emplace_back(value.begin(), value.end());
    }
    return run(arguments);
}
#else
int main(int argc, char **argv)
{
    return run(std::vector<std::string>(argv, argv + argc));
}
#endif
