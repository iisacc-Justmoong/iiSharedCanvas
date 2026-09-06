#include <iiSharedCanvas.h>
#include <QCoreApplication>
#include <QTemporaryDir>
#include <iostream>

using namespace iiSharedCanvas;
int main(int argc, char **argv)
{
    QCoreApplication app(argc, argv);
    int failures = 0;
    const auto check = [&](bool ok, const char *message) {
        if (!ok) { std::cerr << message << '\n'; ++failures; }
    };
    AudioAsset asset{"voice", 48000, 2, {-32768, 32767, -1, 0, 1234, -4321}};
    const auto encoded = encodeAudioWav(asset);
    check(encoded.ok() && encoded.bytes.size() == 56, "PCM16 WAV encoding");
    if (!encoded.ok()) { return 1; }
    const auto decoded = decodeAudioWav(encoded.bytes, {.assetId = "voice"});
    check(decoded.ok() && decoded.asset.samples == asset.samples
          && decoded.asset.sampleRate == 48000 && decoded.asset.channelCount == 2,
          "stereo sample identity including signed extrema");
    auto invalid = encoded.bytes; invalid.pop_back();
    check(!decodeAudioWav(invalid).ok(), "truncation rejected");
    invalid = encoded.bytes; invalid[34] = 24;
    check(decodeAudioWav(invalid).result.code == MediaIoCode::UnsupportedFeature, "non-PCM16 rejected explicitly");
    invalid = encoded.bytes; invalid[32] = 2;
    check(!decodeAudioWav(invalid).ok(), "bad stereo block alignment rejected");
    invalid = encoded.bytes; invalid[28] ^= 1;
    check(!decodeAudioWav(invalid).ok(), "bad byte rate rejected");
    AudioImportOptions limited; limited.limits.maxDecodedBytes = 10;
    check(decodeAudioWav(encoded.bytes, limited).result.code == MediaIoCode::LimitExceeded, "decoded byte budget");
    MediaLimits outputLimit; outputLimit.maxOutputBytes = 55;
    check(encodeAudioWav(asset, outputLimit).result.code == MediaIoCode::LimitExceeded, "output budget before allocation");
    asset.samples.pop_back();
    check(!encodeAudioWav(asset).ok(), "partial stereo sample frame rejected");
    QTemporaryDir temp(QStringLiteral(IISHAREDCANVAS_TEST_OUTPUT_DIR) + "/audio-codec-XXXXXX");
    check(temp.isValid(), "workspace output directory");
    asset.channelCount = 1;
    const auto path = temp.filePath("voice.wav").toStdString();
    check(exportAudioWav(asset, path).ok(), "atomic local WAV export");
    check(exportAudioWav(asset, path).code == MediaIoCode::AlreadyExists, "WAV collision protection");
    check(importAudioWav(path).asset.samples == asset.samples, "local WAV import");
    return failures ? 1 : 0;
}
