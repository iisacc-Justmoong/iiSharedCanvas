if(NOT DEFINED IISHAREDCANVAS_SOURCE_DIR)
    message(FATAL_ERROR "IISHAREDCANVAS_SOURCE_DIR is required")
endif()

function(require_text file_path expected)
    file(READ "${file_path}" contents)
    string(FIND "${contents}" "${expected}" position)
    if(position EQUAL -1)
        message(FATAL_ERROR "${file_path} must contain: ${expected}")
    endif()
endfunction()

file(READ "${IISHAREDCANVAS_SOURCE_DIR}/CMakeLists.txt" cmake_lists)
string(REGEX MATCHALL "find_package\\(" direct_find_packages "${cmake_lists}")
list(LENGTH direct_find_packages direct_dependency_count)
if(NOT direct_dependency_count EQUAL 5)
    message(FATAL_ERROR "iiSharedCanvas must have only the reviewed iiFileProvider, iiPaintEngine, zlib and libzip dependencies plus test-only SQLite")
endif()

require_text("${IISHAREDCANVAS_SOURCE_DIR}/CMakeLists.txt"
             "find_package(iiPaintEngine 0.1.0 CONFIG REQUIRED)")
require_text("${IISHAREDCANVAS_SOURCE_DIR}/CMakeLists.txt"
             "find_package(SQLite3 3.26 REQUIRED)")
require_text("${IISHAREDCANVAS_SOURCE_DIR}/CMakeLists.txt" "find_package(ZLIB 1.2.9 REQUIRED)")
require_text("${IISHAREDCANVAS_SOURCE_DIR}/cmake/iiSharedCanvasConfig.cmake.in" "find_dependency(ZLIB 1.2.9 REQUIRED)")
require_text("${IISHAREDCANVAS_SOURCE_DIR}/CMakeLists.txt" "find_package(libzip 1.7.3 CONFIG REQUIRED)")
require_text("${IISHAREDCANVAS_SOURCE_DIR}/cmake/iiSharedCanvasConfig.cmake.in" "find_dependency(libzip 1.7.3 CONFIG REQUIRED)")
foreach(module Audio/AudioCodec Bitmap/BitmapCodec Vector/VectorCodec Video/VideoCodec Media/MediaIo Layered/LayeredDocumentCodec)
    require_text("${IISHAREDCANVAS_SOURCE_DIR}/CMakeLists.txt" "src/${module}.cpp")
    require_text("${IISHAREDCANVAS_SOURCE_DIR}/src/iiSharedCanvas.h" "${module}.h")
    require_text("${IISHAREDCANVAS_SOURCE_DIR}/install.sh" "${module}.h")
