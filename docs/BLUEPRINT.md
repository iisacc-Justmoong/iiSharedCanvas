<a id="iisharedcanvas-blueprint"></a>

# iiSharedCanvas 설계도

상태: 0부터 3까지의 단계가 완료되었습니다. 한계가 설정된 대형 캔버스 상호 작용 완료; 크로스 플랫폼 제품 강화는 계속 열려 있습니다.

<a id="1-product-objective"></a>

## 1. 제품 목표

하나의 캔버스 문서는 이러한 콘텐츠 클래스를 레이어 순서로 함께 표시해야 합니다.

1. iiPaintEngine 브러시로 생성된 픽셀을 포함한 정적 래스터 픽셀.
2. 정적 네이티브 벡터 경로.
3. 래스터 또는 타임라인에서 키프레임가 선택한 벡터 자산입니다.

인접한 가져오기 모델은 호출자가 명시적으로 이를 커밋된 래스터 픽셀로 처리할 때까지 디코딩된 카메라 RAW 센서 샘플과 공통 캡처/보정 메타데이터를 전달할 수 있습니다. RAW 센서 데이터는 세 번째 문서 레이어 유형이 아닙니다.

선택 가능한 문서 레시피는 Stable Diffusion 프롬프트, 샘플러 설정, 모델 식별자, 정확한 ComfyUI 그래프 메타데이터, 그리고 손실 없는 Stable Diffusion 생성 매개변수 텍스트를 유지할 수 있습니다. 이 레시피는 콘텐츠가 어떻게 생성되거나 정제되었는지를 설명하지만 렌더링된 콘텐츠가 아니며 추론 런타임 런타임에 추가하지 않습니다.

첫 번째 상업적 이점은 각 제품이 고유한 혼합 미디어 문서 모델을 만들도록 강요하지 않고 미래의 데스크탑, 모바일 및 웹 제품을 위한 재사용 가능한 저작 교환 레이어입니다.

<a id="11-specification-authority"></a>

### 1.1 사양 권한

iiSharedCanvas는 iisacc의 표준 캔버스 표준입니다. `Document`, 렌더링 의미, 경계 편집, 유효성 검사 규칙 및 `.iisc` 인코딩은 상위 공급 측 계약입니다. 제품 애플리케이션은 해당 계약을 사용합니다. 그들은 공동 소유하거나 재정의하지 않습니다.

라이브러리는 이 저장소에서 증명된 재사용 가능한 캔버스 도메인 요구 사항에서 진화합니다. 제품별 ViewModels, QML 속성 이름, 세션 객체, 도구 관행 및 호환성 별명은 소비자 소유 어댑터에 속합니다. 제품 통합이 iiSharedCanvas 와 충돌하는 경우 제품을 먼저 변경합니다. 라이브러리는 요구 사항이 일반적이며, 특정 제품 참조 없이 설명되고, 자체 모델, 렌더링, 형식 및 패키지 게이트를 통과할 때만 변경됩니다.

개발과 채택은 별도의 단계를 사용합니다. iiSharedCanvas는 독립적으로 지정, 구현, 테스트, 버전 관리 및 설치됩니다. 그런 다음에야 고정 계약에 대한 소비자 채택 작업이 시작됩니다. 저장소 간 병렬 호환성 개발은 이 프로젝트 정책의 범위를 벗어납니다.

<a id="2-boundary-and-ownership"></a>

## 2. 경계 및 소유권

~~~mermaid
flowchart LR
    APP["Application / LVRS QML UI"] --> ITEM["BitmapItem"]
    APP --> CANVAS["CanvasItem / SharedCanvas"]
    APP --> ISC["iiSharedCanvas"]
    APP --> CRAW["CameraRawData import model"]
    APP --> GENPARAMS["StableDiffusionGenerationParameters parser"]
    APP --> SDMETA["StableDiffusionMetadata recipe"]
    CRAW -. "explicit RAW processing" .-> RASTER
    GENPARAMS --> SDMETA
    ITEM --> EDIT["BitmapEditor"]
    CANVAS --> EDIT
    EDIT --> DOC
    APP --> VEDIT["VectorEditor"]
    VEDIT --> DOC
    ISC --> DOC["Document + validation"]
    SDMETA --> DOC
    DOC --> STACK["Ordered layers"]
    DOC --> FRAMES["Sparse Frame owners"]
    DOC --> ASSETS["Asset registry"]
    STACK --> STATIC["Static source"]
    STACK --> ANIM["Keyframed source / derived frame index"]
    ASSETS --> RASTER["RasterAsset"]
    ASSETS --> CHUNKS["ChunkedRasterAsset"]
    ASSETS --> VECTOR["VectorAsset"]
    FRAMES --> KEYS["Directly owned keyframes"]
    KEYS --> EVAL["Frame evaluator"]
    ANIM --> EVAL
    STATIC --> EVAL
    EVAL --> RESOLVED["Resolved asset"]
    RESOLVED --> RASTER
    RESOLVED --> CHUNKS
    RESOLVED --> VECTOR
    VECTOR --> VRAST["Bounded CPU vector tile rasterizer"]
    RASTER --> FRAME["FrameRenderer"]
    VRAST --> FRAME
    FRAME --> IIPE["iiPaintEngine compositor"]
    CANVAS --> ASYNC["AsyncFrameRenderer"]
    ASYNC --> FRAME
    FRAME --> TILES["LOD texture tiles"]
    TILES --> GPU["Qt Quick scene graph / GPU transform"]
~~~

iiPaintEngine는 비트맵 코덱, 브러시 래스터화, ARGB 픽셀 저장 프리미티브, 래스터 혼합 의미 체계, 아핀 변환 유형 및 좌표 의미 체계를 소유합니다.

iiSharedCanvas 라스터는 혼합 콘텐츠 문서 식별자, 순서 있는 레이어, 벡터 기하학, 희소 무한 캔버스 섹션 좌표, 자산 참조, 타임라인 의미론, 교차 콘텐츠 검증, 파일 형식 버전 관리, Stable Diffusion 레시피 메타데이터, 선택된 래스터 편집 조정, 프레임 컴포지션 및 비선형 비디오 편집 프로젝트에 대한 형식 중립 데이터 계약을 소유합니다. 재사용 가능한 비트맵, SVG / SVGZ, PDF - 내보내기 및 캔버스 애니메이션 미디어 어댑터도 소유합니다. 그들은 문서 경계에서 외국 미디어를 변환하며, 자산에 외국 코덱 페이로드를 추가하거나 레이어에서 콘텐츠 종류를 혼합하지 않습니다. 코드크 구현 단위는 표준 C++ 공개 옵션/결과 뒤에 검토된 Qt 원시 데이터를 사용합니다. 도메인 모델, 편집기 및 렌더러는 직접적인 Qt 포함에서 독립적으로 유지됩니다. 프라이빗 코드크 헤더는 함께 위치한 `_p.hpp` 파일을 사용하며, 공개 API 로 설치되지 않습니다.

