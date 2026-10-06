<a id="iisharedcanvas-iisc-format-version-1"></a>

# iiSharedCanvas `.iisc` 형식 버전 1

상태: 정규 이진 계약을 구현했습니다.

본 문서는 라이브 저작 저장소가 아닌 버전-1   **교환 스냅샷**를 지정합니다. [PERSISTENCE .md](PERSISTENCE.md) 는 애플리케이션 id `0x49495343`, 스키마 1, 그리고 증분식 라이트-투루 편집을 가진 SQLite 작업 파일을 지정합니다. 두 가지 모두 `.iisc` 를 사용하며 헤더로 구별합니다. `encodeIisc` / `decodeIisc` 는 원래 스냅샷 바이트를 보존하고, `DocumentFile::open` 는 레거시 스냅샷을 다시 작성하지 않습니다. 편집하기 전에 새 작업 파일로 명시적으로 가져옵니다.

<a id="identity"></a>

## 신원

- 확장자: `.iisc`
- 미디어 유형: `application/vnd.iisacc.ii-shared-canvas`
- 현재 모델 버전: 메이저 1, 마이너 19
- 정수 바이트 순서: little-endian
- 부동소수점 표현: IEEE 754 바이너리64, 리틀 엔디언 비트로 저장됩니다
- 래스터 채널 표현: 32-bit ARGB는 iiPaintEngine에 의해 정의됩니다.
- 텍스트 표현: 정형 UTF-8에 작은 엔디안 `u32` 바이트 수가 접두어 있습니다

버전 1는 단일 바이너리 컨테이너입니다. ZIP가 아니며 엔트리 이름이나 내부 파일 경로가 없습니다. 이렇게 하면 아카이브 순서, 재귀 및 경로 탐색이 형식에서 제거되며, 아카이브나 JSON 의존성을 추가하지 않습니다.

<a id="container-header"></a>

## 컨테이너 헤더

모든 컨테이너는 이 정확한 32-byte 헤더로 시작합니다:

|오프셋|크기|필드|계약|
| ---: | ---: | --- | --- |
| 0 | 8 |마법| `49 49 53 43 0D 0A 1A 0A` (`IISC\r\n\x1A\n`) |
| 8 | 2 |소령|문서 형식 주요|
| 10 | 2 |미성년자|문서 형식 마이너|
| 12 | 4 |깃발|0 버전 1|
| 16 | 8 |페이로드 크기|다음 바이트의 정확한 수|
| 24 | 4 |검사 합계|페이로드의 CRC-32/ISO-HDLC|
| 28 | 4 |예약되어 있는|0 버전 1|

정확한 페이로드가 즉시 따라옵니다. 리더는 문서를 노출하기 전에 잘린 페이로드, 선언된 페이로드 이후의 바이트,0가 아닌 플래그 또는 예약된 필드, 체크섬 불일치 및 지원되지 않는 버전을 거부합니다.

<a id="primitive-encoding"></a>

## 원시 인코딩

- `u8`, `u16`, `u32`, `u64`, `i16`, 및 `i32`는 고정폭 소수점 값입니다.
- `f64`는 IEEE 754 `double`의 리틀 엔디언 비트 표현입니다.
- 불리언 값은 `u8`이며 정확히 0 또는 1이어야 합니다.
- 문자열은 `u32 byteCount` 뒤에 정확히 그 수의 정규 UTF-8 바이트가 뒤따릅니다. Embedded NUL 바이트는 유효한 UTF-8 데이터이며 문자열을 종료하지 않습니다.
- 카운트는 `u32`이며, 집계 리더 제한은 `reserve` 또는 픽셀 할당 전에 확인됩니다.

<a id="payload-order"></a>

## 페이로드 순서

페이로드에는 하나의 정규 서열이 있습니다:

~~~text
i32 canvasWidth
i32 canvasHeight
[if minor >= 1]
  u8 canvasMode       // 0 finite, 1 infinite
  [if canvasMode == 1]
    i32 canvasOriginX
    i32 canvasOriginY
    i32 chunkSize
u32 frameRateNumerator
u32 frameRateDenominator
u32 frameCount

u32 assetCount
Asset assets[assetCount]

u32 layerCount
Layer layers[layerCount]

[if minor >= 2]
  bool hasStableDiffusionMetadata
  [if hasStableDiffusionMetadata]
    StableDiffusionMetadata generation

[if minor >= 4]
  u32 audioAssetCount
  AudioAsset audioAssets[audioAssetCount]
  u32 audioTrackCount
  AudioTrackLayer audioTracks[audioTrackCount]
[if minor >= 5]
  string authorshipJson
[if minor >= 18]
  u32 artboardCount
  Artboard artboards[artboardCount]
~~~

자산과 레이어는 문서 벡터 순서를 유지합니다. 레이어는 아래에서 위로 순서대로 정렬됩니다.

버전 1.1 은 영역과 타임라인 사이에 무한 캔버스 메타데이터를 추가합니다. `canvasWidth` 와 `canvasHeight` 는 현재 할당된 렌더링 영역을 설명하며 개념적 경계가 아닙니다. 그 좌상단 월드 좌표는 `(canvasOriginX, canvasOriginY)` 입니다. 섹크 크기는 32 를 통해 4096 픽셀의 2 의 거듭제곱입니다. 버전 1.0 은 이 블록을 생략하며 항상 기원 `(0, 0)` 의 유한 캔버스로 디코딩됩니다.

버전 1.2는 레이어 컬렉션 뒤에 옵션인 Stable Diffusion 생성 메타데이터를 추가합니다. 블록을 추가하면 이전의 모든 오프셋과 기록이 변경되지 않습니다. 버전 1.0 및 1.1는 레이어 수집 직후에 종료되며, 따라서 생성 메타데이터 없이 디코딩됩니다.

버전 1.3는 해당 레이어의 전체 소스 페이로드 바로 뒤에 각 레이어 레코드에 선택적인 포함 프레임 범위를 추가합니다. 버전 1.0는 1.2 레이어 레코드가 소스 이후에 종료되며 범위 바이트를 포함하지 않으므로 정규 인코딩은 변경되지 않습니다. 생성 메타데이터 블록은 전체 레이어 컬렉션을 계속 따라갑니다.

버전 1.4는 전체 메타데이터 블록 뒤에 소유된 PCM16 오디오 자산과 편집 가능한 오디오 트랙 레이어를 추가합니다. 버전 1.0부터 1.3까지는 오디오 테일이 없으며, 빈 오디오 컬렉션으로 디코딩됩니다; 정규 바이트는 변경되지 않습니다. 작가는 오래된 모델 버전의 오디오를 포기하는 대신 거부합니다. 오디오 자산 및 트랙은 시각 레이어와 무관하게 벡터 순서를 유지합니다.

