<a id="depth-controlnet-layers-0150--native-format-19"></a>

# 깊이 ControlNet 레이어 ( 0.15.0 / 네이티브 형식 1.9 )

`DepthLayer`는 `DepthAsset`가 지원하는 전용 `LayerRole::ControlNet` / `ControlNetKind::Depth` 레이어입니다. 그 표현은 비트맵입니다. `StaticSource`는 매 프레임마다 하나의 깊이 필드를 보유하고, `KeyframedSource`는 기존 타임라인 프레임/홀드 및 레이어 범위 규칙을 통해 다른 필드를 선택합니다.

<a id="value-contract"></a>

## 가치 계약

깊이는 **정규화된 근접도**로 작성되었으며, 거리 측정치나 계산된 역거리 공식이 아닙니다. `0` 는 비어있는/열린 공간을 의미하며, `1` 는 캔버스 카메라와의 접촉을 의미합니다. 양수 중간 값은 점유된 공간을 나타내며, 값이 클수록 카메라에 더 가깝습니다. 영원 평면 기하학은 영에서 없고 추가적인 알 수 없는 값 센티넬도 없습니다. 카메라 보정, 클리핑 평면, 단위, 깊이 추정 모델, 또는 시점 재구정이 암시되지 않습니다.

`DepthAsset`는 ID가 양수인 `CanvasExtent viewport`를 포함하며, 정확히 `width * height` 유한 `double` 값을 행 순서로 정렬합니다. 도메인은 [0,1]입니다. 좌표 주소 네이티브 자산 픽셀 (x 오른쪽, y 아래). 범위 밖의 값, NaN /infinity, 크기가 일치하지 않는 경우, 누락된 자산 및 잘못된 출처 유형은 트랜잭션 방식으로 거부됩니다. 투명도를 사용하여 빈 공간을 인코딩하지 마십시오.

네이티브 필드는 IEEE 바이너리64 정밀도를 유지합니다. `depthRasterPreview` 와 `renderDepthControlMap` 맵은 모든 RGB 채널에서 `round(value * 255)` 에 매핑되며, 알파 255를 포함합니다. 따라서 0 = 불투명한 검정, 0.5 = RGB ( 128,128,128 ), 1 = 불투명한 흰색입니다. 이것은 직접적인 선형 수치 인코딩입니다: 제어 출력에 sRGB 감마, 프레임별 최소/최대 정규화, 반전, 보간 또는 알파 블렌딩이 적용되지 않습니다. 매우 작은 양의 값은 RGB8 에서 검정으로 양자화될 수 있습니다; 출력의 원래 `values` 와 `occupiedPixels` (값이 0 보다 큼) 은 완전히 비어 있는 공간과 구별을 유지합니다. 정밀도가 필요한 소비자는 미리보기에서 깊이를 복구하는 대신 `values` 를 사용해야 합니다.

<a id="editing-and-output"></a>

## 편집 및 출력

```cpp
Document document;
document.extent = {2, 2};
DocumentEditor editor(document);
auto assetResult = editor.insertDepthAsset(
    {"depth.frame0", {2, 2}, {0.0, 0.25, 0.5, 1.0}});
DepthLayer layer;
layer.properties = {"depth.layer", "Depth"};
layer.source = StaticSource{"depth.frame0"};
auto layerResult = editor.insertDepthLayer(layer);
auto editResult = editor.setDepthSample("depth.frame0", 1, 0, 0.375);
auto map = renderDepthControlMap(document, "depth.layer", 0);
// 데이터를 사용하기 전에 편집 결과와 map.ok()를 확인한다.
```

대량 편집에는 `replaceDepthAsset`를, 개별 픽셀에는 `setDepthSample`를 사용하십시오. 각 편집은 다른 문서 작업과 동일한 검증, 롤백, 리비전 및 파일 바인드 트랜잭션 규칙을 사용합니다. 공유 소스 자산 편집은 이를 참조하는 모든 레이어/프레임을 업데이트합니다; 독립적인 콘텐츠가 필요할 경우 먼저 별도의 자산을 삽입하십시오. 컬러 `BitmapEditor` 브러시는 깊이 자산을 바인딩할 수 없습니다. 기본으로 생성된 필드는 값의 크기를 명시적으로 지정해야 합니다; 빈 캔버스의 경우 0.0를 입력하십시오.

동적 깊이를 위해 `insertDepthLayer(layer, {{0,"a"},{1,"b"}})`와 `KeyframedSource{}` 또는 `setKeyframedSource`를 사용하십시오. 프레임 소스는 서로 다른 네이티브 범위를 가질 수 있으며, 내보낸 지도 차원은 선택된 자산의 차원입니다. 프레임 변경 시 시간 보간이나 자동 깊이 정규화가 발생하지 않습니다.

`ControlNetSettings` 는 시맨틱 섹먼트와 자세와 공유됩니다. 제어 맵은 `enabled`, 타임라인 경계 및 레이어 프레임 범위를 존중합니다. 표시 가시성, 불투명도, 블렌드 모드 및 변환은 네이티브 제어 값을 변경하지 않습니다. 일반 아트웍 구성은 레이어를 제외하며, 명시적인 레이어 미리보기는 사용 가능하고 표시 변환을 사용할 수 있습니다. 출력은 최대 16 Mi 픽셀로 기본 설정되며, RGB, 스칼라 및 점유율 버퍼 할당 전에 예산이 확인됩니다 (13 바이트/샘플, 할당기 오버헤드 제외). `depthRasterPreview` 는 유효하지 않은 데이터/한계에 대해 예외를 던지고, `renderDepthControlMap` 는 메시지를 보고합니다.

<a id="persistence-and-interchange"></a>

## 지속성 및 교환

패키지 0.15.0는 공개 변형 레이아웃(6 자산 및 6 레이어 변형)을 변경하므로 소비자는 재구성해야 합니다. 네이티브 형식 1.9는 자산 태그 5, 키프레임이 적용된 콘텐츠 태그 3, 레이어‐롤 태그 3를 추가합니다. 이전 형식은 깊이를 버리기보다 거부합니다. 기존 1.0 – 1.8 문서는 여전히 읽을 수 있습니다.

깊이 자산은 id, i32 너비, i32 높이, u64 샘플 수, 그 다음 행 우선 리틀 엔디안 f64 샘플을 저장합니다. 깊이 레이어는 일반 속성/소스 및 ControlNet 활성화/ modelId / modelRevision / conditioningScale / guidanceStart / guidanceEnd 필드를 운동 전에 사용합니다. 개수, 입력 바이트 사용 가능 여부, 주소 공간 및 전역 `SerializationLimits::maximumDepthSamples` (모든 자산에 대한 기본 64 Mi 샘플) 은 할당 전에 확인됩니다. 문서 검증에서 인코딩/디코딩 시 유효하지 않은 값이 거부됩니다.

스냅샷과 SQLite 작업 파일 경로는 모두 정확한 값을 유지합니다. 작업 파일은 변경된 자산을 기록하고 변경되지 않은 레코드를 재사용합니다; 스키마 버전은 1로 유지됩니다. PSD와 타임라인 교환은 지원되지 않는 ControlNet 콘텐츠를 거부하며, 타임라인 패키지에 포함된 고아 깊이 자산을 포함합니다. 이 스칼라 계약에 의해 외부 모델 호환성이나 생성된 이미지 품질이 주장되지 않습니다.

자세한 레이어 및 개체 필드는 다음을 통해 원자적으로 편집할 수도 있습니다.
모든 동적 소스 상태를 포함하는 [공통 매개변수 API](CONTROLNET_PARAMETERS.md).
