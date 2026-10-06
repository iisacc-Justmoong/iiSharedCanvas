<a id="iisharedcanvas-data-and-mutation-api"></a>

# iiSharedCanvas 데이터 및 돌연변이 API

## Raster presentation (0.25.0)

`FrameRenderTileRequest::sampling` and the five-argument `renderFrameRegion` select
`RasterSampling::Nearest` or `RasterSampling::Smooth`. The existing overload retains
nearest sampling. `CanvasItem::smoothRendering` defaults to true and is independent
of document persistence. Display LOD uses the mounted window's physical pixel ratio.
See [Raster presentation quality](RASTER_SAMPLING.md) for filtering and validation.

iiSharedCanvas 는 C++23 집합형 타입으로 자신의 표준 캔버스 데이터를 노출하고, 수동으로 교차 참조 불변량을 유지하지 않는 호출자를 위해 검증된 편집기를 추가합니다. 집합형은 직렬화된 진실이며, 편집기는 동일한 데이터에 대한 편의 및 안전 경계로, 두 번째 모델이 아닙니다.

<a id="native-video-and-motion-graphics"></a>

## 네이티브 비디오 및 모션 그래픽

`Document/CanvasSampling.h` 는 모든 시각적 레이어 타입을 디코딩 없이 평가합니다. `VideoAsset` /  `VideoLayer` ,  `VideoPlayback` ,  `MotionValue` 와  `MotionKeyframe` 는  `Document/Document.h` 의 공개 집합형입니다. `findVideoAsset` 와 `findVideoLayer` 는 타입화된 값을 노출합니다. `sampleLayerAt` 는 가시성, 합성된 아핀 변환 및 곱해진 불투명도를 반환하며, `videoFrameIndexAt` /  `resolveVideoFrameAt` 는 선택된 소유 프레임 또는 빈 결과를 반환합니다. `DocumentEditor::insertVideoAsset` ,  `replaceVideoAsset` ,  `setVideoPlayback` 와  `setLayerMotion` 는 무작위 수정 보존이 없는 검증된 트랜잭션 편집입니다. `setLayerMotion(id, {})` 는 애니메이션을 지웁니다. `importVideoAsset(path, id, options)` 는 `VideoAssetImportResult` 를 반환하며, `importVideo` 는 비트맵 비트맵 -키 동작을 유지합니다. 정확한 타이밍 및 변환 의미는  [CANVAS_MEDIA .md](CANVAS_MEDIA.md) 를 참조하십시오. PSD 와 타임라인 XML 어댑터는 `UnsupportedFeature` 로 새로운 네이티브 비디오/모션을 거부하며, `.iisc` 와 `exportVideo` 는 혼합 캔버스를 지원합니다.

<a id="media-import-and-export"></a>

## 미디어 가져오기 및 내보내기

`Bitmap/BitmapCodec.h` ,  `Vector/VectorCodec.h` ,  `Video/VideoCodec.h` ,  `Layered/LayeredDocumentCodec.h` 와  `Media/MediaIo.h` 는 우산 헤더에 의해 내보내지고 설치 패키지에 설치됩니다. 옵션과 결과는 인라인 `ok()` 검사기를 가진 공개 집합형입니다. `MediaIoResult` 는 유효하지 않은 데이터/옵션, 지원되지 않는 기능, 누락된 런타임 런타임 의존성, 제한, 충돌, I/O 실패, 취소 및 시간 초과를 구별하며, `warnings` 는 성공적이지만 손실 있는 변환을 설명합니다.

비트맵 비트맵 바이트/파일 리더는 `BitmapImportResult::asset` ( `RasterAsset` ), 감지된 형식 및 텍스트 캐리어를 반환합니다. SVG 리더는 `VectorImportResult::asset` ( `VectorAsset` )을 반환합니다. 비디오 가져오기는 하나의 타입화된 비트맵 레이어와 프레임 소유 키를 포함한 완전한 `MediaDocumentResult::document` 을 반환합니다. 아무것도 기존 문서를 변형하지 않으며, 기존 `DocumentFile::edit` 경계 내부에 반환된 값을 커밋합니다. 바이트 내보내기는 `MediaBytesResult` 을 반환하며, 파일 내보내기는 `MediaIoResult` 을 반환하고 명시적인 `overwrite` 과 작업 파일 보호만 제공합니다.

레이어화된 문서 리더 `decodeLayeredDocument` (바이트) 와 `importLayeredDocument` (로컬 경로) 는 `LayeredDocumentImportResult` : `document` , `format` 와 `result` 를 반환합니다. `layeredDocumentFormats()` 는 평평하게 import 된 비트맵 에서 독립적으로 레이어를 보존하는 ORA 와 PSD 서브셋 리더를 나열합니다. 각 소스 픽셀 레이어는 자체 편집 가능한 속성을 가진 네이티브 `StaticBitmapLayer` 와 `RasterAsset` 가 됩니다. 실패는 부분 문서를 노출하거나 아무런 알림 없이 로 병합된 이미지로 전환하는 것을 결코 허용하지 않습니다. 옵션은 결정론적인 `idPrefix` , 미디어 제한, `maxLayers` 와 `maxArchiveEntries` 를 소유하며, 기본값은 추가 백엔드 설정이 필요하지 않습니다. 소스 문서는 그대로 유지되며, 기존 작업 파일은 `DocumentFile::edit` 를 통해만 변경됩니다.

`encodePsd(document, options)` 는 PSD 바이트를 반환하며, `exportPsd(document, path, options)` 는 로컬 PSD 를 원자적으로 씁니다. `PsdExportOptions` 는 `overwrite` (거짓), `limits` 와 `maxLayers` ( 4096 )을 소유합니다. 모두 항상 네이티브 프레임 0 를 샘플링하며, 프레임 선택 옵션은 고의적으로 없습니다. 벡터 레이어는 내장 벡터 PDF 스마트 오브젝트로 변환되어 픽셀 캐시가 생성됩니다. 정수 번역된 비트맵 레이어는 원래 픽셀과 오프셋을 그대로 유지하며, 캔버스 밖 픽셀도 포함됩니다. 기타 비트맵 변환과 분할 비트맵은 캔버스 절단 픽셀 레이어에 렌더링됩니다. 레이어 이름/순서, 가시성, 불투명도 및 4 네이티브 블렌드 모드는 PSD 정밀도로 유지됩니다. 애니메이션과 뷰포트 손실은 네이티브 문서를 변경하지 않고 경고만 반환합니다. 이 API 는 `Document::timeline` 에 관한 것이며, 별도의 오디오/비디오 `TimelineProject` 모델이 아닙니다. PSD 리더는 엄격한 픽셀 레이어 하위 집합으로 남아 있으며, 이러한 스마트 오브젝트를 다시 가져오지 않습니다. [PSD_EXPORT .md](PSD_EXPORT.md) 에서 정확한 범위와 의미를 확인하세요.

전체 형식 제한, 옵션 및 사용 예: [MEDIA_IO.md](MEDIA_IO.md).

`Timeline/TimelineInterchange.h` 는 `exportTimelineInterchange(document, directory, options)` 와 집계 `TimelineInterchangeOptions` :  `sequenceName` ,  `limits` ,  `maxLayers` ,  `maxClips` 를 노출하며, `MediaIoResult` 를 반환하고 2 XML 타임라인이 포함된 새 디렉토리를 원자적으로 게시합니다. 모든 네이티브 유지 간격과 레이어 수명은 비활성화된 숨겨진 레이어를 포함하여 독립적으로 편집 가능한 클립으로 변환됩니다. 기존 패키지를 덮어쓰거나 소스를 수정하지 않습니다. 확인하세요.
변환 경계의 경우 [TIMELINE_INTERCHANGE.md](TIMELINE_INTERCHANGE.md)입니다.

<a id="working-file-authoring"></a>

## 작업 파일 작성

`DocumentFile` 는 커밋된 캔버스를 소유하며 `const Document *` 만 노출합니다. `create(path, document, limits)` 는 기존 경로를 거부하며, `open(path, limits)` 는 작업 파일을 유효성 검사합니다. 각 편집자의 `bind(DocumentFile &)` 오버로드는 승인된 편집을 즉시 작성합니다. `DocumentFile::edit` 는 사용자 정의 집계 변경에 대한 유효성 검사된 원자 콜백을 제공합니다. 닫기나 새로고침은 저장 작업을 수행하지 않습니다. `DocumentFileResult` 는 거부, I/O, 충돌, 스키마 및 제한 오류를 보고하며, `lastWriteStatistics` 는 논리적 증분 쓰기 횟수를 노출합니다.

`DocumentEditor` 와 `CanvasItem` 은 파일 바인딩된 경우 그들의 변형 가능한 `document()` 오버로드로부터 의도적으로 null 을 반환합니다. 뷰를 위해 const 오버로드를 사용하거나, `file.edit` 또는 파일 바인딩 편집기를 사용하여 변형하십시오. 독립적인 집계 API 는 여전히 메모리에서 작동합니다. 새로운 파일 바인딩 오버로드, `CanvasItem::createFile`, `openFile`, `filePath`, 수명 주기 규칙, 스트로크/실패 취소 동작, 및 레거시 임포트는 [PERSISTENCE .md](PERSISTENCE.md)에 지정되어 있습니다.

<a id="camera-raw-authoring-objects"></a>

## 카메라 RAW 저작 객체