기존 1.2 메타데이터 레코드 내에서, `generationParametersText` 는 호환 가능한 이미지 캐리어에서 추출된 완전한 스테이블 디퓨전 생성 매개변수 텍스트를 포함합니다. 생성 매개변수 텍스트는 정준 `.iisc`   왕복 변환 (왕복 변환) 동안 바이트 정밀로 유지되며, 파싱은 별도의 디코딩 뷰를 생성하고 이 문자열을 다시 작성하지 않습니다. PNG   `parameters`, EXIF   `UserComment` 또는 기타 캐리어 식별자는 추출이 가져오는 어댑터에 속하므로 영속화되지 않습니다. 알 수 없는 또는 중복 매개변수 쌍은 최종 의미론적 값이 또한 타입화된 필드로나 `extraParameters` 로 투영되더라도 이 공식 문자열에서 복구 가능합니다. 이 호환성 리더는 1.2 바이너리 필드나 오프셋을 변경하지 않으므로 포맷 버전 증분 없이도 필요합니다.

<a id="assets"></a>

## 자산

모든 자산은 다음과 같이 시작합니다:

~~~text
u8 kind       // 0 래스터, 1 벡터, 2 청크형 래스터(minor >= 1)
string id
~~~

자산 ID 는 문서 식별자이며 경로가 아닙니다.

<a id="raster-asset"></a>

### 래스터 자산

~~~text
i32 width
i32 height
u64 pixelCount
u8 encoding       // 0 원시 ARGB32, 1 런렝스 ARGB32
u64 encodedBytes
u8 data[encodedBytes]
~~~

`pixelCount`는 `width * height`와 같아야 합니다. 원시 인코딩은 픽셀당 정확히 하나의 리틀 엔디안 `u32`를 행‐대순으로 포함합니다. 런 길이 인코딩은 `u32 runLength, u32 argb` 레코드를 포함합니다; 런 길이는 양수이며 정확한 픽셀 수를 채우고, 인접한 레코드는 동일한 ARGB 값을 반복할 수 없습니다.

라이터는 바이트 수가 원시 ARGB32보다 엄격히 적을 때에만 런-길이 인코딩을 사용합니다. 그렇지 않으면 원시 인코딩을 사용합니다. 독자는 반대 선택을 거부하므로, 하나의 래스터는 하나의 정형 표현을 갖게 됩니다.

브러시 궤적, 압력 샘플, 곡선, 댑 및 재생 명령은 여기에서 표시되지 않습니다. iiPaintEngine는 브러시 출력을 먼저 커밋하고, `.iisc`는 생성된 픽셀만 저장합니다.

<a id="chunked-raster-asset"></a>

### 청크된 래스터 자산

~~~text
u32 chunkCount
repeat chunkCount:
  i32 column
  i32 row
  RasterPayload pixels
~~~

`RasterPayload` 는 위의 래스터 자산에 의해 정의된 너비, 높이, 픽셀 수, 인코딩, 인코딩된 바이트 수, 및 데이터 시퀀스입니다; 이는 ID 를 반복하지 않습니다. 각 청크는 정확히 `chunkSize` x `chunkSize` 입니다. 청크는 `(column, row)` 가 `[column * chunkSize, (column + 1) * chunkSize)` 월드 픽셀 영역을 `[row * chunkSize, (row + 1) * chunkSize)` 로 식별하며, 음수 좌표를 포함합니다. 청크는 고유하며 행 다음 열 순서로 오름차순으로 인코딩됩니다. 누락된 청크는 투명하며 래스터 페이로드를 소비하지 않습니다. 청크화된 래스터 는 무한 캔버스에서만 유효하며 1.1버전에서 처음 나타납니다.

<a id="vector-asset"></a>

### 벡터 자산

~~~text
i32 viewportWidth
i32 viewportHeight
u32 pathCount
VectorPath paths[pathCount]
~~~

벡터 경로는:

~~~text
u32 commandCount
PathCommand commands[commandCount]
bool hasFill
[u32 fillArgb]
bool hasStroke
[u32 strokeArgb, f64 strokeWidth]
~~~

경로 명령 태그 및 필드는 다음과 같습니다:

|태그하다|작업|필드|
| ---: | --- | --- |
| 0 | MoveTo | `f64 x, f64 y` |
| 1 | LineTo | `f64 x, f64 y` |
| 2 | QuadraticTo |제어점, 종점|
| 3 | CubicTo |제어 1, 제어 2, 엔드 포인트|
| 4 | ClosePath |없음|

알 수 없는 태그 안전하게 거부한다. 유효한 경로는 MoveTo로 시작하며 눈에 보이는 채우기 또는 스트로크가 있습니다. 벡터 기하학은 저장/로드 및 마이그레이션 전반에 걸쳐 네이티브를 유지합니다.

<a id="layers-and-timeline"></a>

## 레이어 및 타임라인

레이어는 다음과 같이 인코딩됩니다:

~~~text
string id
string name
bool visible
f64 opacity
f64 m11, m12, m21, m22, translationX, translationY
u8 blendMode
[if minor >= 18]
  bool hasArtboard
  [if hasArtboard]
    string artboardId
[if minor >= 19]
  u8 visualLayerKind // 0 static bitmap, 1 static vector, 2 dynamic bitmap, 3 dynamic vector, 255 nonspatial
u8 sourceKind
LayerSource source
[if minor >= 3]
  bool hasFrameRange
  [if hasFrameRange]
    u32 firstFrame
    u32 lastFrame
~~~

블렌드 태그는 0 오버, 1 곱하기, 2 스크린, 및 3 오버레이입니다. iiPaintEngine 디스틴네이션-아웃은 즉시 브러시 지우기를 위해 예약되어 있으며 지속되는 레이어 블렌드 모드가 아닙니다.

아핀 변환은 `(x, y)`를 다음과 같이 매핑합니다:

~~~text
x' = x * m11 + y * m21 + translationX
y' = x * m12 + y * m22 + translationY
~~~

소스 종류 0는 정적이며 하나의 자산 ID 문자열을 저장합니다. 그 콘크리트 층 유형은 참조된 자산 종류입니다. 소스 종류 1는 애니메이션 레이어를 표시하고 다음 레이어 주요 와이어 프로젝션을 저장합니다:

~~~text
u8 contentKind       // 0 raster payload, 1 vector payload
u32 keyframeCount
repeat keyframeCount:
  u32 frame
  string assetId
~~~

