"""Exercise the public Python controller, including edit history and SVG bytes."""
import base64
import sys
import xml.etree.ElementTree as ET

sys.path.insert(0, sys.argv[2])
from iisharedcanvas import CanvasDocument, calculate_channels

library = sys.argv[1]
with CanvasDocument.raster(2, 1, 0xffff0000, library=library) as canvas:
    canvas.put_fill("blue", "blue-layer", extent=(1, 1), color=0xff0000ff)
    assert canvas.composite((0, 0, 2, 1), layers=["blue-layer"]) == [0xff0000ff, 0]
    assert canvas.composite((0, 0, 2, 1)) == [0xff0000ff, 0xffff0000]
    assert canvas.composite((0, 0, 2, 1), selection=bytes([128, 0])) == [0x800000ff, 0]
    css = canvas.layer_css("blue-layer")
    assert "#0000ffff" in css["declarations"] and not css["losses"]
    canvas.set_group("group", "Group")
    canvas.assign_group("blue-layer", "group")
    assert any("group" in loss for loss in canvas.layer_css("blue-layer")["losses"])

with CanvasDocument.raster(2, 1, 0xff102030, library=library) as canvas:
    canvas.apply_image("layer", [0xffc80000, 0xff640000], (2, 1),
                       source_channel="red", target_channel="green", selection=bytes([255, 0]))
    assert canvas.pixel(0, 0) == 0xff10c830 and canvas.pixel(1, 0) == 0xff102030
    canvas.undo()
    assert canvas.pixel(0, 0) == 0xff102030
    canvas.redo()
    assert canvas.pixel(0, 0) == 0xff10c830
    coverage = calculate_channels([0xff400000, 0xff800000], [0xff000000, 0xff000000], (2, 1),
                                  first_channel="red", second_channel="alpha", invert_first=True,
                                  blend="multiply", library=library)
    assert coverage == bytes([191, 127])
    canvas.save_selection("calc", "Calculated", 2, 1, coverage)
    assert canvas.selection_coverage("calc", 0, 0) == 191
    canvas.undo()
    canvas.redo()
    assert canvas.selection_coverage("calc", 1, 0) == 127
    with CanvasDocument.from_bytes(canvas.to_bytes(), library=library) as reopened:
        assert reopened.selection_coverage("calc", 0, 0) == 191
    with CanvasDocument.from_channel(coverage, (2, 1), library=library) as channel_document:
        assert channel_document.pixel(0, 0) == 0xffbfbfbf and channel_document.pixel(1, 0) == 0xff7f7f7f
        assert channel_document.snapshot().undo_entries == 0

with CanvasDocument.raster(2, 1, 0xff804020, library=library) as source:
    with CanvasDocument.raster(2, 1, 0xff001122, library=library) as target:
        pixels = source.composite((0, 0, 2, 1), layers=["layer"], frame=0)
        target.apply_image("layer", pixels, (2, 1), source_channel="red", target_channel="blue")
        assert target.pixel(0, 0) == 0xff001180 and source.pixel(0, 0) == 0xff804020
        target.undo()
        assert target.pixel(0, 0) == 0xff001122

with CanvasDocument.raster(1, 1, 0xffff0000, library=library) as canvas:
    canvas.set_frame_count(3)
    canvas.apply_image("layer", [0xff00ff00], (1, 1), frame=1)
    assert [canvas.pixel(0, 0, frame) for frame in range(3)] == [0xffff0000, 0xff00ff00, 0xffff0000]
    canvas.undo()
    assert [canvas.pixel(0, 0, frame) for frame in range(3)] == [0xffff0000] * 3
    canvas.redo()
    with CanvasDocument.from_bytes(canvas.to_bytes(), library=library) as reopened:
        assert [reopened.pixel(0, 0, frame) for frame in range(3)] == [0xffff0000, 0xff00ff00, 0xffff0000]

with CanvasDocument.raster(2, 1, 0xffffffff, library=library) as canvas:
    canvas.put_vector("vector", "shape", [("M", 0, 0), ("L", 1, 0),
                                           ("L", 1, 1), ("Z",)], color=0xff00ff00)
    result = canvas.selected_layers_svg(["shape"], (0, 0, 2, 1))
    assert not result["rasterized"]
    root = ET.fromstring(result["svg"])
    assert root.find(".//{http://www.w3.org/2000/svg}path") is not None
    canvas.set_mask("shape", "pixels")
    result = canvas.selected_layers_svg(["shape"], (0, 0, 2, 1))
    assert result["rasterized"] and result["warnings"]
    image = ET.fromstring(result["svg"]).find("{http://www.w3.org/2000/svg}image")
    assert image is not None and base64.b64decode(image.attrib["href"].split(",", 1)[1]).startswith(b"\x89PNG\r\n\x1a\n")
    canvas.put_vector("vector2", "plain", [("M", 0, 0), ("L", 1, 0),
                                            ("L", 1, 1), ("Z",)], color=0xff0000ff)
    result = canvas.selected_layers_svg(["shape", "plain"], (0, 0, 2, 1))
    root = ET.fromstring(result["svg"])
    assert result["rasterized"] and result["rasterized_layers"] == ["shape"]
    assert root.find(".//{http://www.w3.org/2000/svg}path") is not None
    assert root.find("{http://www.w3.org/2000/svg}image") is not None