endforeach()
require_text("${IISHAREDCANVAS_SOURCE_DIR}/docs/MEDIA_IO.md" "DocumentFile::edit")
require_text("${IISHAREDCANVAS_SOURCE_DIR}/docs/DEPENDENCIES.md" "runtime executables")
require_text("${IISHAREDCANVAS_SOURCE_DIR}/docs/DEPENDENCIES.md" "libzip")
require_text("${IISHAREDCANVAS_SOURCE_DIR}/docs/MEDIA_IO.md" "importLayeredDocument")
require_text("${IISHAREDCANVAS_SOURCE_DIR}/docs/LAYERED_IMPORT_CLI.md" "iisc-import")
require_text("${IISHAREDCANVAS_SOURCE_DIR}/CMakeLists.txt" "add_executable(iisc-import tools/iisc-import.cpp)")
require_text("${IISHAREDCANVAS_SOURCE_DIR}/CMakeLists.txt" "set_target_properties(iisc-import PROPERTIES INSTALL_RPATH_USE_LINK_PATH TRUE)")
require_text("${IISHAREDCANVAS_SOURCE_DIR}/install.sh" "\"\${import_executable}\" --help")
require_text("${IISHAREDCANVAS_SOURCE_DIR}/CMakeLists.txt" "src/Layered/PsdWriter.cpp")
require_text("${IISHAREDCANVAS_SOURCE_DIR}/src/Layered/LayeredDocumentCodec.h" "PsdExportOptions")
require_text("${IISHAREDCANVAS_SOURCE_DIR}/docs/API.md" "encodePsd")
require_text("${IISHAREDCANVAS_SOURCE_DIR}/docs/PSD_EXPORT.md" "Smart Object")
require_text("${IISHAREDCANVAS_SOURCE_DIR}/docs/PSD_EXPORT_CLI.md" "iisc-export-psd")
require_text("${IISHAREDCANVAS_SOURCE_DIR}/docs/DEPENDENCIES.md" "PSD 내보내기")
require_text("${IISHAREDCANVAS_SOURCE_DIR}/CMakeLists.txt" "add_executable(iisc-export-psd tools/iisc-export-psd.cpp)")
require_text("${IISHAREDCANVAS_SOURCE_DIR}/install.sh" "\"\${export_psd_executable}\" --help")
require_text("${IISHAREDCANVAS_SOURCE_DIR}/tests/consumer/main.cpp" "verifyPsdExport")
require_text("${IISHAREDCANVAS_SOURCE_DIR}/tests/consumer/main.cpp" "verifyTimelineInterchange")
require_text("${IISHAREDCANVAS_SOURCE_DIR}/src/iiSharedCanvas.h" "Timeline/TimelineInterchange.h")
require_text("${IISHAREDCANVAS_SOURCE_DIR}/install.sh" "Timeline/TimelineInterchange.h")
require_text("${IISHAREDCANVAS_SOURCE_DIR}/install.sh" "\"\${export_timeline_executable}\" --help")
require_text("${IISHAREDCANVAS_SOURCE_DIR}/docs/TIMELINE_INTERCHANGE.md" "source.iisc")
require_text("${IISHAREDCANVAS_SOURCE_DIR}/docs/DEPENDENCIES.md" "OpenTimelineIO")
require_text("${IISHAREDCANVAS_SOURCE_DIR}/CMakeLists.txt" "add_executable(iisc-export-timeline tools/iisc-export-timeline.cpp)")
require_text("${IISHAREDCANVAS_SOURCE_DIR}/CMakeLists.txt"
             "src/File/DocumentFile.cpp")
require_text("${IISHAREDCANVAS_SOURCE_DIR}/cmake/iiSharedCanvasConfig.cmake.in"
             "find_dependency(iiFileProvider 0.5.0 CONFIG REQUIRED)")
require_text("${IISHAREDCANVAS_SOURCE_DIR}/CMakeLists.txt"
             "set(CMAKE_FIND_FRAMEWORK LAST)")

require_text("${IISHAREDCANVAS_SOURCE_DIR}/docs/PERSISTENCE.md"
             "There is no save method")
require_text("${IISHAREDCANVAS_SOURCE_DIR}/docs/DEPENDENCIES.md"
             "sqlite.org/copyright.html")
require_text("${IISHAREDCANVAS_SOURCE_DIR}/CMakeLists.txt"
             "project(iiSharedCanvas VERSION 0.28.0 LANGUAGES CXX)")
require_text("${IISHAREDCANVAS_SOURCE_DIR}/CMakeLists.txt"
             "SOVERSION \"\${PROJECT_VERSION_MAJOR}.\${PROJECT_VERSION_MINOR}\"")
require_text("${IISHAREDCANVAS_SOURCE_DIR}/CMakeLists.txt"
             "COMPATIBILITY ExactVersion")
require_text("${IISHAREDCANVAS_SOURCE_DIR}/tests/consumer/CMakeLists.txt"
             "find_package(iiSharedCanvas 0.28.0 CONFIG REQUIRED)")
require_text("${IISHAREDCANVAS_SOURCE_DIR}/CMakeLists.txt"
             "iiPaintEngine::iiPaintEngine")
require_text("${IISHAREDCANVAS_SOURCE_DIR}/CMakeLists.txt"
             "src/Document/Document.cpp")
require_text("${IISHAREDCANVAS_SOURCE_DIR}/CMakeLists.txt"
             "src/Document/DocumentEditor.cpp")
