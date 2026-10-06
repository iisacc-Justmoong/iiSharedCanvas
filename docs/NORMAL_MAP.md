<a id="normal-map-controlnet-layer"></a>

# 노멀 맵 ControlNet 레이어

패키지 0.19.0는 `NormalMapAsset`와 `NormalMapLayer`를 공개 변형에 추가하며, 소비자에게 재구축을 요구합니다. 네이티브 모델 버전은 1.13이며, SQLite 작업 파일 스키마는 1로 유지됩니다. 새로운 종속성, 일반 추정기 또는 추론 런타임가 포함되어 있지 않습니다.

<a id="native-data-contract"></a>

## 네이티브 데이터 계약

`NormalMapSample { double x=0, y=0, z=1; bool valid=true; }` 는 고정된 캔버스-카메라 기준의 표면 법선을 설명합니다: +X 오른쪽, +Y 아래, +Z 관찰자 쪽. 이것들은 부호화된 방향 성분이며, RGB  색상, 계량적 깊이, 또는 메쉬 접선 공간 텍스처가 아닙니다. 음수 Z 가 지원됩니다. 각 유효한 성분은 유한하며 [-1,1]범위 내에 있고, 절대 벡터 길이 오차는 1e-6이하입니다. 0  벡터는 유효한 법선이 아닙니다. 누락된 샘플은 `{0,0,0,false}`  를 독점적으로 사용합니다. SDK 는 아무런 알림 없이 정규화 또는 구멍 채우기 대신 유효하지 않은 데이터를 거부합니다.

`NormalMapAsset { id, viewport, samples }` 는 양의 너비/높이를 소유하며 정확히 너비*높이 행주요 샘플을 가집니다. 작성된 이진64 구성 요소와 유효성을 스냅샷 및 작업 파일 라운드 트립을 통해 모두 보존합니다. `NormalMapLayer { properties, source, control }` 는 NormalMapAsset 만 참조합니다. `StaticSource` 는 `StaticBitmap` 을 생성하며, `KeyframedSource` 는 `DynamicBitmap` 를 유지 방식만 사용하는 소스 변경 사항으로 정수 타임라인 프레임에서 생성합니다. `LayerRole::ControlNet` 와 `ControlNetKind::NormalMap` 를 가지며, 라인 제어 레이어가 아닙니다. 모든 공유 ControlNet 설정이 지원됩니다.

<a id="rendering-and-model-adapters"></a>

## 렌더링 및 모델 어댑터

`normalMapRasterPreview(asset, options={})`와 `renderNormalMapControlMap(document, layerId, frame, options={})`는 불투명한 RGB8 픽셀을 생성합니다. 각 선택된 서명된 구성 요소는 `round((n+1)*127.5)`에 의해 매핑됩니다. 기본 RGB 순서는 X,Y,Z이며, 전면을 향한 `(0,0,1)`는 `#8080FF`가 됩니다. 누락된 샘플은 불투명한 검은색이 됩니다. `NormalMapControlMapResult`는 추가로 정확한 네이티브 `samples`, `validPixels` ( 0/1 ), 그리고 실패 `message`를 반환합니다. 소비자는 누락된 콘텐츠를 구분하기 위해 유효성 마스크를 사용해야 합니다.

`NormalMapRenderOptions`는 `channelOrder = Xyz | Zyx`, `flipY = false` 및 `maximumPixels = 16*1024*1024`를 지원합니다. Y 반전은 양자화 전에 발생하며, 채널 순서는 출력 RGB만 변경됩니다. 정확히 반환된 샘플은 그대로 유지됩니다. 예산을 초과하는 알 수 없는 채널 열거와 출력 크기는 거부됩니다. Preview는 잘못된 입력/옵션인 경우 invalid_argument를, 예산 실패인 경우 length_error를 발생시킵니다; 문서 제어 내보내기는 정상적이지 않은 결과를 반환합니다.

