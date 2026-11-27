"""Exercise the optional Python bridge through owned copies and edit history."""
import sys

sys.path.insert(0, sys.argv[2])
from iisharedcanvas import CanvasDocument

library = sys.argv[1]
with CanvasDocument.raster(2, 2, 0xff224466, library=library) as child:
    with CanvasDocument.raster(2, 2, 0, library=library) as parent:
        parent.embed(child, "copy", "linked")
        assert parent.snapshot().assets == 2
        assert parent.pixel(0, 0) == 0xff224466
        parent.capture_last_edit()
        parent.toggle_last_edit()
        assert parent.pixel(0, 0) == 0
        parent.toggle_last_edit()
        assert parent.pixel(0, 0) == 0xff224466
        child.begin_reeditable("pixels")
        child.reedit_adjustment(exposure=1)
        child.reedit_adjustment(exposure=2)
        adjusted = child.pixel(0, 0)
        child.reedit_effect(16)  # RasterEffectKind::Invert
        child.reedit_adjustment(exposure=2)
        assert child.pixel(0, 0) == adjusted
        before = child.to_bytes()
        try:
            child.reedit_adjustment(exposure=float("nan"))
        except RuntimeError:
            pass
        else:
            raise AssertionError("invalid adjustment must fail")
        assert child.to_bytes() == before
        assert parent.pixel(0, 0) == 0xff224466
        parent.replace_linked(child, "copy")
        assert parent.pixel(0, 0) == child.pixel(0, 0)
        parent.undo()
        assert parent.pixel(0, 0) == 0xff224466
        parent.redo()
        encoded = parent.to_bytes()
        with CanvasDocument.from_bytes(encoded, library=library) as reopened:
            assert reopened.pixel(0, 0) == child.pixel(0, 0)