`Camera/CameraRaw.h`는 수입측 형식 중립 모델입니다. `Document`에 세 번째 콘텐츠 종류를 추가하지 않으며 제조업체 RAW 파일이 이러한 집계가 공통 센서 데이터를 나타낼 수 있다는 이유만으로 디코딩되었다고 주장하지 않습니다.

|데이터|공개 필드|의미|
| --- | --- | --- |
| `CameraRawData` | `image`, `color`, `camera`, `lens`, `capture` |1 디코딩됨 RAW 페이로드 및 해당 처리 메타데이터|
| `CameraRawSensorImage` | `kind`, `extent`, `bitsPerSample`, `samplesPerPixel`, `samples`, `orientation`, `activeArea`, `defaultCrop`, `colorChannels`, `cfaPattern`, `blackLevel`, `whiteLevel` |정수 센서 코드 및 공간/샘플 해석|
| `CameraRawCfaPattern` | `columns`, `rows`, `channelIndices` |`colorChannels`에 대한 행-열 반복 참조|
| `CameraRawLevelPattern` | `columns`, `rows`, `values` |행-열-평면 순서로 0-light 값을 반복|
| `CameraRawColorProfile` | `asShotNeutral`, `calibrations` |옵션 캡처 화이트 밸런스 및 XYZ-카메라 매트릭스|
| `CameraRawColorCalibration` | `illuminant`, `xyzToCamera` |1행 주요 `colorChannels × 3` 매트릭스 및 해당 발광 라벨|
| `CameraRawCameraMetadata` | `manufacturer`, `model`, `uniqueModel`, `serialNumber` |제조업체별 태그가 없는 카메라 신원|
| `CameraRawLensMetadata` |식별 및 초점 및 F값 제한|렌즈 식별 및 선택적 포지티브 광학 범위|
| `CameraRawCaptureMetadata` |노출, F값, ISO, 초점 거리, 초점 거리, 노출 보정|캡처된 이미지에 대한 선택적 노출 시간 설정|

샘플은 하나부터 32 유효한 비트까지의 부호 없는 정수이며, 행-픽셀-평면 인터리빙을 사용합니다. CFA 또는 단색 이미지는 픽셀당 하나의 샘플을 가지며, 선형 RAW 이미지는 각 색상 채널마다 하나의 인터리빙 평면을 가집니다. 활성 영역과 자르는 영역은 절대 센서 좌표를 사용합니다. 제외된 경우, 활성 영역은 전체 센서이고 자르는 영역은 활성 영역입니다. CFA 및 블랙 레벨 패턴 기원은 활성 영역의 기원입니다. 제외된 블랙 레벨은 영을 의미하고, 제외된 화이트 레벨은 선언된 비트 깊이의 최대 정수 코드를 의미합니다.

`cameraRawSampleAt`는 저장된 센서 코드를 읽습니다. `cameraRawChannelIndexAt`는 CFA 셀 또는 선형 평면을 해당 인덱스 채널에 매핑합니다. `cameraRawBlackLevelAt` 및 `cameraRawWhiteLevelAt`는 처리 범위를 적용하지 않고 노출합니다. 범위를 벗어난 좌표, 잘못된 패턴 또는 잘못된 평면은 `std::nullopt`를 반환합니다.

`validateCameraRaw` 는 RAW 카메라 집합을 `validate(Document)` 에서 독립적으로 유효성 검사합니다. 그것은 크기 곱셈 또는 샘플 수 불일치, 선언된 비트 깊이를 초과하는 샘플, 유효하지 않은 활성/자르는 영역, 불일치한 CFA /채널 맵, 형식화된 블랙과 화이트 레벨, 음이 아닌 아샷 중립 값, 유한하지 않거나 잘못 크기의 색상 행렬, 반전 렌즈 범위, 및 유효하지 않은 노출 메타데이터를 거부합니다. 유효성 검사는 절대 디모자이크, 정규화, 색상 변환, 렌더링, 또는 소스 값을 변형하지 않습니다.

`CameraRawData` 은 `.iisc` 버전으로 인코딩되지 않으며 1.1 버전을 통해만 가져오기 전용으로 유지되고 1.3버전까지 유지됩니다. 캔버스 비트맵 를 원호출자가 원하면 선택한 어댑터를 통해 파일을 명시적으로 디코딩하고, 집계 결과를 검증하며, 선택한 RAW 처리를 수행한 후 결과 ARGB 픽셀을 `RasterAsset` 로 커밋해야 합니다. 집계가 벡터와 문자열을 소유하지만 내부 동기화를 제공하지 않으므로, 한 소유자가 변형을 조정해야 하며 다른 스레드는 이를 읽습니다.

<a id="stable-diffusion-generation-metadata"></a>

## 안정적인 확산 생성 메타데이터

`Metadata/StableDiffusionMetadata.h`는 선택적 문서 수준 생성 레시피를 정의합니다. 이는 출처 및 상호 운용성을 위한 메타데이터이지 제3의 계층 유형이나 추론 또는 모델 로딩 인터페이스가 아닙니다.

|데이터|공개 필드|의미|
| --- | --- | --- |
| `StableDiffusionMetadata` |프롬프트, 출력 설정, `samplingPasses`, `models`, `loras`, 소프트웨어/출처 문자열, `generationParametersText`, `comfyUi`, `extraParameters`|하나의 완전한 생성 또는 개선 레시피|
| `StableDiffusionSamplingPass` | `nodeId`, `seed`, `steps`, `cfgScale`, `samplerName`, `scheduler`, `denoiseStrength`, `startStep`, `endStep` |하나의 독립적으로 식별된 샘플러 호출; 여러 패스가 보존됩니다.|
| `StableDiffusionModelResource` | `role`, `name`, `hash`, `hashType`, `uri` |체크포인트, VAE, ControlNet, 또는 내장된 모델 바이트가 없는 기타 명명된 모델 참조|
| `StableDiffusionLora` | `name`, `hash`, `modelStrength`, `clipStrength` |하나의 LoRA와 그 2 적용 강점|
| `ComfyUiMetadata` | `promptJson`, `workflowJson`, `extraPngInfo` |정확한 API 실행 그래프, UI-복원 그래프 및 확장 JSON|
| `StableDiffusionMetadataEntry` | `key`, `value` |주문된 확장 값; ComfyUI 값은 JSON이고 일반 값은 불투명합니다. UTF-8|
| `StableDiffusionGenerationParameters` |`rawText`, 프롬프트, 주문됨 `parameters`|무손실 안정 확산 생성 텍스트 및 디코딩된 키/값 보기|
| `StableDiffusionGenerationParametersParseResult` |`generationParameters`, 공통 `metadata`, 유형 `issues`|안전하게 거부하는 호환성 읽기 및 재사용 가능한 공통 프로젝션|

`parseStableDiffusionGenerationParameters` 는 호스트가 PNG `parameters` 에서 추출한 후 UTF-8 인포텍스트를 EXIF `UserComment` 또는 다른 이미지 캐리어에서 수락합니다. 이미지 파일을 열지 않습니다. 파서는 상위 공급 측 터미널 라인 규칙을 따릅니다: 3 개 이상의 키/값 필드가 파라미터 라인을 다른 프롬프트 라인에서 구별합니다. 프롬프트 라인을 잘라내고 `Negative prompt:` 에서 분할하며, JSON -따로따로 인용된 파라미터 문자열을 디코딩하고, 원본 소스와 순서쌍을 보존합니다. 중복 쌍은 `StableDiffusionGenerationParameters::parameters` 에 유지되며, 타입 변환과 `findStableDiffusionGenerationParameter` 는 최종 발생을 사용합니다.

일반 투영은 `stable-diffusion.main` 를 사용하며, 적용 가능한 경우 `stable-diffusion.hires` 샘플링 ID 를 사용합니다. 누락된 일정과 CLIP 스킵 값은 상위 공급 측 기본값 `Automatic` 와 1로 매핑됩니다. 오래된 Hires 기록은 메인 샘플러와 스케줄러를 상속하며, Hires 단계 값이 0 일 경우 메인 단계 수를 재사용합니다. `Size`, `Batch size`, 숫자 샘플러 필드, 체크포인트, VAE, Hires 체크포인트, 리파이너, 그리고 `Version` 는 해당 타입 동등값으로 매핑됩니다. 현재 10-16 진법 체크포인트/ VAE 해시는 `sha256-prefix-10` 를 사용하며, 64-16 진법 해시는 `sha256` 를 사용합니다; 더 오래된 충돌에 취약한 8-자 모델 해시는 `sha256-partial-prefix-8` 로 표시되며 전체 다이제스트로 오인되지 않습니다. 모든 비프로젝션 및 미래 필드는 `extraParameters` 에 마지막 값으로 유지되며, 정렬된/원본 뷰는 모든 발생을 유지합니다.

`StableDiffusionGenerationParametersParseCode` 는 빈 또는 잘못된 UTF-8, 누락된 매개변수 줄, 잘못된 인용/키-값 구문, 잘못된 정수, 숫자, 크기 및 범위, 그리고 공통 메타데이터 계약을 위반하는 프로젝션을 구별합니다. 실패한 결과는 진단과 원본 복구용으로 검사할 수 있지만 신뢰할 수 있는 생성 레시피로 승인되어서는 안 됩니다. 성공한 결과는 항상 `validateStableDiffusionMetadata` 를 통과합니다.

