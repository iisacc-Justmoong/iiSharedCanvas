"""Verify the Python sequence surface against a native, owned-media fixture."""
import subprocess
import sys
import tempfile
from pathlib import Path

sys.path.insert(0, sys.argv[2])
from iisharedcanvas import (CanvasDocument, analyze_reframe_frames,
                            estimate_optical_flow, interpolate_optical_flow,
                            flatten_nested_sequence, read_raw_sequence_frame, open_flattened_sequence_source,
                            replace_flattened_sequence_source)

library, driver, directory = sys.argv[1], sys.argv[3], sys.argv[4]
Path(directory).mkdir(parents=True, exist_ok=True)
with tempfile.TemporaryDirectory(dir=directory) as temporary:
    path = Path(temporary) / "sequence.iisc"
    subprocess.run([driver, "--fixture", str(path)], check=True)
    source = path.read_bytes()

def canvas():
    return CanvasDocument.from_bytes(source, library=library)

with canvas() as value:
    assert value.sequence_snapshot()["sequences"][0]["id"] == "sequence"
    assert value.match_source_frame("sequence", 3)[0]["source_frame"] == 3
    assert [item["sequence_frame"] for item in value.match_sequence_frames("sequence", "source", "video", 6)] == [6]
    value.edit_sequence_settings("sequence", name="new name", frame_rate=(30, 1))
    assert value.sequence_snapshot()["sequences"][0]["name"] == "new name"
    value.undo()
    assert value.sequence_snapshot()["sequences"][0]["name"] == ""
    value.redo()
    assert value.sequence_snapshot()["sequences"][0]["name"] == "new name"
    with CanvasDocument.from_bytes(value.to_bytes(), library=library) as reopened:
        assert reopened.sequence_snapshot()["sequences"][0]["name"] == "new name"
        assert reopened.snapshot().undo_entries == 0

# The Python projection retains an irrational source position as an exact
# cubic expression, including the rational values of every control point.
with canvas() as value:
    project = value.sequence_project_model()
    sequence = project["sequences"][0]
    sequence["editingFrameRate"]["numerator"] = 72
    clip = sequence["tracks"][0]["value"]["clips"][0]["properties"]
    first = value.timeline_default("TimelineTimeMapPoint")
    last = value.timeline_default("TimelineTimeMapPoint")
    first["interpolation"] = 2
    first["outgoing"]["valueOffset"] = 8.0
    last["timelineTicks"] = last["sourceTicks"] = 24
    last["incoming"]["timeOffset"] = -24.0
    last["incoming"]["valueOffset"] = -8.0
    clip["timeMap"] = [first, last]
    value.set_sequence_project_model(project, media=value.sequence_owned_media())
    match = value.match_source_frame("sequence", 1)[0]
    assert match["source_frame"] == 5 and match["source_tick_den"] == 0
    curve = match["source_tick_bezier"]
    assert curve["target"] == {"numerator": "1", "denominator": "3"}
    assert curve["time_control"][-1] == {"numerator": "24", "denominator": "1"}
    assert curve["value_control"][1] == {"numerator": "8", "denominator": "1"}
    assert value.match_sequence_frames("sequence", "source", "video", 5)[0]["sequence_frame"] == 1

with canvas() as value:
    project = value.sequence_project_model()
    sequence = project["sequences"][0]
    sequence["editingFrameRate"]["numerator"] = 72
    clip = sequence["tracks"][0]["value"]["clips"][0]["properties"]
    clip["videoSampling"] = 3  # OpticalFlow
    value.set_sequence_project_model(project, media=value.sequence_owned_media())
    matches = value.match_source_frame("sequence", 1)
    assert [item["source_frame"] for item in matches] == [0, 1]
    lower, upper = (item["interpolation"][0] for item in matches)
    assert lower["mode"] == "optical_flow" and not lower["upper"] and upper["upper"]
    assert lower["source_tick_num"] == 1 and lower["source_tick_den"] == 3
    assert lower["lower_time"] == {"numerator": "0", "denominator": "1"}
    assert lower["upper_time"] == {"numerator": "1", "denominator": "1"}

# Decode ownership and flow estimation remain caller controlled; Python can
# also inject a different engine's dense vector fields into rasterization.
first = [0xff000000 | (((x * 61 + y * 37 + x * y * 19) & 255) * 0x010101)
         for y in range(5) for x in range(7)]
