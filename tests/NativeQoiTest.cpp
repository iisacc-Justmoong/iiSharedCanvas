#include <iiSharedCanvasNative.h>

#include <algorithm>
#include <cstdint>
#include <iostream>
#include <stdexcept>
#include <string>
#include <vector>

namespace {
void require(bool value, const char *message) { if (!value) throw std::runtime_error(message); }
}

int main(int argc, char **argv)
{
    try {
        using namespace iiSharedCanvas;
        const std::vector<std::uint8_t> fixture{
            'q','o','i','f', 0,0,0,6, 0,0,0,1, 4,0,
            0xff,255,0,0,255, 0xc0, 0x7f, 0xa1,0x88, 50, 0xfe,4,5,6,
            0,0,0,0,0,0,0,1};
        auto decoded = decodeBitmap(fixture);
        const std::vector<std::uint32_t> expected{0xffff0000,0xffff0000,0xff000101,
                                                  0xff010202,0xffff0000,0xff040506};
        require(decoded.ok() && decoded.format == "qoi" && decoded.asset.pixels.pixels == expected,
                "QOI reference fixture must decode all chunk types");
        auto pixels = makeRasterLayer(4, 2, 0);
        pixels.pixels = {0x00112233,0x00112233,0x80112233,0xff112233,
                         0xff112233,0xffffffff,0xff010203,0x00000000};
        BitmapExportOptions options; options.format = "qoi";
        if (argc == 2 && std::string(argv[1]) == "--decode-hex") {
            std::string text;
            std::cin >> text;
            require(text.size() % 2 == 0, "invalid hexadecimal fixture");
            std::vector<std::uint8_t> bytes;
            for (std::size_t i = 0; i < text.size(); i += 2)
                bytes.push_back(std::uint8_t(std::stoul(text.substr(i, 2), nullptr, 16)));
            auto value = decodeBitmap(bytes);
            require(value.ok() && value.asset.pixels.pixels == pixels.pixels,
                    "independent QOI encoder changed pixels");
            return 0;
        }
        auto encoded = encodeBitmap(pixels, options);
        require(encoded.ok(), "QOI encoding failed");
        if (argc == 2 && std::string(argv[1]) == "--encode-hex") {
            constexpr char hex[] = "0123456789abcdef";
            for (const auto byte : encoded.bytes) std::cout << hex[byte >> 4] << hex[byte & 15];
            return 0;
        }
        auto roundtrip = decodeBitmap(encoded.bytes);
        require(roundtrip.ok() && roundtrip.asset.pixels.pixels == pixels.pixels,
                "QOI must preserve pixels including transparent RGB");
        const auto formats = bitmapFormats();
        require(std::any_of(formats.begin(), formats.end(), [](const auto &format) {
                    return format.name == "qoi" && format.canRead && format.canWrite;
                }), "QOI must be reported as a supported extended format");
        const auto basic = bitmapFormats({}, false);
        require(std::none_of(basic.begin(), basic.end(), [](const auto &format) { return format.name == "qoi"; }),
                "QOI must be absent when extended codecs are disabled");
        BitmapImportOptions disabled; disabled.extendedCodecs = false;
        require(!decodeBitmap(encoded.bytes, disabled).ok(), "disabled extended QOI must not decode");
        options.extendedCodecs = false;
        require(!encodeBitmap(pixels, options).ok(), "disabled extended QOI must not encode");
        options.extendedCodecs = true; options.text = {{"parameters", "prompt"}};
        require(encodeBitmap(pixels, options).ok() && !encodeBitmap(pixels, options).result.warnings.empty(),
                "QOI metadata loss must be reported");
        auto truncated = fixture; truncated.pop_back();
        require(!decodeBitmap(truncated).ok(), "QOI end marker is required");
        auto malformed = fixture; malformed[14] = 0xfd;
        require(!decodeBitmap(malformed).ok(), "QOI runs cannot exceed the image size");
        BitmapImportOptions limited; limited.limits.maxPixelsPerFrame = 1;
        require(decodeBitmap(fixture, limited).result.code == MediaIoCode::LimitExceeded,
                "QOI pixel limit must be checked before allocation");
        options.text.clear(); options.limits.maxOutputBytes = 8;
        require(encodeBitmap(pixels, options).result.code == MediaIoCode::LimitExceeded,
                "QOI output bytes must be bounded");
        auto linear = fixture; linear[13] = 1;
        require(decodeBitmap(linear).ok() && !decodeBitmap(linear).result.warnings.empty(),
                "linear QOI input must report its unchanged color values");
        return 0;
    } catch (const std::exception &error) {
        std::cerr << error.what() << '\n';
        return 1;
    }
}