require_text("${IISHAREDCANVAS_SOURCE_DIR}/CMakeLists.txt" "src/Document/ArtboardEditor.cpp")
require_text("${IISHAREDCANVAS_SOURCE_DIR}/CMakeLists.txt" "docs/ARTBOARDS.md")
require_text("${IISHAREDCANVAS_SOURCE_DIR}/tests/consumer/CMakeLists.txt" "iiSharedCanvas.InstalledArtboard")
require_text("${IISHAREDCANVAS_SOURCE_DIR}/tests/consumer/CMakeLists.txt" "iiSharedCanvas.InstalledDynamicFrameContent")
require_text("${IISHAREDCANVAS_SOURCE_DIR}/CMakeLists.txt" "docs/DYNAMIC_FRAME_CONTENT.md")
require_text("${IISHAREDCANVAS_SOURCE_DIR}/src/Document/DocumentEditor.h" "setDynamicFrameContent")
require_text("${IISHAREDCANVAS_SOURCE_DIR}/CMakeLists.txt" "PROPERTIES ENVIRONMENT_MODIFICATION")
require_text("${IISHAREDCANVAS_SOURCE_DIR}/tests/consumer/CMakeLists.txt" "PROPERTIES ENVIRONMENT_MODIFICATION")
require_text("${IISHAREDCANVAS_SOURCE_DIR}/src/Document/Document.h" "CurrentFormatMinor = 19")
require_text("${IISHAREDCANVAS_SOURCE_DIR}/CMakeLists.txt"
             "src/Bitmap/BitmapEditor.cpp")
require_text("${IISHAREDCANVAS_SOURCE_DIR}/CMakeLists.txt"
             "src/Bitmap/ChunkedBitmapEditor.cpp")
require_text("${IISHAREDCANVAS_SOURCE_DIR}/CMakeLists.txt"
             "src/Camera/CameraRaw.cpp")
require_text("${IISHAREDCANVAS_SOURCE_DIR}/CMakeLists.txt"
             "src/Metadata/StableDiffusionGenerationParameters.cpp")
require_text("${IISHAREDCANVAS_SOURCE_DIR}/CMakeLists.txt"
             "src/Metadata/StableDiffusionMetadata.cpp")
require_text("${IISHAREDCANVAS_SOURCE_DIR}/CMakeLists.txt"
             "src/QtAdapter/BitmapItem.cpp")
require_text("${IISHAREDCANVAS_SOURCE_DIR}/CMakeLists.txt"
             "src/QtAdapter/CanvasItem.cpp")
require_text("${IISHAREDCANVAS_SOURCE_DIR}/CMakeLists.txt"
             "src/Render/FrameRenderer.cpp")
require_text("${IISHAREDCANVAS_SOURCE_DIR}/CMakeLists.txt"
             "src/QtAdapter/AsyncFrameRenderer.cpp")
require_text("${IISHAREDCANVAS_SOURCE_DIR}/CMakeLists.txt"
             "src/Serialization/IiscCodec.cpp")
require_text("${IISHAREDCANVAS_SOURCE_DIR}/CMakeLists.txt"
             "src/Timeline/TimelineProject.cpp")
require_text("${IISHAREDCANVAS_SOURCE_DIR}/CMakeLists.txt"
             "src/Timeline/TimelineEditor.cpp")
require_text("${IISHAREDCANVAS_SOURCE_DIR}/CMakeLists.txt"
             "src/Validation/Validation.cpp")
require_text("${IISHAREDCANVAS_SOURCE_DIR}/CMakeLists.txt"
             "src/Vector/VectorEditor.cpp")
require_text("${IISHAREDCANVAS_SOURCE_DIR}/CMakeLists.txt"
             "$<BUILD_INTERFACE:\${CMAKE_CURRENT_SOURCE_DIR}/src>")
require_text("${IISHAREDCANVAS_SOURCE_DIR}/CMakeLists.txt"
             "BUILD_RPATH \"$<TARGET_FILE_DIR:iiPaintEngine::iiPaintEngine>\"")
require_text("${IISHAREDCANVAS_SOURCE_DIR}/CMakeLists.txt"
             "LINKER:-rpath,$<TARGET_FILE_DIR:iiPaintEngine::iiPaintEngine>")
