<a id="media-interchange"></a>

# 미디어 교환

미디어 어댑터는 커밋된 픽셀 또는 네이티브 경로를 가져오며, 외부 파일, 포인터 궤적 또는 비디오 디코더를 문서 레이어에 삽입하지 않습니다. 그들은 쓰기 통과 `DocumentFile` 소유자와 독립적입니다. 열려 있는 작업 파일에 import를 삽입하려면, 해당 파일의 자산/레이어/키를 하나의 `DocumentFile::edit` 트랜잭션 안에 추가하십시오. 가져오기 자체는 문서를 변형하지 않습니다. 명시적 교환 내보내기는 수동 저장이 없는 작업 파일 I/O를 대체하지 않습니다.

<a id="layered-documents"></a>

## 계층화된 문서

`layeredDocumentFormats()` 는 레이어 보존 OpenRaster ( `ora` ) 및 Photoshop ( `psd` ) 리더를 광고합니다. `decodeLayeredDocument(bytes, options)` 와 `importLayeredDocument(localPath, options)` 는 접미사가 아닌 콘텐츠로 입력을 식별합니다. 해당 함수는 분리된, 검증된 `LayeredDocumentImportResult` 를 포함하며, 선택된 형식과 `Document` 및 `MediaIoResult` 를 반환합니다. 지원되지 않는 형식, 지원되지 않는 그리기 의미론, 손상된 데이터 및 리소스 한도는 전체 가져오기를 실패시킵니다; 실패 시 반환된 문서는 비어 있습니다. 원본 레이어에 대해 합성 합성 비트맵 가 대체되지 않습니다.

지원되는 픽셀 레이어는 별도의 `RasterAsset` / `StaticBitmapLayer` 쌍으로 매핑됩니다. 캔버스 크기, 아래에서 위로 순서, 이름, 가시성, 불투명도, 부호화된 오프셋 및 지원되는 블렌드 모드는 네이티브 편집 가능 필드로 유지됩니다. 소스 파일, 아카이브 엔트리 및 외부 디코더는 영구적으로 저장된 레이어 콘텐츠가 결코 되지 않습니다. OpenRaster 는 ZIP / XML 와 PNG 레이어 페이로드를 사용합니다. PSD 는 버전 1, 8비트 RGB 래스터 레이어를 지원하며, 완전한 Photoshop 장면 해석자가 아닙니다. 충실하게 표현될 수 없는 마스크, 조정 레이어, 지원되지 않는 그룹화/블렌드 및 기타 의미론은 거부되며, 숨겨진 것들도 포함됩니다. 중립 통과 ORA 그룹은 명시적인 경고와 함께 그룹 해제될 수 있습니다. 정확한 부분집합, 압축 모드, 메타데이터/색상 처리 및 거부 규칙은 [OPENRASTER_IMPORT .md](OPENRASTER_IMPORT.md) 에 명시되어 있으며
[PSD_IMPORT.md](PSD_IMPORT.md).

`LayeredDocumentImportOptions::idPrefix` 는 `"import"` 로 기본값입니다. 비어 있지 않은 표준 형식 UTF-8 이며, 최대 1024 바이트이고 NUL 가 없어야 합니다. 각 가져온 쌍은 `<prefix>-asset-<index>` / `<prefix>-layer-<index>` 를 받으며, 0 에서 아래에서 위로 순서대로 시작합니다. 중복된 소스 이름은 중복된 id 를 생성하지 않습니다. 여러 가져오기를 하나의 문서에 삽입할 때 고유한 접두사를 선택해야 합니다; 기존 문서의 암묵적인 변형이나 이름 변경은 없습니다. `maxLayers` 는 기본적으로 4096 로, `maxArchiveEntries` 는 16384로 기본값입니다. `limits` 도 입력 바이트, 디코딩된 데이터, 레이어 픽셀 차원 및 XML 깊이를 제한합니다; 형식별 회계를 참조하세요. 0 예산은 해당 리소스를 허용하지 않습니다. 한계는 어댑터 할당을 제한하며, 제 3 자 라이브러리의 전체 프로세스 메모량을 제한하지 않습니다. 리더는 파일을 추출하거나, 애플리케이션을 실행하거나, 리소스를 가져오거나, FFmpeg 를 사용하지 않습니다. libzip 는 ZIP 읽기를 공급하며, Qt 와 zlib 는 재사용됩니다.

