#include "OpenRasterParser_p.hpp"

#include "Bitmap/BitmapCodec.h"
#include "Media/MediaIo_p.hpp"
#include "Validation/Validation.h"

#include <QByteArray>
#include <QString>
#include <QXmlStreamReader>

#include <zip.h>

#include <algorithm>
#include <array>
#include <cmath>
#include <cstring>
#include <limits>
#include <memory>
#include <optional>
#include <string_view>
#include <unordered_map>
#include <unordered_set>

namespace iiSharedCanvas::layered_detail {
namespace {
using media_detail::error;

struct ZipDeleter {
    void operator()(zip_t *archive) const { zip_discard(archive); }
};
using Zip = std::unique_ptr<zip_t, ZipDeleter>;

struct ZipEntry {
    zip_uint64_t index = 0;
    zip_uint64_t size = 0;
    bool directory = false;
};

struct LayerDescription {
    LayerProperties properties;
    std::string source;
};

bool validUtf8(const std::string &value)
{
    return value.find('\0') == std::string::npos
        && QString::fromUtf8(value).toUtf8().toStdString() == value;
}

bool safeArchivePath(const std::string &path, bool allowDirectory)
{
    if (path.empty() || !validUtf8(path) || path.front() == '/'
        || path.find('\\') != std::string::npos || path.find(':') != std::string::npos
        || std::any_of(path.begin(), path.end(), [](unsigned char ch) { return ch < 32 || ch == 127; })) {
        return false;
    }
    std::size_t offset = 0;
    while (offset < path.size()) {
        const auto separator = path.find('/', offset);
        const auto count = separator == std::string::npos ? path.size() - offset : separator - offset;
        const std::string_view component(path.data() + offset, count);
        if (component.empty() || component == "." || component == "..") { return false; }
        if (separator == std::string::npos) { return true; }
        offset = separator + 1;
    }
    return allowDirectory;
}

MediaIoResult checkRawZipNames(std::span<const std::uint8_t> bytes,
                               const LayeredDocumentImportOptions &options)
{
    // libzip 1.11.4 replaces embedded filename NULs with spaces, even with
    // ZIP_FL_ENC_RAW. This narrow classic-ZIP envelope check preserves and
    // validates the original names before handing all payload work to libzip.
    // It intentionally does not parse ZIP64, extra fields, or compressed data.
    if (bytes.size() < 22) { return error(MediaIoCode::InvalidData, "truncated OpenRaster ZIP end record"); }
    const auto little16 = [&](std::size_t offset) {
        return std::uint16_t(std::uint16_t(bytes[offset]) | (std::uint16_t(bytes[offset + 1]) << 8));
    };
    const auto little32 = [&](std::size_t offset) {
        return std::uint32_t(little16(offset)) | (std::uint32_t(little16(offset + 2)) << 16);
    };
    const auto signature = [&](std::size_t offset, std::uint8_t a, std::uint8_t b) {
        return bytes[offset] == 'P' && bytes[offset + 1] == 'K'
            && bytes[offset + 2] == a && bytes[offset + 3] == b;
    };
    const auto firstCandidate = bytes.size() > 22 + 65535 ? bytes.size() - 22 - 65535 : 0;
    std::optional<std::size_t> endRecord;
    for (std::size_t offset = bytes.size() - 22;; --offset) {
        if (signature(offset, 5, 6) && little16(offset + 20) == bytes.size() - offset - 22) {
            endRecord = offset;
            break;
        }
        if (offset == firstCandidate) { break; }
    }
    if (!endRecord) { return error(MediaIoCode::InvalidData, "OpenRaster ZIP end record is missing or has trailing bytes"); }
    const auto end = *endRecord;
    const auto count = little16(end + 10);
    const auto directorySize = little32(end + 12);
    const auto directoryOffset = little32(end + 16);
    if (count == 0xffffU || little16(end + 8) == 0xffffU
        || directorySize == 0xffffffffU || directoryOffset == 0xffffffffU) {
        return error(MediaIoCode::UnsupportedFeature, "ZIP64 OpenRaster archives are outside the supported bounded profile");
    }
    if (little16(end + 4) != 0 || little16(end + 6) != 0 || little16(end + 8) != count) {
        return error(MediaIoCode::UnsupportedFeature, "multi-disk OpenRaster ZIP archives are unsupported");
    }
    if (count > options.maxArchiveEntries) {
        return error(MediaIoCode::LimitExceeded, "OpenRaster archive entry count exceeds the limit");
    }
    if (directoryOffset > end || directorySize != end - directoryOffset) {
        return error(MediaIoCode::InvalidData, "OpenRaster ZIP directory extent is inconsistent");
    }
    std::unordered_set<std::string> names;
    std::size_t offset = directoryOffset;
    for (std::uint32_t index = 0; index < count; ++index) {
        if (end - offset < 46 || !signature(offset, 1, 2)) {
            return error(MediaIoCode::InvalidData, "truncated OpenRaster ZIP directory record");
        }
        const auto nameSize = little16(offset + 28);
        const auto extraSize = little16(offset + 30);
        const auto commentSize = little16(offset + 32);
        const auto localOffset = little32(offset + 42);
        if (localOffset == 0xffffffffU || little32(offset + 20) == 0xffffffffU
            || little32(offset + 24) == 0xffffffffU || little16(offset + 34) == 0xffffU) {
            return error(MediaIoCode::UnsupportedFeature, "ZIP64 OpenRaster entries are outside the supported bounded profile");
        }
        if (little16(offset + 34) != 0) {
            return error(MediaIoCode::UnsupportedFeature, "multi-disk OpenRaster ZIP entries are unsupported");
        }
        const auto variableSize = std::size_t(nameSize) + extraSize + commentSize;
        if (variableSize > end - offset - 46) {
            return error(MediaIoCode::InvalidData, "truncated OpenRaster ZIP filename or metadata");
        }
        const auto nameOffset = offset + 46;
        const std::string name(reinterpret_cast<const char *>(bytes.data() + nameOffset), nameSize);
        if (!safeArchivePath(name, true) || !names.insert(name).second) {
            return error(MediaIoCode::InvalidData, "OpenRaster raw ZIP names must be distinct safe UTF-8 paths without NUL");
        }
        if (localOffset > directoryOffset || directoryOffset - localOffset < 30
            || !signature(localOffset, 3, 4)) {
            return error(MediaIoCode::InvalidData, "OpenRaster ZIP local header is outside its payload area");
        }
        const auto localNameSize = little16(std::size_t(localOffset) + 26);
        const auto localExtraSize = little16(std::size_t(localOffset) + 28);
        if (std::size_t(localNameSize) + localExtraSize > directoryOffset - localOffset - 30
            || localNameSize != nameSize
            || !std::equal(bytes.begin() + std::ptrdiff_t(nameOffset),
                           bytes.begin() + std::ptrdiff_t(nameOffset + nameSize),
                           bytes.begin() + std::ptrdiff_t(std::size_t(localOffset) + 30))) {
            return error(MediaIoCode::InvalidData, "OpenRaster raw local and central filenames differ");
        }
        offset += 46 + variableSize;
    }
    return offset == end ? MediaIoResult{}
        : error(MediaIoCode::InvalidData, "OpenRaster ZIP directory entry count or extent does not match");
}

MediaIoResult readEntry(zip_t *archive, const ZipEntry &entry, QByteArray *output = nullptr)
{
    if (output) {
        if (entry.size > std::uint64_t(std::numeric_limits<qsizetype>::max())) {
            return error(MediaIoCode::LimitExceeded, "OpenRaster ZIP entry exceeds the container allocation limit");
        }
        output->clear();
        output->reserve(qsizetype(entry.size));
    }
    auto *file = zip_fopen_index(archive, entry.index, ZIP_FL_UNCHANGED);
    if (!file) { return error(MediaIoCode::InvalidData, "cannot open OpenRaster ZIP entry"); }
    std::array<char, 64 * 1024> block{};
    std::uint64_t consumed = 0;
    MediaIoResult result;
    while (true) {
        const auto count = zip_fread(file, block.data(), block.size());
        if (count < 0) {
            result = error(MediaIoCode::InvalidData, "OpenRaster ZIP entry failed decompression or CRC validation");
            break;
        }
        if (count == 0) {
            if (consumed != entry.size) {
                result = error(MediaIoCode::InvalidData, "OpenRaster ZIP entry length does not match its directory");
            }
            break;
        }
        if (std::uint64_t(count) > entry.size - consumed) {
            result = error(MediaIoCode::InvalidData, "OpenRaster ZIP entry expands beyond its declared size");
            break;
        }
        consumed += std::uint64_t(count);
        if (output) { output->append(block.data(), qsizetype(count)); }
    }
    if (zip_fclose(file) != 0 && result.ok()) {
        result = error(MediaIoCode::InvalidData, "OpenRaster ZIP entry checksum validation failed");
    }
    if (!result.ok() && output) { output->clear(); }
    return result;
}

MediaIoResult inspectArchive(zip_t *archive, const LayeredDocumentImportOptions &options,
                             std::unordered_map<std::string, ZipEntry> &entries,
                             std::uint64_t &expandedBytes)
{
    const auto count = zip_get_num_entries(archive, ZIP_FL_UNCHANGED);
    if (count < 0) { return error(MediaIoCode::InvalidData, "cannot inspect OpenRaster ZIP directory"); }
    if (std::uint64_t(count) > options.maxArchiveEntries) {
        return error(MediaIoCode::LimitExceeded, "OpenRaster archive entry count exceeds the limit");
    }
    if (count == 0) { return error(MediaIoCode::InvalidData, "OpenRaster ZIP is empty"); }
    for (zip_uint64_t i = 0; i < zip_uint64_t(count); ++i) {
        zip_stat_t stat;
        zip_stat_init(&stat);
        constexpr auto required = ZIP_STAT_SIZE | ZIP_STAT_COMP_SIZE | ZIP_STAT_CRC
            | ZIP_STAT_COMP_METHOD | ZIP_STAT_ENCRYPTION_METHOD;
        if (zip_stat_index(archive, i, ZIP_FL_UNCHANGED, &stat) != 0
            || (stat.valid & required) != required) {
            return error(MediaIoCode::InvalidData, "incomplete OpenRaster ZIP directory entry");
        }
        const auto *rawName = zip_get_name(archive, i, ZIP_FL_UNCHANGED | ZIP_FL_ENC_RAW);
        if (!rawName || !safeArchivePath(rawName, true)) {
            return error(MediaIoCode::InvalidData, "OpenRaster ZIP names must be safe relative UTF-8 paths");
        }
        const std::string name(rawName);
        const bool directory = name.back() == '/';
        if (entries.contains(name)) {
            return error(MediaIoCode::InvalidData, "duplicate OpenRaster ZIP entry name");
        }
        zip_uint8_t operatingSystem = 0;
        zip_uint32_t attributes = 0;
        if (zip_file_get_external_attributes(archive, i, ZIP_FL_UNCHANGED, &operatingSystem, &attributes) != 0) {
            return error(MediaIoCode::InvalidData, "cannot validate OpenRaster ZIP entry attributes");
        }
        if (operatingSystem == ZIP_OPSYS_UNIX || operatingSystem == ZIP_OPSYS_OS_X) {
            const auto type = (attributes >> 16) & 0170000U;
            if (type != 0 && type != (directory ? 0040000U : 0100000U)) {
                return error(MediaIoCode::UnsupportedFeature, "OpenRaster ZIP symlinks and special files are unsupported");
            }
        }
        if (stat.encryption_method != ZIP_EM_NONE) {
            return error(MediaIoCode::UnsupportedFeature, "encrypted OpenRaster ZIP entries are unsupported");
        }
        if (stat.comp_method != ZIP_CM_STORE && stat.comp_method != ZIP_CM_DEFLATE) {
            return error(MediaIoCode::UnsupportedFeature, "OpenRaster supports only stored or deflated ZIP entries");
        }
        if (i == 0 && (name != "mimetype" || stat.comp_method != ZIP_CM_STORE)) {
            return error(MediaIoCode::InvalidData, "OpenRaster first ZIP entry must be an uncompressed mimetype");
        }
        if ((directory && stat.size != 0) || stat.comp_size > options.limits.maxInputBytes) {
            return error(MediaIoCode::InvalidData, "invalid OpenRaster ZIP entry dimensions");
        }
        if (stat.size > options.limits.maxDecodedBytes - expandedBytes) {
            return error(MediaIoCode::LimitExceeded, "OpenRaster total ZIP expansion exceeds the decoded byte limit");
        }
        expandedBytes += stat.size;
        entries.emplace(name, ZipEntry{i, stat.size, directory});
    }
    // Validate every payload, including unused previews/metadata, before accepting the archive.
    // libzip owns ZIP parsing, consistency, decompression, and CRC checks; nothing is extracted.
    for (const auto &[name, entry] : entries) {
        const auto checked = readEntry(archive, entry);
        if (!checked.ok()) { return checked; }
    }
    return {};
}

MediaIoResult checkAttributes(const QXmlStreamReader &xml,
                              std::initializer_list<std::u16string_view> allowed)
{
    for (const auto &attribute : xml.attributes()) {
        if (!attribute.namespaceUri().isEmpty()
            || std::none_of(allowed.begin(), allowed.end(), [&](auto name) {
                   return attribute.name() == QStringView(name.data(), qsizetype(name.size()));
               })) {
            return error(MediaIoCode::UnsupportedFeature,
                         "unsupported OpenRaster attribute: " + attribute.qualifiedName().toString());
        }
    }
    return {};
}

std::optional<std::int32_t> signedInteger(QStringView text)
{
    bool good = false;
    const auto value = text.toInt(&good);
    return good ? std::optional<std::int32_t>(value) : std::nullopt;
}

MediaIoResult commonProperties(const QXmlStreamAttributes &attributes, LayerProperties &properties)
{
    properties.name = attributes.value(u"name").toString().toStdString();
    if (attributes.hasAttribute(u"opacity")) {
        bool good = false;
        properties.opacity = attributes.value(u"opacity").toDouble(&good);
        if (!good || !std::isfinite(properties.opacity) || properties.opacity < 0 || properties.opacity > 1) {
            return error(MediaIoCode::InvalidData, "OpenRaster opacity must be finite and between zero and one");
        }
    }
    if (attributes.hasAttribute(u"visibility")) {
        const auto visibility = attributes.value(u"visibility");
        if (visibility != u"visible" && visibility != u"hidden") {
            return error(MediaIoCode::InvalidData, "invalid OpenRaster visibility");
        }
        properties.visible = visibility == u"visible";
    }
    if (attributes.hasAttribute(u"composite-op")) {
        const auto mode = attributes.value(u"composite-op");
        if (mode == u"svg:src-over") { properties.blendMode = RasterBlendMode::SourceOver; }
        else if (mode == u"svg:multiply") { properties.blendMode = RasterBlendMode::Multiply; }
        else if (mode == u"svg:screen") { properties.blendMode = RasterBlendMode::Screen; }
        else if (mode == u"svg:overlay") { properties.blendMode = RasterBlendMode::Overlay; }
        else { return error(MediaIoCode::UnsupportedFeature, "unsupported OpenRaster composite operation"); }
    }
    return {};
}

MediaIoResult parseStack(const QByteArray &source, const LayeredDocumentImportOptions &options,
                         CanvasExtent &extent, std::vector<LayerDescription> &layers,
                         std::vector<std::string> &warnings)
{
    if (source.contains('\0') || QString::fromUtf8(source).toUtf8() != source) {
        return error(MediaIoCode::InvalidData, "OpenRaster stack.xml must contain valid UTF-8");
    }
    QXmlStreamReader xml(source);
    enum class Context { Image, RootStack, Group, Layer };
    std::vector<Context> contexts;
    bool sawImage = false;
    bool sawRootStack = false;
    int versionPatch = 0;
    while (!xml.atEnd()) {
        const auto token = xml.readNext();
        if (token == QXmlStreamReader::DTD || token == QXmlStreamReader::EntityReference
            || token == QXmlStreamReader::ProcessingInstruction) {
            return error(MediaIoCode::UnsupportedFeature, "OpenRaster entities, DTDs, and processing instructions are disabled");
        }
        if (token == QXmlStreamReader::StartDocument && !xml.documentEncoding().isEmpty()
            && xml.documentEncoding().compare(u"UTF-8", Qt::CaseInsensitive) != 0) {
            return error(MediaIoCode::UnsupportedFeature, "OpenRaster XML encoding must be UTF-8");
        }
        if (token == QXmlStreamReader::StartElement) {
            if (contexts.size() >= options.limits.maxXmlDepth) {
                return error(MediaIoCode::LimitExceeded, "OpenRaster XML nesting exceeds the depth limit");
            }
            if (!xml.namespaceUri().isEmpty()) {
                return error(MediaIoCode::UnsupportedFeature, "OpenRaster namespaced drawing elements are unsupported");
            }
            const auto name = xml.name();
            const auto attributes = xml.attributes();
            if (contexts.empty()) {
                if (sawImage || name != u"image") {
                    return error(MediaIoCode::InvalidData, "OpenRaster XML requires one image root");
                }
                auto result = checkAttributes(xml, {u"version", u"w", u"h", u"name", u"xres", u"yres"});
                if (!result.ok()) { return result; }
                const auto version = attributes.value(u"version");
                if (version.size() != 5 || !version.startsWith(u"0.0.")
                    || version.back() < u'1' || version.back() > u'6') {
                    return error(MediaIoCode::UnsupportedFeature, "unsupported OpenRaster format version");
                }
                versionPatch = version.back().unicode() - u'0';
                const auto width = signedInteger(attributes.value(u"w"));
                const auto height = signedInteger(attributes.value(u"h"));
                if (!width || !height || *width <= 0 || *height <= 0) {
                    return error(MediaIoCode::InvalidData, "OpenRaster image dimensions must be positive 32-bit integers");
                }
                extent = {*width, *height};
                result = media_detail::checkExtent(extent, options.limits);
                if (!result.ok()) { return result; }
                if (attributes.hasAttribute(u"xres") || attributes.hasAttribute(u"yres")) {
                    const auto xres = signedInteger(attributes.value(u"xres"));
                    const auto yres = signedInteger(attributes.value(u"yres"));
                    if (!xres || !yres || *xres <= 0 || *yres <= 0) {
                        return error(MediaIoCode::InvalidData, "OpenRaster resolution requires positive xres and yres");
                    }
                    warnings.emplace_back("OpenRaster physical print resolution is not retained by the canvas model");
                }
                if (attributes.hasAttribute(u"name")) {
                    warnings.emplace_back("OpenRaster image title is not retained by the canvas model");
                }
                sawImage = true;
                contexts.push_back(Context::Image);
                continue;
            }
            if (contexts.back() == Context::Image) {
                if (name != u"stack" || sawRootStack) {
                    return error(MediaIoCode::InvalidData, "OpenRaster image requires exactly one root stack");
                }
                const auto result = checkAttributes(xml, {u"name", u"opacity", u"visibility",
                                                          u"composite-op", u"isolation"});
                if (!result.ok()) { return result; }
                if (!attributes.isEmpty()) {
                    warnings.emplace_back("OpenRaster root stack attributes are ignored; the root has fixed isolated rendering semantics");
                }
                sawRootStack = true;
                contexts.push_back(Context::RootStack);
                continue;
            }
            if (contexts.back() == Context::Layer) {
                return error(MediaIoCode::UnsupportedFeature, "OpenRaster layer child elements are unsupported");
            }
            if (name == u"stack") {
                auto result = checkAttributes(xml, {u"name", u"opacity", u"visibility", u"composite-op",
                                                    u"isolation", u"x", u"y"});
                if (!result.ok()) { return result; }
                LayerProperties group;
                result = commonProperties(attributes, group);
                if (!result.ok()) { return result; }
                if (attributes.value(u"isolation") != u"auto" || group.opacity != 1.0 || !group.visible
                    || group.blendMode != RasterBlendMode::SourceOver) {
                    return error(MediaIoCode::UnsupportedFeature,
                                 "OpenRaster isolated or non-neutral groups cannot be represented without changing compositing");
                }
                for (const auto axis : {u"x", u"y"}) {
                    if (attributes.hasAttribute(axis)) {
                        if (versionPatch >= 6 || !signedInteger(attributes.value(axis))) {
                            return error(MediaIoCode::UnsupportedFeature, "OpenRaster group coordinates are not supported by this version");
                        }
                        warnings.emplace_back("Legacy OpenRaster group coordinates are ignored as specified; child offsets remain absolute");
                    }
                }
                warnings.emplace_back("OpenRaster neutral pass-through group hierarchy and group names were flattened into independent layers");
                contexts.push_back(Context::Group);
            } else if (name == u"layer") {
                auto result = checkAttributes(xml, {u"src", u"name", u"x", u"y", u"opacity", u"visibility", u"composite-op"});
                if (!result.ok()) { return result; }
                if (layers.size() >= options.maxLayers) {
                    return error(MediaIoCode::LimitExceeded, "OpenRaster layer count exceeds the limit");
                }
                LayerDescription layer;
                result = commonProperties(attributes, layer.properties);
                if (!result.ok()) { return result; }
                layer.source = attributes.value(u"src").toString().toStdString();
                if (!safeArchivePath(layer.source, false)) {
                    return error(MediaIoCode::InvalidData, "OpenRaster layer source must be a safe internal archive path");
                }
                if (!QString::fromStdString(layer.source).endsWith(u".png")) {
                    return error(MediaIoCode::UnsupportedFeature, "OpenRaster layer sources must be PNG images");
                }
                if (layer.source == "Thumbnails/thumbnail.png") {
                    return error(MediaIoCode::InvalidData, "OpenRaster thumbnail cannot be a layer source");
                }
                for (const auto axis : {u"x", u"y"}) {
                    if (!attributes.hasAttribute(axis)) { continue; }
                    const auto value = signedInteger(attributes.value(axis));
                    if (!value) { return error(MediaIoCode::InvalidData, "OpenRaster layer offset must be a signed 32-bit integer"); }
                    if (QStringView(axis) == u"x") { layer.properties.transform.translationX = *value; }
                    else { layer.properties.transform.translationY = *value; }
                }
                layers.push_back(std::move(layer));
                contexts.push_back(Context::Layer);
            } else {
                return error(MediaIoCode::UnsupportedFeature, "unsupported OpenRaster drawing element: " + name.toString());
            }
        } else if (token == QXmlStreamReader::EndElement) {
            if (contexts.empty()) { return error(MediaIoCode::InvalidData, "unbalanced OpenRaster XML"); }
            contexts.pop_back();
        } else if (token == QXmlStreamReader::Characters && !xml.isWhitespace()) {
            return error(MediaIoCode::UnsupportedFeature, "OpenRaster text drawing content is unsupported");
        }
    }
    if (xml.hasError() || !sawImage || !sawRootStack || !contexts.empty()) {
        return error(MediaIoCode::InvalidData, xml.hasError() ? xml.errorString() : "incomplete OpenRaster XML document");
    }
    return {};
}
} // namespace

LayeredDocumentImportResult decodeOpenRaster(std::span<const std::uint8_t> bytes,
                                             const LayeredDocumentImportOptions &options)
{
    const auto fail = [](MediaIoResult result) {
        return LayeredDocumentImportResult{{}, "ora", std::move(result)};
    };
    if (bytes.empty()) { return fail(error(MediaIoCode::InvalidData, "empty OpenRaster archive")); }
    if (bytes.size() > options.limits.maxInputBytes) {
        return fail(error(MediaIoCode::LimitExceeded, "OpenRaster input exceeds the byte limit"));
    }
    // This fixed OpenRaster envelope requirement is not a ZIP directory/parser:
    // libzip validates the complete archive below. Central-directory order alone
    // cannot prove that mimetype is the first physical local file in the input.
    constexpr std::string_view firstName = "mimetype";
    if (bytes.size() < 30 + firstName.size() || bytes[0] != 'P' || bytes[1] != 'K'
        || bytes[2] != 3 || bytes[3] != 4 || bytes[8] != 0 || bytes[9] != 0
        || bytes[26] != firstName.size() || bytes[27] != 0
        || !std::equal(firstName.begin(), firstName.end(), bytes.begin() + 30)) {
        return fail(error(MediaIoCode::InvalidData, "OpenRaster must begin with the stored mimetype local file"));
    }
    auto checked = checkRawZipNames(bytes, options);
    if (!checked.ok()) { return fail(std::move(checked)); }
    zip_error_t zipError;
    zip_error_init(&zipError);
    auto *source = zip_source_buffer_create(bytes.data(), bytes.size(), 0, &zipError);
    Zip archive(source ? zip_open_from_source(source, ZIP_RDONLY | ZIP_CHECKCONS, &zipError) : nullptr);
    if (!archive) {
        if (source) { zip_source_free(source); }
        zip_error_fini(&zipError);
        return fail(error(MediaIoCode::InvalidData, "OpenRaster ZIP is corrupt or inconsistent"));
    }
    zip_error_fini(&zipError);
    std::unordered_map<std::string, ZipEntry> entries;
    std::uint64_t expandedBytes = 0;
    checked = inspectArchive(archive.get(), options, entries, expandedBytes);
    if (!checked.ok()) { return fail(std::move(checked)); }
    const auto mimetype = entries.find("mimetype");
    const auto stack = entries.find("stack.xml");
    if (mimetype == entries.end() || stack == entries.end() || stack->second.directory) {
        return fail(error(MediaIoCode::InvalidData, "OpenRaster requires mimetype and stack.xml entries"));
    }
    QByteArray content;
    checked = readEntry(archive.get(), mimetype->second, &content);
    if (!checked.ok()) { return fail(std::move(checked)); }
    if (content != "image/openraster") {
        return fail(error(MediaIoCode::InvalidData, "OpenRaster mimetype entry is invalid"));
    }
    checked = readEntry(archive.get(), stack->second, &content);
    if (!checked.ok()) { return fail(std::move(checked)); }
    LayeredDocumentImportResult result;
    result.format = "ora";
    std::vector<LayerDescription> layers;
    checked = parseStack(content, options, result.document.extent, layers, result.result.warnings);
    if (!checked.ok()) { return fail(std::move(checked)); }
    content.clear();
    if (!entries.contains("mergedimage.png") || !entries.contains("Thumbnails/thumbnail.png")) {
        result.result.warnings.emplace_back("OpenRaster merged preview or thumbnail is absent; editable layers were imported without using previews");
    }
    for (auto it = layers.rbegin(); it != layers.rend(); ++it) {
        const auto entry = entries.find(it->source);
        if (entry == entries.end() || entry->second.directory) {
            return fail(error(MediaIoCode::InvalidData, "OpenRaster layer references a missing image entry"));
        }
        checked = readEntry(archive.get(), entry->second, &content);
        if (!checked.ok()) { return fail(std::move(checked)); }
        const auto index = std::to_string(result.document.layers.size());
        BitmapImportOptions bitmap;
        bitmap.assetId = options.idPrefix + "-asset-" + index;
        bitmap.format = "png";
        bitmap.applyOrientation = false;
        bitmap.extendedCodecs = false;
        bitmap.limits = options.limits;
        bitmap.limits.maxInputBytes = options.limits.maxDecodedBytes;
        bitmap.limits.maxDecodedBytes = options.limits.maxDecodedBytes - expandedBytes;
        auto decoded = decodeBitmap({reinterpret_cast<const std::uint8_t *>(content.constData()),
                                     std::size_t(content.size())}, bitmap);
        if (!decoded.ok()) { return fail(std::move(decoded.result)); }
        const auto pixelBytes = std::uint64_t(decoded.asset.pixels.pixels.size()) * sizeof(std::uint32_t);
        if (pixelBytes > options.limits.maxDecodedBytes - expandedBytes) {
            return fail(error(MediaIoCode::LimitExceeded, "OpenRaster total pixel storage exceeds the decoded byte limit"));
        }
        expandedBytes += pixelBytes;
        result.result.warnings.insert(result.result.warnings.end(), decoded.result.warnings.begin(), decoded.result.warnings.end());
        if (!decoded.text.empty()) {
            result.result.warnings.emplace_back("OpenRaster layer PNG text metadata is not retained in the canvas document");
        }
        it->properties.id = options.idPrefix + "-layer-" + index;
        result.document.layers.emplace_back(BitmapLayer{std::move(it->properties), StaticSource{decoded.asset.id}});
        result.document.assets.emplace_back(std::move(decoded.asset));
    }
    const auto validation = validate(result.document);
    if (!validation.ok()) {
        return fail(error(MediaIoCode::InvalidData, QString::fromStdString(validation.issues.front().message)));
    }
    return result;
}

} // namespace iiSharedCanvas::layered_detail