require_text("${IISHAREDCANVAS_SOURCE_DIR}/cmake/iiSharedCanvasConfig.cmake.in"
             "find_dependency(iiPaintEngine 0.1.0 CONFIG REQUIRED)")
require_text("${IISHAREDCANVAS_SOURCE_DIR}/README.md"
             "The direct project dependencies are iiPaintEngine 0.1.0, SQLite")
require_text("${IISHAREDCANVAS_SOURCE_DIR}/README.md"
             "`BitmapEditor` binds to a raster asset by id")
require_text("${IISHAREDCANVAS_SOURCE_DIR}/README.md"
             "`ChunkedBitmapEditor` stores only touched chunks")
require_text("${IISHAREDCANVAS_SOURCE_DIR}/README.md"
             "`CanvasItem` is the full-document Qt Quick boundary")
require_text("${IISHAREDCANVAS_SOURCE_DIR}/README.md"
             "`renderFrameRegion` renders one world region")
require_text("${IISHAREDCANVAS_SOURCE_DIR}/README.md"
             "graph textures perform presentation")
require_text("${IISHAREDCANVAS_SOURCE_DIR}/README.md"
             "`DocumentEditor` is the validated structural mutation API")
require_text("${IISHAREDCANVAS_SOURCE_DIR}/README.md"
             "`VectorEditor` binds to a vector asset by id")
require_text("${IISHAREDCANVAS_SOURCE_DIR}/README.md"
             "`StaticBitmapLayer | StaticVectorLayer | DynamicBitmapLayer | DynamicVectorLayer` alternatives")
require_text("${IISHAREDCANVAS_SOURCE_DIR}/README.md"
             "every `Frame` directly owns its")
require_text("${IISHAREDCANVAS_SOURCE_DIR}/README.md"
             "`KeyframedSource::frameIndices` is a derived")
require_text("${IISHAREDCANVAS_SOURCE_DIR}/README.md"
             "The current C++ package version is 0.28.0")
require_text("${IISHAREDCANVAS_SOURCE_DIR}/README.md"
             "`LayerProperties::frameRange` optionally stores an inclusive")
require_text("${IISHAREDCANVAS_SOURCE_DIR}/README.md"
             "`setLayerFrameRange` sets or")
require_text("${IISHAREDCANVAS_SOURCE_DIR}/README.md"
             "`CameraRawData` is a format-neutral decoded Camera RAW aggregate")
require_text("${IISHAREDCANVAS_SOURCE_DIR}/README.md"
             "Each visible document layer is an independent asynchronous render unit")
require_text("${IISHAREDCANVAS_SOURCE_DIR}/README.md"
             "`StableDiffusionMetadata` preserves typed generation parameters")
require_text("${IISHAREDCANVAS_SOURCE_DIR}/README.md"
             "`parseStableDiffusionGenerationParameters` reads Stable Diffusion generation-")
require_text("${IISHAREDCANVAS_SOURCE_DIR}/README.md"
             "ComfyUI `prompt` and `workflow` JSON")
require_text("${IISHAREDCANVAS_SOURCE_DIR}/README.md"
             "`TimelineProject` is the application-neutral authoring model")
require_text("${IISHAREDCANVAS_SOURCE_DIR}/README.md"
             "Container and codec identifiers are open strings")
require_text("${IISHAREDCANVAS_SOURCE_DIR}/docs/API.md"
             "`Layer` | `StaticBitmapLayer \\| StaticVectorLayer \\| DynamicBitmapLayer \\| DynamicVectorLayer`")
require_text("${IISHAREDCANVAS_SOURCE_DIR}/docs/API.md"
             "`validateCameraRaw` 는 RAW 카메라 집합을 `validate(Document)` 에서 독립적으로")
require_text("${IISHAREDCANVAS_SOURCE_DIR}/docs/API.md"
             "renderFrameLayerTiles")
require_text("${IISHAREDCANVAS_SOURCE_DIR}/docs/API.md"
             "validateStableDiffusionMetadata")
require_text("${IISHAREDCANVAS_SOURCE_DIR}/docs/API.md"
             "findStableDiffusionGenerationParameter")
