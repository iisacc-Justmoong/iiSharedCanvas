#include <iiSharedCanvas.h>
#include <QDir>
#include <QFile>
#include <QGuiApplication>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QTemporaryDir>
#include <algorithm>
#include <iostream>

using namespace iiSharedCanvas;
int main(int argc, char **argv)
{
    QGuiApplication app(argc, argv);
    int failures = 0;
    const auto check = [&](bool ok, const std::string &message) {
        if (!ok) { std::cerr << message << '\n'; ++failures; }
    };
    Document document; document.extent = {1280, 720}; document.timeline.frameCount = 120;
    document.audioAssets.push_back({"voice", 48000, 2, std::vector<std::int16_t>(48000 * 2 * 5)});
    for (std::size_t i = 0; i < document.audioAssets[0].samples.size(); ++i) {
        document.audioAssets[0].samples[i] = static_cast<std::int16_t>((i % 100) * 100 - 5000);
    }
    document.audioTracks.push_back({"dialogue", "대사 & Voice", false, -3,
        {{"clip-a", "Opening", "voice", 24, 24, 2000, -6, true},
         {"clip-b", "Closing", "voice", 72, 24, 48000, 0, false}}});
    document.audioTracks.push_back({"music", "Muted music", true, 0,
        {{"clip-c", "Bed", "voice", 0, 120, 0, 0, true}}});
    check(validate(document).ok(), "valid independent audio tracks");
    QTemporaryDir temp(QStringLiteral(IISHAREDCANVAS_TEST_OUTPUT_DIR) + "/audio-package-XXXXXX");
    const auto directory = temp.filePath("package");
    const auto before = encodeIisc(document).bytes;
    const auto result = exportTimelineInterchange(document, directory.toStdString());
    check(result.ok(), "audio-only timeline package: " + result.message);
    if (result.ok()) {
        QFile manifestFile(directory + "/manifest.json"); manifestFile.open(QIODevice::ReadOnly);
        const auto manifest = QJsonDocument::fromJson(manifestFile.readAll()).object();
        const auto tracks = manifest["audioTracks"].toArray();
        check(manifest["version"].toInt() == 2 && tracks.size() == 2, "versioned audio manifest");
        if (tracks.size() == 2) {
            const auto clips = tracks[0].toObject()["clips"].toArray();
            check(clips.size() == 2, "independent cuts preserved");
            if (!clips.isEmpty()) {
                const auto imported = importAudioWav((directory + '/' + clips[0].toObject()["media"].toString()).toStdString());
                check(imported.ok() && imported.asset.samples == document.audioAssets[0].samples, "WAV sample identity and source handles");
            }
        }
        QFile source(directory + "/source.iisc"); source.open(QIODevice::ReadOnly);
        check(source.readAll() == QByteArray(reinterpret_cast<const char *>(before.data()), before.size()), "authoritative audio snapshot");
        check(QDir(directory + "/media").entryList({"*.wav"}, QDir::Files).size() == 1, "repeated source shares one WAV");
        if (qEnvironmentVariableIsSet("IISC_KEEP_AUDIO_FIXTURE")) {
            temp.setAutoRemove(false); std::cout << directory.toStdString() << '\n';
        }
    }
    check(encodeIisc(document).bytes == before, "source never modified");
    TimelineInterchangeOptions limited; limited.maxLayers = 1;
    check(exportTimelineInterchange(document, temp.filePath("too-many").toStdString(), limited).code == MediaIoCode::LimitExceeded, "audio counts in track budget");
    limited = {}; limited.maxClips = 2;
    check(exportTimelineInterchange(document, temp.filePath("too-many-clips").toStdString(), limited).code == MediaIoCode::LimitExceeded, "audio counts in clip budget");
    limited = {}; limited.limits.maxOutputBytes = 400000;
    check(exportTimelineInterchange(document, temp.filePath("too-big").toStdString(), limited).code == MediaIoCode::LimitExceeded
          && !QFile::exists(temp.filePath("too-big")), "audio budget failure publishes nothing");
    document.audioTracks.resize(1); document.audioTracks[0].clips.resize(1);
    document.timeline.frameRate = {30000, 1001};
    auto &shiftedClip = document.audioTracks[0].clips[0];
    shiftedClip.sourceOffsetSamples = 8009;
    document.assets.emplace_back(RasterAsset{"picture", makeRasterLayer(1280, 720, 0xff203040)});
    document.layers.emplace_back(BitmapLayer{{"picture", "Picture"}, StaticSource{"picture"}});
    const auto shiftedDirectory = temp.filePath("shifted");
    const auto shifted = exportTimelineInterchange(document, shiftedDirectory.toStdString());
    check(shifted.ok(), "mixed NTSC timeline with subframe source trim: " + shifted.message);
    if (shifted.ok()) {
        const auto wav = importAudioWav((shiftedDirectory + "/media/audio-0001.wav").toStdString());
        check(wav.ok() && wav.asset.samples.size() + 2 == document.audioAssets[0].samples.size()
            && std::equal(wav.asset.samples.begin(), wav.asset.samples.end(), document.audioAssets[0].samples.begin() + 2),
            "exact one sample frame origin shift, no sample rounding/resampling");
        QFile jsonFile(shiftedDirectory + "/manifest.json"); jsonFile.open(QIODevice::ReadOnly);
        const auto json = QJsonDocument::fromJson(jsonFile.readAll()).object();
        const auto clip = json["audioTracks"].toArray()[0].toObject()["clips"].toArray()[0].toObject();
        check(clip["sourceOffsetSamples"].toString() == "8009" && clip["mediaOffsetSamples"].toString() == "8008"
            && clip["mediaTrimSamples"].toString() == "1", "manifest maps sample-accurate original and export origins");
    }
    return failures ? 1 : 0;
}
