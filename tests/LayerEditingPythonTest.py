"""Exercise the public Python layer editing and exact compound-shape contract."""
import sys
sys.path.insert(0,sys.argv[2])
from iisharedcanvas import CanvasDocument

library=sys.argv[1]
with CanvasDocument.raster(8,2,0xffff0000,library=library) as canvas:
    canvas.put_fill("blue","blue-layer",color=0xff0000ff)
    canvas.put_fill("green","green-layer",color=0xff00ff00)
    canvas.arrange_layers(["green-layer"],"back")
    assert canvas.pixel(0,0)==0xff0000ff
    canvas.undo()
    assert canvas.pixel(0,0)==0xff00ff00
    canvas.align_layers([("blue-layer",(0,0,8,2)),("green-layer",(0,0,8,2))],
                        (1,0,8,2),"left")
    assert canvas.pixel(0,0)==0xffff0000
    canvas.undo()
    canvas.distribute_layers([("layer",(0,0,1,2)),("blue-layer",(3,0,1,2)),
                              ("green-layer",(6,0,1,2))],axis="horizontal",mode="gaps")
    commands=[("M",0,0),("C",.125,.875,1.125,1.875,2,1),("L",0,0),("Z",)]
    canvas.put_vector("curve","curve-layer",commands,color=0xffffffff)
    canvas.put_vector("rect","rect-layer",[("M",2,0),("L",6,0),("L",6,2),("L",2,2),("Z",)],color=0xffffffff)
    canvas.combine_shapes(["curve","rect"],"combined","combined-layer",operation="union",color=0xff336699)
    assert canvas.vector_command("combined",0,1)==("C",.125,.875,1.125,1.875,2.0,1.0)
    canvas.edit_vector_command("combined",0,1,("C",.25,.75,1.25,1.75,2,1))
    assert canvas.vector_command("combined",0,1)[1]==.25
    canvas.undo()
    assert canvas.vector_command("combined",0,1)[1]==.125
    with CanvasDocument.from_bytes(canvas.to_bytes(),library=library) as restored:
        assert restored.vector_command("combined",0,1)==("C",.125,.875,1.125,1.875,2.0,1.0)
    shape_image=canvas.slice((0,0,8,2))
    canvas.flatten("shape-flat","shape-flat-asset")
    try:
        canvas.vector_command("combined",0,1)
        raise AssertionError("flatten retained an exclusively referenced source asset")
    except RuntimeError:
        pass
    canvas.undo()
    assert canvas.vector_command("combined",0,1)==("C",.125,.875,1.125,1.875,2.0,1.0)
    assert canvas.slice((0,0,8,2))==shape_image

with CanvasDocument.raster(3,2,0xffff0000,library=library) as canvas:
    canvas.put_fill("blue","blue-layer",color=0x800000ff)
    original=canvas.slice((0,0,3,2))
    canvas.merge_layers(["layer","blue-layer"],"merged","merged-asset")
    assert canvas.slice((0,0,3,2))==original
    canvas.undo()
    assert len(canvas.query_layers())==2
    canvas.redo()
    canvas.flatten("flat","flat-asset")
    assert canvas.slice((0,0,3,2))==original and len(canvas.query_layers())==1
    canvas.undo()
    assert canvas.slice((0,0,3,2))==original

with CanvasDocument.raster(2,1,0xffff0000,library=library) as canvas:
    canvas.put_fill("blue-top","blue-top-layer",color=0xff0000ff)
    canvas.put_fill("green-top","green-top-layer",color=0xff00ff00)
    original=canvas.slice((0,0,2,1))
    canvas.merge_layers(["blue-top-layer","green-top-layer"],"top-merged","top-asset")
    assert canvas.slice((0,0,2,1))==original and len(canvas.query_layers())==2
    canvas.undo()
    assert canvas.slice((0,0,2,1))==original and len(canvas.query_layers())==3

# Source-over quantization can make a separately merged intermediate differ
# from the original composite. Such a merge must not publish any change.
with CanvasDocument.raster(1,1,0x796df0ef,library=library) as canvas:
    canvas.put_fill("a","a-layer",color=0x33ff3cfc)
    canvas.put_fill("b","b-layer",color=0x1bb564f8)
    original=canvas.to_bytes()
    try:
        canvas.merge_layers(["a-layer","b-layer"],"bad-merge","bad-asset")
        raise AssertionError("merge accepted a composition that changes pixels")
    except RuntimeError as error:
        assert "before and after" in str(error)
    assert canvas.to_bytes()==original and len(canvas.query_layers())==3

with CanvasDocument.raster(1,1,0xffffffff,library=library) as canvas:
    for identifier in ("a","b","c","d"):
        canvas.put_fill(identifier,identifier,color=0xffffffff)
    canvas.set_group("g","First")
    canvas.set_group("h","Second")
    for identifier in ("a","b"): canvas.assign_group(identifier,"g")
    for identifier in ("c","d"): canvas.assign_group(identifier,"h")
    canvas.arrange_layers(["a","c"],"front")
    assert canvas.query_layers()==["layer","b","a","d","c"]
    canvas.undo()
    assert canvas.query_layers()==["layer","a","b","c","d"]