require_text("${IISHAREDCANVAS_SOURCE_DIR}/docs/API.md"
             "거절된 편집은 `revision()` 를 절대 진행시키지")
require_text("${IISHAREDCANVAS_SOURCE_DIR}/docs/API.md"
             "renameAsset")
require_text("${IISHAREDCANVAS_SOURCE_DIR}/docs/API.md"
             "insertKeyframedLayer")
require_text("${IISHAREDCANVAS_SOURCE_DIR}/docs/API.md"
             "`LayerFrameRange` | `firstFrame`, `lastFrame`")
require_text("${IISHAREDCANVAS_SOURCE_DIR}/docs/API.md"
             "`setLayerFrameRange` |선택적 포함 존재 범위를 설정하거나 초기화")
require_text("${IISHAREDCANVAS_SOURCE_DIR}/docs/API.md"
             "`layerExistsAt`는 선택적 포함 범위를 적용한 후")
require_text("${IISHAREDCANVAS_SOURCE_DIR}/docs/API.md"
             "표준 오름차순 `layerId` 순서")
require_text("${IISHAREDCANVAS_SOURCE_DIR}/docs/API.md"
             "저장된 모든 프레임은 비어 있지 않으며")
require_text("${IISHAREDCANVAS_SOURCE_DIR}/docs/API.md"
             "ensureInfiniteCanvasRegion")
require_text("${IISHAREDCANVAS_SOURCE_DIR}/docs/API.md"
             "appendQuadraticBezierTo")
require_text("${IISHAREDCANVAS_SOURCE_DIR}/docs/API.md"
             "`TimelineTrack` 는 `TimelineVideoTrack`")
require_text("${IISHAREDCANVAS_SOURCE_DIR}/docs/API.md"
             "setRenderVideoCodec")
require_text("${IISHAREDCANVAS_SOURCE_DIR}/docs/BLUEPRINT.md"
             "포인터 궤적, 곡선, Dab 스트림, 재생 명령")
require_text("${IISHAREDCANVAS_SOURCE_DIR}/docs/BLUEPRINT.md"
             "`BitmapItem` 는 선택된 래스터 자산의 하나에 대한 Qt 퀵 디스플레이 경계")
require_text("${IISHAREDCANVAS_SOURCE_DIR}/docs/BLUEPRINT.md"
             "카메라 RAW 파일 디코딩, 디모자이크, 및 톤")
require_text("${IISHAREDCANVAS_SOURCE_DIR}/docs/BLUEPRINT.md"
             "생성 메타데이터 캐리어 추출")
require_text("${IISHAREDCANVAS_SOURCE_DIR}/docs/BLUEPRINT.md"
             "하나의 불변 문서 스냅샷")
require_text("${IISHAREDCANVAS_SOURCE_DIR}/docs/BLUEPRINT.md"
             "`TimelineProject`는 캔버스 `Document`와 독립적")
require_text("${IISHAREDCANVAS_SOURCE_DIR}/docs/FORMAT.md"
             "버전 1 자산 참조 키는 홀드 샘플링만 사용")
require_text("${IISHAREDCANVAS_SOURCE_DIR}/docs/FORMAT.md"
             "`Document::frames` 는 엄격히 증가하는 희소")
require_text("${IISHAREDCANVAS_SOURCE_DIR}/docs/FORMAT.md"
             "희소 `Frame` 레코드와 모든 실제 `Keyframe` 를 소유")
require_text("${IISHAREDCANVAS_SOURCE_DIR}/docs/FORMAT.md"
             "버전 1.1 은 영역과 타임라인 사이에 무한 캔버스 메타데이터")
require_text("${IISHAREDCANVAS_SOURCE_DIR}/docs/FORMAT.md"
             "브러시 입력은 영속화된 콘텐츠 종류가 아닙니다")
require_text("${IISHAREDCANVAS_SOURCE_DIR}/docs/FORMAT.md"
             "`CameraRawData` 는 `.iisc` 버전 1.1 에 의해 인코딩되지")
require_text("${IISHAREDCANVAS_SOURCE_DIR}/docs/FORMAT.md"
             "레이어 병렬 렌더링은 지속된 레이어 순서를 변경하지")
