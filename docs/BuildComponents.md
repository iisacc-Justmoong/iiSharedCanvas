# Processing and presentation components

The SDK exports separate CMake components:

| Component | Target | Responsibilities |
| --- | --- | --- |
| Core | `iiSharedCanvas::Core` | Document values, edits, validation, codecs, persistence, processing, and CPU rendering |
| Quick | `iiSharedCanvas::Quick` | Qt Quick items and their asynchronous presentation adapter; depends on Core |

The existing `iiSharedCanvas::iiSharedCanvas` library combines the components
available in the installation. Existing source consumers keep their full
presentation surface. Backend consumers request `COMPONENTS Core` and include
`iiSharedCanvasCore.h` or specific headers. Core does not discover or link Qt
Quick or QML. The four command-line tools also link Core only.

```cmake
find_package(iiSharedCanvas CONFIG REQUIRED COMPONENTS Core)
target_link_libraries(worker PRIVATE iiSharedCanvas::Core)
```

`IISHAREDCANVAS_BUILD_QUICK=OFF` produces a backend-only installation. Core still
uses Qt Core for the existing file/authorship adapters and Qt Gui for raster,
vector, font, and PDF codecs. This boundary removes presentation dependencies;
it does not claim that those remaining codecs have been converted to native
standard C++. Implementations live in the additional Core and Quick libraries.
The legacy macOS dylib re-exports both libraries to preserve existing public
symbol lookup. New source consumers retain the old complete CMake target;
backend consumers explicitly select Core. Prebuilt binaries on other platforms
must be relinked for the split package; their binary compatibility is unverified.

Document formats and public C++ document values are unchanged. Header and
library versions must match across a process, including iiSocietyHelper.
The standalone Core consumer builds with Qt QML and Quick discovery disabled,
round-trips an IISC document and its metadata, and renders the restored pixels.

macOS에서 Core 설치 경로가 컴파일러의 암묵적 라이브러리 경로에 포함되면
Core 대상은 실행 시에도 같은 라이브러리를 찾을 수 있도록 런타임 검색 경로를
전달한다. `tests/core_runtime_path`는 이 조건에서 설치된 Core 설정을 검증한다.
검사할 스테이징 경로를 `IISHAREDCANVAS_TEST_PACKAGE_PREFIX`로 지정하면
`iiSharedCanvas.InstalledCoreRuntimePath` CTest가 이 소비자 구성을 검사한다.