파서는 구문에서 `StableDiffusionMetadata::software`를 추론하지 않습니다. AUTOMATIC1111는 호환되는 텍스트 형식에 대한 참조 구현이지만 다른 생산자가 동일한 모양을 내보낼 수 있습니다. 캐리어 어댑터는 작성자에 대한 별도의 증거가 있는 경우에만 소프트웨어 출처를 설정합니다.

ComfyUI   `promptJson`  및  `workflowJson` 는 독립적으로 선택 사항입니다. 존재할 경우, 각각은 완전한 JSON 객체여야 합니다. `.iisc` 왕복 변환 동안 정확한 문자열로 유지됩니다; 라이브러리는 객체 키를 재배열하거나 숫자를 표준화하지 않습니다. `extraPngInfo`  예약된  `prompt` 와  `workflow` 키를 거부하고, 유효한  JSON 값을 요구하며, 고유한 키를 요구합니다. 일반적인 `extraParameters` 은 비어 있지 않은 고유한 키를 필요로 하며, 타입화된 모델을 변경할 가치가 없는 애플리케이션 필드를 유지할 수 있습니다.

`validateStableDiffusionMetadata`  첨부되었지만 빈 객체를 거부하고,  0  output/batch/ CLIP  값, 빈 또는 수치적으로 유효하지 않은 샘플링 패스, 불완전한 모델 식별자, 비유한  LoRA  강도, 잘못된 형식의  ComfyUI   JSON , 및 중복 확장 키를 거부합니다. `validate(Document)`  또한 1.2보다 이전 형식에서는 이 메타데이터를 안전하게 거부합니다. JSON 및 모든 생성 메타데이터는 신뢰할 수 없는 데이터입니다: 검증은 구조를 증명할 뿐 노드, 모델 URI, 해시 또는 레시피가 안전하거나 사용 가능하다는 것을 증명하지 않습니다.

`DocumentEditor::setStableDiffusionMetadata` 는 전체 레시피를 원자적으로 유효성 검사하고 설치합니다. 편집이 성공할 때만 문서 버전을 1.2 로 업그레이드하며, 거부 시 메타데이터와 버전을 모두 복원합니다. `clearStableDiffusionMetadata` 는 물리적 포맷을 다운그레이드하지 않고 레시피를 제거합니다. 두 작업 모두 레이어, 자산, 또는 렌더링된 픽셀을 변경하지 않습니다.

<a id="video-editing-timeline-objects"></a>

## 비디오 편집 타임라인 개체

`TimelineProject` 는 독립적인 공개 집계이며 `Document` 의 구성원이 아닙니다. 프로젝트 메타데이터, `TimelineMediaSource` 엔트리를, `TimelineSequence` 엔트리를, 빈, 및 `TimelineRenderProfile` 전달 구성을 포함합니다. 미디어 소스는 여러 개의 원본, 프록시, 최적화, 또는 사용자 정의 `TimelineMediaRepresentation` 객체를 보유할 수 있습니다. 각 표현은 컨테이너 설명자와 비디오, 오디오, 자막, 또는 데이터 스트림의 `TimelineMediaStream` 변형을 소유합니다.

모든 미디어 및 편집 위치는 서명된 틱을 사용합니다. `TimelineTimeBase` 는 틱당 초를 결정하며, `TimelineFrameRate` 는 정밀한 유리수 FPS 를 독립적으로 표현합니다. `timelineTicksToSeconds` 는 확인된 편의 변환이며, 정수 틱 값과 유리수는 여전히 권위 있는 것으로 남아있습니다. 비디오 스트림은 가변 프레임률 샘플 타이밍을 보유할 수 있으며, 시퀀스, 소스, 시간 코드, 및 출력 프레임률은 의도적으로 분리됩니다.

`TimelineTrack` 는 `TimelineVideoTrack` , `TimelineAudioTrack` , `TimelineSubtitleTrack` , 및 `TimelineDataTrack` 의 변형입니다. 각 트랙은 해당 구체적 유형의 클립만 저장합니다. 일반 클립 속성은 소스 스트림, 타임라인 및 소스 범위, 재생 속도, 링크 그룹, 역할, 시간 리매핑, 효과, 자동화, 및 마커를 식별합니다. 비디오 클립은 변환 및 자르기 데이터를 추가하며, 오디오 클립은 이득, 패닝, 페이드, 및 채널 행렬을 추가합니다. 자막 클립은 텍스트 및 스타일을 추가합니다. 안정 ID 조회는 `findTimelineMediaSource` , `findTimelineMediaRepresentation` , `findTimelineMediaStream` , `findTimelineSequence` , `findTimelineTrack` , 및 `findTimelineClip` , `findTimelineRenderProfile` 에 의해 제공됩니다.

소스 범위는 참조된 미디어 스트림, 중첩된 시퀀스 또는 생성된 소스 `TimelineTimeBase` 를 사용하며, 타임라인 범위는 소유 시퀀스 시간 기준을 사용합니다. 루프가 없는 클립에 시간 맵이 없는 경우, 검증은 이러한 정확한 유리수를 비교하여 전체 지속 시간을 고려하기 위해 `playbackRate` 를 요구합니다. `timeMap` 가 존재할 경우 그것만 오프셋 0 를 통해 클립 지속 시간까지 전체 클립을 매핑하므로 `playbackRate` 는 1/1이어야 합니다. 음수 상수 비율은 역방향 재생을 모델링합니다.

클립 조회는 비소유 유형 보기를 반환합니다. 빈 뷰에는 스트림 종류가 없으며 프로젝트를 재배치할 수 있는 변형으로 인해 뷰가 무효화됩니다. 호출자는 `TimelineEditor` 커밋이 성공할 때마다 이를 다시 해결해야 합니다.

표현은 공유 스트림 ID 를 논리적 미디어 식별자로 사용합니다. 검증은 반복된 모든 ID 가 스트림 종류, 시간 기준, 시작 틱, 지속 시간을 유지하도록 요구하며, 코덱, 해상도, 픽셀 형식 및 기타 표현 세부 사항은 다를 수 있습니다. 이는 활성 프록시 선택이 변경될 때 하나의 클립 소스 범위가 안정적임을 의미합니다.

`validateTimelineProject` 는 모든 관찰된 문제를 `TimelineValidationResult` 로 반환합니다. 그것은 수치 도메인, 고유성, 참조, 스트림 종류, 클립 범위, 가변 프레임률 샘플, 자동화 곡선, 전환 및 전달 프로파일을 확인합니다. 그것은 구조적 의미만 유효성을 검사하며, 어댑터는 설치된 코덱 구현이 요청된 설정을 처리할 수 있는지 결정합니다. 2-측면 전환은 인접한 `from` 끝/ `to` 시작을 컷으로 사용하며 시작, 중앙, 끝 또는 사용자 정의 정렬을 준수해야 합니다. 한 측면 전환은 명시적인 들어오는 페이드 또는 나가는 페이드입니다. 비디오 및 오디오 클립은 각각 시퀀스 캔버스 또는 믹스 형식을 요구합니다. 알 수 없는 미디어 지속 시간은 서명 틱 오버플로우 검사를 비활성화하지 않으며, 알려진 샘플 바이트 범위는 표현 파일 크기에 맞아야 합니다. 자막 이미지 리소스 ID 는 동일한 표현 내의 첨부 파일로만 해결됩니다.

`TimelineEditor` 는 한 caller-owned 프로젝트에 바인딩되고 미디어 소스, 시퀀스, 렌더 프로파일, 타입화된 트랙, 및 타입화된 클립에 대해 stable-id CRUD 를 노출합니다. 또한 `setSequenceFrameRate`, `setRenderContainer`, `setRenderVideoCodec`, 및 `setRenderAudioCodec` 를 제공합니다. 모든 작업은 커밋하기 전에 후보 복사를 유효성 검사합니다. 거절된 편집은 `revision()` 를 절대 진행하지 않으며 프로젝트의 부분적 변형을 절대 수행하지 않습니다. 공공 집계 편집은 허용되지만, 외부에서 무효인 프로젝트는 수정되거나 재바인딩될 때까지 후속 편집자 작업을 차단합니다. 다른 무효인 프로젝트에 바인딩하는 것은 현재 유효한 바인딩이나 그 리비전을 버리지 않고 거절됩니다.

이 모듈의 어떤 기능도 미디어 열기, 컨테이너 탐색, 코덱 디코딩 또는 인코딩, 시퀀스 렌더링, 프로젝트 파일 쓰기 등을 수행하지 않습니다. `TimelineProject`는 `.iisc` 버전 1.3로 인코딩되지 않습니다.

실제 캔버스 애니메이션 탐색 및 교환은 타임라인 작성 모델이 아닌 `Video/VideoCodec.h`에서 진행됩니다.

<a id="data-ownership-and-identity"></a>

## 데이터 소유권 및 ID

`Document`는 모든 지속형 캔버스 상태를 소유합니다.