와이어 레이아웃은 `.iisc` 1.0 에서 1.2까지 변경되지 않습니다. 메모리에서 `Document::frames` 는 엄격히 증가하는 희소 `Frame` 레코드와 모든 실제 `Keyframe` 를 소유합니다. `KeyframedSource::frameIndices` 는 해당 레이어에 대해 소유된 정확한 증가 프레임 시트를 포함하는 검증된 유도된 인덱스일 뿐입니다. 디코더는 해당 인덱스를 각 포함 와이어 레코드에서 채우고, 키를 `Frame{index, Keyframe{layerId, assetId}}` 로 전치하며, 동시 키를 레이어 ID 로 순서대로 정렬합니다. 인코더는 문서 레이어 순서로 프레임을 소유한 키들을 다시 투영하므로, 사전순이 아닌 레이어 스택도 동일한 바이트로 다시 인코딩됩니다. 한 프레임 내의 동일한 레이어에 대한 빈 프레임과 2 키는 기존 와이어 포맷이 이를 표현할 수 없기 때문에 유효하지 않습니다. 인코딩된 `contentKind` 는 소유 레이어의 정형화된 타입 태그이며, 정적 레이어이거나 참조된 레이어와 자산 종류가 다른 키는 검증에서 거부됩니다.

버전 1 자산 참조 키는 홀드 샘플링만 사용합니다. 선택된 자산은 요청된 프레임에 있거나 그 이전에 있는 요청된 레이어의 마지막 프레임 소유 키입니다. 희소 프레임 인덱스는 엄격히 증가하며 `[0, frameCount)` 범위 내에 유지됩니다; 모든 애니메이션 레이어는 프레임‐0 키를 가지고 있으며, 하나의 프레임은 여러 래스터 및 벡터 레이어에 대한 키를 직접 소유할 수 있습니다.

프레임 범위 끝점은 0기반이며 포함됩니다. 존재하는 범위는 `firstFrame <= lastFrame < frameCount` 를 만족하며, 레이어는 존재하지 않고 해당 구간 바깥에 렌더링된 픽셀을 기여하지 않습니다. 부재하는 범위는 레이어가 전체 타임라인에 걸쳐 존재함을 의미합니다. 범위는 정적과 키프레임이 적용된 비트맵 또는 벡터 레이어에 동일하게 적용됩니다. 그것은 파괴적이지 않은 존재 게이트이며, 키프레임 는 범위를 벗어날 수 있고, 범위에 진입하면 즉시 해당 프레임 이전 또는 해당 프레임의 마지막 프레임 기반0유지 값을 사용합니다.

<a id="audio-assets-and-track-layers-14"></a>

## 오디오 자산 및 트랙 레이어 ( 1.4 )

오디오 자산은 다음과 같이 인코딩됩니다:

~~~text
string id
u32 sampleRate
u16 channelCount
u64 sampleCount
i16 samples[sampleCount]
~~~

샘플은 채널 순서로 서명된 리틀엔디언 PCM16 입니다. 개수는 채널 전체의 스칼라 샘플 총 개수이며, `sampleCount / channelCount` 는 소스 샘플 프레임의 개수입니다. 샘플 속도는 8000 – 192000 Hz, 채널 개수는 하나 또는 2이며, 샘플은 채널 개수로 나누어떨어지는 개수를 가진 비어있지 않습니다. 이것은 디코딩된 오디오 샘플을 내장하며, 외부 경로나 원래 압축된 코덱 스트림이 아닙니다. 오디오 및 시각적 자산 id 는 문서 정체성 네임스페이스를 공유합니다.

오디오 트랙 레이어는 다음과 같이 인코딩됩니다:

~~~text
string id
string name
bool muted
f64 gainDb
u32 clipCount
repeat clipCount:
  string id
  string name
  string assetId
  u32 startFrame
  u32 durationFrames
  u64 sourceOffsetSamples
  f64 gainDb
  bool enabled
~~~

오디오 및 시각적 레이어 id 는 레이어 정체성 네임스페이스를 공유합니다. 오디오 클립 id 는 모든 오디오 트랙에서 고유합니다. 클립은 오버랩 없이 한 트랙 내에서 시작 프레임 순서로 정렬됩니다; 별도의 트랙은 오버랩할 수 있습니다. 구간은 `[startFrame, startFrame + durationFrames)` 반개방이며, 양의 지속 시간과 문서 프레임 수보다 이전 또는 같은 종료 프레임을 가집니다. 모든 클립은 오디오 자산을 참조합니다. `sourceOffsetSamples` 은 채널별 소스 샘플 프레임에 대한 음이 아닌 트리밍이며, 스칼라 인터리브 샘플이나 타임라인 프레임이 아닙니다. 필요한 소스 샘플 프레임은 `durationFrames * sampleRate * frameRateDenominator / frameRateNumerator` 의 올림값이며, 트리밍과 그 지속 시간이 자산을 포함하여 무음 또는 비활성화된 클립에도 맞아야 합니다. 이득은 유한하며 -96 에서 24 dB 사이입니다. 무음 및 활성화 플래그는 소스 미디어, 타이밍, 또는 이득 값을 삭제하지 않습니다.

오디오는 시각적 고정 키와 시각적 프레임 렌더링에 독립적입니다. 그 트랙/클립 정보는 명시적인 오디오 교환을 통해 투사될 수 있으며, 네이티브 데이터는 이 컨테이너에서 권위 있고 무손실로 유지됩니다.

<a id="stable-diffusion-generation-metadata"></a>

## 안정적인 확산 생성 메타데이터

선택적 버전 1.2 레코드는 정확히 이 순서대로 인코딩됩니다:

~~~text
string positivePrompt
string negativePrompt
bool hasOutputExtent
[if hasOutputExtent]
  u32 outputWidth
  u32 outputHeight
OptionalU32 batchSize
OptionalU32 clipSkip

u32 samplingPassCount
repeat samplingPassCount:
  string nodeId
  OptionalU64 seed
  OptionalU32 steps
  OptionalF64 cfgScale
  string samplerName
  string scheduler
  OptionalF64 denoiseStrength
  OptionalU32 startStep
  OptionalU32 endStep

u32 modelCount
repeat modelCount:
  string role
  string name
  string hash
  string hashType
  string uri

u32 loraCount
repeat loraCount:
  string name
  string hash
  f64 modelStrength
  f64 clipStrength

string software
string softwareVersion
string createdAt
string generationParametersText
string comfyUiPromptJson
string comfyUiWorkflowJson

u32 comfyUiExtraPngInfoCount
repeat comfyUiExtraPngInfoCount:
  string key
  string jsonValue

u32 extraParameterCount
repeat extraParameterCount:
  string key
  string value
~~~

`OptionalU32`, `OptionalU64`, 및 `OptionalF64`는 지정된 프리미티브가 존재할 때만 뒤에 따라오는 정규 부울 존재 바이트입니다. 출력 범위, 배치 크기, CLIP 스킵 및 단계 수가 존재할 경우 양수입니다. CFG는 유한하고 비음수이며, 제소음 강도는 0부터 1까지 유한합니다; 시작 단계는 종료 단계를 초과하지 않습니다. 샘플링 패스에는 최소 하나의 설정이 포함되어 있습니다.

