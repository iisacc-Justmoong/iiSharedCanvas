# Dynamic linking

Both the native canvas and the legacy Core, facade and optional Quick libraries
default to shared linking. `IISHAREDCANVAS_BUILD_SHARED=OFF` requests static
archives. Shared iOS configurations use frameworks and WebAssembly builds use
side modules. Package the selected canvas library with its matching dependencies.

The Emscripten package configuration enables CMake shared-library imports for side modules. The builder or application must supply the main-module compile and link options.
