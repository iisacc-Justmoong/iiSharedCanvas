"""Exercise the same C++ iiGUI conversions through the installed Python ABI."""
import math
import sys

sys.path.insert(0, sys.argv[2])
from iisharedcanvas import present_canvas_frame, route_canvas_input, translate_canvas_input

library = sys.argv[1]
try:
    translate_canvas_input("begin", "mouse", (0, 0))
    raise AssertionError("an unspecified GUI library path was accepted")
except ValueError:
    pass
pen = translate_canvas_input("begin", "pen", (20, 30), pressure=.4, tilt=(18, -27),
                             rotation_degrees=90, timestamp=12.5, device_id=42,
                             buttons=1, library=library)
assert pen["phase"] == "begin" and pen["device"] == "pen"
assert pen["button"] == "primary"
assert pen["position"] == (20, 30) and pen["pressure"] == .4
assert pen["tilt"] == (.2, -.3) and abs(pen["rotation_radians"] - math.pi / 2) < 1e-12
assert pen["device_id"] == 42 and pen["buttons"] == 1
scroll = translate_canvas_input("scroll", "touch", (1, 2), scroll=(3, -4),
                                control=True, library=library)
assert scroll["kind"] == "scroll" and scroll["scroll"] == (3, -4) and scroll["control"]
middle = translate_canvas_input("begin", "mouse", (1, 2), buttons=4, library=library)
assert middle["button"] == "middle" and not middle["primary_down"]
try:
    translate_canvas_input("begin", "pen", (1, 2), pressure=float("nan"), library=library)
    raise AssertionError("non-finite pressure accepted")
except ValueError:
    pass

pan = route_canvas_input("begin", "mouse", (1, 2), (10, 20), buttons=4, library=library)
assert pan["state"]["mode"] == "pan" and pan["stroke"] is None
pan = route_canvas_input("update", "mouse", (1, 2), (14, 17), buttons=4,
                         state=pan["state"], viewport=pan["viewport"], library=library)
assert pan["viewport_changed"] and pan["viewport"]["offset"] == (4, -3)
pen_gesture = route_canvas_input("begin", "eraser", (3, 4), (4, 5), pressure=.6,
                                 device_id=8, selected=True, shift=True, library=library)
assert pen_gesture["stroke"]["phase"] == "begin" and pen_gesture["eraser"] and pen_gesture["stroke"]["shift"]
pen_gesture = route_canvas_input("cancel", "eraser", (3, 4), (4, 5), device_id=8,
                                 state=pen_gesture["state"], selected=True, library=library)
assert pen_gesture["stroke"]["phase"] == "cancel" and pen_gesture["state"]["mode"] == "idle"

size, pixels = present_canvas_frame([0xffff0000, 0xff00ff00, 0xff0000ff, 0x80112233],
                                    (2, 2), (0, 0, 2, 2), (2, 2), library=library)
assert size == (2, 2) and pixels == bytes([255, 0, 0, 255, 0, 255, 0, 255,
                                           0, 0, 255, 255, 17, 34, 51, 128])
try:
    present_canvas_frame([0xffff0000] * 4, (2, 2), (0, 0, 2, 2), (2, 2),
                         maximum_pixels=1, library=library)
    raise AssertionError("pixel budget accepted")
except ValueError:
    pass
