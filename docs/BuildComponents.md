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

The existing CanvasItem rendering test keeps its QML construction fixture in
`tests/Legacy/CanvasItemQmlFixture.qml`; its other presentation and rendering
checks remain in the same test. This isolation does not migrate the Quick
component itself to LVUI.

The CanvasItem test now creates its private resource output directory before
its first file operation. A clean macOS Quick-enabled build and the offscreen
CanvasItem test passed on 2026-11-15.

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

`Core`와 `Quick`의 설치 패키지는 `SameMinorVersion`을 사용한다. 최소 버전 `0.35.39`는 `0.35.41`을 허용하며 `0.35.38`과 다른 메이저·마이너를 거부한다.
같은 메이저·마이너의 패치는 바이너리 ABI를 유지한다. 같은 프로세스의 소비자는 한 패키지의 헤더와 라이브러리를 사용하며, `EXACT`를 명시하면 요청한 버전을 요구한다.
독립 Core 소비자는 QML·Quick 탐색을 끄고 IISC 문서와 메타데이터를 왕복하며 복원한 픽셀을 렌더링한다.
IISC 편집 코덱의 내부 이름은 Qt 키워드 매크로와 구분한다. `install.sh`는 새 설치 경로의 라이브러리와 CMake 설정으로 소비자를 검증한다.

macOS에서 Core 설치 경로가 컴파일러의 암묵적 라이브러리 경로에 포함되면
Core 대상은 실행 시에도 같은 라이브러리를 찾을 수 있도록 런타임 검색 경로를
전달한다. `tests/core_runtime_path`는 이 조건에서 설치된 Core 설정을 검증한다.
검사할 스테이징 경로를 `IISHAREDCANVAS_TEST_PACKAGE_PREFIX`로 지정하면
`iiSharedCanvas.InstalledCoreRuntimePath` CTest가 이 소비자 구성을 검사한다.
