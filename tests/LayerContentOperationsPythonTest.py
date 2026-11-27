"""Exercise native layer-content behavior through the installed Python contract."""
import sys

sys.path.insert(0, sys.argv[2])
from iisharedcanvas import CanvasDocument

library = sys.argv[1]
with CanvasDocument.raster(4, 2, 0xff808080, library=library) as canvas:
    canvas.put_fill("fill", "fill-layer", kind="solid", color=0xffff0000)
    assert canvas.pixel(0, 0) == 0xffff0000
    canvas.put_fill("fill", kind="pattern", pattern="pixels")
    assert canvas.pixel(0, 0) == 0xff808080
    canvas.undo()
    assert canvas.pixel(0, 0) == 0xffff0000
    canvas.redo()
    assert canvas.pixel(0, 0) == 0xff808080
    canvas.put_fill("fill", kind="linear", start=(0, 0), end=(4, 0),
                    stops=[(0, 0xff000000), (1, 0xffffffff)])
    assert canvas.pixel(0, 0) != canvas.pixel(3, 0)
    canvas.put_adjustment("adjust", "adjust-layer", exposure=1)
    before = canvas.slice((0, 0, 4, 2))
    assert len(before) == 8
    canvas.set_styles("fill-layer", [{"kind": "overlay", "color": 0xffff0000}])
    assert canvas.slice((0, 0, 2, 1), layer="fill-layer") == [0xffff0000, 0xffff0000]
    canvas.set_mask("fill-layer", "pixels", density=.5)
    saved = canvas.to_bytes()
    with CanvasDocument.from_bytes(saved, library=library) as restored:
        assert restored.slice((0, 0, 4, 2)) == canvas.slice((0, 0, 4, 2))
    canvas.undo()
    canvas.redo()
    canvas.set_group("outer", "Outer", visible=False)
    canvas.set_group("inner", "Inner", parent="outer")
    canvas.assign_group("fill-layer", "inner")
    assert canvas.slice((0, 0, 2, 1), layer="fill-layer") == [0, 0]
    canvas.set_group("outer", "Outer", visible=True)
    assert any(canvas.slice((0, 0, 2, 1), layer="fill-layer"))
    assert len(canvas.slice((0, 0, 4, 2), output_extent=(1, 1), sampling="smooth")) == 1
    try:
        canvas.slice((0, 0, 1, 1), layer="adjust-layer")
    except RuntimeError:
        pass
    else:
        raise AssertionError("an adjustment cannot be exported without its backdrop")