모든 지속된 모델 필드는 공개 집계 데이터입니다. `DocumentEditor` 는 해당 집계 위에 있는 안전한 구조적 변형 경계입니다: 이는 타임라인, 자산, 레이어, 키프레임, 및 벡터 경로에 대해 안정적인 ID 조회와 명시적인 삽입, 교체, 이름 변경, 이동, 및 제거 작업을 제공합니다. 모든 승인된 편집은 전체 문서 검증을 보존하며, 거절된 편집은 이전 값을 보존합니다. 유한한 비트맵 픽셀은 `BitmapEditor` 의 책임이며, 희소 무한한 비트맵 픽셀은 `ChunkedBitmapEditor` 의 책임입니다. 네이티브 M/L/Q/C/Z 명령 편집은 `VectorEditor` 의 책임입니다.

`DocumentFile` 는 쓰기 통과 작성 거래를 소유합니다. 모든 편집자 종류와 Qt 어댑터는 이에 바인딩할 수 있으므로, 편집 호출이 반환되기 전에 승인된 변경 사항은 영구적입니다. 그 커밋된 뷰는 const 이며, 임의의 사용자 정의 변경 사항은 동일한 검증된 `edit` 경계를 사용합니다. 물리적 기록 매핑, 실패 규칙, 및 레거시 인터체인지 분리는 PERSISTENCE .md 에 지정되어 있습니다.

애플리케이션은 UI, 도구, 재생 제어, 선택 경험, 작업 파일 경로 할당, 네트워킹, 협업, 및 선택된 카메라 RAW 디코더 및 처리 파이프라인을 소유합니다. 카메라 RAW 파일 디코딩, 디모자이크, 및 톤 렌더링은 일반적인 데이터 객체 바깥에 유지됩니다. Stable Diffusion 생성 메타데이터 캐리어 추출, 추론, 모델 해상도/다운로드, 그래프 실행, 및 신뢰 정책 또한 애플리케이션 또는 어댑터 책임으로 유지됩니다. 비트맵 텍스트 캐리어 추출 및 캔버스 애니메이션 탐지, 디코드, 인코드, 믹스 및 기능 발견은 라이브러리의 별도 미디어 어댑터에 의해 구현되며; 재생 및 `TimelineProject` 시퀀스/오디오 렌더링은 그 바깥에 유지됩니다. 라이브러리 이름은 실시간 협업이 이 마일스톤의 일부임을 암시하지 않습니다.

의존 방향은 일방향입니다:

~~~text
Application
  -> iiSharedCanvas
       -> iiPaintEngine
~~~

iiPaintEngine 는 iiSharedCanvas 를 절대 참조해서는 안 됩니다.

<a id="3-core-data-model"></a>

## 3. 핵심 데이터 모델

`CameraRawData` 는 `Document` 에서 독립적입니다. 그것은 서명되지 않은 정수 센서 페이로드 하나와 색상, 카메라, 렌즈, 및 캡처 메타데이터를 소유합니다. 그 센서 이미지는 CFA, 단색, 및 인터리브된 선형 RAW 를 구별하며; 비트 깊이, 방향, 활성 영역, 기본 컷, 인덱스 채널, CFA 및 블랙 레벨 반복 패턴, 및 화이트 레벨을 유지하고; 샘플을 행-픽셀-평면 순서로 저장합니다. 색상 프로파일은 선택적 애플리케이션 중립 좌표와 하나 이상의 유한한 XYZ -카메라 행렬을 유지합니다. 이 경계는 DNG, LibRaw, 또는 향후 플랫폼 디코더로부터 데이터를 받을 수 있으며, 어느 하나의 디코더를 핵심 모델로 만들지 않습니다.

Camera RAW 객체는 파일을 디코딩하지 않으며, 소스 파일 바이트를 보유하고, 선형화 또는 블랙 뺄셈을 적용하지 않으며, 데모사익, 색상 변환, 톤 맵을 수행하지 않고, `RasterAsset`를 생성하지 않으며, 레이어 스택을 입력하거나, `.iisc` 지속성을 변경하지 않습니다. 그것들은 디코더와 제품에 따라 정책이 달라지는 명시적인 파이프라인 단계입니다.

`StableDiffusionMetadata` 는 `Document` 에 의해 소유된 선택적 상태입니다. 타입화된 프롬프트, 출력 설정, 샘플러 패스, 모델 리소스, LoRAs, 소프트웨어 식별자, 및 확장 항목은 직접 검사 지원을 합니다. Raw ComfyUI `prompt` 및 `workflow` JSON 는 첫 번째가 API 실행 그래프이고 두 번째가 UI 그래프를 복원하기 때문에 서로 분리되어 있습니다. 문자들은 문법으로 검사되어 정확히 보존되며, iiSharedCanvas 는 사용자 정의 노드를 해석하거나 그래프를 실행하거나 모델 리소스를 가져오거나 특정 레이어에 레시피를 연결하지 않습니다.

`StableDiffusionGenerationParameters` 는 Stable Diffusion 생성 매개변수 텍스트에 대한 손실 없는 호환성 뷰입니다. 완전한 텍스트와 정렬된 키/값 시퀀스를 보존하면서 이동 가능한 프롬프트, 차원, 샘플링 패스, 모델 리소스, 및 버전 데이터를 `StableDiffusionMetadata` 로 투영합니다. 알 수 없는 확장 필드는 사용 가능하게 유지되며 잘못된 형식화된 데이터는 안전하게 거부한다 안전하게 거부합니다. 이미지 컨테이너와 모델 런타임 는 이 객체 바깥에 있습니다. 문법만으로는 생성자를 식별할 수 없으므로 소프트웨어 출처는 결코 추론되지 않습니다.