|데이터|공개 필드|의미|
| --- | --- | --- |
| `FormatVersion` | `major`, `minor` |물리적/모델 호환성 버전|
| `CanvasExtent` | `width`, `height` |양수 출력 크기(픽셀)|
| `CanvasMode` | `Finite`, `Infinite` |할당된 영역이 월드 공간으로의 경계인지 이동 가능한 창인지 여부|
| `InfiniteCanvas` | `origin`, `chunkSize` |할당된 월드 원점 및 희소 래스터 청크 차원|
| `Artboard` | `id`, `name`, `region`, `backgroundArgb`, `visible` |한 문서의 독립 대지 영역 및 배경; 중첩 소유 불가|
| `Document` | `artboards` |아래에서 위로 정렬된 대지 목록; 기존 자산 및 타임라인을 공유|
| `Timeline` | `frameRate`, `frameCount` |합리적인 재생 속도 및 정수 프레임 도메인|
| `StableDiffusionMetadata` | `Document::stableDiffusionMetadata` |선택적 형식-1.2 생성 레시피 및 ComfyUI 호환 페이로드|
| `RasterAsset` | `id`, `pixels` |안정적인 ID 및 iiPaintEngine ARGB 픽셀 저장소|
| `ChunkedRasterAsset` | `id`, `chunks` |무한 캔버스를 위한 안정적인 ID 및 표준 희소 래스터 청크|
| `VectorAsset` | `id`, `viewport`, `paths` |안정적인 ID 및 네이티브 벡터 페인트 데이터|
| `VideoAsset` | `id`, `frameRate`, `frames` |소유 고정 속도 ARGB 비디오 프레임|
| `MotionValue` | `position`, `scale`, `anchor`, `rotationDegrees`, `opacity` |편집 가능한 로컬 변환 및 불투명도 승수|
| `MotionKeyframe` | `frame`, `value`, `interpolation` |절대 정수 프레임 속성 키|
| `VectorPath` | `commands`, `fill`, `stroke` |M/L/Q/C/Z 기하학 및 고체 페인트|
| `LayerFrameRange` | `firstFrame`, `lastFrame` |선택적 포함 레이어 존재 경계|
| `LayerProperties` | `id`, `name`, `visible`, `opacity`, `transform`, `blendMode`, `frameRange`, `motion`, `artboardId` |모든 시각적 계층 유형에서 공유되는 프레젠테이션, 타임라인 존재 상태 및 선택적 대지 소속|
| `StaticBitmapLayer` | `properties`, `content: StaticBitmapContent` |단일 래스터 자산|
| `DynamicBitmapLayer` | `properties`, `content: DynamicBitmapContent` |프레임별 래스터 자산|
| `StaticVectorLayer` | `properties`, `content: StaticVectorContent` |단일 네이티브 벡터 자산|
| `DynamicVectorLayer` | `properties`, `content: DynamicVectorContent` |프레임별 네이티브 벡터 자산|
| `VideoLayer` | `properties`, `source`, `playback` |1개의 비디오 소스 및 1개의 소스 트림|
| `VideoPlayback` | `sourceInFrame`, `sourceOutFrame`, `endBehavior` |반 오픈 소스 범위 및 투명/유지 동작|
| `Layer` | `StaticBitmapLayer \| StaticVectorLayer \| DynamicBitmapLayer \| DynamicVectorLayer` |유형별 하단에서 상단까지 합성 항목|
| `StaticSource` | `assetId` |하나의 내구성 있는 자산 참조|
| `KeyframedSource` | `frameIndices` |이 레이어의 키를 소유하는 정확한 프레임들의 파생 증가 인덱스; 해당 레이어는 `Keyframe`를 소유하지 않습니다.|
| `Frame` | `index`, `keyframes` |해당 위치의 모든 키를 직접 소유하는 희소 정수 프레임|
| `Keyframe` | `layerId`, `assetId` |한 대가 소유한 레이어-자산 스위치 `Frame`|
| `Document` | `frames` |빈 컬렉션일 수 있습니다. 저장된 모든 프레임은 비어 있지 않으며 인덱스는 엄격하게 증가합니다|

`Frame::keyframes`는 표준 오름차순 `layerId` 순서를 사용합니다. `layerId`로 키 주소를 지정하세요. `.iisc`는 레이어 주요 와이어에서 문서 레이어 순서를 유지하면서 이 표준 메모리 내 순서로 동시 키를 재구성합니다. 프레임 인덱스와 `(layerId, assetId)` 매핑은 지속 가능한 값입니다.

`KeyframedSource::frameIndices` 는 한계가 설정된 hold 조회에 사용되는 파생된 2 차 인덱스입니다. 그것은 엄격히 증가해야 하며 0에서 시작하고 해당 레이어 id 를 포함하는 `Document::frames` 엔트리의 정확한 집합과 같아야 합니다. 그것은 자산 참조를 복제하지 않으며 레이어에 키프레임 소유권을 부여하지 않습니다. 직접적인 집계 변형은 양쪽을 동기화한 다음 `validate` 를 호출해야 하며, `DocumentEditor` 와 `decodeIisc` 는 인덱스를 자동으로 유지합니다.

`LayerProperties::frameRange` 는 부재이거나 `LayerFrameRange` 에 포함됩니다. 부재인 값은 레이어가 현재 문서 타임라인 전체에 존재한다는 것을 의미합니다. 존재하는 값은 1.3형식을 요구하며, `firstFrame <= lastFrame < timeline.frameCount` 를 만족하고 두 경계 프레임을 포함해야 합니다. 범위는 조회 및 렌더링 동안 레이어의 존재를 제어하며, 범위 밖의 프레임 소유 키프레임 를 삭제하거나 무효화하지 않습니다.

자산 및 레이어 id 는 안정적인 식별자입니다. 벡터 인덱스는 저장 또는 페인트 순서이며 삽입, 제거, 또는 이동 후 변경될 수 있습니다. 컬렉션 변형을 가로지러 요소 포인터를 유지하지 마십시오; 다시 안정적인 id 를 해결하십시오. `BitmapEditor` 는 이미 이 규칙을 따르며 매번 바인딩된 래스터 자산 id 를 해결합니다.

`Frame` 와 `Keyframe` 포인터는 또한 `Document::frames` 에 대한 뷰입니다. 모든 프레임/ 키프레임 삽입, 이동, 제거, 소스 변환, 또는 레이어 제거는 해당 뷰들을 무효화할 수 있습니다. 성공적인 구조적 편집 후 정확한 `FrameIndex` 로 프레임을 다시 획득한 다음 안정된 `layerId` 로 키를 다시 획득해야 합니다. `AssetReference::frameIndex` 는 영속화된 프레임 번호가 아닌 저장 인덱스입니다.

`RasterAsset` 는 유한 문서의 연속적인 이미지/픽셀 자산이며, `ChunkedRasterAsset` 는 무한 문서의 희소 이미지/픽셀 자산이고, `VectorAsset` 는 해당 자산의 네이티브 형상 자산입니다. 이 이름들은 가져오기 애플리케이션의 파일 형식이 아닌 영속화된 표현을 설명합니다. 해독된 PNG, JPEG, 또는 브러시 결과는 따라서 정준 `RasterLayer` 차원과 ARGB 픽셀을 노출하며, `.iisc` 는 소스 코덱 바이트나 브러시 궤적을 유지하지 않습니다. 형상은 뷰포트, 순서 있는 경로, 모든 M/L/Q/C/Z 명령 및 제어점, 채우기 색상, 스트로크 색상, 그리고 스트로크 너비를 평탄화 없이 노출합니다.

<a id="detailed-document-traversal"></a>

## 상세한 문서 순회

`decodeIisc`는 작성에 사용된 것과 동일한 공개 집계 모델을 반환합니다. 불투명한 문서 핸들이나 축소된 요약을 반환하지 않습니다. 호출자는 직렬화된 모든 레이어, 자산, 경로, 픽셀, 소스 참조 및 키프레임를 검사할 수 있습니다.

~~~cpp
const IiscDecodeResult decoded = decodeIisc(bytes);
if (!decoded.ok()) {
    // decoded.error.code, offset과 message를 알린다.
}

for (const Layer &layer : decoded.document.layers) {
    const LayerProperties &properties = layerProperties(layer);
    const std::string &id = properties.id;
    const double opacity = properties.opacity;
    const AffineTransform &transform = properties.transform;

    if (const auto *source = staticLayerSource(layer)) {
        const Asset *asset = findAsset(decoded.document, source->assetId);
        if (std::holds_alternative<StaticBitmapLayer>(layer)) {
            if (const auto *image = asset ? std::get_if<RasterAsset>(asset) : nullptr) {
                const std::int32_t width = image->pixels.width;
                const std::vector<std::uint32_t> &argb = image->pixels.pixels;
            } else if (const auto *sparse = asset
                           ? std::get_if<ChunkedRasterAsset>(asset)
                           : nullptr) {
                const std::vector<RasterChunk> &chunks = sparse->chunks;
            }
        } else if (const auto *shape = asset ? std::get_if<VectorAsset>(asset) : nullptr) {
            for (const VectorPath &path : shape->paths) {
                const std::vector<PathCommand> &commands = path.commands;
                const std::optional<SolidPaint> &fill = path.fill;
                const std::optional<StrokeStyle> &stroke = path.stroke;
            }
        }
    }
}

for (const Frame &frame : decoded.document.frames) {
    for (const Keyframe &keyframe : frame.keyframes) {
        const Layer *layer = findLayer(decoded.document, keyframe.layerId);
        const Asset *asset = findAsset(decoded.document, keyframe.assetId);
    }
}
~~~