모델 리소스는 역할과 이름이 필요합니다. 역할은 체크포인트, VAE, ControlNet 또는 다른 소비자가 이해한 모델 클래스를 식별할 수 있습니다. 해시, 해시 타입 및 URI는 신뢰할 수 있는 해결자가 아니라 출처 문자열입니다. LoRA의 강도는 유한합니다. 이 레코드에는 체크포인트, VAE, LoRA, 임베딩 또는 사용자 정의 노드 바이너리가 삽입되지 않습니다.

그들의 ComfyUI `prompt` 및 `workflow` 값은 독립적이고 선택적인 JSON 객체입니다. 그들의 UTF-8 바이트는 정확히 유지되며, 작성자는 이를 파싱하고 다시 방출하지 않습니다. `prompt` 는 API 실행 그래프를 나타내고 `workflow` 는 UI -복원 그래프를 나타냅니다. 추가 PNG -정보 키는 비어있지 않고 고유하며, 예약된 `prompt` 또는 `workflow` 이름이 될 수 없습니다; 그 값은 완전한 JSON 값입니다. 일반적인 추가 매개변수 키는 비어있지 않고 고유하며, 그 값은 불투명한 UTF-8 입니다.

첨부된 메타데이터 레코드에는 최소 하나의 프롬프트, 설정, 리소스, 호환성 페이로드 또는 확장이 포함되어야 합니다. 신뢰할 수 없는 저작 및 출처 데이터입니다: 디코딩은 그래프를 실행하지 않으며, URI를 해석하거나, 모델을 로드하거나, 렌더링된 픽셀을 변경하지 않습니다.

<a id="render-contract"></a>

## 계약 렌더링

래스터와 벡터 자산은 최근접 이웃 아핀 샘플링을 사용하고 문서 캔버스에 클립합니다. 단일 행렬은 빈 층 풋프린트를 생성합니다. 벡터 채우기는 짝수-오드 규칙을 사용하고, 스트로크는 둥근 풋프린트를 사용합니다. M/L/Q/C/Z 경로는 픽셀당 4x4 커버리지 그리드를 적용한 결정론적 CPU 래스터화를 사용합니다. 해결된 레이어는 iiPaintEngine 불투명도와 블렌드 의미론을 사용하여 아래에서 위로 합성됩니다.

레이어 병렬 렌더링은 지속된 레이어 순서를 변경하지 않습니다. 격리된 레이어 타일, 작업자 완료 순서, 상주 텍스처 캐시 및 최종 구성 타일은 런타임 상태만 해당됩니다. 컴포지션은 불투명도와 블렌드 모드를 적용하기 전에 항상 인코딩된 바텀‐탑 순서를 복원합니다.

<a id="authoring-only-state"></a>

## 저작 전용 상태

`TimelineProject`는 `.iisc` 버전 1.4에 의해 인코딩되지 않았습니다. 비디오 편집 모델은 위의 캔버스 페이로드에 속하지 않는 참조와 미디어 타이밍을 가진 여러 시퀀스, 소스 표현, 타입 스트림, 트랙, 클립, 효과, 마커 및 렌더 프로파일을 가지고 있습니다. 내구성이 뛰어난 비디오 프로젝트 스토리지를 추가하려면 명시적인 자원 제한이 있는 별도의 버전 형식과 무손실 왕복 변환 테스트가 필요합니다; 단순히 메모리 내 모델을 추가하는 것만으로는 `CurrentFormatMinor`가 변경되지 않습니다.

`TimelineProject` 의 컨테이너 및 코덱 설명자는 호스트가 프로브, 디코드, 인코드, 또는 믹스할 수 있다는 증거가 아닌 선언입니다. 이러한 작업은 어댑터 책임으로 남아 있으며 `.iisc` 에 숨은 상태를 기록하지 않습니다.

브러시 입력은 영속화된 콘텐츠 종류가 아닙니다. `BitmapEditor` 은 포인터 제스처가 활성 상태인 동안에만 일시적인 iiPaintEngine dab 스트림을 유지할 수 있습니다. `BitmapItem` 와 `CanvasItem` 뷰포트 상태, 레이어 선택, 되돌리기 이력, 입력 이벤트, 및 UI 도구 상태는 인코딩되지 않습니다.

`CameraRawData` 는 `.iisc` 버전 1.1 에 의해 인코딩되지 않으며 1.4버전 바깥에 남아 있습니다. 그것은 센서 샘플과 캡처/교정 메타데이터를 포함하는 디코드/가입 상태이며, 캔버스 자산이나 세 번째 레이어 종류가 아닙니다. 호스트는 결과가 문서에 속하는 경우 명시적으로 ARGB 픽셀로 처리한 후 `RasterAsset` 를 생성해야 합니다. 현재 컨테이너에 제조업체 RAW 바이트, CFA 페이로드, RAW 프로필, 또는 암시적인 디모자이크링 설정이 추가되지 않습니다.

<a id="default-reader-limits"></a>

## 기본 독자 제한

`SerializationLimits`는 호출자가 설정할 수 있다. 기본값은 다음과 같다:

|리소스|기본 최대값|
| --- | ---: |
|컨테이너 바이트| 1 GiB |
|캔버스 또는 벡터 뷰포트 픽셀|256 Mi 픽셀|
|총 래스터 픽셀|256 Mi 픽셀|
|희소한 래스터 청크| 1,048,576 |
|자산| 65,536 |
|레이어| 65,536 |
|키를 보유한 희박한 프레임| 262,144 |
|하나의 문자열| 1 MiB |
|생성 메타데이터 문자열 하나| 16 MiB |
|총 문자열 바이트 수| 64 MiB |
|패스, 모델, LoRAs 및 확장 목록 전반의 생성 메타데이터 항목| 65,536 |
|벡터 경로| 1,048,576 |
|경로 명령| 16,777,216 |
| Keyframes | 16,777,216 |
|오디오 자산| 65,536 |
|오디오 트랙 레이어| 65,536 |
|전체 오디오 클립| 16,777,216 |
|총 인터리브된 PCM16 스칼라 샘플| 268,435,456 (512 MiB) |