shifted = [first[y * 7 + x - 1] if x else 0 for y in range(5) for x in range(7)]
forward, backward = estimate_optical_flow(first, shifted, (7, 5), search_radius=2, patch_radius=1,
                                         maximum_comparisons=1_000_000, maximum_pixels=35, library=library)
assert forward[2 * 7 + 3] == (1., 0.) and backward[2 * 7 + 4] == (-1., 0.)
shifted2 = [first[y * 7 + x - 2] if x >= 2 else 0 for y in range(5) for x in range(7)]
pixels = interpolate_optical_flow(first, shifted2, (7, 5), [(2., 0.)] * 35, [(-2., 0.)] * 35, .5,
                                  maximum_pixels=35, library=library)
assert pixels[2 * 7 + 3] == first[2 * 7 + 2]

with canvas() as value:
    model = value.sequence_project_model()
    clip = model["sequences"][0]["tracks"][0]["value"]["clips"][0]["properties"]
    clip["loopMode"] = 1
    clip["sourceRange"]["duration"] = 8
    effect = value.timeline_default("TimelineEffect")
    effect["id"], effect["pluginId"] = "effect", "test"
    effect["activeRange"] = {"$optional": True, "value": {"$type": "TimelineTickRange", "start": 2, "duration": 18}}
    clip["effects"] = [effect]
    value.set_sequence_project_model(model, media=value.sequence_owned_media())
    before = [value.match_source_frame("sequence", n)[0]["source_frame"] for n in range(24)]
    value.split_sequence_clips("sequence", {"clip": "right"}, at=5)
    after = value.sequence_project_model()
    clips = after["sequences"][0]["tracks"][0]["value"]["clips"]
    assert clips[0]["properties"]["contentWindow"]["value"]["offsetTicks"] == 0
    assert clips[1]["properties"]["contentWindow"]["value"]["offsetTicks"] == 5
    assert [value.match_source_frame("sequence", n)[0]["source_frame"] for n in range(24)] == before
    with CanvasDocument.from_bytes(value.to_bytes(), library=library) as reopened:
        assert reopened.sequence_project_model() == after
    value.undo()
    assert value.sequence_project_model() == model
    value.redo()
    assert value.sequence_project_model() == after

