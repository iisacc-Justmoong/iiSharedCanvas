#!/usr/bin/env python3
"""Independent stdlib oracle for generated packages and optional FCP re-exports.

This is a read-only development check, not a general untrusted XML importer.
"""

import argparse
from fractions import Fraction
import hashlib
import io
import json
import math
from pathlib import Path
from urllib.parse import unquote, urlparse
import xml.etree.ElementTree as ET
import wave


def bounded_read(path):
    assert path.is_file() and path.stat().st_size <= 32 * 1024 * 1024, path
    return path.read_bytes()


def seconds(value):
    assert value.endswith("s"), value
    return Fraction(value[:-1])


def local_file(value):
    url = urlparse(value)
    assert url.scheme == "file" and url.netloc in ("", "localhost"), value
    return Path(unquote(url.path))


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("package", type=Path)
    parser.add_argument("--fcpxml-roundtrip", type=Path)
    args = parser.parse_args()
    package = args.package.resolve()
    manifest = json.loads(bounded_read(package / "manifest.json"))
    assert manifest["version"] in (1, 2)
    assert manifest["format"] == "iiSharedCanvas.timeline-interchange"
    assert bounded_read(package / "source.iisc").startswith(b"IISC\r\n\x1a\n")
    rate = Fraction(manifest["frameRate"]["numerator"], manifest["frameRate"]["denominator"])
    duration = Fraction(manifest["frameCount"], 1) / rate
    expected = []
    expected_audio = []
    media_hashes = {}
    blend_modes = {"normal": "0", "multiply": "4", "screen": "10", "overlay": "14"}
    for lane, track in enumerate(manifest["tracks"], 1):
        for clip in track["clips"]:
            media = package / clip["media"]
            assert media.resolve().is_relative_to(package)
            png = bounded_read(media)
            assert png.startswith(b"\x89PNG\r\n\x1a\n")
            media_hashes[media.name] = hashlib.sha256(png).digest()
            expected.append((lane, track["name"], Fraction(clip["startFrame"], 1) / rate,
                             Fraction(clip["durationFrames"], 1) / rate,
                             track["visible"], float(track["opacity"]), blend_modes[track["blendMode"]], media.name))

    audio_tracks = manifest.get("audioTracks", [])
    assert not audio_tracks or manifest["version"] == 2
    wav_info = {}
    for lane, track in enumerate(audio_tracks, 1):
        previous_end = 0
        for clip in track["clips"]:
            media = package / clip["media"]
            assert media.resolve().is_relative_to(package)
            wav = bounded_read(media)
            assert wav[:4] == b"RIFF" and wav[8:12] == b"WAVE"
            media_hashes[media.name] = hashlib.sha256(wav).digest()
            with wave.open(io.BytesIO(wav), "rb") as source:
                info = (source.getframerate(), source.getnchannels(), source.getnframes())
                assert source.getsampwidth() == 2 and source.getcomptype() == "NONE"
                assert info == (clip["sampleRate"], clip["channelCount"], int(clip["sampleFrameCount"]))
                assert len(source.readframes(source.getnframes())) == info[1] * info[2] * 2
            assert info[1] in (1, 2)
            wav_info[media.name] = info
            offset = int(clip["mediaOffsetSamples"])
            trim = int(clip["mediaTrimSamples"])
            assert int(clip["sourceOffsetSamples"]) == offset + trim
            quantum = (info[0] * rate.denominator) // math.gcd(rate.numerator, info[0] * rate.denominator)
            assert 0 <= trim < quantum and offset % quantum == 0
            start, count = clip["startFrame"], clip["durationFrames"]
            assert count > 0 and start >= previous_end and start + count <= manifest["frameCount"]
            assert Fraction(count, 1) / rate <= Fraction(info[2] - offset, info[0])
            previous_end = start + count
            name = track["name"] + (" / " + clip["name"] if clip["name"] else "")
            expected_audio.append((-lane, name, Fraction(start, 1) / rate, Fraction(count, 1) / rate,
                                   not track["muted"] and clip["enabled"],
                                   float(track["gainDb"]) + float(clip["gainDb"]),
                                   media.name, Fraction(offset, info[0])))

    legacy = ET.fromstring(bounded_read(package / "timeline.xml"))
    assert legacy.tag == "xmeml" and legacy.get("version") == "5"
    sequence = legacy.find("sequence")
    assert sequence.findtext("name") == manifest["sequenceName"]
    assert int(sequence.findtext("duration")) == manifest["frameCount"]
    legacy_rate = Fraction(int(sequence.findtext("rate/timebase")))
    if sequence.findtext("rate/ntsc") == "TRUE":
        legacy_rate *= Fraction(1000, 1001)
    assert legacy_rate == rate
    width = sequence.findtext("media/video/format/samplecharacteristics/width")
    height = sequence.findtext("media/video/format/samplecharacteristics/height")
    assert (int(width), int(height)) == (manifest["canvas"]["width"], manifest["canvas"]["height"])
    files = {f.get("id"): f.findtext("pathurl") for f in legacy.iter("file") if f.find("pathurl") is not None}
    legacy_actual = []
    tracks = sequence.findall("media/video/track")
    assert len(tracks) == len(manifest["tracks"])
    for lane, track in enumerate(tracks, 1):
        for clip in track.findall("clipitem"):
            media = local_file(files[clip.find("file").get("id")])
            assert hashlib.sha256(bounded_read(media)).digest() == media_hashes[media.name]
            start, end = int(clip.findtext("start")), int(clip.findtext("end"))
            assert int(clip.findtext("in")) == 0 and int(clip.findtext("out")) == end - start
            assert clip.findtext("alphatype") == "straight"
            opacity = float(clip.findtext("filter/effect/parameter/value")) / 100
            legacy_actual.append((lane, clip.findtext("name"), Fraction(start, 1) / rate,
                                  Fraction(end - start, 1) / rate, clip.findtext("enabled") == "TRUE",
                                  opacity, blend_modes[clip.findtext("compositemode")], media.name))
    assert sorted(legacy_actual) == sorted(expected), (legacy_actual, expected)

    def effect_value(clip, effect_id):
        effects = [effect for effect in clip.findall("filter/effect") if effect.findtext("effectid") == effect_id]
        assert len(effects) == 1, (effect_id, ET.tostring(clip))
        return float(effects[0].findtext("parameter/value"))

    legacy_audio = sequence.findall("media/audio/track")
    expected_physical = sum(max((clip["channelCount"] for clip in track["clips"]), default=1)
                            for track in audio_tracks)
    assert len(legacy_audio) == expected_physical
    legacy_by_id = {clip.get("id"): (index, clip_index, clip)
                    for index, track in enumerate(legacy_audio, 1)
                    for clip_index, clip in enumerate(track.findall("clipitem"), 1)}
    physical_index = 0
    audio_index = 0
    for track in audio_tracks:
        channels = max((clip["channelCount"] for clip in track["clips"]), default=1)
        for channel in range(1, channels + 1):
            actual_track = legacy_audio[physical_index + channel - 1]
            assert (actual_track.findtext("enabled") == "TRUE") == (not track["muted"])
            clipitems = actual_track.findall("clipitem")
            expected_channel = [(index, clip) for index, clip in enumerate(track["clips"])
                                if channel <= clip["channelCount"]]
            assert len(clipitems) == len(expected_channel)
            for item, (index, clip) in zip(clipitems, expected_channel):
                expected_clip = expected_audio[audio_index + index]
                source = local_file(files[item.find("file").get("id")])
                assert hashlib.sha256(bounded_read(source)).digest() == media_hashes[expected_clip[6]]
                assert item.findtext("name") == expected_clip[1]
                assert item.findtext("sourcetrack/mediatype") == "audio"
                assert int(item.findtext("sourcetrack/trackindex")) == channel
                assert Fraction(int(item.findtext("start")), 1) / rate == expected_clip[2]
                assert Fraction(int(item.findtext("end")) - int(item.findtext("start")), 1) / rate == expected_clip[3]
                assert Fraction(int(item.findtext("in")), 1) / rate == expected_clip[7]
                assert int(item.findtext("out")) - int(item.findtext("in")) == clip["durationFrames"]
                assert (item.findtext("enabled") == "TRUE") == expected_clip[4]
                assert math.isclose(effect_value(item, "audiolevels"), 10 ** (expected_clip[5] / 20), rel_tol=1e-12)
                assert effect_value(item, "audiopan") == (0 if clip["channelCount"] == 1 else -1 if channel == 1 else 1)
                links = item.findall("link")
                assert len(links) == (2 if clip["channelCount"] == 2 else 0)
                for link in links:
                    linked_track, linked_index, linked_clip = legacy_by_id[link.findtext("linkclipref")]
                    assert link.findtext("mediatype") == "audio" and link.findtext("groupindex") == "1"
                    assert linked_track == int(link.findtext("trackindex"))
                    assert linked_index == int(link.findtext("clipindex"))
                    assert linked_clip.findtext("start") == item.findtext("start")
                    assert linked_clip.findtext("end") == item.findtext("end")
                    assert linked_clip.find("file").get("id") == item.find("file").get("id")
        physical_index += channels
        audio_index += len(track["clips"])

    for file in legacy.iter("file"):
        if file.find("pathurl") is None or file.find("media/audio") is None:
            continue
        source = local_file(file.findtext("pathurl"))
        sample_rate, channel_count, _ = wav_info[source.name]
        assert int(file.findtext("media/audio/samplecharacteristics/samplerate")) == sample_rate
        assert file.findtext("media/audio/samplecharacteristics/depth") == "16"
        assert int(file.findtext("media/audio/channelcount")) == channel_count

    def volume_db(element):
        adjustment = element.find("adjust-volume")
        if adjustment is None:
            return 0.0
        amount = adjustment.get("amount", "0dB")
        assert amount.endswith("dB"), amount
        return float(amount[:-2])

    def verify_fcpxml(path):
        root = ET.fromstring(bounded_read(path))
        project = root.find(".//project")
        assert project.get("name") == manifest["sequenceName"]
        sequence = project.find("sequence")
        assert seconds(sequence.get("duration")) == duration
        formats = {f.get("id"): f for f in root.findall("resources/format")}
        video_format = formats[sequence.get("format")]
        assert seconds(video_format.get("frameDuration")) == 1 / rate
        assert (int(video_format.get("width")), int(video_format.get("height"))) == (int(width), int(height))
        assets = {asset.get("id"): asset for asset in root.findall("resources/asset")}
        media = {key: local_file(asset.find("media-rep").get("src")) for key, asset in assets.items()}
        gap = sequence.find("spine/gap")
        assert gap is not None and seconds(gap.get("duration")) == duration
        actual = []
        for clip in gap.findall("clip"):
            video = clip.find("video")
            if video is None:
                continue
            image = media[video.get("ref")]
            assert hashlib.sha256(bounded_read(image)).digest() == media_hashes[image.name]
            assert seconds(video.get("duration")) == seconds(clip.get("duration"))
            assert seconds(clip.get("start", "0s")) == 0
            blend = clip.find("adjust-blend")
            opacity = float(blend.get("amount", "1")) if blend is not None else 1.0
            mode = blend.get("mode", "0") if blend is not None else "0"
            actual.append((int(clip.get("lane")), clip.get("name"), seconds(clip.get("offset")),
                           seconds(clip.get("duration")), clip.get("enabled", "1") == "1", opacity, mode, image.name))
        assert sorted(actual) == sorted(expected), (actual, expected)
        actual_audio = []
        for clip in gap:
            audio = clip.find("audio") if clip.tag == "clip" else clip if clip.tag == "asset-clip" else None
            if audio is None or int(clip.get("lane", "0")) >= 0:
                continue
            asset = assets[audio.get("ref")]
            source = media[audio.get("ref")]
            assert source.name in wav_info
            assert hashlib.sha256(bounded_read(source)).digest() == media_hashes[source.name]
            sample_rate, channel_count, sample_frames = wav_info[source.name]
            assert asset.get("hasAudio") == "1" and asset.get("hasVideo", "0") == "0"
            assert int(asset.get("audioChannels")) == channel_count
            assert int(asset.get("audioRate")) == sample_rate
            assert seconds(asset.get("duration")) == Fraction(sample_frames, sample_rate)
            source_offset = seconds(audio.get("start", "0s"))
            gain = volume_db(clip)
            enabled = clip.get("enabled", "1") == "1"
            if audio is not clip:
                source_offset += seconds(clip.get("start", "0s")) - seconds(audio.get("offset", "0s"))
                gain += volume_db(audio)
                enabled = enabled and audio.get("enabled", "1") == "1"
            components = clip.findall("audio-channel-source")
            if components:
                component_gains = [volume_db(component) for component in components]
                assert len(set(component_gains)) == 1
                gain += component_gains[0]
                enabled = enabled and all(component.get("enabled", "1") == "1" for component in components)
                if channel_count == 2:
                    mapping = []
                    for component in components:
                        source_channels = [part.strip() for part in component.get("srcCh", "").split(",")]
                        output_channels = [part.strip() for part in component.get("outCh", "").split(",")]
                        assert len(source_channels) == len(output_channels)
                        mapping.extend(zip(source_channels, output_channels))
                    assert sorted(mapping) == [("1", "L"), ("2", "R")], mapping
            actual_audio.append((int(clip.get("lane")), clip.get("name"), seconds(clip.get("offset")),
                                 seconds(clip.get("duration")), enabled, gain, source.name, source_offset))
        actual_audio.sort()
        wanted_audio = sorted(expected_audio)
        assert len(actual_audio) == len(wanted_audio), (actual_audio, wanted_audio)
        for actual_clip, wanted_clip in zip(actual_audio, wanted_audio):
            assert actual_clip[:5] == wanted_clip[:5] and actual_clip[6:] == wanted_clip[6:], (actual_clip, wanted_clip)
            assert math.isclose(actual_clip[5], wanted_clip[5], abs_tol=1e-9), (actual_clip, wanted_clip)

    verify_fcpxml(package / "timeline.fcpxml")
    if args.fcpxml_roundtrip:
        verify_fcpxml(args.fcpxml_roundtrip)
    print(f"Verified {len(tracks)} visual layers / {len(expected)} clips, "
          f"{len(audio_tracks)} audio tracks / {len(expected_audio)} clips, exact {rate} fps / {duration}s, linked PNG/WAV bytes"
          + (" and Final Cut XML re-export" if args.fcpxml_roundtrip else ""))


if __name__ == "__main__":
    main()