리더는 대응하는 컬렉션이나 픽셀 버퍼를 할당하기 전에 선언된 개수와 집계 합계를 확인합니다. 그것은 한계가 설정된 대기 중인 키프레임 에서 고유한 희소 프레임 개수를 유도하며, `Document::frames` 를 예약하거나 구체화하기 전에 프레임 한계를 확인합니다. 해독이 각 프레임 소유 `Keyframe` 에 포함 레이어 id 를 구체화하기 때문에, 총 문자열 제한은 인코딩과 디코딩 양쪽에서 파생된 복사본도 포함하며, 따라서 작은 와이어 기록은 무한히 반복되는 레이어-id 저장으로 증폭될 수 없습니다. 제품은 해당 장치 클래스의 이러한 한도를 낮출 수 있습니다. 레이어 범위 끝점들은 컬렉션을 할당하지 않지만, 디코딩된 문서가 노출되기 전에 양쪽 끝점이 모두 타임라인의 한계가 설정된 `frameCount`에 대해 확인됩니다. 오디오 ID/이름은 기존 문자 제한을 공유합니다. 오디오 샘플 및 클립 총계는 모든 자산/트랙에 걸쳐 누적됩니다. 샘플 카운트는 한계가 설정된 한계에 도달하기 전에 샘플 제한과 컨테이너 바이트 제한의 절반에 의해 제한되며, PCM 할당 전에 독자가 선언된 오디오 컬렉션이 남은 페이로드에 맞는지 확인한 후 이를 예약합니다. 확인된 나눗셈은 바이트 크기 확인에서 오버플로우를 방지합니다.

<a id="compatibility-and-migration"></a>

## 호환성 및 마이그레이션

- 독자는 현재 메이저 버전을 수락하고, 해당 버전보다 새로운 마이너 버전은 수락하지 않습니다.
- 새로운 메이저 또는 마이너는 페이로드 파싱 전에 실패합니다.
- 헤더 플래그와 예약된 필드는 향후 옵트인 변경 사항을 명시적으로 표시합니다.
- 작성자는 구현된 버전에 대해 하나의 정형 표현을 발행합니다.
- 마이그레이션은 래스터 픽셀, 벡터 기하학, 레이어 순서, 변환, 및 타임라인 참조를 보존해야 합니다. 그것은 절대 아무런 알림 없이 벡터 자산을 래스터화하거나 애니메이션을 버리거나 또는 유지된 브러시 궤적을 도입해서는 안 됩니다.

버전 1.0 는 첫 번째 물리적 형식입니다. 버전 1.1 은 유한/무한 모드, 할당된 영역의 세계 기원, 섹트 크기, 그리고 표준 희소 래스터 자산(래스터) 을 추가합니다. 1.0 문서가 0 에서 디코딩될 때 픽셀을 변경하거나 렌더링하지 않고 유한 모드로 마이그레이션됩니다. Version  1.2  선택적 Stable Diffusion 생성 메타데이터를 추가합니다. 선택적 값이 없는 경우 1.0 또는 1.1 문서가 해당 선택적 값을 사용하지 않고 그대로 디코딩된 후 바이트 동일하게 다시 인코딩되지만, 호출자가 명시적으로 메타데이터를 첨부하지 않는 한 버전 1.3 는 각 레이어 기록에 하나의 선택적 포함 존재 범위를 추가합니다. 레이어 1.0, 1.1또는 1.2 는 범위가 없이 디코딩되고 바이트 동일하게 다시 인코딩되지만, 범위를 추가하려면 문서를 버전 1.3로 업그레이드해야 합니다. 버전 1.4 는 네이티브 오디오 자산과 트랙 레이어를 추가합니다. 문서를 버전 1.0 에서 버전 1.3 로 업그레이드해야 디코딩 시 빈 오디오 컬렉션이 있고 다시 인코딩 시 바이트 동일하게 유지됩니다. 오디오를 추가하려면 문서를 버전 1.4로 업그레이드해야 합니다. `DocumentEditor` 는 편집된 문서를 현재 마이너 버전으로 원자적으로 업그레이드합니다. 미래 마이너 또는 메이저 버전 지원은 명시적인 디코더와 정형화된 다시 인코더 및 렌더링된 프레임 동등성 테스트가 필요합니다.

각 메모리 내 레이어 소스의 키프레임 소유권을 `Document::frames` 로 이동하면 버전-1 와이어 레이아웃이 변경되거나 마이너 버전이 증가하지 않습니다. 레이어 메이저 기록과 프레임 메이저 집계는 동일한 `(layerId, frame, assetId)` 관계의 손실 없는 전치입니다.

프레임 소유 키를 도입한 공개 C++ 집계 변경 사항은 패키지 0.2.0 와 SOVERSION 0.2 로 배포되었으며 0.1.x 에서부터 소비자 재빌드가 필요합니다. 집계 레이아웃을 다시 변경하는 `LayerProperties::frameRange` 변경 사항이 추가되었으므로 패키지 0.3.0 와 SOVERSION 0.3 로 배포되었으며 0.2.x 에서부터 소비자 재빌드가 필요합니다. 파일 기반 편집은 패키지 0.4.0 와 SOVERSION 0.4 에 별도의 작업 파일 소유자를 추가하며 해당 패키지에 대해 소비자를 재빌드합니다. 이 캐노니컬 스냅샷 바이트를 변경하지 않습니다. 패키지 ABI 버전화는 이 파일 형식과 별개입니다: `.iisc` 는 이제 1.17가 되었으며, 캐노니컬 1.0, 1.1, 1.2, 및 1.3 픽스처 은 여전히 바이트 동일하게 다시 인코딩됩니다.

패키지 0.5.0 의 미디어 교환은 스냅샷 와이어 형식이나 작업 파일 스키마를 변경하지 않습니다. 외부 이미지/비디오는 커밋된 ARGB 래스터 자산과 프레임 소유 키가 되며, SVG 는 네이티브 경로 명령어가 됩니다. 원본 코덱 바이트, SVG 마크업, 패킷 타임스탬프 및 디코더 상태는 `.iisc` 에 내장되지 않습니다. 나중에 1.4 모델은 명시적인 네이티브 오디오 데이터를 추가하며, 비디오 가져오기 어댑터는 영화의 오디오를 암묵적으로 가져오지 않습니다. SVG / PDF /이미지/비디오 내보내기는 명시적으로 별도의 교환 작업이며, 작업 파일의 쓰기 통과 편집을 결코 대체하지 않습니다.

레이어 보존 가져오기는 패키지 0.6.0 에서도 스냅샷 버전 1.3 와 작업 파일 스키마 1 를 변경하지 않습니다. 지원되는 OpenRaster 와 PSD 픽셀 레이어는 새로 작성된 캔버스와 같은 일반적인 정적 `StaticBitmapLayer` / `RasterAsset` 쌍이 됩니다. 레이어 속성과 커밋된 픽셀은 새로 작성된 캔버스와 같은 동일한 네이티브 기록을 사용하며, 외부 ZIP / PSD 페이로드, 편집기별 객체 및 지원되지 않는 그리기 의미는 내장되거나 아무런 알림 없이 미리보기로 대체되지 않습니다. `iisc-import` 유틸리티는 `DocumentFile::create` 를 통해 새로운 작업 파일을 생성합니다.