성공적인 가져오기에서 직접 새로운 네이티브 작업 파일을 생성하십시오:

```cpp
auto imported = importLayeredDocument("/art/source.ora");
if (!imported.ok()) { /* surface imported.result and stop */ }
else {
    // 가져온 작품을 표시하기 전에 imported.result.warnings를 알린다.
    DocumentFile file;
    auto created = file.create("/art/converted.iisc", imported.document);
    // created.ok()를 확인한다. 기존 대상은 절대로 교체하지 않는다.
}
```

기존 파일에서는 고유한 id 접두사를 사용하여 반환된 자산과 레이어를 하나의 `DocumentFile::edit` 트랜잭션 안에서 추가한다. 가져오기는 프레임 0의 정적 상태이며 고유 캔버스 범위를 가지므로 대상 배치를 명시적으로 선택한다. 검증 실패나 충돌은 전체 트랜잭션을 롤백한다. 표준 스냅샷 바이트에는 `encodeIisc(imported.document)`를 계속 사용할 수 있고, `DocumentFile::create`는 즉시 기록되는 SQLite 작업 파일 변형을 생성한다. 설치된 [`iisc-import`](LAYERED_IMPORT_CLI.md) 유틸리티도 입력 파일과 새 `.iisc` 출력 경로를 명령줄 인자로 받는다.

`encodePsd(document)` 와 `exportPsd(document, localPath)` 는 네이티브 프레임 0에서 정적 PSD 를 내보냅니다. `layeredDocumentFormats()` 는 `psd.canWrite = true` 를 보고하며, OpenRaster 는 읽기 전용 읽기 전용으로 유지됩니다. 벡터는 임베디드 스마트 오브젝트에서 벡터 PDF 잠재 표현 내용을 유지하며, 래스터 캐시는 표시 호환성을 제공합니다. 단순 정수 변환을 가진 픽셀 레이어는 소스 픽셀과 오프셋을 유지합니다. 기타 비트맵 변환/조각은 문서 뷰포트 맵에 투영되어 베이킹됩니다. 애니메이션, 클리핑 및 정밀도 손실은 경고로 보고됩니다. PSD 내보내기는 `.iisc` 스냅샷 또는 작업 파일 스키마를 변경하지 않으며 전체 네이티브 타임라인의 백업이 아닙니다. [PSD_EXPORT .md](PSD_EXPORT.md) 와 설치된 [`iisc-export-psd`](PSD_EXPORT_CLI.md) 유틸리티를 참조하세요.

일반적인 CTest 픽스처는 로컬에서 제작되며 다운로드가 필요하지 않습니다. 추가로 독립적으로 제작된 픽스처의 경우, 해당 로컬 경로를 `iiSharedCanvasLayeredDocumentCodecTest`에 전달하십시오; 선택적 프로브는 네이티브 직렬화, rendered-frame identity 및 working-file reopen를 확인하고 실제 레이어 순서/이름/오프셋을 출력합니다. 입력 파일을 절대 수정하지 않습니다.

## Bitmap

`bitmapFormats()` 는 각 방향별로 독립적으로 실제 Qt 플러그인과 선택적 FFmpeg 확장 이미지 코덱을 보고합니다. `decodeBitmap` / `encodeBitmap`  바이트를 받습니다;  `importBitmap` / `exportBitmap`  로컬 파일을 사용합니다;  `exportBitmapFrame`  하나의 캔버스 프레임을 합성합니다. PNG ,  JPEG ,  BMP , 포터블  비트맵  형식, 아이콘,  TIFF ,  WebP ,  HEIC  및  JPEG   2000 는 배포된  Qt  빌드에 의존합니다. 선택적 확장 어댑터는  TGA ,  QOI ,  OpenEXR ,  DPX , Radiance  HDR ,  PCX  및  SGI  읽기/쓰기 플러스  PSD 와  DDS  리더를 해당 코덱이 존재할 때만 추가합니다. PSD 는  합성  이미지를 가져오며  Photoshop  레이어를 가져오지 않습니다; 지원 가능한 편집 가능한  PSD  하위 집합을 위해  `importLayeredDocument` 를 사용하세요. DDS 는 하나의 서페이스를 가져오며 텍스처/큐브맵 자산을 가져오지 않습니다. `extendedCodecs = false` 는 이 백엔드를 비활성화하며  `bitmapFormats(backend, false)` 는 오직  Qt 만 쿼리합니다. 카메라  RAW 디코딩은 지원되지 않습니다. 비트맵  작성기는  PSD 를 작성하지 않습니다; 위에서 설명된 별도의 레이어된  `encodePsd` / `exportPsd`   API 를 사용하세요. 이름은 모든 하위 타입, 페이지 레이아웃, 색상 모델, 또는 인코더 옵션에 대한 보장이 아닙니다.