with canvas() as value, tempfile.TemporaryDirectory(dir=directory) as temporary:
    import copy
    model=value.sequence_project_model()
    child=copy.deepcopy(model["sequences"][0]);child["id"]="nested"
    overlay=copy.deepcopy(child["tracks"][0]);overlay["value"]["properties"]["id"]="overlay"
    overlay["value"]["clips"][0]["properties"]["id"]="overlay-clip"
    child["tracks"].append(overlay)
    parent=model["sequences"][0]["tracks"][0]["value"]["clips"][0]["properties"]
    parent["source"]={"$variant": 1, "value": {"$type": "TimelineSequenceReference", "sequenceId": "nested", "kind": 0}}
    effect=value.timeline_default("TimelineEffect");effect["id"]="generation-control";effect["pluginId"]="example"
    parameter=value.timeline_default("TimelineProperty");parameter["key"]="strength";parameter["value"]={"$variant":3,"value":0.25}
    effect["parameters"]=[parameter];parent["effects"]=[effect]
    model["sequences"].append(child)
    value.set_sequence_project_model(model,media=value.sequence_owned_media())
    snapshot=Path(temporary)/"source.tmp.iisc";note=Path(temporary)/"track-group.tmp.md"
    rendered=[]
    def render(path,sequence,clip,frame,extent):
        assert Path(path).is_file() and note.is_file()
        assert sequence=="sequence" and clip=="clip" and extent==(4,4)
        rendered.append(frame)
        return [0xff000000 | frame] * 16
    flatten_nested_sequence(value,"sequence","clip",snapshot_path=snapshot,note_path=note,renderer=render,
                            source_id="baked-source",representation_id="baked-rep",stream_id="baked-stream",
                            owned_media_id="baked-bytes",original_media_id="original-bytes",maximum_frames=30,maximum_frame_bytes=4096)
    assert rendered==list(range(24))
    baked=value.sequence_project_model()
    assert baked["sequences"][0]["tracks"][0]["value"]["clips"][0]["properties"]["source"]["value"]["mediaSourceId"]=="baked-source"
    raw=next(m["data"] for m in value.sequence_owned_media() if m["id"]=="baked-bytes")
    assert read_raw_sequence_frame(raw,5,maximum_pixels=16,library=library)==(4,4,[0xff000005]*16)
    snapshot.unlink();note.unlink()
    with CanvasDocument.from_bytes(value.to_bytes(),library=library) as reopened:
        assert reopened.sequence_project_model()==baked
        with open_flattened_sequence_source(reopened,"sequence","clip") as editable:
            assert editable.sequence_project_model()==model
            editable.edit_sequence_settings("sequence",name="edited original")
            edited_model=editable.sequence_project_model()
            edited_model["sequences"][0]["tracks"][0]["value"]["clips"][0]["properties"]["effects"][0]["parameters"][0]["value"]["value"]=0.75
            editable.set_sequence_project_model(edited_model,media=editable.sequence_owned_media())
            replace_flattened_sequence_source(reopened,"sequence","clip",editable)
        with open_flattened_sequence_source(reopened,"sequence","clip") as edited:
            assert edited.sequence_snapshot()["sequences"][0]["name"]=="edited original"
            assert edited.sequence_project_model()["sequences"][0]["tracks"][0]["value"]["clips"][0]["properties"]["effects"][0]["parameters"][0]["value"]["value"]==0.75
        reopened.undo()
        with open_flattened_sequence_source(reopened,"sequence","clip") as old:
            assert old.sequence_snapshot()["sequences"][0]["name"]==""
        reopened.redo()
        snapshot2=Path(temporary)/"source-again.tmp.iisc";note2=Path(temporary)/"track-group-again.tmp.md"
        def render_again(path,sequence,clip,frame,extent):
            assert Path(path).is_file() and note2.is_file()
            return [0xff00aa00 | frame] * (extent[0]*extent[1])
        flatten_nested_sequence(reopened,"sequence","clip",snapshot_path=snapshot2,note_path=note2,renderer=render_again,
                                source_id="baked-source-2",representation_id="baked-rep-2",stream_id="baked-stream-2",
                                owned_media_id="baked-bytes-2",original_media_id="original-bytes",
                                maximum_frames=30,maximum_frame_bytes=4096)
        with open_flattened_sequence_source(reopened,"sequence","clip") as latest:
            assert latest.sequence_snapshot()["sequences"][0]["name"]=="edited original"
    value.undo();assert value.sequence_project_model()==model
    value.redo();assert value.sequence_project_model()==baked

with canvas() as value, tempfile.TemporaryDirectory(dir=directory) as temporary:
    value.set_sequence_project_model(model,media=value.sequence_owned_media())
    snapshot=Path(temporary)/"source.tmp.iisc";note=Path(temporary)/"track-group.tmp.md"
    def concurrent_render(path,sequence,clip,frame,extent):
        if frame==0:value.edit_sequence_settings("sequence",name="concurrent edit")
        return [0xffabcdef] * (extent[0]*extent[1])
    try:
        flatten_nested_sequence(value,"sequence","clip",snapshot_path=snapshot,note_path=note,renderer=concurrent_render,
                                source_id="baked-source",representation_id="baked-rep",stream_id="baked-stream",
                                owned_media_id="baked-bytes",original_media_id="original-bytes",maximum_frames=30,maximum_frame_bytes=4096)
        raise AssertionError("a stale render must not replace the concurrent edit")
    except RuntimeError:
        pass
    assert value.sequence_snapshot()["sequences"][0]["name"]=="concurrent edit"
    assert all(item["id"]!="baked-bytes" for item in value.sequence_owned_media())
    assert snapshot.is_file() and note.is_file()

with canvas() as value:
    value.split_sequence_clips("sequence", {"clip": "right"}, at=12)
    assert len(value.sequence_through_edits("sequence", ["track"])) == 1
    value.place_sequence_transition("sequence", "track", identifier="fade", kind="builtin.cross-dissolve",
                                    from_clip="clip", to_clip="right", start=10, duration=4)
    assert value.default_transition("audio", duration=12)["type"] == "builtin.equal-power-fade"
    value.undo()
    value.edit_sequence_tracks("sequence", ["track"], muted=True)
    assert value.sequence_snapshot()["sequences"][0]["tracks"][0]["muted"]