`FrameRenderResult::ok()`, `IiscEncodeResult::ok()` 및 `IiscDecodeResult::ok()`는 인라인 집계 검사기입니다. 해당 성공 테스트는 별도의 내보낸 멤버 ABI를 추가하지 않고도 Windows DLL 빌드를 포함하여 정적 및 공유 라이브러리 소비자에게 동일하게 제공됩니다.

위 참조들은 `decoded.document` 에 대한 뷰이며, 소유하는 컬렉션이 구조적으로 변경될 때까지만 유효합니다. 직접적인 집계 변형은 허용되지만, 호출자는 렌더링 또는 재인코딩 전에 `validate(decoded.document)` 를 실행해야 합니다. `DocumentEditor` 는 편집이 자동으로 교차 참조 불변량을 보존해야 할 때 원자적인 대안입니다.

<a id="lookup-api"></a>

## API 조회

`Document/Document.h`는 해당되는 경우 변경 가능 및 const 오버로드를 노출합니다.

- `findAsset`, `findRasterAsset` 및 `findVectorAsset`는 안정적인 자산 ID를 확인합니다.
- `findChunkedRasterAsset`는 희소한 래스터 정체성을 해석하고, `findRasterChunk`는 서명된 `(column, row)` 좌표를 해석합니다;
- `canvasOrigin`와 `canvasRegion`는 하나의 쿼리를 통해 0-origin 유한 기하학 또는 현재 할당된 무한 세계 영역을 노출합니다;
- `assetIndex`는 자산의 현재 저장 위치를 노출합니다.
- `findLayer` 및 `layerIndex`는 레이어 ID와 아래쪽에서 위쪽 위치를 확인합니다.
- `findStaticBitmapLayer`, `findStaticVectorLayer`, `findDynamicBitmapLayer`, `findDynamicVectorLayer`는 정확히 해당 타입만 반환한다.
- `layerProperties`는 공유 속성을 빌리며, `layerSource`는 분리된 소스 사본을 반환한다. `staticLayerSource`와 `keyframedLayerSource`는 콘텐츠의 참조/인덱스를 빌린다;
- `layerExistsAt`는 선택적 포함 범위를 적용한 후 타임라인 프레임 내에서 레이어가 존재하는지 여부를 보고합니다;
- `findFrame` 및 `frameIndex`는 정확한 희소 프레임 레코드를 확인합니다.
- `findKeyframe(Frame, layerId)`와 `keyframeIndex`는 직접 프레임 소유권을 검사하고, `findKeyframe(Document, layerId, frame)`는 두 개의 정확한 조회를 결합합니다;
- `resolveAssetAt`는 키프레임이 적용된 소스의 파생된 소유자 프레임 인덱스와 정확한 프레임/키 바이너리 조회를 통해 타임라인 보류 샘플링을 수행합니다;
- `assetReferences`는 모든 참조 레이어와 선택적 프레임 및 키프레임 저장 인덱스를 반환합니다.

이러한 도우미는 포인터 또는 `std::optional` 값을 반환하고 절대로 발생시키지 않습니다. 누락된 ID와 정확한 프레임은 null 또는 `std::nullopt`를 반환합니다.

<a id="documenteditor-lifecycle"></a>

## DocumentEditor 수명주기

`DocumentEditor`는 현재 유효한 `Document`만 바인딩합니다. 비소유 포인터를 유지하므로 문서는 편집기보다 오래 지속되어야 하며 메모리에서 이동해서는 안 됩니다. `unbind()`는 해당 관계를 해제합니다. `document()`는 고급 일괄 작업을 위해 의도적으로 동일한 집계를 노출합니다.

각 작업은 `DocumentEditResult`를 반환합니다.

- `ok()`는 적용된 편집 및 유효한 no-ops에 대해 true입니다.
- `changed`는 문서가 변경된 경우에만 true입니다.
- `code`는 조회, 유형, 참조, 인덱스, 소스, 키프레임, 입력 또는 검증 거부를 식별합니다;
- `path`는 거부된 데이터 위치를 식별합니다.
- `message`는 로컬라이제이션 키가 아니라 인간이 읽을 수 있는 진단이며, 메시지 텍스트가 아니라 `code`에 분기됩니다.

거절된 편집은 `revision()` 를 절대 진행시키지 않으며 부분적인 변경을 남기지 않습니다. 성공적인 변경은 이를 정확히 한 번만 진행시킵니다. 유효한 무작업은 진행시키지 않고 성공합니다. 모든 작업 전에 편집기는 문서를 무효화하게 만든 직접적인 외부 변경을 감지하고, `InvalidDocument` 로 추가적인 변형을 거부합니다. 다른 무효화된 문서를 바인딩하는 것도 현재 유효한 바인딩이나 해당 리버시를 버리지 않고 거부됩니다.

<a id="document-and-timeline-operations"></a>

## 문서 및 타임라인 작업

|방법|작업|
| --- | --- |
| `setCanvasExtent` |양의 출력 범위를 교체하십시오; 이는 자산을 재현하지 않습니다.|
| `ensureInfiniteCanvasRegion` |요청된 세계 영역을 무한 캔버스로 통합하고 청크 경계에 맞게 성장을 정렬합니다|
| `setFrameRate` |비0 유리 프레임 속도를 교체하십시오.|
| `setFrameCount` |정수 프레임 도메인의 크기를 조정하고, 키프레임 또는 명시적인 레이어 경계를 제외하는 축소를 거부합니다.|
| `setStableDiffusionMetadata` |전체 레시피를 검증하고 원자적으로 부착하여 레거시 형식을 1.2로 업그레이드하십시오.|
| `clearStableDiffusionMetadata` |렌더링 콘텐츠를 변경하거나 형식을 다운그레이드하지 않고 선택적 레시피를 제거합니다.|

<a id="asset-operations"></a>

## 자산 운영

|방법|작업|
| --- | --- |
| `insertRasterAsset` |ID를 입력하고 인덱스 또는 `AppendDocumentIndex`에서 `RasterLayer`를 완성하십시오.|
| `insertVectorAsset` |ID인 뷰포트를 삽입하고, 정렬된 경로 컬렉션을 입력하십시오.|
| `replaceRasterPixels` |래스터 치수와 ARGB 저장소를 원자적으로 교체합니다.|
| `replaceVectorData` |벡터 뷰포트와 모든 경로를 원자적으로 교체합니다|
| `renameAsset` |ID의 이름을 바꾸고 모든 static/keyframe 참조를 다시 작성하십시오.|
| `moveAsset` |ID 변경 없이 저장 순서 변경 또는 순서 렌더링|
| `removeAsset` |참조되지 않은 자산만 제거합니다. 레이어로 계단식으로 배열되지 않음|

<a id="layer-operations"></a>

## 레이어 작업

|방법|작업|
| --- | --- |
| `insertLayer` |아래에서 위로 가는 인덱스에 전체 레이어를 삽입하십시오.|
| `insertKeyframedLayer` |키프레임이 적용된 레이어와 모든 `KeyframePlacement` 값을 원자적으로 하나의 하단‐위 인덱스에 삽입하십시오.|
| `replaceLayer` |하나의 레이어를 원자적으로 교체하고 모든 참조의 유효성을 검사합니다.|
| `renameLayer` |안정적인 레이어 ID 교체|
| `setLayerName` |사용자에게 보이는 이름을 교체하십시오.|
| `setLayerVisible` |렌더링 참여 전환|
| `setLayerOpacity` |0에서 1까지의 유한 불투명도 설정|
| `setLayerTransform` |완전 유한 아핀 변환을 교체하십시오.|
| `setLayerBlendMode` |지원되는 문서 선택 합성 모드|
| `setLayerFrameRange` |선택적 포함 존재 범위를 설정하거나 초기화하고, 커밋된 범위를 1.3형식으로 업그레이드합니다.|
| `setStaticSource` |소스를 하나의 자산 참조로 교체|
| `setKeyframedSource` |레이어에 애니메이션을 표시하고 `KeyframePlacement` 입력을 자체 희소 프레임에 원자적으로 배포합니다.|
| `moveLayer` |아래에서 위로 변경 합성 순서|
| `removeLayer` |자산을 삭제하지 않고 레이어 제거|

<a id="keyframe-operations"></a>

## 키프레임 작업

`insertKeyframe`, `setKeyframeAsset`, `moveKeyframe`, 및 `removeKeyframe` 는 안정적인 레이어 ID 와 정확한 프레임으로 키프레임이 적용된 키프레임 레이어를 처리합니다. 삽입은 필요에 따라 희소 `Frame` 를 생성하며, 이동은 키를 프레임 소유자 간에 전달하고, 제거는 비워진 소유자도 삭제합니다. 편집기는 한 프레임 내 중복 레이어 키, 범위 밖의 프레임, 레이어/자산 유형 불일치, 빈 키프레임이 적용된 키프레임 레이어, 그리고 필수 프레임을 제거하거나 이동시키는 모든 편집을 거부합니다 -0 키. 레이어 및 자산 이름 변경은 프레임 소유된 안정적인 참조를 다시 작성하며, 정적 소스로 전환하거나 레이어를 제거하면 해당 레이어의 키만 제거됩니다. 레이어 범위는 존재와 렌더링만 제한합니다: 키프레임 키프레임은 `firstFrame` 이전이나 `lastFrame` 이후에 남아 있을 수 있으며, 이는 후속 범위 확장 시 유지 상태와 데이터를 보존합니다.

<a id="vector-path-operations"></a>

