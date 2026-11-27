"""The Python surface uses the same native document, history and file format."""
import sys
from pathlib import Path

sys.path.insert(0, sys.argv[2])
from iisharedcanvas import CanvasDocument, LayerLocks

library=sys.argv[1]
with CanvasDocument.raster(2,2,0xff808080,library=library) as canvas, \
        CanvasDocument.raster(2,2,0xffff0000,library=library) as child:
    canvas.embed(child,"owned","top")
    canvas.save_selection("soft","Soft",2,2,bytes((0,64,128,255)))
    assert canvas.selection_coverage("soft",1,0)==64
    canvas.set_group("g","Group",mode="isolated")
    canvas.assign_group("top","g")
    canvas.link_layers("layer","top")
    canvas.set_layer_lock("top",LayerLocks.TRANSFORM)
    state=canvas.relationships()
    assert state["groups"][0]["mode"]=="isolated"
    assert state["links"]==[["layer","top"]]
    assert canvas.query_layers(name_contains="top")==["top"]
    encoded=canvas.to_bytes()
    with CanvasDocument.from_bytes(encoded,library=library) as restored:
        assert restored.selection_coverage("soft",0,1)==128
        assert restored.relationships()==state
    canvas.undo()
    assert not canvas.relationships()["layer_locks"]
    canvas.redo()
    assert canvas.relationships()==state