require_text("${IISHAREDCANVAS_SOURCE_DIR}/docs/FORMAT.md"
             "버전 1.2는 레이어 컬렉션 뒤에 옵션인 Stable Diffusion 생성 메타데이터")
require_text("${IISHAREDCANVAS_SOURCE_DIR}/docs/FORMAT.md"
             "generationParametersText")
require_text("${IISHAREDCANVAS_SOURCE_DIR}/docs/FORMAT.md"
             "`TimelineProject`는 `.iisc` 버전 1.4에 의해 인코딩되지")
require_text("${IISHAREDCANVAS_SOURCE_DIR}/install.sh"
             "ctest --test-dir")
require_text("${IISHAREDCANVAS_SOURCE_DIR}/install.sh"
             "Serialization/IiscCodec.h")
require_text("${IISHAREDCANVAS_SOURCE_DIR}/install.sh"
             "Bitmap/ChunkedBitmapEditor.h")
require_text("${IISHAREDCANVAS_SOURCE_DIR}/install.sh"
             "Camera/CameraRaw.h")
require_text("${IISHAREDCANVAS_SOURCE_DIR}/install.sh"
             "Metadata/StableDiffusionGenerationParameters.h")
require_text("${IISHAREDCANVAS_SOURCE_DIR}/install.sh"
             "Metadata/StableDiffusionMetadata.h")
require_text("${IISHAREDCANVAS_SOURCE_DIR}/install.sh"
             "QtAdapter/AsyncFrameRenderer.h")
require_text("${IISHAREDCANVAS_SOURCE_DIR}/install.sh"
             "Timeline/TimelineProject.h")
require_text("${IISHAREDCANVAS_SOURCE_DIR}/install.sh"
             "Timeline/TimelineEditor.h")
require_text("${IISHAREDCANVAS_SOURCE_DIR}/install.sh"
             "Vector/VectorEditor.h")
require_text("${IISHAREDCANVAS_SOURCE_DIR}/install.sh"
             "libiiSharedCanvas.0.28.0.dylib")
require_text("${IISHAREDCANVAS_SOURCE_DIR}/tests/consumer/main.cpp"
             "setLayerFrameRange")
require_text("${IISHAREDCANVAS_SOURCE_DIR}/tests/consumer/main.cpp"
             "layerExistsAt")
require_text("${IISHAREDCANVAS_SOURCE_DIR}/LICENSE"
             "GNU AFFERO GENERAL PUBLIC LICENSE")
require_text("${IISHAREDCANVAS_SOURCE_DIR}/NOTICE.md"
             "SPDX-License-Identifier: AGPL-3.0-only")
require_text("${IISHAREDCANVAS_SOURCE_DIR}/CMakePresets.json"
             "\"binaryDir\": \"\${sourceDir}/build\"")

if(EXISTS "${IISHAREDCANVAS_SOURCE_DIR}/include")
    message(FATAL_ERROR "the source tree must not use a separate include/ directory")
endif()

file(GLOB_RECURSE public_headers
     "${IISHAREDCANVAS_SOURCE_DIR}/src/*.h")
foreach(public_header IN LISTS public_headers)
    file(READ "${public_header}" public_header_contents)
    if(public_header_contents MATCHES "[Aa]utomatic1111")
        message(FATAL_ERROR
                "Public API must use format-domain names, not a producer name: ${public_header}")
    endif()
endforeach()

# The repository contract keeps iiSharedCanvas upstream and product-neutral.
require_text("${IISHAREDCANVAS_SOURCE_DIR}/AGENTS.md"
             "직렬화 표준인 공식 캔버스 문서")
require_text("${IISHAREDCANVAS_SOURCE_DIR}/AGENTS.md"
             "소비자 채택은 병렬 호환성 실행이 아니라 순차적")
require_text("${IISHAREDCANVAS_SOURCE_DIR}/docs/BLUEPRINT.md"
             "iiSharedCanvas는 iisacc의 표준 캔버스 표준")
require_text("${IISHAREDCANVAS_SOURCE_DIR}/README.md"
             "Consumer applications do not shape this public contract in parallel")