## 벡터 경로 작업

`insertVectorPath`, `replaceVectorPath`, `moveVectorPath`, 및 `removeVectorPath` 는 안정적인 ID 로 벡터 자산을 처리합니다. 그 인덱스는 네이티브 페인트 순서입니다. 각 삽입되거나 대체된 경로는 `MoveTo` 로 시작해야 하며, 유한한 좌표를 포함하고, 필 또는 스트로크를 가지며, 스트로크가 존재할 경우 유한한 양의 스트로크 너비를 사용해야 합니다.

`VectorEditor` 는 세분화된 네이티브 기하학 경계입니다. 이는 안정적인 ID 로 하나의 `VectorAsset` 를 바인딩하고, 읽기 전용 경로 검사 기능을 노출하며, 포인터를 `Document::assets` 에 캐싱하는 대신 각 편집마다 자산을 다시 해결합니다. `createPath` 는 `MoveTo` 로 스타일화된 경로를 시작하며, `insertPath`, `movePath`, 및 `removePath` 는 네이티브 페인트 순서를 관리하고, `setViewport` 는 바인딩된 자산 뷰포트 를 다른 자산에 영향을 주지 않고 대체합니다.

`appendMoveTo` 는 다른 서브 경로를 시작합니다. `appendLineTo` 는 선형 섹션을 추가하며, `appendQuadraticBezierTo` 와 `appendCubicBezierTo` 는 각각 하나와 두 제어점을 가진 비제어 섹션을 추가합니다. `insertCommand`, `replaceCommand`, 및 `removeCommand` 는 네이티브 M/L/Q/C/Z 명령 시퀀스를 직접 처리합니다. `setAnchorPoint` 은 M/L의 점 또는 Q/C의 끝점을 편집하며, `setControlPoint` 는 2 차 명령에 인덱스 0 를, 0 또는 1 을 3 차 명령에 받습니다. `closePath` 과 `openPath` 은 꼬리 `ClosePath` 를 관리하며, `setPathPaint` 는 선택형 실 채우기와 스트로크를 함께 변경합니다.

편집기는 하나의 경로를 복사하고 제안된 변경을 적용한 후, 대체를 `DocumentEditor` 에 위임하므로 전체 문서 검증이 커밋 경계입니다. 거절된 편집은 기하학, 페인트, 및 `revision()` 를 정확히 유지하며, 유효한 멱등성 편집은 `ok() == true` 와 `changed == false` 를 반환합니다. 경계 문서가 편집기보다 오래 살아야 합니다. 경로 명령 데이터는 배치 알고리즘을 위해 `VectorPath::commands` 를 통해 직접 편집 가능하지만, 직접 편집은 렌더링 또는 인코딩 전에 `validate(document)` 를 필요로 합니다.

<a id="raster-pixel-operations"></a>

## 래스터 픽셀 작업

구조적 래스터 교체는 `DocumentEditor::replaceRasterPixels` 에 속하며, 세밀한 픽셀은 `BitmapEditor` 에 속하고 이는 `pixelAt`, `setPixel`, `replacePatch`, `replacePixels`, `clear`, 스트리밍 브러시/지우개 입력, 더티 경계, 버전, 및 픽셀 스냅샷 취소/재실행을 노출합니다. 브러시 입력은 항상 픽셀을 커밋하며 유지된 문서 기하학이 결코 되지 않습니다.

`ChunkedBitmapEditor` 는 `ChunkedRasterAsset` 의 대응하는 저작 경계를 제공합니다. 음수 좌표를 포함한 세계 좌표는 부호 있는 덩어리 좌표로 매핑됩니다. 그것은 비어 있지 않은 교체 픽셀 또는 브러시 출력에 의해 닿은 덩어리만 저장하며, 누락된 덩어리는 투명하게 읽힙니다. 브러시 제스처, 지우기, 할당된 영역 전체 교체, 더티 경계, 및 취소/재실행은 동일한 커밋된 픽셀 계약에 따릅니다. 현재 역사 정책은 희소 덩어리 집합을 스냅샷하며 입력 궤적을 절대 직렬화하지 않습니다.

`CanvasItem::selectedRasterPixels()` 는 연속된 호환성 뷰입니다. 유한한 래스터 저장소를 직접 반환하며, 희소 할당 영역을 최대 16,777,216 픽셀까지만 물질화할 수 있습니다. 해당 예산을 초과하면 null 을 반환하며, 큰 희소 호출자는 단일 할당을 강요하는 대신 `ChunkedRasterAsset::chunks` 를 순회하거나 한계가 설정된 `renderFrameRegion` 를 요청합니다.

무한 캔버스는 개념적으로 무한하지만, 임의의 순간에는 유한한 할당 영역에서 작동합니다. 카메라 소유자는 가시 세계 영역과 선택한 프리페치 마진을 덮도록 `ensureInfiniteCanvasRegion` 를 요청합니다. 문서의 기원과 범위는 해당 요구 사항이 섹트 경계를 넘을 때만 증가합니다. 기존 섹트 좌표와 픽셀은 절대 이동하지 않습니다.

<a id="canvasitem-integration"></a>

## CanvasItem 통합

`CanvasItem::document()`는 바운드 집계를 반환하고 `CanvasItem::documentEditor()`는 영구 구조 편집기를 반환합니다. 단일 구조 변경의 경우 `editDocument`를 선호합니다.

~~~cpp
const DocumentEditResult result = canvas.editDocument(
    [](DocumentEditor &editor) {
        return editor.setLayerOpacity("ink", 0.65);
    });
~~~

성공하면 항목이 다시 렌더링을 예약하고, 선택한 래스터 레이어를 확인하거나 지우고, 일반 변경 신호를 내보내고, 타임라인이 짧아지면 표시된 프레임을 고정합니다. `*canvas.document()`를 직접 변경하는 코드는 `validate` 및 `canvas.refresh()` 자체를 호출해야 합니다.

`createInfiniteRasterDocument(width, height, chunkSize)` 는 선택된 빈 `ChunkedRasterAsset` 하나를 생성합니다. QML 는 읽기 `infiniteCanvas`, `canvasOriginX`, `canvasOriginY`, `canvasChunkSize` 를 호스팅한 후 카메라가 이동하면 `ensureInfiniteCanvasRegion(x, y, width, height)` 를 호출합니다. 반환된 맵은 `changed` 와 정확한 `left`, `top`, `right`, `bottom` 픽셀 성장을 포함하므로 호스트는 카메라와 오버레이 위치를 보존하면서 디스플레이 항목을 리사이즈할 수 있습니다.

`CanvasItem::refresh()` 는 현재 문서를 유효성 검사하고 스냅샷을 찍은 다음 가시 타일을 대기열에 추가하며, 그 부울 결과는 이미 픽셀이 생성되었다는 것을 의미하는 것이 아니라 요청이 승인되었음을 의미합니다. `refreshAsync()` 는 새로운 콘텐츠 수정판을 반환합니다. 호출자가 프레젠테이션을 기다려야 할 때 `rendering` 또는 `renderCompleted(requestId)` 를 관찰합니다. `residentTileCount` 는 64 개의 합성 타일로 제한되며, `residentLayerTileCount` 는 독립적으로 캐시된 레이어 타일을 보고하며 256로 제한됩니다. 각 텍스처는 최대 512 x 512 픽셀입니다. `framePixels()` 는 전체 문서를 덮는 단일 풀 해상도 타일이 있을 때만 호환성 뷰로 남으며, 큰 타일 프레임에 대해서는 의도적으로 null 을 반환합니다.

`gpuAccelerated` 는 활성 Qt Quick 렌더러가 Metal , Vulkan , Direct3D , 또는 OpenGL 인지 보고하며, `graphicsBackend` 는 그 이름을 노출합니다. CPU 워커는 여전히 결정론적 벡터 래스터화와 iiPaintEngine 블렌드 의미를 소유하고 있으며, GPU 는 텍스처 업로드, Qt Quick 시나리오 내 합성, 근접 이웃 샘플링, 패닝, 그리고 줌을 소유합니다. 소프트웨어 Qt Quick 백엔드는 계속 작동하지만 GPU 가속화를 보고하지 않습니다.

비 QML 호출자를 위해, `renderFrameLayerTiles` 는 한계가 설정된 요청 배치에 대해 한 레이어만 렌더링합니다. `renderFrameLayers` 는 하단에서 상단 문서 순서로 모든 독립적인 레이어 배치를 반환하며, `composeFrameLayers` 는 그 불투명도와 블렌드 메타데이터를 적용하여 최종 공간 타일을 생성합니다. `renderFrameRegion` 와 `renderFrameTiles` 는 동일한 경계 경계를 사용하므로, 그 출력은 합성된 프레임만 필요한 호출자와 호환됩니다. 숨겨진 레이어와 선택적 포함 `frameRange` 바깥의 레이어는 배치에서 순서 있는 동일성과 메타데이터를 유지하지만, 유효한 무관성을 보고하며 픽셀 타일을 할당하지 않습니다. 두 범위 경계는 모두 렌더링되며, 부재한 범위는 전체 타임라인을 덮습니다.