with canvas() as value:
    called = []
    def detector(frame):
        called.append(frame)
        return [(1, "person", .9, float(frame), 0., 2., 3.)]
    analyzed = analyze_reframe_frames(value, [(0, 0), (1, .1)], detector,
                                      id_prefix="automatic", maximum_gap=2, minimum_overlap=.2)
    assert len(called) == 2 and len(analyzed) == 1 and len(analyzed[0]["observations"]) == 2
    subjects = value.track_sequence_subjects([(0, 1, "person", .9, 0., 0., 2., 3.),
                                               (1, 1, "person", .8, .1, 0., 2., 3.)],
                                              id_prefix="subject", maximum_gap=2, minimum_overlap=.2)
    assert len(subjects) == 1 and len(subjects[0]["observations"]) == 2
    boxes = [(x["time"], subjects[0]["id"], x["x"], x["y"], x["width"], x["height"])
             for x in subjects[0]["observations"]]
    value.apply_sequence_reframe("sequence", "clip", "detected", observations=boxes,
                                 selected_subject=subjects[0]["id"], source_extent=(4, 4), target_extent=(4, 4))
    value.undo()
    value.trim_sequence_clips("sequence", {"clip": (3, 18)})
    assert value.sequence_gaps("sequence", ["track"]) == [{"track": "track", "start": 0, "duration": 3},
                                                               {"track": "track", "start": 21, "duration": 3}]
    value.extend_sequence_clips("sequence", ["clip"], edge="start", playhead=0)
    assert value.sequence_snapshot()["sequences"][0]["tracks"][0]["clips"][0]["start"] == 0
    value.apply_sequence_reframe("sequence", "clip", "reframe", keyframes=[(0, 2., 2., 1.), (10, 3., 2., 1.5)])
    assert value.sequence_snapshot()["sequences"][0]["tracks"][0]["clips"][0]["effects"] == 1

with canvas() as value:
    value.lift_sequence_range("sequence", ["track"], start=4, duration=4, id_prefix="lift")
    assert value.sequence_gaps("sequence", ["track"])[0]["duration"] == 4
    value.close_sequence_gap("sequence", ["track"], start=4, duration=4)
    assert value.sequence_snapshot()["sequences"][0]["tracks"][0]["clips"][1]["start"] == 4
    value.undo()
    value.undo()
    value.extract_sequence_range("sequence", ["track"], start=4, duration=4, id_prefix="extract")
    assert value.sequence_snapshot()["sequences"][0]["tracks"][0]["clips"][1]["start"] == 4

with canvas() as value:
    import copy
    model=value.sequence_project_model()
    track=model["sequences"][0]["tracks"][0]["value"]
    original=track["clips"][0]
    first=copy.deepcopy(original);second=copy.deepcopy(original)
    first["properties"]["timelineRange"]={"$type":"TimelineTickRange","start":0,"duration":8}
    first["properties"]["sourceRange"]={"$type":"TimelineTickRange","start":0,"duration":8}
    second["properties"]["id"]="second"
    second["properties"]["timelineRange"]={"$type":"TimelineTickRange","start":16,"duration":8}
    second["properties"]["sourceRange"]={"$type":"TimelineTickRange","start":16,"duration":8}
    track["clips"]=[first,second]
    effect=value.timeline_default("TimelineEffect");effect["id"]="track-effect";effect["pluginId"]="example"
    effect["activeRange"]={"$optional":True,"value":{"$type":"TimelineTickRange","start":1,"duration":20}}
    track["properties"]["effects"]=[effect]
    value.set_sequence_project_model(model,media=value.sequence_owned_media())
    value.close_sequence_gap("sequence",["track"],start=8,duration=8)
    assert value.sequence_track_effect_time("sequence","track",(9,2))==(9,2)
    assert value.sequence_track_effect_time("sequence","track",(9,1))==(17,1)
    assert value.sequence_track_effect_time("sequence","track",(17,1)) is None
    with CanvasDocument.from_bytes(value.to_bytes(),library=library) as reopened:
        assert reopened.sequence_track_effect_time("sequence","track",(9,1))==(17,1)
    value.undo();assert value.sequence_project_model()==model

