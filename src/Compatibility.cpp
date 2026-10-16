// The existing library remains a concrete link target. Its public C++ symbols
// are implemented by Core and Quick and re-exported by the macOS dylib facade.
extern "C" void iiSharedCanvasCompatibilityAnchor() {}