픽셀은  sRGB 에서 직선  8-비트  ARGB 가 됩니다. Qt  경로에서는 태그가 있는 프로파일이 변환되고,  EXIF  방향은 기본적으로 적용되며, 비트 깊이/색상 변환이 보고됩니다. 태그가 없는 입력은  sRGB 로 해석됩니다. 고 비트 깊이/ HDR / CMYK 작성 데이터는 유지되지 않습니다. 확장된 코덱은 소스 ICC 프로필을 적용하지 않으며, 메타데이터, 색상 프로파일 충실도 및 보조 채널의 손실을 보고합니다. EXR / HDR 는 시그니처가 없는 RGB 를 사용하여 sRGB 주색상을 적용한 후, sRGB 디스플레이로 변환하고 HDR 톤 매핑 없이 SDR 로 클램핑합니다. 그들을 내보내면 8비트 캔버스를 선형 부동소수점 저장으로 확장하며, 원래 HDR 값을 복구할 수 없습니다. 그들의 16채널당 RGBA 비트 중간 버퍼도 `maxDecodedBytes` 를 따릅니다. 불투명 출력은 명시적인 매트와 알파 손실을 보고합니다. PNG 텍스트 캐리어는 생성 매개변수 텍스트를 포함한 UTF-8 키/값 데이터로 제공되며, 기타 메타데이터는 보존된 것으로 간주되지 않습니다. `imageIndex` 는 페이지/프레임을 선택하며, 애니메이션은 `importVideo` 에 속합니다. 선택 사항인 `format` 힌트는 신뢰할 수 있는 콘텐츠 서명이 없는 형식 (예: 오래된 TGA) 을 지원합니다. 파일 가져입기는 콘텐츠를 먼저 시도한 다음, 비트맵 리더가 식별되지 않은 경우에만 알려진 접미사를 시도합니다. JPEG   2000 리더는 사전 디코딩 크기 쿼리가 없는 경우, 할당 확인을 위해 검증된 JP2 헤더/코드스트림 차원을 사용합니다. PNG 청크 프레임링과 CRC 는 디코딩 전에 확인되며, 완전한 IEND 푸터를 포함하며, 잘린 PNG 의 관대한 수리는 허용되지 않습니다. 이것은 모든 다른 제 3 자 디코더가 모든 손상된 입력을 거부한다는 주장이 아닙니다. PNG 값은 공백을 유지하고 UTF-8 ; PNG 키워드는 표준의 1..79-바이트 인쇄 가능한 라틴-1 제한을 따릅니다. EXIF / XMP / IPTC 보존이 독자에 적용되는 방향 및 색상 변환을 초과하여 주장되지 않습니다. Export `format`는 기본값이 PNG이며, 대상 접미사로부터 추론되지 않습니다. `quality`는 -1(백엔드 기본값) 또는 0입니다..100 for Qt 인코더용; 확장된 코덱은 명시적인 고정 픽셀 형식과 코덱 기본값을 사용하며, 이 품질 노브는 사용하지 않습니다.

<a id="vector"></a>

## 벡터

`decodeSvg` / `importSvg`가 SVG를 읽고 SVGZ를 gzip합니다. 지원되는 고정된 경로와 도형은 편집 가능한 M/L/Q/C/Z 명령에 매핑됩니다. 지원되지 않는 그리기 기능은 전체 가져오기에서 실패하며, 아무런 알림 없이는 부분적인 그림을 반환하지 않습니다. XML 엔터티, 외부 리소스, 스크립트 및 무한 중첩은 거부됩니다.