with canvas() as value:
    value.extract_subsequence("sequence", start=4, duration=8, new_sequence="copy", mode="copy", id_prefix="copy")
    assert [s["id"] for s in value.sequence_snapshot()["sequences"]] == ["sequence", "copy"]
    value.simplify_sequence("copy", remove_empty_tracks=True)
    value.add_sequence_track("copy", "captions", kind="subtitle")
    value.edit_caption("copy", "captions", identifier="c", text="안녕", start=0, duration=4, language="ko")
    assert value.sequence_snapshot()["sequences"][1]["tracks"][1]["clips"][0]["text"] == "안녕"

# Python can start a project without requiring a C++ fixture, and can transfer
# an existing project plus its owned bytes as one independent history change.
with CanvasDocument.raster(4, 4, library=library) as fresh:
    fresh.create_sequence_project("new-project", "new", duration=24, time_base=(1, 24),
                                  frame_rate=(24, 1), extent=(4, 4))
    assert fresh.sequence_snapshot()["sequences"][0]["id"] == "new"
    try:
        fresh.create_sequence_project("other", "other", duration=1, time_base=(1, 24),
                                      frame_rate=(24, 1), extent=(4, 4))
        raise AssertionError("creating a second project must fail without changing history")
    except RuntimeError:
        pass
    assert fresh.snapshot().undo_entries == 1
    fresh.add_sequence_track("new", "captions", kind="subtitle")
    fresh.edit_caption("new", "captions", identifier="c", text="start", start=0, duration=4)
    fresh.undo()
    fresh.undo()
    fresh.undo()
    assert fresh.snapshot().undo_entries == 0
    fresh.redo()
    with CanvasDocument.from_bytes(fresh.to_bytes(), library=library) as reopened:
        assert reopened.sequence_snapshot()["id"] == "new-project"
    with canvas() as original:
        try:
            fresh.copy_sequence_from(original)
            raise AssertionError("replacing an existing project requires explicit opt-in")
        except RuntimeError:
            pass
        fresh.copy_sequence_from(original, replace=True)
        original.edit_sequence_settings("sequence", name="source changed")
    assert fresh.sequence_snapshot()["id"] == "project"
    assert fresh.sequence_snapshot()["sequences"][0]["name"] == ""
    assert fresh.match_source_frame("sequence", 3)[0]["source_frame"] == 3
    with CanvasDocument.from_bytes(fresh.to_bytes(), library=library) as reopened:
        assert reopened.match_source_frame("sequence", 3)[0]["source_frame"] == 3
    fresh.undo()
    assert fresh.sequence_snapshot()["id"] == "new-project"

with CanvasDocument.raster(4, 4, library=library) as empty:
    with canvas() as original:
        empty.copy_sequence_from(original)
    assert empty.match_source_frame("sequence", 3)[0]["source_frame"] == 3
    assert bytes((0, 1, 2, 253, 254, 255)) in empty.to_bytes()
    empty.undo()
    assert empty.snapshot().undo_entries == 0
    empty.redo()
    with CanvasDocument.from_bytes(empty.to_bytes(), library=library) as reopened:
        assert reopened.match_source_frame("sequence", 3)[0]["source_frame"] == 3

# All timecode and subtitle-style fields are edited and persisted together.
with canvas() as value:
    code = {"start_frame": 108000, "frame_rate": [30000, 1001], "nominal_fps": 30,
            "counting": "drop_frame", "wrap_24_hours": True}
    value.edit_sequence_settings("sequence", name="timed", timecode=code)
    assert value.sequence_snapshot()["sequences"][0]["timecode"] == code
    value.undo()
    assert value.sequence_snapshot()["sequences"][0]["timecode"] is None
    assert value.sequence_snapshot()["sequences"][0]["name"] == ""
    value.redo()
    value.add_sequence_track("sequence", "subtitles", kind="subtitle")
    style = {"font_family": "Inter", "font_size": 22.5,
             "fill": [1., .5, .25, 1.], "outline": [0., 0., 0., 1.],
             "outline_width": 1.5, "background": [.1, .2, .3, .4],
             "bold": True, "italic": False, "underline": True,
             "horizontal_alignment": "center", "vertical_alignment": "bottom",
             "writing_direction": "ltr",
             "properties": [
                 {"key": "empty", "type": "null", "value": None},
                 {"key": "flag", "type": "bool", "value": True},
                 {"key": "integer", "type": "int", "value": 42},
                 {"key": "number", "type": "number", "value": 2.5},
                 {"key": "text", "type": "string", "value": "가나다"},
                 {"key": "ratio", "type": "rational", "value": [3, 2]},
                 {"key": "color", "type": "color", "value": [.1, .2, .3, .4]},
                 {"key": "point", "type": "vector2", "value": [3., 4.]},
                 {"key": "values", "type": "numbers", "value": [1., 2., 3.]},
             ]}
    value.edit_caption("sequence", "subtitles", identifier="caption", text="내용",
                       start=0, duration=4, style=style)
    snapshot = value.sequence_snapshot()["sequences"][0]
    assert snapshot["tracks"][1]["clips"][0]["style"] == style
    with CanvasDocument.from_bytes(value.to_bytes(), library=library) as reopened:
        snapshot = reopened.sequence_snapshot()["sequences"][0]
        assert snapshot["timecode"] == code
        assert snapshot["tracks"][1]["clips"][0]["style"] == style
    value.undo()
    assert value.sequence_snapshot()["sequences"][0]["tracks"][1]["clips"] == []
    value.redo()
    value.edit_sequence_settings("sequence", timecode=None)
    assert value.sequence_snapshot()["sequences"][0]["timecode"] is None
    value.undo()
    assert value.sequence_snapshot()["sequences"][0]["timecode"] == code