PSD 내보내기는 패키지 0.7.0 에서도 스냅샷 버전 1.3 와 작업 파일 스키마 1 를 변경하지 않습니다. `Document::timeline` 를 0프레임에서 투영합니다. 내장된 PDF 스마트 오브젝트 페이로드들은 내보낸 PSD 에만 존재하며, 네이티브 자산에는 절대 존재하지 않습니다. 네이티브 경로, 미래 키프레임, 레이어 수명 및 캔버스 기점은 공식적인 `.iisc` 에 유지됩니다; PSD 는 네이티브 왕복 변환 컨테이너가 아닌 인터체인지 스냅샷입니다. 별도의 `TimelineProject` 오디오/비디오 모델은 내보내지지 않습니다.

패키지 0.8.0 의 타임라인 인터체인지는 네이티브 포맷을 그대로 유지합니다. 버전-1 `manifest.json` 는 정확한 프레임 간격, 유리수 프레임 속도, 레이어 ID, 자산 ID, 생성된 미디어 경로 및 투영 경고 기록을 포함합니다. 레거시 XML 와 FCPXML 는 독립적인 PNG 레이어 상태를 참조하며, `source.iisc` 는 완전한 정통 네이티브 문서를 유지합니다. 이는 아웃바운드 편집자 교환이며, XML 에서 네이티브 왕복 변환 나 TimelineProject 직렬화 가 아닙니다.

<a id="authorship-extension-15"></a>

## 저작자 확장 ( 1.5 )

1.4 오디오 트랙 섹션 이후, 1.5 는 길이 접두사가 붙은 UTF-8 문자열 ( `u32` 바이트 길이, 바이트) 하나를 추가하며, 이는 iiFileProvider `Authorship` JSON 덤프를 포함합니다. 레코드당 한도는 1 MiB 이며, 이는 집계 문자열/컨테이너 한도에 참여합니다. 수정은 정확한 부호 없는 십진 문자열이며, 기여자 프로필, 첫 번째/마지막 기여 시간 및 최신 기여자 참조를 포함합니다. 이 스키마에는 인증 필드가 존재하지 않습니다. 잘못된 JSON, 알 수 없는 스키마, 중복 저자, 유효하지 않은 식별자/시간 및 미래 네이티브 버전은 거부됩니다. 비어있는 장부들은 명시적으로 인코딩됩니다. 소 버전이 5미만인 경우 비어 있지 않은 저자ship은 인코딩될 수 없습니다. 전통 버전은 편집될 때까지 바이트 동일인 코딩을 유지합니다.

<a id="native-video-and-motion-extension-16"></a>

## 네이티브 비디오 및 모션 확장 (1.6)

마이너 6는 자산 태그 `3`를 추가합니다: 일반 자산 ID 뒤에 `u32 rateNumerator`, `u32 rateDenominator`, `u32 frameCount`를 입력하고, 그 다음에 많은 일반 래스터 레코드를 기록합니다. 모든 소유 프레임은 동일한 양의 범위를 가지고 있습니다. 프레임 수는 새로운 `maximumTotalVideoFrames` 예산을 공유하고, 픽셀은 `maximumTotalRasterPixels`와 공유합니다. 정적 소스 참조 태그 3가 `VideoLayer`를 재구성합니다. 키프레임이 적용된 소스 콘텐츠 태그는 래스터 /vector only로 유지되며, 비디오는 자체 시간 샘플링을 사용합니다.

각 레이어의 1.3 옵션 프레임 범위 뒤에, 마이너 6는 `u32 motionKeyCount`를 씁니다. 각 키는 `u32 frame`, 6 `f64` 값(위치 x/y, 스케일 x/y, 앵커 x/y), `f64 rotationDegrees`, `f64 opacity`, 그리고 `u8 interpolation`(0 Hold, 1 Linear, 2 SmoothStep)입니다. 이 키들은 엄격히 정렬되어 있으며, 타임라인 - 한계가 설정된. 그들은 자산 참조 키와 무관하게 `maximumTotalMotionKeyframes`를 사용합니다.

비디오 레이어에만 `u32 sourceInFrame`, Boolean `hasSourceOutFrame`, 선택적 `u32 sourceOutFrame`(전용) 및 `u8 endBehavior`(0 Transparent, 1 Hold)를 추가하십시오. 메타데이터, 오디오 및 저작자를 포함한 모든 다른 섹션은 이전 주문을 유지합니다. 알 수 없는 태그와 잘못된 참조 안전하게 거부한다. 모델 1.0 – 1.5는 비디오 또는 모션 필드를 포함하지 않을 수 있으며, 인코딩은 변경되지 않습니다. 샘플링 의미에 대해서는 [CANVAS_MEDIA .md](CANVAS_MEDIA.md)를 참조하십시오.

<a id="public-layer-classification-package-0120"></a>

## 공개 레이어 분류 (패키지 0.12.0)

4   비트맵 /vector  `LayerKind`  값은 새로 생성된 와이어 태그가 아닌 파생됩니다.  비트맵 /vector 레이어는 정적 소스가 있는 경우  StaticBitmap / StaticVector ;  키프레임이 적용된  소스는 키 하나라도  DynamicBitmap / DynamicVector  입니다. 비디오 레이어는  DynamicBitmap  입니다. 왜냐하면 소유된 픽셀 프레임이 타임라인 시간에 따라 변경되기 때문입니다. 기존 스냅샷 및  SQLite  필드는 형식 버전을 변경하지 않고 이 의미를 완전히 보존합니다.

<a id="controlnet-semantic-layer-extension-17"></a>

## ControlNet 의미 계층 확장 ( 1.7 )

선택적 프레임 범위 이후 및 모션 키 이전, 모든 레이어는 하나 u8  역할 태그를 추가합니다:  0  는 일반 예술 작품이며,  1  는 의미론적  ControlNet  레이어입니다. 알 수 없는 태그  안전하게 거부한다 . 태그  1  는 밀집  래스터  콘텐츠에만 합법적이며 다음에 따라집니다:

1. Boolean이 활성화되었습니다; 모델 ID/수정 문자열; f64 스케일, 가이드 시작/끝.
2. 분류 ID/버전/소스 URI 문자열; u32 무효 마스크/제어 색상.
3. U32 클래스 수. 각 클래스: u32 id; key/name/description/category/external-id 문자열; optional-u32 parent; u32 제어 색상; u32 별칭 카운트 및 문자열.
4. U32 지역 수. 각 지역: u32 id/class-id/mask-color; name/description/generator/source-reference 문자열; optional-u32 인스턴스 ID; optional-f64 신뢰도; u8 원산지 (0 매뉴얼, 1 import, 2 모델); u32 속성 카운트 및 키/값 문자열 쌍.

기존 모션 키와 비디오 재생 필드는 그대로 유지됩니다. 모든 정수 및 선택적/문자열 인코딩은 기존의 리틀 엔디안 프리미티브를 재사용합니다. 컬렉션 총계와 문자열 바이트는 인코더/디코더 리소스 제한을 공유합니다. 동일한 확장자는 작업 파일 레이어 레코드에 적용되므로, 의미 전용 업데이트는 변경되지 않은 래스터 레코드를 유지합니다. 모델은 SEMANTIC_SEGMENT .md에 문서화되어 있습니다.

