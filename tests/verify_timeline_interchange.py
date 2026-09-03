#!/usr/bin/env python3
"""Independent stdlib oracle for generated packages and optional FCP re-exports.

This is a read-only development check, not a general untrusted XML importer.
"""

import argparse
from fractions import Fraction
import hashlib
import json
from pathlib import Path
from urllib.parse import unquote, urlparse
import xml.etree.ElementTree as ET


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
    assert manifest["version"] == 1
    assert manifest["format"] == "iiSharedCanvas.timeline-interchange"
    assert bounded_read(package / "source.iisc").startswith(b"IISC\r\n\x1a\n")
    rate = Fraction(manifest["frameRate"]["numerator"], manifest["frameRate"]["denominator"])
    duration = Fraction(manifest["frameCount"], 1) / rate
    expected = []
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
        media = {asset.get("id"): local_file(asset.find("media-rep").get("src"))
                 for asset in root.findall("resources/asset")}
        gap = sequence.find("spine/gap")
        assert gap is not None and seconds(gap.get("duration")) == duration
        actual = []
        for clip in gap.findall("clip"):
            video = clip.find("video")
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

    verify_fcpxml(package / "timeline.fcpxml")
    if args.fcpxml_roundtrip:
        verify_fcpxml(args.fcpxml_roundtrip)
    print(f"Verified {len(tracks)} independent layers, {len(expected)} clips, exact {rate} fps / {duration}s, linked PNG bytes"
          + (" and Final Cut XML re-export" if args.fcpxml_roundtrip else ""))


if __name__ == "__main__":
    main()