`AsyncFrameRenderer::request` 는 값 스냅샷 또는 공유 불변 스냅샷을 가져오고, 스레드 풀 사전 검사 에서 해당 스냅샷을 한 번만 검증한 후, 한계가 설정된 전역 워커 집단에 레이어 인덱스를 분배합니다. 거절된 사전 검사 는 `renderFrameLayers` 와 동일한 에러를 반환하며 부분적인 레이어 배치가 없습니다. 요청들은 최신 대기 중인 작업으로 합쳐지며, 렌더러는 소유 스레드에서 `finished` 를 방출합니다. `lastLayerResult()` 는 격리된 레이어 배치를 검사하고, `lastResult()` 는 합성된 배치를 검사합니다. `takeLayerResult()` 와 `takeResult()` 는 GUI 스레드 픽셀 복제 없이 픽셀 저장소를 전달합니다. 실시간 스트로크 동안 다음 불변 스냅샷 이전 병합 지연은 `livePreviewFrameIntervalMs` 입니다; 최종 커밋은 여전히 최신 문서 상태를 예약합니다.

<a id="standalone-example"></a>

## 독립형 예

~~~cpp
Document canvas;
canvas.extent = {1920, 1080};
canvas.timeline = {{24, 1}, 48};
DocumentEditor canvasEditor(canvas);

canvasEditor.insertRasterAsset(
    "paint.pixels", makeRasterLayer(1920, 1080, 0x00000000U));
canvasEditor.insertLayer(StaticBitmapLayer{
    {"paint.layer", "Paint", true, 1.0, {}, RasterBlendMode::SourceOver},
    StaticSource{"paint.pixels"},
});
canvasEditor.renameAsset("paint.pixels", "paint.frame.0");

BitmapEditor pixels(canvas, "paint.frame.0");
pixels.setPixel(10, 10, 0xffffcc00U);
~~~

<a id="threading-and-persistence"></a>

## 스레딩과 지속성

`Document` ,  `DocumentFile` ,  `DocumentEditor` ,  `BitmapEditor` ,  `ChunkedBitmapEditor` ,  `VectorEditor` , 및 제한된 `CanvasItem` 는 범용 동기화 변형 객체가 아닙니다. 한 소유 스레드에서 한 문서를 변형합니다; Qt 퀵 항목은 GUI 스레드 객체로 남습니다. `CanvasItem` 는 작업자가 이를 보기 전에 검증된 불변 렌더링 스냅샷을 복사하므로 작업자가 호출자 변형과 경쟁하지 않습니다. `AsyncFrameRenderer` 도 요청 수명 동안 불변 스냅샷을 소유하거나 공유합니다. 안정 ID 조회는 컬렉션 재배치가 끊긴 캐시 자산 포인터가 되는 것을 방지하지만, 소스 문서의 동시 변형을 안전하지 않게 만듭니다.

`validate(document)` 를 호출한 후 `renderFrame` ,  `renderFrameRegion` , 또는 `encodeIisc` 를 직접 집계 변형 후 호출합니다. 직렬화기 는 집계 상태만 지속하며, 편집자 버전 카운터, 되돌리기 스택, 선택 및 콜백은 런타임 상태입니다. 파일 기반 저작의 경우 검증 및 직접 디스크 커밋은 편집 경계에서 자동으로 수행됩니다. 렌더링 스냅샷은 분리되어 있으며 파일을 쓸 수 없습니다.

<a id="persisted-audio-timeline"></a>

## 지속되는 오디오 타임라인

`Document::audioAssets`는 `AudioAsset` PCM16 데이터를 소유하고 `Document::audioTracks`는 `AudioTrackLayer` 클립 레이어를 소유한다. `findAudioAsset`, `findAudioTrack`, `findAudioClip`는 형식 지정 조회를 노출한다. `DocumentEditor`는 자산·트랙·클립을 삽입/교체/제거하고 트랙을 트랜잭션으로 재정렬한다. 오디오 클립은 비디오 프레임 위치/재생 시간과 채널별 샘플 프레임 원본 오프셋을 사용한다. `audioSampleFrameCount`는 원본 검증에 필요한 한계가 설정된 정확한 올림값을 계산한다. 검증과 편집 예제는 [오디오 계약](AUDIO_TIMELINE.md)을 참고한다. `decodeAudioWav` / `importAudioWav`는 분리된 PCM16 자산을 반환하며, `encodeAudioWav` / `exportAudioWav`는 모노/스테레오 RIFF/WAVE 컨테이너에 부호 있는 샘플을 보존한다. 가져오기/내보내기는 명시적인 바이트 예산으로 8000..192000 Hz를 지원하며, 리샘플링이나 암시적 외부 디코더는 없다. 미지원 비트 깊이·압축·채널 배치·잘못된 청크 경계·부분 샘플 프레임은 실패한다.

`CanvasItem`는 문서 브러시 위치를 선택한 래스터 자산에 매핑할 때 현재 프레임에서 `sampleLayerAt`를 반전합니다. `exportPdf`는 요청된 각 페이지에 대해 동일한 모션 변환 및 비디오 소스 프레임을 평가합니다.

<a id="staticdynamic-x-bitmapvector-0120"></a>

## 네 가지 실제 레이어/콘텐츠 타입(0.28.0)

`LayerKind`는 비트맵 /벡터 아트워크를 4 방식으로 분류합니다:

|종류|표현|타임라인 내용|
| --- | --- | --- |
| `StaticBitmap` |비트맵(청크된 래스터 포함)|1픽셀 자산|
| `StaticVector` |벡터|단일 경로 자산|
| `DynamicBitmap` | 비트맵 |네이티브 비디오를 포함한 프레임 선택 픽셀|
| `DynamicVector` |벡터|프레임 선택 경로 자산|

네 가지 `LayerKind`는 각 실제 레이어와 콘텐츠 타입에 대응한다. `layerKind`는 `std::optional<LayerKind>`를 반환하며 비공간 IP-Adapter에는 값이 없다. 정적 콘텐츠는 `assetId`, 동적 콘텐츠는 `frameIndices`를 가진다. 비디오와 기존 공간 조건부 레이어는 전용 역할을 유지하며 네 가지 시각 분류에 대응한다. [FOUR_LAYER_TYPES.md](FOUR_LAYER_TYPES.md)에 데이터 소유권과 마이그레이션 계약을 정의했다.

```cpp
editor.insertStaticLayer({"background", "Background"},
                         LayerRepresentation::Bitmap, "background-pixels");
editor.insertDynamicLayer({"drawing", "Animated drawing"},
                          LayerRepresentation::Vector,
                          {{0, "pose-a"}, {1, "pose-b"}, {2, "pose-c"}});
auto kind = layerKind(*findLayer(document, "drawing")); // DynamicVector
```

생성 시 자산 참조, 표현, 프레임 범위, 프레임-0 콘텐츠 및 레이어 ID 를 원자적으로 유효성 검사하며, `DocumentFile` 에 바인딩된 경우에도 포함됩니다. `setStaticSource` 과 `setKeyframedSource` 는 기존 비트맵 /벡터 레이어를 변환하며, `insertKeyframe` 과 `setKeyframeAsset` 는 동적 프레임 콘텐츠를 편집합니다. 한 콘텐츠 키만 여전히 동적 레이어를 나타냅니다. 키 사이에는 이전 키가 유지되며, 프레임마다 변경되는 콘텐츠에는 프레임당 하나의 키를 사용하세요. `renderFrame` 과 `CanvasItem` 는 기존 공유 샘플러를 통해 현재 프레임을 해결합니다. 인접한 콘텐츠 상태가 동일하다면 다른 픽셀을 생성하기 위해 프레임 변경이 필요하지 않습니다.

정적은 시간에 따른 콘텐츠를 의미하며 불변성을 의미하지 않습니다: 자산 편집은 모든 프레임의 그림을 변경합니다. 변환/불투명도 운동과 가시성 범위는 콘텐츠 종류와 독립적입니다. 라이브 프로듀서는 편집기를 통해 새로운 픽셀/경로 자산과 콘텐츠 키를 커밋할 수 있습니다; 이 API 는 카메라/네트워크 캡처나 지속 콜백을 제공하지 않습니다. 네이티브 비디오는 기존 유리 비율 샘플러를 유지합니다.

스냅샷 1.19는 대지 소속 뒤에 명시적인 타입 바이트를 기록하고 타입/콘텐츠 불일치를 거부한다. 작업 파일 스키마 1은 동일한 레이어 레코드를 증분 저장한다. 기존 1.0~1.18은 네 가지 메모리 타입으로 복원되며 기존 버전의 정규 바이트를 유지한다.

<a id="controlnet-semantic-segments-0130"></a>

## ControlNet 의미 체계 세그먼트(0.13.0)

`SemanticSegmentLayer` 는 검증된 영역 식별자, 명시적인 의미론적 분류 체계 및 ControlNet 설정을 소유합니다. 정적과 동적 식별자 마스크는 기존 타임라인을 공유합니다. `renderSemanticControlMap` 는 정확한 클래스 색상, 레이블 및 영역 기하학을 생성하는 반면 일반 아트웍 렌더링은 조건부 레이어를 제외합니다. [를 참조하여 완전한 객체 및 지속성 계약을](SEMANTIC_SEGMENT.md)확인하세요.

<a id="full-body-hands-and-dense-facial-pose-0140"></a>

## 전신, 손, 밀도 높은 표정 포즈 (0.14.0)

`PoseAsset` / `PoseLayer`는 희소 가중치 표현 대상, static/dynamic 소스, 정확한 네이티브 지속성 및 명시적인 OpenPose 프로젝션을 갖춘 523 얼굴 앵커를 포함하여 개인당 590 앵커를 소유합니다. [포즈 계약](POSE.md)를 참조하세요.