편집 가능한 하위 집합은 절대/상대 M/L/H/V/Q/T/C/S/A/Z 경로 구문, 직사각형 (둥근 모서리 포함), 원, 타원, 선, 다각형 및 다각형을 포함합니다. 그룹은 아핀 행렬/이동/확대/회전/왜곡 변환, 상속된 고체 페인트 및 인라인 프레젠테이션 스타일을 적용할 수 있습니다. 뷰포트는 viewBox, `preserveAspectRatio` 맞춤 정렬 또는 없음, 및 px/in/cm/mm/pt/pc 길이를 지원하며; 백분율은 알려진 뷰포트 를 필요로 합니다. 타원 호는 경고와 함께 세제곱 베지에로 변환됩니다. 영이 아닌 채우기는 네이티브 짝-홀 모델로 정규화되며, 지원되지 않는 스트로크 캡/조인/대시 또는 불균일 스트로크 변환은 편집 가능한 채우기 윤곽으로 변환됩니다. 이러한 변환은 곡선을 평탄화하고 소스 경로 신원을 보존하지 않습니다. 네이티브 둥근 스트로크 및 짝-홀 경로는 직접 선/2 차/3 차 명령을 유지합니다.

그라디언트, 텍스트, CSS 클래스/스타일시트, 정의/사용 참조, 클리핑, 마스크, 필터, 중첩 뷰포트 및 비자명 그룹 불투명도는 편집 가능한 리더에 의해 아무런 알림 없이에 의해 근사되지 않습니다. `title`, `desc`, 저작 속성 및 메타데이터는 캔버스 객체로 유지되지 않습니다. 편집 가능성을 유지할 필요가 없을 때는 명시적인 래스터 대체 경로를 사용하십시오; 그 충실도는 Qt의 플러그인에 달려 있습니다.

`encodeSvg` / `exportSvg` 는 네이티브 편집 가능한 SVG 또는 SVGZ 를 방출합니다. `exportPdf` 는 벡터 경로와 비트맵 콘텐츠를 별도의 드로잉 작업으로 방출하며, 포함 프레임 범위로 선택된 선택적인 여러 페이지가 있습니다. PDF 는 작업 파일이 아닙니다. 1 인치당 96 캔버스 픽셀을 사용합니다. 격리된 벡터 레이어의 불투명도는 경고와 함께 레이어마다 래스터화되며, 지원되지 않는 블렌드 모드는 `rasterizeUnsupportedBlending` 가 전체 프레임 래스터화를 명시적으로 허용하지 않는 한 내보내기를 거부합니다. `rasterizeVectorFile` 는 호출자가 지정한 픽셀 차원에서 사용 가능한 Qt SVG / PDF 이미지 플러그인을 사용하여 별도의 명시적인 손실 비트맵 내보내기를 사용합니다. 편집 가능한 PDF / EPS / AI 내보내기는 제공되지 않습니다. SVG 내보내기는 `VectorAsset` 하나를 사용하며, PDF, 비트맵 프레임 또는 비디오를 통한 혼합 캔버스 내보내기가 가능합니다.

<a id="video"></a>

## 비디오

`videoCapabilities` 는 모든 호스트가 모든 코덱을 내보내는 고정된 약속이 아닌 구성된 FFmpeg 런타임 를 쿼리합니다. `probeVideo` 는 로컬 미디어를 검사합니다. `importVideo` 는 프레임 소유 키를 가진 비트맵 레이어로 디코딩하여 시간 스탬프를 유리상수 프레임 속도로 표준화합니다. `firstFrame` / `frameCount` 는 샘플링된 범위를 선택하여 0로 다시 기준을 잡습니다. 오디오 및 자막 트랙은 캔버스 픽셀이 아니며 내보내지지 않으며 오디오 존재는 보고됩니다.