채널 규칙은 소비 모델 어댑터에 의해 선택되어야 합니다. 원본
[ControlNet MiDaS 주석자](https://github.com/lllyasviel/ControlNet/blob/main/annotator/midas/__init__.py)는 XYZ를 정규화하고 이를 이미지 채널로 변환합니다, 원본은
[일반2이미지 파이프라인](https://github.com/lllyasviel/ControlNet/blob/main/gradio_normal2image.py) 은 조건부 텐서를 구성하기 전에 해당 채널을 반전시킵니다. `Zyx` 은 명시적인 반전을 허용합니다. 이는 범용 모델 사전 설정이나 비트 동일 MiDaS 전처리에 대한 주장이 아닙니다: 참조는 채널을 잘라내고 이 SDK 는 이를 반올림하며, 추정기, 기저 변환, 리사이징 및 배경 정책은 어댑터 책임으로 남아 있습니다.

내보내기에서는 네이티브 자산 그리드를 사용하고 `control.enabled`, 타임라인 및 레이어 프레임 범위를 준수합니다. 가시성, 불투명도, 아핀 변환 및 움직임을 표시하며, 노멀을 회전·재샘플하거나 컨디셔닝을 변경하지 않습니다. 일반 아트워크/ CanvasItem 구성 및 PDF는 레이어를 제외합니다; 명시적인 레이어 미리보기가 제공되며 기본 XYZ 색상을 일반 디스플레이 변환과 함께 사용합니다. PSD와 타임라인 교환은 지원되지 않는 일반 콘텐츠를 폐기하기보다 거부합니다.

<a id="editing-example"></a>

## 편집 예시

```cpp
Document doc;
doc.extent = {2, 1};
doc.timeline.frameCount = 2;
DocumentEditor editor(doc);
auto assetResult = editor.insertNormalMapAsset(
    {"normals", {2,1}, {{0,0,1}, {0.6,0,0.8}}});
NormalMapLayer layer;
layer.properties = {"normal-layer", "Surface normals"};
layer.source = StaticSource{"normals"};
auto layerResult = editor.insertNormalMapLayer(layer);
auto editResult = editor.setNormalMapSample("normals", 1, 0, {0,1,0});
auto map = renderNormalMapControlMap(doc, "normal-layer", 0);
// 출력을 사용하기 전에 각 편집 결과의 ok()와 map.ok()를 확인한다.
```

`replaceNormalMapAsset`는 ID를 유지하면서 전체 샘플 그리드를 대체합니다. `setNormalMapSample`는 동일한 검증된 편집 트랜잭션을 통해 하나의 좌표를 교체합니다. `setKeyframedSource`는 레이어를 프레임당 자산으로 전환합니다; `setControlNetSettings`는 컨디셔닝 구성을 편집합니다. 잘못된 벡터, 좌표, 참조 및 혼합된 자산 종류는 상태/수정을 그대로 유지합니다. DocumentEditor가 DocumentFile에 바인딩될 때, 이러한 작업은 동일한 트랜잭션을 동기화하여 지속합니다. `BitmapEditor`는 이 데이터를 바인딩하거나 페인팅할 수 없습니다.

<a id="verification"></a>

## 검증

`NormalMapTest.cpp`는 정확한 RGB 값, 서명된 구성 요소, 누락된 샘플, 채널 변환, 잘못된 데이터 롤백, 소스 타이핑, static/dynamic 해상도, 출력/ 직렬화 예산, 스냅샷 및 작업 파일 왕복, 체크섬 보정 오류 페이로드 거부, 디스플레이 분리 및 지원되지 않는 해외 내보내기를 포함합니다. 설치된 패키지 소비자는 동일한 계약 테스트를 실행합니다.

자세한 레이어 및 개체 필드는 다음을 통해 원자적으로 편집할 수도 있습니다.
모든 동적 소스 상태를 포함하는 [공통 매개변수 API](CONTROLNET_PARAMETERS.md).