<a id="depth-controlnet-0150"></a>

## 깊이 ControlNet (0.15.0)

`DepthAsset`, `DepthLayer`, `insertDepthAsset`, `replaceDepthAsset`, `insertDepthLayer`, `setDepthSample` 및 `renderDepthControlMap`를 사용합니다. 참조
스칼라 계약의 경우 [DEPTH.md](DEPTH.md), static/dynamic 예시 및 제한 사항입니다.

<a id="line-art-controlnet-0160"></a>

## 라인 아트 ControlNet (0.16.0)

`LineArtAsset` 및 `LineArtLayer`는 static/dynamic 잉크 적용 범위를 제공합니다. `insertLineArtAsset`, `replaceLineArtAsset`, `insertLineArtLayer`, `setLineArtSample` 및 `renderLineArtControlMap`를 사용합니다. [LINE_ART.md](LINE_ART.md)를 참조하세요.

<a id="binary-line-controls-0170"></a>

## 바이너리 라인 컨트롤(0.17.0)

`CannyAsset` / `CannyLayer` 및 `ScribbleAsset` / `ScribbleLayer`는 독립적인 static/dynamic 이진 라인 컨트롤입니다. 삽입/대체 자산, 레이어 삽입, 샘플 설정 및 렌더링 제어 지도 API는 기존 라인 아트 워크플로를 따릅니다. `isLineControlNetLayer`는 모든 3 컨트롤을 그룹합니다. 보다
[BINARY_LINE_CONTROL.md](BINARY_LINE_CONTROL.md).

## MLSD (0.18.0)

네이티브 편집 가능한 직선 조건을 지정하려면 `MlsdSegment`, `MlsdAsset`, `MlsdLayer`, `insertMlsdAsset`, `replaceMlsdAsset`, `insertMlsdLayer`, `setMlsdSegment` 및 `renderMlsdControlMap`를 사용하세요. `isLineControlNetLayer`에는 MLSD가 포함됩니다. [MLSD.md](MLSD.md)를 참조하세요.

<a id="normal-map-0190"></a>

## 노멀 맵(0.19.0)

`NormalMapSample`, `NormalMapAsset`, `NormalMapLayer`, `insertNormalMapAsset`, `replaceNormalMapAsset`, `insertNormalMapLayer`, `setNormalMapSample`, 및 `renderNormalMapControlMap` 를 밀집 표면 법선 조건화에 사용합니다. 기본 출력은 XYZ RGB 입니다; `NormalMapRenderOptions` 는 XYZ / ZYX, Y 반전 및 maximumPixels 를 선택합니다. `findNormalMapAsset` / `findNormalMapLayer`, `validateNormalMapAsset` 및 `normalMapRasterPreview` 는 공개됩니다. 네이티브 샘플은 서명된 XYZ 를 유지하며 아무런 알림 없이 다시 정규화되지 않습니다. [NORMAL_MAP .md](NORMAL_MAP.md)를 참조하세요.

<a id="shuffle-0200"></a>

## 셔플(0.20.0)

`ShuffleAsset { id, viewport, colors }` 는 행 우선 `ShuffleColor { red, green, blue }` RGB8 값을 소유합니다. `ShuffleLayer` 는 속성, 출처 및 ControlNet 설정을 가집니다. `insertShuffleAsset`, `replaceShuffleAsset`, `insertShuffleLayer` 및 `setShuffleSample` 를 검증된 트랜잭션에 사용하세요; `setControlNetSettings` 와 `setKeyframedSource` 는 공유 레이어 계약을 사용합니다. `renderShuffleControlMap` 는 불투명한 픽셀과 정확한 색상을 반환합니다. `shuffleRasterPreview`, 타입화된 찾기 도우미, `validateShuffleAsset`, 및 `makeShuffleAsset` 는 공개적입니다. 후자는 불투명한 소스 이미지, 출력 범위 및 명시적인 `ShuffleCoordinate` 필드를 받습니다. [SHUFFLE .md](SHUFFLE.md)를 참조하세요.

<a id="tile-0210"></a>

## 타일(0.21.0)

`TileAsset { id, viewport, colors }` 는 행 우선 `TileColor { red, green, blue }` 를 소유합니다. `TileLayer` 는 속성, 타입화된 static/keyframed 소스 및 ControlNet 설정을 소유합니다. `insertTileAsset`, `replaceTileAsset`, `insertTileLayer`, `setTileSample`, 타입화된 찾기 도우미, `validateTileAsset`, `makeTileAsset` 및 `tileRasterPreview` 는 공개적입니다. `renderTileControlMap` 는 완전한 네이티브 이미지를 내보냅니다; `renderTileControlRegion` 는 전체 크기의 출력을 먼저 할당하지 않고 명시적인 범위 내 `TileRegion { x, y, width, height }` 를 내보냅니다. 결과에는 픽셀, 색상, 소스 영역 및 메시지/확인 상태가 포함됩니다. [TILE .md](TILE.md)를 참조하세요.

<a id="reference-0220"></a>

## 참조 (0.22.0)

`ReferenceColor`, `ReferenceAsset`, `ReferenceMode`, `ReferenceSettings` 및 `ReferenceLayer` 는 RGB 참조와 그 적용 계약을 모델링합니다. `makeReferenceAsset`, 타입화된 찾기 도우미, `validateReferenceAsset`, `validateReferenceSettings`, `referenceRasterPreview`, `renderReferenceControlMap`, `insertReferenceAsset`, `replaceReferenceAsset`, `insertReferenceLayer`, `setReferenceSample` 및 `setReferenceSettings` 는 공개적입니다. 출력에는 정확한 RGB 및 참조별 공통 ControlNet 설정이 포함됩니다. REFERENCE .md 를 참조하세요.

<a id="ip-adapter-embeddings-0230"></a>

## IP-어댑터 임베딩(0.23.0)

`IpAdapterEmbeddingStage`, `IpAdapterBranch`, `IpAdapterEmbeddingDescriptor`, `IpAdapterTensor`, `IpAdapterAsset`, `IpAdapterLayer`, `IpAdapterExportOptions` 및 `IpAdapterEmbeddingResult` 는 이미지 임베딩 계약을 구성합니다. 공개된 찾기 헬퍼 `validateIpAdapterAsset`, `validateIpAdapterLayerSources`, `exportIpAdapterEmbeddings` 와 편집기 `insertIpAdapterAsset`, `replaceIpAdapterAsset`, `insertIpAdapterLayer`, `setIpAdapterValue` 는 타입화된 조회, 프레임 선택 및 트랜잭션 편집을 지원합니다. `setControlNetSettings` 는 어댑터 식별자, 스케일 및 일정을 적용합니다. IP_ADAPTER .md 를 참조하세요.

`LayerRepresentation::Embedding`은 비공간 텐서 조건부를 나타낸다. `layerKind`에는 값이 없으며 네 가지 아트워크 종류를 늘리지 않는다. 일반 아트워크 팩토리는 Embedding을 거부한다. 전용 IP-Adapter 생성자를 사용한다. 임베딩 렌더 결과는 `spatial == false`이고 아트워크 합성에서 제외된다.

<a id="detailed-controlnet-parameters-0240"></a>

## 자세한 ControlNet 매개변수(0.24.0)

모든 12 조건부 타입은 `getControlNetParameters` 와 `DocumentEditor::controlNetParameters` 를 통해 타입화된 레이어/객체 스냅샷을 노출합니다. `setControlNetParameters` 는 하나의 레이어와 모든 고유한 소스 상태를 원자적으로 적용하며, `patchControlNetSettings` 는 다른 것들을 대체하지 않고 선택된 공통 필드를 편집합니다. 상세 필드, 출력 옵션, 소유권 및 롤백 규칙은 문서화되어 있습니다.
[CONTROLNET_PARAMETERS.md](CONTROLNET_PARAMETERS.md). 현재 네이티브 모델은 명시적 타입을 포함한 1.19이다.

## Dynamic frame content (0.27.1)

`DocumentEditor::setDynamicFrameContent(layerId, frame, RasterAsset)` and the
`VectorAsset` overload write a fresh content asset and insert or replace the exact
frame-owned key in one validated edit. Every frame may have its own pixels, vector
viewport, paths, fills and strokes. The existing dynamic source must have the
matching content kind; static layers are not implicitly converted.

The supplied asset id must be nonempty and unused. This prevents an accidental
overwrite of another frame/layer's shared data. Replaced assets remain available
for explicit `removeAsset` cleanup. Use existing asset editors for deliberately
shared edits and `setKeyframeAsset` for deliberately shared references. Per-frame
independence requires a separate key and asset at every frame; sparse content keys
continue to hold until the next key. See [DYNAMIC_FRAME_CONTENT.md](DYNAMIC_FRAME_CONTENT.md).

## Artboards (0.27.0)

`Artboard`, `Document::artboards`, and `LayerProperties::artboardId` define non-nesting
canvas groups. `findArtboard`, `documentViewRegion`, `renderArtboard` and the artboard
methods on `DocumentEditor` expose validated editing and rendering.
`sampleLayerAt` returns world coordinates after applying board-local motion.
See [ARTBOARDS.md](ARTBOARDS.md) for the complete public contract. Native model is 1.19.