`exportVideo` 는 선택된 포함 캔버스 범위를 순서대로 렌더링하여 RGBA 프레임을 FFmpeg 로 스트리밍하며, 원본 프레임 디렉토리를 덤프하지 않습니다. 기본 출력은 Matroska 에서 FFV1/BGRA 로 손실 없는 것입니다. 컨테이너, 인코더 및 픽셀 형식은 명시적인 문자열이며 파일 확장자가 이를 선택하지 않습니다. MP4 /H.264  와  HEVC ,  WebM/VP9 ,  MOV / QuickTime  애니메이션 및  ProRes ,  AVI/FFV1 ,  GIF  와  APNG 는 해당 인코더가 존재할 때 상호 운용성 테스트 프로파일입니다. H.264/YUV420 및 유사한 프로필은 인코더 호환 가능한 차원 (일반적으로 짝수 너비/높이) 을 요구합니다. 지원되지 않는 조합은 실패하며, 암시적인 코덱 또는 픽셀 형식 치환은 수행되지 않습니다. 불투명한 픽셀 형식은 구성된 매트 (matte) 를 사용합니다. GIF 는 팔레트/시간 양자화를 보고하며, 손실 압축 비디오 형식은 색상/정밀도 감소를 보고합니다. 오디오는 내보내지지 않습니다. 이것은 캔버스 애니메이션 교환이며, `TimelineProject` 오디오 믹서, 시퀀스 렌더러 또는 프로젝트 직렬화기 가 아닙니다.

<a id="safety-and-execution"></a>

## 안전 및 실행

모든 경로는 로컬이며, 모든 프로세스 인수는 쉘 없이 전달되며, 미디어 입력은 네트워크 프로토콜을 요청할 수 없습니다. 충돌은 `overwrite` 가 명시적으로 활성화될 때만 거부되며, 항상 작업 중인 `.iisc` 파일을 거부합니다. 실패한 내보내기는 부분적인 목적지를 게시하지 않습니다. 한도는 인코딩된 바이트, 픽셀 수, 총 디코딩된 프레임 저장, 벡터 명령 및 XML 깊이에 의해 제한됩니다. FFmpeg 호출에는 시간 제한과 협력적 취소가 있습니다. 한도는 제 3 자 코덱의 OS 샌드박스가 아닙니다: 불신할 수 있는 데이터에 대해 유지 관리된 코덱 빌드를 사용하세요. 특히, 확장된 이미지 탐지는 한 프레임을 디코딩하여 크기를 얻을 수 있으며, 한도는 반환된 데이터와 어댑터 버퍼에 의해 제한되며, 피크 코덱 프로세스 메모리가 아닙니다. `timeoutMs` 는 각 하위 프로세스에 적용되며, 전체 다중 프로세스 작업에는 적용되지 않습니다. 호출은 동기식이며 독립적인 입력에 대해 재진입 가능하고, 소비자가 소유한 워커에서 실행될 수 있으며; 파일에 묶인 편집은 여전히 소유자의 스레드에서 실행됩니다. 어댑터를 사용하기 전에 Qt 를 초기화하세요 (전체 `QGuiApplication` /이미지 기능 세트에 대한 PDF 입니다). 선택 가능한 백엔드는 절대 자동으로 다운로드되지 않습니다.

<a id="usage"></a>

## 사용법

먼저 가져온 다음 작업 파일의 검증된 트랜잭션에 삽입하십시오. 자산 및 레이어 ID는 호출자 소유이며 대상 문서에서 고유해야 합니다:

```cpp
BitmapImportOptions input;
input.assetId = "photo-1";
auto photo = importBitmap("/art/photo.tiff", input);
if (!photo.ok()) { /* surface photo.result and stop */ }
else {
    auto changed = file.edit([&](Document &draft) {
        draft.assets.emplace_back(photo.asset);
        draft.layers.emplace_back(StaticBitmapLayer{
            {"photo-layer-1", "Photo"}, StaticSource{photo.asset.id}});
        return true;
    });
    // changed.ok()를 확인한다. 성공한 결과는 이미 기록되었으며 save 호출이 없다.
}
```

명시적인 동영상 프로필은 컨테이너, 코덱 및 알파 정책을 표시하도록 유지합니다:

```cpp
VideoExportOptions movie;
movie.container = "mp4";
movie.codec = "libx264";
movie.pixelFormat = "yuv420p";
movie.matteArgb = 0xff000000U;
auto exported = exportVideo(document, "/art/preview.mp4", movie);
// exported.ok()를 확인하고 exported.warnings를 알린다.
```

