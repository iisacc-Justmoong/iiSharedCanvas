"""Python clone must own its data and start with a fresh history."""
import sys
sys.path.insert(0,sys.argv[2])
from iisharedcanvas import CanvasDocument, scan_transparent_bounds

library=sys.argv[1]
assert scan_transparent_bounds([0,0x01000000,0x80000000,0],2,2,origin=(-3,4),library=library)==(-3,4,2,2)
assert scan_transparent_bounds([0],1,1,library=library) is None
with CanvasDocument.raster(2,1,0xff123456,library=library) as original:
    original.put_fill("blue","blue-layer",color=0xff0000ff)
    with original.clone() as copy:
        assert copy.pixel(0,0)==0xff0000ff
        assert copy.snapshot().undo_entries==0
        copy.put_fill("green","green-layer",color=0xff00ff00)
        assert copy.pixel(0,0)==0xff00ff00 and original.pixel(0,0)==0xff0000ff
        assert original.snapshot().undo_entries==1

with CanvasDocument.raster(3,3,0xff123456,library=library) as canvas:
    canvas.crop_canvas(1,1,2,2)
    assert (canvas.snapshot().width,canvas.snapshot().height)==(2,2)
    assert canvas.pixel(0,0)==0xff123456
    canvas.undo()
    assert (canvas.snapshot().width,canvas.snapshot().height)==(3,3)
    canvas.resize_canvas(5,5,anchor="center")
    assert canvas.pixel(0,0)==0 and canvas.pixel(1,1)==0xff123456
    canvas.undo()
    assert (canvas.snapshot().width,canvas.snapshot().height)==(3,3)
    assert canvas.content_bounds()==(0,0,3,3)
    canvas.set_pixel_aspect_ratio(2,1)
    assert canvas.pixel_aspect_ratio()==(2,1)
    width,height,pixels=canvas.render_display(sampling="nearest")
    assert (width,height)==(6,3) and len(pixels)==18
    canvas.undo()
    assert canvas.pixel_aspect_ratio()==(1,1)
    canvas.resample_document(6,3)
    assert (canvas.snapshot().width,canvas.snapshot().height)==(6,3)
    assert canvas.pixel(5,2)==0xff123456
    canvas.undo()
    canvas.rotate_image(90)
    assert (canvas.snapshot().width,canvas.snapshot().height)==(3,3)
    assert canvas.pixel(0,0)==0xff123456
    canvas.undo()
    canvas.resample_document(6,6,mode="replace_pixels")
    assert (canvas.snapshot().width,canvas.snapshot().height)==(6,6)
    assert canvas.pixel(5,5)==0xff123456
    canvas.undo()

with CanvasDocument.raster(2,1,0,library=library) as canvas:
    canvas.put_fill("dot","dot-layer",extent=(1,1),color=0xffffffff)
    canvas.align_layers([("dot-layer",(0,0,1,1))],(3,0,1,1),"left")
    canvas.set_group("hidden","Hidden",visible=False)
    canvas.assign_group("dot-layer","hidden")
    assert canvas.content_bounds() is None
    assert canvas.content_bounds(scope="all_layers",region=(-1,0,6,1))==(3,0,1,1)