<a id="native-pose-extension-18"></a>

## 네이티브 포즈 확장 ( 1.8 )

자산 태그  4: id 문자열, i32   뷰포트  너비/높이, u32  사람 수. 사람당: id/이름/트랙-id 문자열, 활성화된 불리언, 정확히  590  앵커가  PoseGroup   V1  순서로, 그 다음 u32  표현 수. 각 앵커는 f64  x/y, 선택적 f64  z, f64  신뢰도, u8  가시성 ( 0  미삭제,  1  삭제됨,  2  가려짐), 불리언 잠금. 각 표현은 id/이름 문자열, f64  가중치, u32  델타 수; 각 델타는 u8  그룹, u32  앵커 인덱스, f64  dx/dy/dz. 모든 기존 엔디안/선택적 규칙이 적용됩니다. V1  는  32  이름이 붙은 그룹을 가집니다; 크기와 해부학적 순서는  POSE .md 와  Pose.h . 에 있습니다.

키프레임이 적용된 소스 콘텐츠 종류 2는 포즈를 나타내며, 정적 종류는 해당 자산으로부터 추론됩니다. 레이어 역할 태그 2는 Pose 콘텐츠 및 버전 >= 1.8인 경우에만 합법입니다. 활성화된 부울, 모델 ID/리비전 문자열 및 f64 스케일/시작/엔드를 기존 모션 키 앞에 저장합니다. 알 수 없는 태그/가시성/그룹 안전하게 거부한다 . 작동하는 자산/계층 레코드는 동일한 문법을 사용하며, SQLite 스키마 변경이 없습니다.

<a id="depth-extension-19"></a>

## 깊이 확장 ( 1.9 )

자산 태그 5는 id, i32 width/height, u64 카운트와 카운트 리틀 엔디안 f64 정규화된 근접 샘플을 저장합니다. 카운트는 너비 * 높이와 같습니다; 유한한 값은
[0,1]만 0는 비어 있음을 의미하고, 하나는 카메라 접촉을 의미합니다. 키프레임이 적용된 콘텐츠 태그 3는 DepthAsset가 필요하고, 레이어 역할 태그 3는 깊이 콘텐츠가 필요하며, 동작 전 Pose와 동일한 ControlNet 필드를 저장합니다. 태그는 1.9이전에 불법입니다. 글로벌 maximumDepthSamples 예산과 남은 바이트 수는 할당 전에 적용됩니다. [DEPTH .md](DEPTH.md)를 참조하십시오.

<a id="line-art-extension-110"></a>

## 라인 아트 확장 ( 1.10 )

자산 태그  6  는 id, i32  너비/높이, u64  수 및 행 우선 리틀 엔디안 f64  잉크 커버를  [0,1]에 저장합니다. 0 는 흰색 배경이며, 하나는 검은색 잉크입니다. 카운트는 너비 * 높이와 같아야 합니다. 키프레임이 적용된 콘텐츠 태그 4 와 레이어 역할 태그 4 는 LineArt 콘텐츠를 요구합니다. 역할은 운동 전에 공유된 ControlNet 필드를 저장합니다. 이 태그들은 1.10이전에는 불법입니다. 전역 maximumLineArtSamples 와 남은 바이트는 할당 전에 확인되며, 문서 유효성 검사는 비유한 또는 범위 밖 샘플을 거부합니다. [LINE_ART .md](LINE_ART.md)를 참조하세요.

<a id="cannyscribble-extension-111"></a>

## Canny/Scribble 확장 ( 1.11 )

자산 태그 7/8 는 Canny/Scribble 입니다. 각각은 id, i32 너비/높이, u64 카운트 및 u8 바이너리 샘플 ( 0 또는  1 )을 저장하며, ARGB 또는 f64를 저장하지 않습니다. 카운트는 너비 * 높이와 같아야 합니다. 키프레임이 적용된 콘텐츠와 레이어 역할 태그 5/6 는 일치하는 Canny/Scribble 자산 유형을 요구합니다. 역할은 운동 전에 공통 ControlNet 필드를 저장합니다. 이 태그들은 1.11가 필요합니다. 종류별 전역 maximumCannySamples 와 maximumScribbleSamples 예산, 주소 공간 및 사용 가능한 바이트는 할당 전에 확인됩니다. 비이진 값은 문서 유효성 검사를 실패합니다. 보라
[BINARY_LINE_CONTROL.md](BINARY_LINE_CONTROL.md).

<a id="mlsd-extension-112"></a>

## MLSD 확장 ( 1.12 )

자산 태그 9 는 id, i32 너비/높이, u32 섹먼트 수를 저장한 후 각 섹먼트: 길이 접두사 id, f64 x1/y1/x2/y2/신뢰도, u8 활성화.
[0,1] 는 [0,1]에서 서로 다른 위치와 신뢰도를 가집니다. ID 는 고유/비어 있지 않습니다. 키프레임이 적용된 콘텐츠와 레이어 역할 태그 7 는 MLSD 자산과 공통 ControlNet 설정을 운동 전에 저장합니다. 태그들은 1.12를 요구합니다. maximumMlsdSegments 는 모든 자산을 함께 캡니다; 하나의 자산은 최대 100000 섹먼트를 허용합니다. 할당 전에 남은 바이트가 확인됩니다 ( 45 섹먼트당 최소 바이트). [MLSD .md](MLSD.md)를 참조하세요.

<a id="normal-map-extension-113"></a>

## 노멀맵 확장 ( 1.13 )

자산 태그 10 는 id, i32 너비/높이, u64 샘플 수를 저장한 후 행 우선 샘플 f64 x, f64 y, f64 z, u8 유효 (엄격히 0 또는 1 ): 25 바이트당 샘플. 개수가 *  너비와 높이의 곱과 같아야 합니다. 유효한 유한한 구성 요소는 [-1,1] 에 있으며, 벡터 길이는 1 에서 1e-6만큼만 다릅니다. 무효/누락된 샘플은 모든0 구성 요소를 가져야 합니다. 인코드 또는 디코드 시 정규화가 발생하지 않습니다. 키프레임이 적용된 콘텐츠와 레이어 역할 태그 8 는 NormalMap 를 식별합니다. 모션 전에 모든 새 태그는 모델 ControlNet 설정을 저장합니다. 모든 새 태그는 모델 1.13를 요구합니다. `maximumNormalMapSamples` (기본값 16 Mi 샘플) 은 할당 전에 모든 자산의 합계를 제한하며, 총 입력 바이트 및 픽셀 차원 제한과 함께 작용합니다. 디코더는 예약 전에 남은 바이트 및 주소 가능 벡터 용량을 확인합니다. 출력 채널 순서 및 Y 반전은 호출 사이트 옵션이며 영구 저장된 네이티브 데이터가 아닙니다. [NORMAL_MAP .md](NORMAL_MAP.md)를 참조하세요.