`TimelineProject`는 캔버스 `Document`와 독립적입니다. 원본 및 대체 표현, 타이핑된 미디어 스트림, 다중 시퀀스, 비디오/오디오/자막/데이터 트랙 및 클립, 전환, 효과 및 자동화, 마커, 링크 그룹, 빈 및 렌더 프로파일을 사용하여 미디어 소스를 모델링합니다. 안정적인 문자열 아이덴티티는 참조를 형성하고, 트랙 및 클립 변형은 콘텐츠 종류를 구조적으로 구분합니다.

타임라인 시간은 명시적인 유리수 시간 기준과 짝을 이룬 부호화된 틱입니다. 시퀀스 편집 FPS, 소스 명목/평균/최소/최대 FPS, 시간코드, 및 렌더링 출력 FPS 는 서로 다른 값입니다. 가변 프레임 레이트 비디오는 샘플당 PTS, DTS, 지속 시간, 키프레임, 오프셋, 및 크기를 유지할 수 있습니다. 컨테이너와 코덱은 오픈 식별자 및 형식화된 공통 필드와 정렬된 확장 옵션을 사용하므로 새로운 코덱이나 믹서도 닫힌 열거형을 확장할 필요가 없습니다. 대체 표현은 공유 논리적 스트림의 종류와 타이밍 신원을 불변으로 유지하므로 프록시 선택은 기존 클립 컷을 재해석할 수 없습니다. 클립 소스 틱은 해당 논리적 스트림, 중첩된 시퀀스, 또는 생성된 소스 시간 기준을 사용합니다. 상수 재생은 정확한 유리수 매핑이며, 명시적인 시간 맵이 대신 아무런 알림 없이 로 합성됩니다. 전이 정렬은 인접한 컷을 기준으로 정의되며, 시퀀스는 해당 클립 종류를 실제로 포함할 때만 필요한 시각/오디오 구성 설정을 획득합니다.

`TimelineEditor` 는 검증된 구조적 변경을 후보 복사 트랜잭션을 통해 적용합니다. 참조된 미디어나 시퀀스를 제거하거나, 잘못된 트랙과 스트림 종류를 혼합하거나, 유효하지 않은 유리수를 제공하는 경우 전체 편집이 거부되고 리비전이 유지됩니다. 표준 라이브러리 전용 데이터 모델에는 FFmpeg 링크 의존성이 없습니다. 별도로 검토되는 비디오 어댑터는 이제 제공된 FFmpeg 런타임 를 캔버스 애니메이션 교환을 위해 실행합니다; DEPENDENCIES.md 를 참조하세요.

문서는 형식 버전, 유한/무한 모드, 현재 할당된 캔버스 영역의 양수 값, 선택적 월드 원점 및 청크 크기, 유리수 프레임 속도, 프레임 수, 자산 레지스트리, 및 순서 있는 레이어 스택을 포함합니다. 유한 캔버스는 한계가 설정된 에서 원점 0에 제한됩니다. 무한 캔버스는 지원되는 부호화된 좌표 도메인 내에서 개념적 월드가 무한한 채로 청크 경계를 따라 할당된 영역을 바깥쪽으로 확장합니다.

자산은 다음 중 하나입니다.

- RasterAsset: 안정적인 ID와 iiPaintEngine RasterLayer ARGB 픽셀.
- ChunkedRasterAsset : 안정 ID와 정규 행 메이저 스파스 RasterChunk 항목은 서명된 세계 열 및 행으로 주소 지정됩니다.
- VectorAsset: 안정적인 ID, 양수 뷰포트 및 순서가 지정된 경로입니다.

벡터 v1는 의도적으로 명시된 목표에 필요한 형상(이동, 선, 2차 곡선, 3차 곡선, 닫기, 솔리드 채우기 및 솔리드 스트로크)만 지원합니다. 텍스트, 그라데이션, 마스크, 부울 연산 및 효과는 첫 번째 모델에서 예측되지 않습니다.

Layer는 2 완전하고 구조적으로 평행한 유형의 공개 변형입니다:

- StaticBitmapLayer / DynamicBitmapLayer: 공유 LayerProperties와 각각 StaticBitmapContent / DynamicBitmapContent를 가지며 RasterAsset 또는 ChunkedRasterAsset만 참조한다.
- StaticVectorLayer / DynamicVectorLayer: 공유 LayerProperties와 각각 StaticVectorContent / DynamicVectorContent를 가지며 VectorAsset만 참조한다.

레이어 소스는 다음 중 하나입니다.

- StaticSource: 자산 ID 1개.
- KeyframedSource: 레이어가 프레임 소유 키를 해석하는 파생된 보조 `frameIndices` 인덱스입니다. 해당 제품은 `Keyframe`를 소유하고 있지 않으며, 그 콘텐츠 종류는 소유한 실제 비트맵 또는 벡터 레이어 타입에 의해 고정됩니다.

`Document::frames` 는 완전히 정적인 문서의 경우 비어 있을 수 있습니다. 저장된 모든 `Frame` 는 비어 있지 않으며, 엄격히 증가하는 인덱스 순서로 나타나고, 자신의 정확한 정수 위치를 위해 직접 `{layerId, assetId}` 키프레임 를 소유하며, 여러 비트맵 및 벡터 레이어의 키를 함께 소유할 수 있습니다. 프레임은 한 레이어에 2 키를 소유할 수 없으며, 정적인 레이어는 키로 이름 지어질 수 없으며, 모든 애니메이션 레이어는 초기 키를 0프레임에 가집니다. 모든 자산은 참조된 레이어 유형과 일치해야 하며 모든 키프레임은 문서 키프레임 카운트 안에 유지됩니다. 이것은 평가를 결정론적으로 만들고, 중복된 키프레임 소유 없이 정의되지 않은 프리롤 동작을 제거합니다.

한 `Frame` 내의 키는 정렬된 `layerId` 순서를 따른다. `KeyframedSource::frameIndices` 는 해당 레이어 id 를 포함하는 증가하는 프레임 집합과 정확히 일치해야 한다. 이는 권위 있는 콘텐츠가 아닌 성능 지표이며, 검증은 양방향으로 확인하고 모든 편집자 변형 및 디코더는 이를 재구성한다. 직접적인 집계 구성은 검증 전에 프레임 소유자와 일치하는 유도된 인덱스를 모두 제공해야 한다.

<a id="4-time-semantics"></a>

## 4. 시간 의미론

