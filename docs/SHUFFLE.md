<a id="shuffle-controlnet-layer"></a>

# ControlNet 레이어를 섞다

패키지 0.20.0는 전용 Shuffle 변형을 추가합니다; 소비자는 재구성해야 합니다. 네이티브 모델 버전은 1.14이며, SQLite 작업 파일 스키마는 여전히 1입니다. 새로운 의존성, 모델 다운로드 또는 추론 백엔드가 도입되지 않습니다.

<a id="data-and-layer-identity"></a>

## 데이터 및 계층 정체성

RGB 콘텐츠/참조 이미지에서 조건 생성을 셔플합니다. 네이티브 객체는 **가 준비한 컨디셔닝 이미지**이며, 숨겨진 전처리가 없습니다:

- `ShuffleColor { uint8_t red, green, blue }`는 정확한 RGB8 채널을 저장합니다.
- `ShuffleAsset { id, viewport, vector<ShuffleColor> colors }`는 양의 너비/높이와 정확히 width*height 행‐주요 색상을 가지고 있습니다. 알파 또는 색상 정규화가 적용되지 않으며, 전체가 검은색인 이미지가 유효한 콘텐츠입니다.
- `ShuffleLayer { properties, source, control }` 참조는 ShuffleAsset 만 참조합니다. `LayerRole::ControlNet`, `ControlNetKind::Shuffle`, 그리고 `StaticBitmap` 또는 `DynamicBitmap` 입니다. 이는 라인 제어 레이어가 아닙니다.

정적 레이어는 하나의 이미지를 유지합니다. 키프레임이 적용된 소스는 선택된 이미지를 다음 자산 참조 키프레임까지 보유합니다; 일반적인 프레임 선택은 콘텐츠를 무작위화하지 않습니다. 모든 공유 모델 ID/수정, 컨디셔닝 스케일 및 가이드 설정이 제공됩니다. 소스 차원은 캔버스 차원과 다를 수 있습니다.

<a id="preparing-a-shuffled-image"></a>

## 섞인 이미지를 준비하고 있습니다

공식 [ContentShuffleDetector](https://github.com/lllyasviel/ControlNet-v1-1-nightly/blob/main/annotator/shuffle/__init__.py) 는 선형 샘플링을 사용하여 소음에서 유도된 X/Y 좌표를 통해 소스를 리매핑합니다. 이 SDK 는 NumPy / OpenCV 랜덤 전처리 파이프라인의 비트 동일 복사가 아닌 명시적인 리매핑 작업을 제공합니다.

`makeShuffleAsset(id, source, outputExtent, coordinates, maxPixels=16Mi)` 는 불투명한 RasterLayer 와 출력 픽셀당 하나 `ShuffleCoordinate { double u, v }` 를 받습니다. 좌표는 [0,1]내에서 유한해야 합니다. 그들은 `(u*(width-1), v*(height-1))` 주원소 픽셀 중심으로 매핑되며, 4 주위 RGB 값은 선형적으로 보간된 후 가장 가까운 정수로 반올림됩니다. 엔드포인트와 원소 픽셀 소스가 지원됩니다. 보간은 선형 광선이 아닌 인코딩된 RGB 채널 공간에서 수행됩니다. 투명 입력은 거부되며, 호출자는 명시적으로 배경을 합성합니다.

호출자가 시드로 생성한 노이즈를 포함하여 필드를 제공한다. 반환되는 분리된 자산은 최종 색상을 포함하며 외부 원본 참조를 소유하지 않는다. 원본과 좌표 필드는 모두 변경하지 않는다. insertShuffleAsset 또는 replaceShuffleAsset로 영속 저장한다. 외부 감지기가 준비한 맵도 직접 저장할 수 있다. 난수 시드, 원본 궤적 또는 재생성 명령은 영속 저장하지 않는다.

```cpp
Document doc;
doc.extent = {2,1};
DocumentEditor editor(doc);
RasterLayer source = {2,1,{0xffff0000,0xff0000ff}};
auto prepared = makeShuffleAsset("colors", source, {2,1}, {{1,0},{0,0}});
auto assetResult = editor.insertShuffleAsset(std::move(prepared));
ShuffleLayer layer;
layer.properties = {"shuffle", "Shuffle reference"};
layer.source = StaticSource{"colors"};
auto layerResult = editor.insertShuffleLayer(layer);
auto map = renderShuffleControlMap(doc,"shuffle",0); // blue, red
// 출력을 사용하기 전에 편집 결과와 map.ok()를 확인한다.
```

<a id="editing-rendering-and-persistence"></a>

## 편집, 렌더링 및 지속성

`setShuffleSample`는 검증된 DocumentEditor 트랜잭션을 통해 하나의 색상을 편집합니다. `replaceShuffleAsset`는 전체 이미지 보존 ID를 교체합니다. `insertShuffleLayer`, `setKeyframedSource` 및 `setControlNetSettings`는 공유 계약을 따릅니다. 실패한 좌표, 차원, 참조 또는 설정은 데이터와 수정 사항을 보존합니다. DocumentFile에 바인드된 편집은 동기적으로 커밋되며, 변경되지 않은 색상 데이터는 제어 전용 편집에 재사용됩니다. BitmapEditor는 이 전용 자산 유형을 바인딩할 수 없습니다.

`shuffleRasterPreview` 는 정확한 불투명한 RGB 픽셀을 반환합니다. `renderShuffleControlMap` 는 해당 픽셀, 정확한 색상 및 메시지/확인 상태를 반환합니다. 둘 다 16 Mi 출력 픽셀 예산으로 기본값이며, 출력 할당 전에 확인됩니다. 제어 내보내기는 활성화, 프레임 범위 및 타임라인을 존중하지만 표시 가시성, 불투명도 및 아핀/운동 변환을 무시합니다. 그것은 이미지를 재배열하거나 정규화하거나 리사이즈하지 않습니다. 리매핑 및 직접 미리보기 API 는 잘못된 입력에 대해 invalid_argument 를, 초과된 출력 예산에 대해 length_error 를 던집니다; 문서 내보내기는 실패를 반환합니다.

일반 아트워크/ CanvasItem / PDF 구성은 ControlNet 레이어를 제외합니다. 명시적인 레이어별 미리보기가 계속 제공됩니다. PSD와 타임라인 교환은 조용한 손실을 방지하기 위해 콘텐츠를 섞는 것을 거부하며, 타임라인 교환에 포함된 고아 자산을 포함합니다. 네이티브 스냅샷 및 작업 파일은 모든 RGB 채널과 소스 키프레임를 보존합니다.

<a id="verification"></a>

## 검증

ShuffleTest는 정확한 채널 출력, 아이덴티티/코너/센터/원픽셀 샘플링 재매핑, 잘못된 좌표/알파/카운트, 출력 및 누적된 직렬화 예산, static/dynamic 분류 및 선택, 제어/디스플레이 분리, 트랜잭션 롤백, 잘못된 체크섬-정확 스냅샷, 작업 파일 재개, 레코드 재사용 및 지원되지 않는 외부 내보내기를 포함합니다. 설치된 소비자는 동일한 계약 테스트를 실행합니다. 이는 실제 ControlNet 추론 품질을 입증하지 않습니다.

자세한 레이어 및 개체 필드는 다음을 통해 원자적으로 편집할 수도 있습니다.
모든 동적 소스 상태를 포함하는 [공통 매개변수 API](CONTROLNET_PARAMETERS.md).