# The complete typed project tree and document-owned bytes are available to
# Python, including variant/optional tags and binary codec metadata.
with canvas() as original, CanvasDocument.raster(4, 4, library=library) as target:
    project = original.sequence_project_model()
    media = original.sequence_owned_media()
    assert project["$type"] == "TimelineProject" and media[0]["id"] == "owned-video"
    media[0]["data"] = bytearray(media[0]["data"])
    target.set_sequence_project_model(project, media=media)
    media[0]["data"][0] = 99
    assert target.sequence_owned_media()[0]["data"] == bytes((0, 1, 2, 253, 254, 255))
    assert target.sequence_project_model() == project
    target.undo()
    target.redo()
    with CanvasDocument.from_bytes(target.to_bytes(), library=library) as reopened:
        assert reopened.sequence_project_model() == project
        assert reopened.sequence_owned_media()[0]["data"] == bytes((0, 1, 2, 253, 254, 255))

with CanvasDocument.raster(4, 4, library=library) as target:
    project = target.timeline_default("TimelineProject")
    sequence = target.timeline_default("TimelineSequence")
    project["id"] = "from-python"
    project["activeSequenceId"] = "main"
    sequence["id"] = "main"
    sequence["durationTicks"] = 24
    sequence["canvasExtent"]["width"] = sequence["canvasExtent"]["height"] = 4
    timecode = target.timeline_default("TimelineTimecode")
    timecode["startFrame"] = 48
    sequence["timecode"] = {"$optional": True, "value": timecode}
    project["sequences"].append(sequence)
    target.set_sequence_project_model(project, media=[])
    assert target.sequence_project_model() == project

    target._bind_sequence_model()
    schema = target._json("sequence_model_schema")
    assert len(schema) == 61
    for name, fields in schema.items():
        default = target.timeline_default(name)
        assert default["$type"] == name and set(default) == {"$type", *fields}
    assert target.sequence_snapshot()["sequences"][0]["timecode"]["start_frame"] == 48
    bad = dict(project); bad["unexpected"] = 1
    try:
        target.set_sequence_project_model(bad, media=[])
        raise AssertionError("unknown model fields must be rejected")
    except ValueError:
        pass
    assert target.sequence_project_model() == project

if len(sys.argv) > 5:
    with tempfile.TemporaryDirectory(dir=directory) as temporary:
        path = Path(temporary) / "rich-sequence.iisc"
        subprocess.run([sys.argv[5], "--fixture", str(path)], check=True)
        with CanvasDocument.from_bytes(path.read_bytes(), library=library) as rich, \
             CanvasDocument.raster(16, 16, library=library) as copied:
            project = rich.sequence_project_model()
            media = rich.sequence_owned_media()
            copied.set_sequence_project_model(project, media=media)
            assert copied.sequence_project_model() == project
            assert copied.sequence_owned_media() == media
            with CanvasDocument.from_bytes(copied.to_bytes(), library=library) as reopened:
                assert reopened.sequence_project_model() == project
                assert reopened.sequence_owned_media() == media