시간은 정수 프레임 위치에 유리 프레임 속도를 더한 값으로 저장됩니다. 부동 타임스탬프는 유지되지 않습니다.

단계 0 평가에서는 래스터 및 벡터 키프레임 모두에 대해 홀드 샘플링을 사용합니다. 활성 자산은 요청된 프레임 또는 그 이전의 마지막 키프레임입니다. 타임라인 안전하게 거부한다 외부 샘플링.

구체적인 제품에 필요할 때까지 보간은 제외됩니다. 래스터 보간은 일반적으로 크로스 페이드 또는 광학 방법을 의미합니다. 벡터 보간에는 일치하는 경로 토폴로지가 필요합니다. 이를 하나의 일반 보간 플래그로 취급하면 잘못된 추상화가 생성됩니다.

별도의 비디오 편집 `TimelineProject`는 이 캔버스의 프레임 색인 대신 유리수 시간 기준과 부호 있는 tick을 사용한다. 효과 및 시간 재매핑 키프레임은 hold, linear 또는 Bezier 보간을 명시적으로 선택할 수 있다. 이는 `Document` 키프레임 샘플링이나 `.iisc` 버전 1의 의미를 바꾸지 않는다.

<a id="5-brush-semantics"></a>

## 5. 브러시 의미론

브러시는 저작 작업이지 지속 가능한 장면 형상이 아닙니다.

~~~text
pointer input
  -> iiPaintEngine brush rasterization
  -> committed RasterLayer pixels
  -> iiSharedCanvas RasterAsset
~~~

iiSharedCanvas에서는 포인터 궤적, 곡선, Dab 스트림, 재생 명령 또는 유지된 브러시 스트로크를 직렬화할 수 없습니다. 브러시 사전 설정은 나중에 선택적 제작 메타데이터로 저장될 수 있지만 렌더링된 사실은 픽셀로 유지됩니다.

<a id="implemented-selected-bitmap-authoring-boundary"></a>

### selected-비트맵 작성 경계 구현

`BitmapEditor` 는 각 작업마다 변경 가능한 `RasterAsset` 를 안정적인 id 로 해결하므로 문서 자산 벡터 이동이 캐시된 자산 포인터를 남길 수 없습니다. 그것은 디코딩된 비트맵 입력에 대한 완전한 `RasterLayer` 교체, 직접 픽셀 또는 직사각형 패치 편집, 명확화, 그리고 스트리밍 브러시/지우개 입력을 받습니다. 스트리밍 입력은 제스처가 활성 상태인 동안에만 iiPaintEngine `RasterDabStream` 를 사용하며, 그 후 직접 자산 픽셀에 커밋됩니다. `Document` 에 포인트 목록이나 재생 가능한 스트로크는 추가되지 않습니다.

실행 취소/다시 실행은 최대 32 개의 불변, 공유 전체 래스터 스냅샷을 저장하므로 트랜잭션 복사본은 히스토리의 픽셀을 중복하지 않습니다. 이는 의도적으로 가장 간단하고 올바른 첫 번째 정책이며 전체 브러시 제스처를 원자적으로 만듭니다. 패치 기반 히스토리는 실제 문서 크기가 필요한 메모리 예산을 확립한 후에만 이를 대체해야 합니다.

`ChunkedBitmapEditor`는 세계 좌표에서 동일한 커밋된 픽셀 브러시 계약을 적용합니다. 래스터 샘플을 수신하는 청크만 할당합니다. 누락된 청크는 투명합니다. 스파스 실행 취소/다시 실행은 청크 컬렉션의 스냅샷을 생성하며, 영역 증가는 청크의 좌표나 픽셀 페이로드를 다시 쓰지 않습니다.

<a id="implemented-native-vector-authoring-boundary"></a>

### 네이티브 벡터 제작 경계 구현

`VectorEditor` 는 각 작업마다 하나의 변경 가능한 `VectorAsset` 를 안정적 id 로 해결합니다. 이는 경로 페인트 순서를 관리하며, 뷰포트 , 솔리드 채움/선, 그리고 네이티브 M/L/Q/C/Z 명령을 관리합니다. 선형 끝점, 이차 제어/끝점, 그리고 세차 제어 점/끝점은 경로를 래스터 픽셀로 변환하지 않고 독립적으로 편집할 수 있습니다.

세밀한 편집은 복사된 `VectorPath` 에서 작동하며 `DocumentEditor::replaceVectorPath` 를 통해 커밋합니다. 따라서 전체 문서 검증은 원자적 경계입니다: 유효하지 않은 좌표, 페인트, 명령 인덱스, 자산 유형, 또는 외부로 손상된 문서 상태는 소스 경로와 편집자 수정본을 변경하지 않습니다. `VectorEditor` 는 두 번째 벡터 모델, 유지된 포인터 입력, 또는 영속된 히스토리를 추가하지 않습니다.

`BitmapItem` 는 선택된 래스터 자산의 하나에 대한 Qt 퀵 디스플레이 경계입니다. 그것은 엔진의 ARGB 저장을 페인트 시간에 `QImage::Format_ARGB32` 로 변환하며, 최근접 이웃 스케일링을 사용하고, 마우스 페인트, 명시적 압력 부담 스트roke 호출, 클리어, 픽셀, 실행 취소/다시 실행, 줌, 그리고 패닝을 노출합니다. 그것의 독립적인 `createBitmap` 경로는 최소한 한 레이어의 문서를 소유하며, C++ `bind` 경로는 호출자 소유의 문서를 편집하며, 그 수명 주기와 GUI 스레드 접근은 호출자의 책임으로 남아 있습니다. 이 항목은 문서 레이어를 구성하지 않으며 입력 이벤트를 직렬화하지 않습니다.