<a id="shuffle-extension-114"></a>

## 셔플 확장 ( 1.14 )

자산 태그 11 는 id, i32 너비/높이, u64 개수를 저장한 후, 행 우선 u8 빨강, u8 초록, u8 파랑 ( 3 픽셀당 바이트) 을 저장합니다. 양수 차원은 개수를 곱해야 합니다. 키프레임이 적용된  콘텐츠와 역할 태그  9  는 셔플 자산과 모델 버전  1.14 를 필요로 하며, 일반적인  ControlNet  설정은 모션 앞에 선행합니다. `maximumShuffleSamples`  ( 16  Mi 기본값)은 자산 전체의 합계를 제한합니다. 해독은 예약 전에 개수, 남은 바이트 및 주소 가능 용량을 확인합니다. 정규 데이터는 준비된  RGB  이미지이며, 소스 이미지, 리매핑 필드 및 무작위 생성기 상태는 지속되지 않습니다. SHUFFLE .md 를 참조하세요.

<a id="tile-extension-115"></a>

## 타일 확장 ( 1.15 )

자산 태그  12  는 id, i32  너비/높이, u64  색상 개수를 저장한 후, 행 우선 u8  빨강, u8  초록, u8  파랑 ( 3  픽셀당 바이트)을 저장합니다. 차원은 양수여야 하며 그 곱은 개수와 일치해야 합니다. 키프레임이 적용된  콘텐츠와 역할 태그  10  는 타일 자산과 모델  1.15를 필요로 합니다. 역할은 모션 앞에 일반적인  ControlNet  설정을 저장합니다. `maximumTileSamples`  ( 16  Mi 기본값)은 모든 자산의 타일 색상을 제한하며, 할당 전에 남은 바이트와 벡터 용량을 확인합니다. 분할 계획, 겹침, 업스케일 인자 또는 추론 상태는 직렬화되지 않습니다. 공간 참조에서 파생된 소유 복사본인 잘린 출력은 추가적으로 영속화된 자산이 아닙니다. [TILE .md](TILE.md)를 참조하세요.

<a id="reference-extension-116"></a>

## 참조 확장 ( 1.16 )

자산 태그 13 는 id, i32 너비/높이, u64 개수, 그 다음 행 우선 RGB8 삼중항을 저장합니다. 양수 차원은 개수를 곱해야 합니다. 키프레임이 적용된 콘텐츠와 역할 태그 11 는 참조 자산과 버전 1.16를 요구합니다. 일반적인 ControlNet 설정 이후, 역할은 u8 모드 ( 0 주의,  1   AdaIN ,  2   AttentionAdaIN ) 및 f64   styleFidelity (유한한 [0,1])을 저장한 후 운동이 시작됩니다. 알 수 없는 모드 안전하게 거부한다 입니다. `maximumReferenceSamples` (기본값 16 Mi) 는 모든 참조 자산에 걸친 총 색상 수를 제한합니다. 할당 전에 개수, 남은 바이트 및 벡터 용량이 확인됩니다. SQLite 스키마는 1 로 유지됩니다; 임베딩이나 모델 텐서는 영속화되지 않습니다. [REFERENCE .md](REFERENCE.md)를 참조하세요.

<a id="ip-adapter-extension-117"></a>

## IP - 어댑터 확장 ( 1.17 )

자산 태그 14 는 id, u8 임베딩 단계 ( 0 풀링된 인코더,  1 인코더 숨겨진 상태,  2 투영된 이미지 프롬프트 토큰), 그 다음 순서대로 6 문자열을 저장합니다:  encoderId ,  encoderRevision ,  adapterId ,  adapterRevision ,  baseModelId ,  preprocessingId 입니다. 모두 필수입니다. 텐서는 u32   tokenCount , u32   channelCount , u64 스칼라 개수를 저장한 후 리틀엔디안 IEEE 바이너리32 값을 저장합니다. 개수는 tokenCount * channelCount 와 같아야 하며 두 차원은 양수여야 하고 모든 값은 유한해야 합니다. 조건부 텐서는 엄격한 u8 무조건 존재 플래그와 존재할 경우의 그 텐서가 뒤를 잇습니다. 가지들은 모양을 공유하며 풀링된 인코더 임베딩은 하나의 토큰을 가집니다. 배치는 암묵적으로 하나입니다.

콘텐츠 태그 12 와 역할 태그 12 는 IP -어댑터 레이어를 식별합니다. 역할 이후 일반 ControlNet 설정이 저장된 다음 모션이 저장됩니다. 어댑터 id/리비전은 참조된 모든 자산과 일치해야 하며, 동적 레이어의 설명자와 모양은 고정되어 있습니다. `maximumIpAdapterValues` (기본값 16 Mi 스칼라) 는 모든 자산과 두 가지 가지의 합계를 계산하며, 문자 제한은 모든 기원 필드를 포함합니다. 할당 전에 개수, 페이로드 가용성 및 주소 공간이 확인됩니다. 임베딩은 1.17 를 필요로 하며, 알 수 없는 태그/단계와 오래된 잘못 표시된 파일은 안전하게 거부한다 안전하게 거부합니다. SQLite 스키마는 1입니다.

## Artboard extension (1.18)

Immediately after each layer blend-mode byte, 1.18 writes a bool ownership flag and,
when true, a string artboard id. After the authorship string, the payload appends
u32 artboardCount, then id string, name string, i32 x, i32 y, i32 width, i32 height,
u32 backgroundArgb and bool visible for each board in bottom-to-top order.
Pre-1.18 records omit these fields and retain canonical byte compatibility.
Unknown versions, invalid membership, duplicate ids, bounded-geometry violations
and configured artboard/string/pixel limits are rejected. See [ARTBOARDS.md](ARTBOARDS.md).

## Explicit layer type extension (1.19)

After optional artboard membership, each record stores `visualLayerKind` with
0 = StaticBitmapLayer, 1 = StaticVectorLayer, 2 = DynamicBitmapLayer,
3 = DynamicVectorLayer and 255 = nonspatial IP-Adapter conditioning. Source
references and role/media payloads follow in their existing layout. Unknown tags
and declared-type/content mismatches are invalid data, including with a valid CRC.
The four concrete artwork types own their corresponding distinct content types.
See [FOUR_LAYER_TYPES.md](FOUR_LAYER_TYPES.md) for the public model and migration.

Versions 1.0–1.18 omit this byte. Their source/asset/role fields construct the
correct concrete memory type and untouched legacy bytes remain canonical. The
working-file schema stays 1; its layer record uses model version 1.19.
