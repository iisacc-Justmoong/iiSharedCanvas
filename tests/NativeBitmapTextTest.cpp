#include <iiSharedCanvasNative.h>
#include <zlib.h>

#include <algorithm>
#include <cstdint>
#include <iostream>
#include <stdexcept>
#include <string>
#include <vector>

namespace {
using Bytes = std::vector<std::uint8_t>;

void require(bool value, const char *message)
{
    if (!value) throw std::runtime_error(message);
}

void be32(Bytes &out, std::uint32_t value)
{
    out.push_back(std::uint8_t(value >> 24)); out.push_back(std::uint8_t(value >> 16));
    out.push_back(std::uint8_t(value >> 8)); out.push_back(std::uint8_t(value));
}

// Construct independent PNG chunks, rather than relying on the writer under test.
void appendChunk(Bytes &png, const char *type, const Bytes &data)
{
    Bytes chunk;
    be32(chunk, std::uint32_t(data.size()));
    chunk.insert(chunk.end(), type, type + 4);
    chunk.insert(chunk.end(), data.begin(), data.end());
    const auto crc = crc32(0, chunk.data() + 4, uInt(chunk.size() - 4));
    be32(chunk, std::uint32_t(crc));
    png.insert(png.end() - 12, chunk.begin(), chunk.end());
}

Bytes compressText(const std::string &text)
{
    uLongf count = compressBound(text.size());
    Bytes output(count);
    require(compress2(output.data(), &count, reinterpret_cast<const Bytef *>(text.data()),
                      text.size(), Z_BEST_COMPRESSION) == Z_OK, "fixture compression");
    output.resize(count);
    return output;
}
}

int main()
{
    try {
        using namespace iiSharedCanvas;
        auto raster = makeRasterLayer(2, 1, 0);
        raster.pixels = {0x7f123456, 0x00112233};
        BitmapExportOptions options;
        options.text = {{"parameters", "first line\nSteps: 20, Seed: 42"},
                        {"description", "Unicode: 한글 🙂"}, {"café", "résumé"}};
        const auto encoded = encodeBitmap(raster, options);
        require(encoded.ok(), "native PNG metadata writer must succeed");
        const auto decoded = decodeBitmap(encoded.bytes);
        require(decoded.ok() && decoded.text == options.text, "UTF-8 PNG metadata round-trip");
        require(decoded.asset.pixels.pixels == raster.pixels, "metadata must not change pixels");

        auto plain = encodeBitmap(raster);
        require(plain.ok(), "PNG fixture encoding");
        Bytes latin{'L','a','t','i','n',0,0xe9};
        appendChunk(plain.bytes, "tEXt", latin);
        Bytes zipped{'Z','i','p',0,0};
        auto compressed = compressText("compressed line\nnext");
        zipped.insert(zipped.end(), compressed.begin(), compressed.end());
        appendChunk(plain.bytes, "zTXt", zipped);
        Bytes international{'U','T','F','8',0,1,0,0,0};
        compressed = compressText("안녕\n🙂");
        international.insert(international.end(), compressed.begin(), compressed.end());
        appendChunk(plain.bytes, "iTXt", international);
        auto imported = decodeBitmap(plain.bytes);
        require(imported.ok() && imported.text == std::vector<MediaTextEntry>{
                    {"Latin", "é"}, {"Zip", "compressed line\nnext"}, {"UTF8", "안녕\n🙂"}},
                "external PNG text variants must decode");

        auto duplicate = options; duplicate.text.push_back(options.text.front());
        require(encodeBitmap(raster, duplicate).result.code == MediaIoCode::InvalidArgument,
                "duplicate metadata keys must be rejected");
        auto badKey = options; badKey.text = {{"한글", "x"}};
        require(encodeBitmap(raster, badKey).result.code == MediaIoCode::InvalidArgument,
                "non-Latin-1 PNG keywords must be rejected");
        auto badQuality = options; badQuality.quality = 101;
        require(encodeBitmap(raster, badQuality).result.code == MediaIoCode::InvalidArgument,
                "bitmap quality outside its public range must be rejected");
        auto corrupt = plain.bytes; corrupt[29] ^= 1;
        require(!decodeBitmap(corrupt).ok(), "corrupt PNG CRC must be rejected");
        corrupt = plain.bytes; corrupt.pop_back();
        require(!decodeBitmap(corrupt).ok(), "truncated PNG must be rejected");
        BitmapImportOptions limited; limited.limits.maxDecodedBytes = 8;
        require(decodeBitmap(plain.bytes, limited).result.code == MediaIoCode::LimitExceeded,
                "expanded metadata must respect the decoded-byte limit");
        auto jpegOptions = options; jpegOptions.format = "jpeg";
        auto jpeg = encodeBitmap(raster, jpegOptions);
        require(jpeg.ok() && !jpeg.result.warnings.empty(), "other formats report metadata loss");
        return 0;
    } catch (const std::exception &error) {
        std::cerr << error.what() << '\n';
        return 1;
    }
}