`CanvasItem` 는 QML 에 `SharedCanvas` 로 등록되어 있으며, 전체 문서 디스플레이 및 래스터 저작 경계입니다. 캐시는 한계가 설정된 LOD 텍스처 타일을 캐시하고 타임라인 프레임을 전환하며, 가장 가까운 이웃 GPU 줌 및 패닝을 적용하고, 완전한 혼합된 프레임이 표시된 상태에서 붓/지우개 편집을 위해 래스터 문서 레이어를 선택할 수 있습니다. 입력은 문서 좌표로 표현되며, `BitmapEditor` 가 수신하기 전에 선택된 레이어의 아핀 변환을 통해 반전됩니다. 호출자가 소유한 문서는 여전히 권위 있고 명시적이며, `refresh()` 는 외부 변형을 관찰합니다. 그의 GUI 스레드는 검증된 문서 상태를 스냅샷으로 찍고, 병합 작업자는 누락된 가시성/미리 로드 타일만 렌더링하며, Qt 퀵 시나리오 그래프는 해당 타일을 업로드하고 변환합니다. 애플리케이션 선택 UX, 도구, 재생 제어 및 문서 카탈로그 정책은 항목 바깥에 있습니다. 파일 바운드 항목은 모든 콘텐츠 편집을 `DocumentFile` 를 통해 라우팅하며, 렌더링은 결코 디스크 쓰기를 예약하지 않으며 편집이 영구화되는 시점을 결정하지 않습니다.

애플리케이션 부트스트랩 경로에 대해, `createRasterDocument` 는 선택된 투명 래스터 자산과 레이어 하나를 설치합니다. `replaceSelectedPixels` 는 파일 시스템 왕복 없이 디코딩된 이미지 가져오기를 지원합니다. Qt 어댑터는 붓 기능, 간격, 3점 압력 곡선, 압력-불투명도 매핑, 안정화 값, 도구 모드, 태블릿/마우스 상태, 그리고 스트로크 수에 대한 일반적인 비트맵 저작도구 제어를 노출합니다. 안정화는 일시적인 입력 평활화이며, 이 설정들 중 어느 것도 문서 형식에 포함되지 않습니다. 모델 편집은 동기적으로 커밋되며, 결과 프레임은 비동기적으로 다시 렌더링됩니다. `livePreviewFrameIntervalMs` 는 활성 스트로크 스냅샷 스케줄링을 제한하며, 다중 스레드 이벤트 속성은 호스트 입력 구성으로 남아 있습니다. 대체된 작업자 요청은 `AsyncFrameRenderer` 독립적으로 합쳐집니다.

`createInfiniteRasterDocument` 는 선택된 빈 희소 래스터 레이어 하나를 설치합니다. 호스트는 `ensureInfiniteCanvasRegion` 를 통해 카메라 수요를 공급합니다. 문서 는 해당 수요를 할당된 월드 영역과 합치며 각 새로운 에지를 차크 경계 바깥쪽으로 둥글게 만듭니다. `CanvasItem` 는 월드 기원을 노출하고 정확한 4쪽 성장을 반환하여 시각적 크기 조정과 카메라 보존을 소비자에게 맡깁니다. UI 입니다.

<a id="6-implemented-render-pipeline"></a>

## 6. 렌더 파이프라인 구현

`renderFrame`, `renderFrameRegion` 및 `renderFrameTiles`는 공개 레이어 렌더링 경계를 통해 다음 단계를 실행합니다.

1. 문서를 검증합니다.
2. 각 가시 레이어에 대해 유한한 래스터, 희소 청크된 래스터 또는 벡터 자산을 해결하십시오. 해당 레이어에 포함된 `frameRange`가 존재할 경우 해당 프레임이 포함되어 있습니다. 범위를 벗어나서는 격리된 결과는 문서 슬롯을 유지하지만 효과적인 가시성을 보고하고 픽셀 타일을 포함하지 않습니다.
3. 각 레이어를 고립된 한계가 설정된 타일로 래스터화하고, 전체 강도 픽셀을 유지하면서 아핀 변환을 적용하며, 불투명도와 블렌드 모드를 메타데이터로 제공합니다.
4. 합성는 iiPaintEngine 불투명도와 블렌드 의미론을 사용하여 해당 레이어 타일을 아래에서 위로 배치합니다.
5. 문서 자산을 변형하지 않고 전체 프레임 또는 한계가 설정된 월드 영역 타일을 반환합니다.

따라서 정적 및 애니메이션 콘텐츠는 프레임 평가 후 하나의 렌더링 경로를 공유합니다. 별도의 애니메이션 캔버스가 없습니다.

`renderFrameLayerTiles` 는 독립적으로 주소 지정 가능한 레이어 작업이며, `renderFrameLayers` 는 안정적인 순서 배치 목록을 반환하고, `composeFrameLayers` 는 명시적인 합성 경계입니다. 레이어는 하나의 불변 문서 스냅샷에서 동시 렌더링됩니다. `AsyncFrameRenderer` 는 전역 Qt 스레드 풀이 허용하는 레이어 작업자보다 더 많이 사용하지 않으며, 먼저 불변 스냅샷을 사전 검사 작업에서 정확히 한 번만 유효성 검사합니다. 실패한 사전 검사 는 레이어 작업자를 배포하지 않으며, 유효한 요청은 문서 순서로 레이어 결과를 합치고 부분 프레임을 노출하지 않고 최종 합성을 수행합니다.

버전 1는 워커 CPU에서 M/L/Q/C/Z 경로를 래스터화하며, 결정론적 4x4 커버리지 샘플링, 짝수-오드 채우기 및 라운드 스트로크 풋프린트를 제공합니다. 전체 iiPaintEngine 아핀 행렬을 가장 가까운 이웃 자산 샘플링과 함께 적용하고 결과를 캔버스에 클립합니다. 소스오버, 곱셈, 화면 및 오버레이는 iiPaintEngine 컴포지터에 위임됩니다. Destination-out은 브러시 지우개 모드로 남아 있으며, 검증은 이를 문서 레이어 혼합 모드로 거부합니다.

Qt 어댑터는2 의 LOD 의 2의 거듭제곱을 뷰포트 줌에서 선택하고, 64 개 이하의 주류 합성 512-텍셀 타일과 256 개의 격리된 레이어 타일을 유지하며, 대기 중인 렌더링을 최신 불변 스냅샷/요청으로 합칩니다. 패닝과 줌은 `QSGTransformNode` 를 즉시 업데이트합니다. 소스 오버 레이어는 독립적인 시나리오 그래프 불투명도 노드 하에 업로드되며, 곱셈, 스크린, 오버레이 또는 불완전한 레이어 캐시는 결정론적인 iiPaintEngine 합성 타일을 사용합니다. 하드웨어 기반 Qt 퀵은 타일 텍스처 샘플링과 시나리오 합성을 소유하며, 소프트웨어 백엔드는 동일한 노드로 정확성을 유지합니다.

<a id="7-implemented-serialization"></a>