if(NOT IS_DIRECTORY "${IISHAREDCANVAS_SOURCE_DIR}/src")
    message(FATAL_ERROR "co-located public headers and implementations must live under src/")
endif()

foreach(module Audio Bitmap Camera Document File Layered Media Metadata QtAdapter Render Serialization Timeline Validation Vector Video)
    if(EXISTS "${IISHAREDCANVAS_SOURCE_DIR}/${module}")
        message(FATAL_ERROR "${module} must live under src/, not at the repository root")
    endif()
    file(GLOB module_headers "${IISHAREDCANVAS_SOURCE_DIR}/src/${module}/*.h")
    file(GLOB module_implementations "${IISHAREDCANVAS_SOURCE_DIR}/src/${module}/*.cpp")
    if(NOT module_headers OR NOT module_implementations)
        message(FATAL_ERROR "src/${module} must contain co-located headers and implementations")
    endif()
    foreach(module_header IN LISTS module_headers)
        get_filename_component(source_name "${module_header}" NAME_WE)
        if(NOT EXISTS "${IISHAREDCANVAS_SOURCE_DIR}/src/${module}/${source_name}.cpp")
            message(FATAL_ERROR "${module_header} must have a co-located implementation")
        endif()
    endforeach()
    foreach(module_implementation IN LISTS module_implementations)
        get_filename_component(source_name "${module_implementation}" NAME_WE)
        if(NOT EXISTS "${IISHAREDCANVAS_SOURCE_DIR}/src/${module}/${source_name}.h"
           AND NOT EXISTS "${IISHAREDCANVAS_SOURCE_DIR}/src/${module}/${source_name}_p.hpp")
            message(FATAL_ERROR "${module_implementation} must have a co-located public or private header")
        endif()
    endforeach()
endforeach()

file(GLOB_RECURSE core_sources
     "${IISHAREDCANVAS_SOURCE_DIR}/src/iiSharedCanvas.h"
     "${IISHAREDCANVAS_SOURCE_DIR}/src/Bitmap/*.h"
     "${IISHAREDCANVAS_SOURCE_DIR}/src/Bitmap/*.cpp"
     "${IISHAREDCANVAS_SOURCE_DIR}/src/Camera/*.h"
     "${IISHAREDCANVAS_SOURCE_DIR}/src/Camera/*.cpp"
     "${IISHAREDCANVAS_SOURCE_DIR}/src/Metadata/*.h"
     "${IISHAREDCANVAS_SOURCE_DIR}/src/Metadata/*.cpp"
     "${IISHAREDCANVAS_SOURCE_DIR}/src/Document/*.h"
     "${IISHAREDCANVAS_SOURCE_DIR}/src/Document/*.cpp"
     "${IISHAREDCANVAS_SOURCE_DIR}/src/File/*.h"
     "${IISHAREDCANVAS_SOURCE_DIR}/src/Layered/*.h"
     "${IISHAREDCANVAS_SOURCE_DIR}/src/Layered/*.cpp"
     "${IISHAREDCANVAS_SOURCE_DIR}/src/Media/*.h"
     "${IISHAREDCANVAS_SOURCE_DIR}/src/Video/*.h"
     "${IISHAREDCANVAS_SOURCE_DIR}/src/Render/*.h"
     "${IISHAREDCANVAS_SOURCE_DIR}/src/Render/*.cpp"
     "${IISHAREDCANVAS_SOURCE_DIR}/src/Serialization/*.h"
     "${IISHAREDCANVAS_SOURCE_DIR}/src/Serialization/*.cpp"
     "${IISHAREDCANVAS_SOURCE_DIR}/src/Timeline/*.h"
     "${IISHAREDCANVAS_SOURCE_DIR}/src/Timeline/*.cpp"
     "${IISHAREDCANVAS_SOURCE_DIR}/src/Validation/*.h"
     "${IISHAREDCANVAS_SOURCE_DIR}/src/Validation/*.cpp"
     "${IISHAREDCANVAS_SOURCE_DIR}/src/Vector/*.h"
     "${IISHAREDCANVAS_SOURCE_DIR}/src/Vector/*.cpp")
