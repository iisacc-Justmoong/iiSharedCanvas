#!/usr/bin/env python3
"""Exercise the installed Python raster API, including temporary stroke pixels."""

import copy
import sys
import unittest


if len(sys.argv) != 3:
    raise SystemExit("expected the bridge library and Python module directory")
LIBRARY, MODULE_DIRECTORY = sys.argv[1:]
sys.argv = sys.argv[:1]
sys.path.insert(0, MODULE_DIRECTORY)
from iisharedcanvas import ProjectedRaster, RasterSample  # noqa: E402


class ProjectedStrokeTest(unittest.TestCase):
    def test_preview_commit_history_and_cancel(self):
        with ProjectedRaster(16, 16, library=LIBRARY) as canvas:
            first = RasterSample(2, 2, 0x804080FF)
            old_tail = RasterSample(3, 3, 0x804080FF)
            new_tail = RasterSample(4, 4, 0x804080FF)
            canvas.begin_stroke()
            canvas.update_stroke([first], [old_tail])
            self.assertNotEqual(canvas.pixel(2, 2), 0)
            self.assertNotEqual(canvas.pixel(3, 3), 0)
            self.assertEqual(canvas.stroke_source_pixel(3, 3), 0)
            canvas.update_stroke([], [new_tail])
            self.assertEqual(canvas.pixel(3, 3), 0)
            shown = canvas.pixel(4, 4)
            canvas.end_stroke()
            self.assertEqual(canvas.pixel(4, 4), shown)
            canvas.undo()
            self.assertEqual(canvas.pixel(2, 2), 0)
            self.assertEqual(canvas.pixel(4, 4), 0)
            canvas.redo()
            self.assertEqual(canvas.pixel(4, 4), shown)
            canvas.begin_stroke()
            canvas.update_stroke([], [old_tail])
            canvas.cancel_stroke()
            self.assertEqual(canvas.pixel(3, 3), 0)
            self.assertEqual(canvas.pixel(4, 4), shown)

    def test_invalid_inputs_are_rejected(self):
        with self.assertRaises(ValueError):
            ProjectedRaster(0, 16, library=LIBRARY)
        with self.assertRaises(MemoryError):
            ProjectedRaster(1 << 20, 1 << 20, library=LIBRARY)
        canvas = ProjectedRaster(16, 16, library=LIBRARY)
        with self.assertRaises(TypeError):
            copy.copy(canvas)
        with self.assertRaises(TypeError):
            copy.deepcopy(canvas)
        canvas.begin_stroke()
        with self.assertRaises(ValueError):
            canvas.update_stroke([RasterSample(-1, 0, 0xFFFFFFFF)], [])
        self.assertEqual(canvas.pixel(0, 0), 0)
        canvas.cancel_stroke()
        canvas.close()
        with self.assertRaises(RuntimeError):
            canvas.pixel(0, 0)

    def test_eraser_and_no_op_redo(self):
        with ProjectedRaster(8, 8, 0xFFFFFFFF, erase=True, library=LIBRARY) as canvas:
            canvas.begin_stroke()
            canvas.update_stroke([], [RasterSample(3, 3, 0x80000000)])
            shown = canvas.pixel(3, 3)
            self.assertLess(shown >> 24, 255)
            canvas.end_stroke()
            self.assertEqual(canvas.pixel(3, 3), shown)
            canvas.undo()
            self.assertEqual(canvas.pixel(3, 3), 0xFFFFFFFF)
            canvas.begin_stroke()
            canvas.update_stroke([], [])
            canvas.end_stroke()
            canvas.redo()
            self.assertEqual(canvas.pixel(3, 3), shown)


if __name__ == "__main__":
    unittest.main()