## 7. 직렬화를 구현했습니다

교환 스냅샷은 FORMAT .md 에 정의된 표준 이진 `.iisc` 컨테이너입니다. `encodeIisc` 와 `decodeIisc` 는 표준 라이브러리의 필드 코덱을 유지합니다. Write-through 작업 파일은 PERSISTENCE .md 에서 별도로 지정되며, 고정된 원본 래스터 저장과 함께 해당 필드 인코딩을 재사용합니다. 스냅샷의 32-바이트 헤더는 버전, 페이로드 크기, 및 CRC-32 를 기록합니다. 페이로드는 네이티브 래스터 /vector 자산, 희소 청크, 할당된 월드 기하학, 순서 있는 레이어, 변환, 타임라인 참조, 및 아카이브 경로 없이 선택 형식1.2 생성 메타데이터를 저장합니다. ComfyUI JSON 는 정확한 UTF-8 로 유지되며 그래프 파싱이나 정규화 대신 한계가 설정된 문법 검증을 받습니다.

작성자는 원시 또는 런 길이 ARGB32를 결정적으로 선택하고 문서에 대해 하나의 바이트 표현을 방출합니다. 리더는 파싱 전에 체크섬과 정확한 페이로드 길이를 검증하고, 할당 전에 설정된 집계 제한을 적용하며, 정규 UTF-8와 레코드 태그를 검증하고, 노출 전에 완성된 문서를 검증합니다.

<a id="8-validation-and-security-invariants"></a>

## 8. 검증 및 보안 불변량

카메라 RAW 검증은 문서 검증과 별개입니다:

- 센서 범위, 유의 비트 깊이, 샘플‐평면 카운트 및 정확한 샘플 저장은 정수 오버플로우 없이 일치해야 합니다.
- 활성 영역은 센서에 포함되며 기본 자르기는 활성 영역에 포함됩니다.
- CFA 셀은 기존 채널을 참조합니다. 선형 평면은 채널에 해당합니다.
- 흑백 레벨, as-shot 중성 좌표, 색 매트릭스, 렌즈 범위 및 캡처 값은 유한하고, 정확히 크기가 되며, 물리적으로 정렬되어야 합니다.
- 경계가 확인된 접근은 잘못된 좌표나 잘못된 패턴에 대해 값을 반환하지 않으며, 암시적 이미지 처리를 수행하지 않습니다.

- 알 수 없는 최신 형식 버전 안전하게 거부한다.
- 캔버스, 래스터 및 벡터 범위는 양수입니다.
- 무한 캔버스는 1.1+ 형식, 32부터 4096까지의 제곱2 청크 크기, 그리고 할당된 영역이 한계가 설정된인 서명된 좌표가 필요합니다.
- 희소 청크는 고유하고, 행이 크며, 정확히 구성된 청크 크기와 동일합니다; 유한 캔버스는 희소 래스터 자산을 거부합니다.
- 래스터 치수는 정확한 ARGB 픽셀 수와 동일합니다.
- 자산 ID와 레이어 ID는 비어 있지 않고 고유합니다.
- 모든 레이어 참조가 해결됩니다.
- 레이어 불투명도는 유한하며 0부터 1까지입니다.
- 변환 및 벡터 좌표는 유한합니다.
- 벡터 경로는 MoveTo로 시작하며 눈에 보이는 채우기 또는 선이 있습니다.
- 타임라인 속도와 프레임 수가0이 아닙니다.
- 희소 프레임은 비어 있지 않으며, 모든 애니메이션 레이어의 초기 키인 0로 시작하고, 엄격히 증가하며, 범위 내에 유지됩니다. 각 프레임은 각 레이어당 최대 하나의 키를 소유하며, 각 키는 참조된 실제 비트맵 또는 벡터 레이어 타입에 의해 콘텐츠 종류를 고정합니다.
- 안정적인 확산 메타데이터는 1.2형식이 필요하며, 최소 하나의 페이로드를 포함하고, 차원 및 이산 카운트를 양수로 유지하며, sampler/ LoRA 숫자 값을 유한하고 정의된 범위 내에 유지해야 합니다.
- ComfyUI 프롬프트 및 워크플로 페이로드는 JSON 객체이며, 확장 값은 고유한 비예약 키 아래에서 유효한 JSON입니다. 이러한 모든 메타데이터는 신뢰되지 않으며 검증이나 렌더링에 의해 실행되지 않습니다.

직렬화는 할당 전에 컨테이너 크기, 디코딩된 픽셀, 컬렉션, 벡터, 키프레임, 메타데이터 항목 및 문자열 제한을 적용합니다. 바이너리 전송에는 아카이브 항목이나 경로가 없으므로 버전 1에는 아카이브 재귀 및 경로 탐색이 존재하지 않습니다.

<a id="9-dependency-review"></a>

## 9. 의존성 검토

iiPaintEngine   0.1.0 이 선택된 이유는 사용자의 기존  비트맵 -only 엔진이며 이미  RasterLayer , 브러시 래스터화, 블렌드 모드, 및 변환을 제공하기 때문입니다. 그것은  AGPL-3.0 -only 이며  Qt / LVRS 를 간접적으로 포함합니다.  iiSharedCanvas 는 따라서  AGPL-3.0 -only 하에 시작됩니다.

SQLite 는 두 번째 직접 의존성으로, 내구성 인크리멘털 파일 트랜잭션을 공급하며, DEPENDENCIES .md 는 유지 관리, 라이선스, 및 크기 검토를 기록합니다. 그의 타입은 사적인 상태로 유지됩니다. Qt 코어 스레드 풀/미래 원시 함수와 공개 Qt 퀵 씬 그래프는 iiPaintEngine 의 기존 Qt 타겟을 통해 도착합니다. `.iisc` 코덱은 표준 라이브러리를 사용합니다. SVG 임포트 맵은 엄격한 고체 기하학 어휘를 Qt XML /path 원시 함수를 통해 매핑하며, SVGZ 는 검토된 zlib 압축을 사용합니다. 기존 Qt Gui 코덱과 PDF 작성은 이미지 및 PDF 교환을 제공합니다. 텍스트 모양 지정과 GPU 벡터 경로 래스터화는 여전히 별개의 결정입니다.