# Codec implementation adapters may use the reviewed Qt dependency. Domain
# models, editors, renderers and domain/media public headers remain Qt-free.
list(FILTER core_sources EXCLUDE REGEX "/(Bitmap/BitmapCodec|Bitmap/ExtendedBitmapCodec|Vector/VectorCodec|Vector/SvgParser|Layered/LayeredDocumentCodec|Layered/OpenRasterParser|Layered/PsdParser|Layered/PsdWriter|Timeline/TimelineInterchange|Timeline/TimelineXmlWriter)\\.cpp$")
foreach(source_file IN LISTS core_sources)
    file(READ "${source_file}" source_contents)
    if(source_contents MATCHES "#[ \t]*include[ \t]*<Q[A-Za-z]")
        message(FATAL_ERROR "Core source must not directly include Qt: ${source_file}")
    endif()
endforeach()

require_text("${IISHAREDCANVAS_SOURCE_DIR}/src/Bitmap/BitmapEditor.cpp"
             "appendRasterDabs")
require_text("${IISHAREDCANVAS_SOURCE_DIR}/src/Bitmap/BitmapEditor.cpp"
             "paintRasterSamples")
require_text("${IISHAREDCANVAS_SOURCE_DIR}/src/Bitmap/ChunkedBitmapEditor.cpp"
             "projectBrushDabs")
require_text("${IISHAREDCANVAS_SOURCE_DIR}/src/Render/FrameRenderer.h"
             "return status == FrameRenderStatus::Success;")
require_text("${IISHAREDCANVAS_SOURCE_DIR}/src/Serialization/IiscCodec.h"
             "return error.code == IiscErrorCode::None;")
require_text("${IISHAREDCANVAS_SOURCE_DIR}/src/QtAdapter/BitmapItem.cpp"
             "QImage::Format_ARGB32")
require_text("${IISHAREDCANVAS_SOURCE_DIR}/src/QtAdapter/CanvasItem.cpp"
             "QSGSimpleTextureNode")
require_text("${IISHAREDCANVAS_SOURCE_DIR}/src/QtAdapter/CanvasItem.cpp"
             "createTextureFromImage")
require_text("${IISHAREDCANVAS_SOURCE_DIR}/src/QtAdapter/CanvasItem.cpp"
             "QSGTransformNode")
require_text("${IISHAREDCANVAS_SOURCE_DIR}/src/QtAdapter/AsyncFrameRenderer.cpp"
             "QThreadPool::globalInstance()")
require_text("${IISHAREDCANVAS_SOURCE_DIR}/src/QtAdapter/AsyncFrameRenderer.cpp"
             "remainingWorkers")

file(READ "${IISHAREDCANVAS_SOURCE_DIR}/src/QtAdapter/CanvasItem.h" canvas_item_header)
if(canvas_item_header MATCHES "QQuickPaintedItem")
    message(FATAL_ERROR "CanvasItem must use bounded scene-graph tiles, not QQuickPaintedItem")
endif()

require_text("${IISHAREDCANVAS_SOURCE_DIR}/src/iiSharedCanvas.h" "Document/CanvasSampling.h")
require_text("${IISHAREDCANVAS_SOURCE_DIR}/docs/CANVAS_MEDIA.md" "VideoAsset")
require_text("${IISHAREDCANVAS_SOURCE_DIR}/docs/FORMAT.md" "native-video-and-motion-extension-16")
require_text("${IISHAREDCANVAS_SOURCE_DIR}/tests/consumer/CMakeLists.txt" "CanvasMediaTest.cpp")

require_text("${IISHAREDCANVAS_SOURCE_DIR}/CMakeLists.txt" "docs/FOUR_LAYER_TYPES.md")
require_text("${IISHAREDCANVAS_SOURCE_DIR}/tests/consumer/CMakeLists.txt" "iiSharedCanvas.InstalledFourLayerType")
foreach(_type StaticBitmap StaticVector DynamicBitmap DynamicVector)
    require_text("${IISHAREDCANVAS_SOURCE_DIR}/src/Document/Document.h" "struct ${_type}Layer")
    require_text("${IISHAREDCANVAS_SOURCE_DIR}/src/Document/Document.h" "struct ${_type}Content")
endforeach()