`BitmapCodecTest`, `VectorCodecTest`, `VideoCodecTest` 및 설치된 패키지 소비자는 실제 바이트/파일을 연습하고 독립적으로 구성된 픽스처 를 수행합니다. 검증 호스트는 Qt 6.8.3 와 FFmpeg 9.0.1 를 사용하며; 기능 게이트 테스트는 동일한 선택 가능한 코덱이 소비자의 머신에 존재하는지 확인하지 않습니다.

[DEPENDENCIES .md](DEPENDENCIES.md) 에서 런타임 패키징과 라이선스에 대해 확인하세요.

<a id="editable-editor-timelines"></a>

## 편집 가능한 편집기 타임라인

`exportTimelineInterchange`와 `iisc-export-timeline`는 전체 캔버스 타임라인을 레거시 XML·FCPXML·독립 레이어 상태 PNG의 새 패키지로 내보낸다. 레이어 순서·이름·hold 키 컷·수명·가시성·합성는 계속 독립적으로 표현한다. 이는 평탄화한 `exportVideo` 작업이나 프레임0의 PSD 투영이 아니다. 네이티브 스냅샷은 `source.iisc`로 포함한다. 정확한 경계·리소스 제한·원본 보존·편집기 가져오기 지침은 [TIMELINE_INTERCHANGE.md](TIMELINE_INTERCHANGE.md)를 참고한다.

<a id="audio-wav"></a>

## 오디오 WAV

`importAudioWav` 와 `decodeAudioWav` 는 PCM16 모노/스테레오 자산을 8000..192000 Hz 로 분리된 서명 상태로 반환합니다. `encodeAudioWav` 와 `exportAudioWav` 은 샘플을 정확히 유지하며; 내보내는 것은 충돌을 거부하고 원자적으로 게시합니다. RIFF 데이터 할당 전에 PCM chunk 크기, 형식 일관성, 프레임 정렬 및 구성된 입력/해독/출력 한도가 확인됩니다. 부수적인 섹션은 경고와 함께 생략됩니다; 압축된 WAV, RF64, 24/32비트 및 부동소수점 샘플은 지원되지 않습니다. 작업 중인 문서에 오디오를 첨부하려면 `DocumentEditor` 또는 `DocumentFile::edit` 를 사용하세요. [오디오 타임라인](AUDIO_TIMELINE.md) 에서 XML 내보내기 및 의도된 경계를 확인하세요.

<a id="native-video-assets-and-motion-0110"></a>

## 네이티브 비디오 자산 및 모션 ( 0.11.0 )

`importVideoAsset(path, assetId, options)` 는 단일 소유된 일정한 비율 비디오 자산으로 `VideoAssetImportResult` 를 반환합니다. 그것은 `importVideo` limits, trimming, backend policy, cancellation 및 conversion warnings 을 재사용하며, `importVideo` 자체는 기존 비트맵 -key 반환 모델을 유지합니다. `exportVideo` 샘플은 네이티브 `VideoLayer` 프레임을 추출하고, 모든 시각적 레이어의 motion keys 를 canonical renderer 를 통해 통과시킵니다. [CANVAS_MEDIA .md](CANVAS_MEDIA.md)를 확인하세요. PSD 와 타임라인 XML / FCPXML 익스포터는 `UnsupportedFeature` 가 정의될 때까지 네이티브 비디오/모션을 거부하며, 부분적인 출력은 게시되지 않습니다.

PDF 내보내기는 또한 각 요청된 프레임에서 네이티브 비디오와 레이어 움직임을 평가합니다. 불투명한 벡터 기하학은 벡터 출력으로 유지되며, 비디오는 선택된 소유 래스터 프레임에 기여합니다. 기존의 명시적인 블렌드 모드 대체 경로와 그룹 불투명도 래스터화 규칙이 여전히 적용됩니다. PDF 래스터 임베딩은 비디오 프레임을 포함한 무손실 이미지 압축을 요청하여 PDF 백엔드의 기본 JPEG 인코딩을 통해 소스 색상이 변경되는 것을 방지합니다.

## Artboard output

`exportBitmapFrame` and `exportVideo` flatten `documentViewRegion` using its actual
workspace extent. Use `renderArtboard` followed by `exportBitmap` for a selected
board image. PSD, PDF and layered timeline exporters currently reject artboard
documents with `UnsupportedFeature`, preserving ownership rather than losing it
silently. Native `.iisc` remains the editable multi-artboard format.