`Layered` 어댑터는 OpenRaster 를 맵핑하며 한계가 설정된 PSD 픽셀 레이어 하위 집합을 분리된 네이티브 래스터 자산/레이어로 연결합니다. 검토된 libzip 는 ZIP 읽기를 공급하며 Qt XML / PNG 와 zlib 는 재사용됩니다. 아카이브 파일은 추출되지 않으며, 지원되지 않는 레이어 의미에 대한 병합된 미리보기가 대체되지 않습니다. `iisc-import` 유틸리티는 새로운 작업 파일에 기존 `DocumentFile::create` 경계를 사용합니다. 두 번째 지속성 모델이나 소비자별 변환 브릿지를 추가하지 않습니다.

PSD 내보내기는 네이티브 문서를 프레임0으로 투영하는 단방향 작업이다. 내장 벡터 PDF Smart Object를 기존 Qt PDF 작성기로 직렬화하고, 네이티브로 렌더링한 레이어별 픽셀 캐시와 병합 프리뷰를 함께 담는다. 정수 평행 이동 이외의 비트맵 변환은 뷰포트 픽셀에 굽는다. 단순한 비트맵 레이어는 원본 픽셀/오프셋을 유지한다. 네이티브 모델에 PSD 레코드를 추가하거나 타임라인 소유권을 변경하거나 Smart Object 가져오기 지원을 뜻하지 않는다.

비디오 편집 타임라인 모델 또한 C++ 표준 라이브러리 집계와 변형만 사용합니다. FFmpeg / ffprobe 는 실제 비디오 탐지/인코딩/디코딩을 런타임 실행 가능한 선택 사항으로 공급하며, 모든 연결된 소비자에게 큰 코덱 표면을 추가하지 않습니다. 애플리케이션은 경로와 배포 정책을 공급하며, 어댑터는 다운로드기, 네트워크 미디어 입력 또는 숨겨진 바이너리 번들을 갖지 않습니다. MEDIA_IO .md 는 지원되는 경계와 변환 경고를 정의합니다.

[ComfyUI의 워크플로 메타데이터 문서](https://docs.comfy.org/development/api-development/workflow-metadata),
[워크플로우 API 형식](https://docs.comfy.org/development/api-development/workflow-api-format)및 [메타데이터 작성기](https://github.com/Comfy-Org/ComfyUI/blob/master/comfy_api/latest/_ui.py) 는 `prompt` , `workflow` , KSampler, 및 `extra_pnginfo` 계약에 대해 검토되었습니다. [nlohmann/json 3.12.0](https://github.com/nlohmann/json) 도 활성 MIT 라이선스 단일 헤더 구현체로 검토되었습니다. 이 모듈이 도입되지 않은 이유는 이 모듈이 JSON 를 그대로 보존하고 한계가 설정된 문법 검증만 필요로 하며, 해당 좁은 역할에 대한 패키지와 설치된 종속성을 추가하는 것은 유지 관리 및 소비자 표면을 증가시키기 때문입니다. 한계가 설정된 표준 라이브러리 검증기는 공식 문자열을 절대 정규화하지 않습니다.

AUTOMATIC1111의 공식
[정보 텍스트 파서](https://github.com/AUTOMATIC1111/stable-diffusion-webui/blob/master/modules/infotext_utils.py),
[정보텍스트 작가](https://github.com/AUTOMATIC1111/stable-diffusion-webui/blob/master/modules/processing.py),
[이미지 메타데이터 판독기/작성기](https://github.com/AUTOMATIC1111/stable-diffusion-webui/blob/master/modules/images.py)및 [체크포인트 해시 모델](https://github.com/AUTOMATIC1111/stable-diffusion-webui/blob/master/modules/sd_models.py) 는 프롬프트, 열린 필드, 캐리어 키, Hires 기본값, 및 현재/전통적 해시 의미를 위해 검토되었습니다. 캐리어 추출 후 한계가 설정된 표준 라이브러리 파서가 충분하므로 Python , Pillow , EXIF , 이미지 코덱, 또는 WebUI 런타임 가 도입되지 않습니다.

[Adobe DNG 1.7.1](https://helpx.adobe.com/camera-raw/desktop/dng-and-file-formats/digital-negative.html) 는 CFA 및 LinearRaw 해석, 방향, 활성/작업 기하학, 반복 검은색 레벨, 평면별 흰색 레벨, 촬영 중 중립 좌표, 및 카메라 색상 행렬에 대한 현재 공개 참조로 검토되었습니다. 집계는 DNG 태그 번호를 복사하지 않고 TIFF / DNG 전송을 구현하거나 DNG 준수를 주장하는 방식으로 이러한 공통 개념을 재사용합니다.

[LibRaw](https://github.com/LibRaw/LibRaw)  는 독점 카메라 디코딩을 검토받았습니다. 그것은 능동적으로 유지되며 광범위한 카메라 세트를 지원하며, LGPL-2.1 또는 CDDL-1.0 로 이중 라이선스됩니다. 또한 메모리 내 객체에는 유지 및 배포 비용이 불필요한 네이티브 디코더/코덱 표면을 추가합니다. 따라서 현재는 추가되지 않으며, 향후 디코더 어댑터는 iiSharedCanvas 코어 또는 모든 소비자가 이를 수행하도록 강요하지 않고도 이를 의존할 수 있습니다.

The CMake 타겟은 가져온 iiPaintEngine 라이브러리 디렉토리를 빌드 및 설치 rpath 에 기록하며 표준 자매 접두어 대체 경로 를 공급합니다. 공개 타겟은 런타임 검색 디렉토리를 최종 실행 파일로 전파합니다. 왜냐하면 iiPaintEngine 도 공개 ABI 의존성이기 때문입니다. 이는 현재 iiPaintEngine 패키지가 rpath 설치 이름을 사용하는데, CMake 가 릴리스 소비자에게 자동으로 추가하지 않기 때문에 필요합니다.

<a id="10-milestones-and-gates"></a>

## 10. 이정표 및 게이트

<a id="phase-0---blueprint-and-setup"></a>

### 단계 0 - 청사진 및 설정

다음과 같은 경우 완료하세요.

- Git 및 CMake 프로젝트가 존재합니다.
- iiPaintEngine는 페인팅 종속성입니다. SQLite는 검토된 파일 스토리지 종속성입니다.
- 혼합 static/animated 래스터 및 벡터 문서 모델 빌드.
- 형식 중립 카메라 RAW 집계 및 독립적 검증 빌드.
- Typed Stable Diffusion 메타데이터 및 정확한 ComfyUI 그래프 보존 빌드.
- 무손실 AUTOMATIC1111 정보 텍스트 구문 분석 및 유형화된 공통 프로젝션 빌드.
- 유효성 검사 및 보류 평가가 테스트됩니다.
- 설치 가능한 CMake 패키지와 독립형 소비자가 검증되었습니다.
- 청사진 및 형식 문서가 코드와 일치합니다.

<a id="phase-1---durable-format"></a>

### Phase 1 - 내구성 있는 형식

현재 구현이 완료되었습니다.

다음과 같은 경우 완료하세요.

- .iisc 작성자 및 리더 왕복 변환 모든 단계 0 유형.
- 1.2 라운드 트립 생성 메타데이터를 포맷하고, 1.0와 1.1는 바이트 정규화 및 메타데이터가 없는 상태를 유지합니다.
- 표준 직렬화와 버전 호환성 테스트가 통과한다. 버전 1.0에는 마이그레이션이 필요한 이전 물리 형식이 없다.
- 손상되고 크기가 너무 큰 경로 탐색 및 향후 버전 파일 안전하게 거부한다.
- 비트맵 브러시는 유지된 궤적이 없는 픽셀로 왕복을 출력합니다.

<a id="phase-2---frame-renderer"></a>

### Phase 2 - 프레임 렌더러

현재 구현이 완료되었습니다.

다음과 같은 경우 완료하세요.

- 정적 래스터 및 벡터 레이어가 함께 렌더링됩니다.
- 애니메이션 래스터와 벡터 레이어는 경계 프레임에서 결정적으로 렌더링됩니다.
- 변형, 불투명도, 클리핑 및 지원되는 블렌드 모드에는 주요 테스트가 있습니다.
- CPU 출력은 클린 빌드 전반에서 안정적입니다.

<a id="phase-3---authoring-adapter"></a>

### Phase 3 - 저작 어댑터

다음과 같은 경우 완료하세요.

- [x] iiPaintEngine 비트맵 편집은 선택한 RasterAsset에 적용할 수 있습니다.
- [x] 실행 취소/다시 실행은 스트로크 재생이 아닌 픽셀 스냅샷을 저장합니다.
- [x] 재사용 가능한 Qt Quick 비트맵 항목은 해당 자산을 표시하고 조작할 수 있습니다.
- [x] 재사용 가능한 Qt Quick 혼합 문서 항목은 선택된 변환된 래스터 레이어를 프레임으로 렌더링하고 편집합니다.
- [x] 공개 데이터 조회와 `DocumentEditor` API는 직접 벡터 조작이나 부분 무효 상태를 요구하지 않고 검증된 구조 편집을 노출합니다.
- [x] `VectorEditor`는 원자 검증 경로 교체를 통해 선형, 이차 베지어 및 삼차 베지어 경로 기하학을 생성하고 편집합니다.
- [x] 제품 중립적인 C++ 및 QML 계약 테스트는 저작 표면, 혼합된 래스터 /vector/timeline 출력 및 네이티브 `.iisc` 왕복 변환를 검증하며, 소비자 애플리케이션이 API를 정의하지 않습니다.
- [x] 무한 문서는 카메라 수요에 따라 할당된 영역을 확장하고, 희소한 부호 좌표 래스터 청크를 저장, 렌더링, 편집, 실행 취소 및 직렬화합니다.

<a id="phase-4---product-hardening"></a>

### 4 단계 - 제품 경화

The `TimelineInterchange` 어댑터는 지속된 캔버스 타임라인을 짝지어진 XML / FCPXML 매니페스트와 독립적인 PNG 레이어 상태로 투영합니다. 공유된 비공개 계획은 네이티브 키-구간/미디어 준비를 2 구체적 XML 작성기에서 분리합니다. 패키지는 `source.iisc` 를 유지하며 입력을 결코 변형하지 않습니다. 그의 CLI 는 읽기 전용 작업 파일 로더와 PSD 내보내기를 공유하며, 소비자별 모델이나 네이티브 NLE 플러그인은 추가되지 않습니다. 보라
[TIMELINE_INTERCHANGE.md](TIMELINE_INTERCHANGE.md).

다음과 같은 경우 완료하세요.

- 크로스 플랫폼 패키지는 모든 대상에 설치되고 사용됩니다.
- [x] 대화형 렌더링은 한계가 설정된 거주 타일, LOD, 불변 작업자 스냅샷 및 GPU 장면 변환을 사용하여 수만 픽셀 캔버스에 적용합니다.
- 부분 디코딩, 썸네일, 장치 수준 쓰기 대기 시간 및 전원 차단 복구가 측정됩니다.
- 공개 API 호환성 및 파일 마이그레이션 정책이 게시되었습니다.

<a id="native-canvas-media-extension-0110"></a>

## 네이티브 캔버스 미디어 확장(0.11.0)

캔버스 `Document` 는 이제 이미지, 벡터 및 일정 속도의 비디오 자산을 소유하며, 모든 시각적 레이어에 변환/불투명도 모션 그래픽이 적용됩니다. `CanvasSampling` 는 순수 프레임 평가를 중앙집중화하며, 렌더링 및 비동기 타일 프레젠테이션은 동일한 평가된 상태를 소비합니다. 이는 별도의 메타데이터가 풍부한 `TimelineProject` 모델을 문서에 병합하지 않습니다. 영속화된 계약은 `.iisc` 1.6 입니다; 정확한 지원된 동작과 제한 사항은 [CANVAS_MEDIA .md](CANVAS_MEDIA.md) 를 참조하십시오.

## Artboard domain (0.27.0)

Artboards are native document-owned aggregates with stable ids, bounded world
rectangles and local layer ownership. The shared timeline and asset pool remain
owned by `Document`; independent duplication copies referenced source states.
`DocumentEditor` validates and commits model/file edits atomically. The existing
renderer handles local-to-world sampling, clipping and isolated group composition.
Consumer UI controls remain consumer-owned. See [ARTBOARDS.md](ARTBOARDS.md).

0.28.0은 네 가지 실제 레이어 및 전용 콘텐츠 타입을 정의한다. 정적/동적 전환은 실제 타입을 바꾸는 원자 편집이다. 네이티브 1.19는 명시적 타입을 기록한다. [FOUR_LAYER_TYPES.md](FOUR_LAYER_TYPES.md)를 참조한다.
