<a id="line-art-controlnet-layers-0160--native-format-110"></a>

# 라인 아트 ControlNet 레이어 ( 0.16.0 / 네이티브 형식 1.10 )

`LineArtLayer`는 전용 `LayerRole::ControlNet` / `ControlNetKind::LineArt` 레이어입니다. 이는 `LineArtAsset`를 참조하며, 그 ID와 양의 뷰포트 차원 및 행장부호 `std::vector<double> coverage`가 작성된 선 이미지를 설명합니다. 소스는 [0,1]에 있는 너비 *의 유한 샘플을 정확히 포함해야 합니다.

<a id="ink-and-background"></a>

## 잉크와 배경

커버리지 0는 깨끗한 흰색 배경이며, 1는 전체 검은색 잉크입니다. 중간값은 부분적인 잉크 커버리지/라인 강도를 나타내며, 안티앨리어싱된 가장자리와 부드러운 윤곽을 유지합니다. 이는 거리, 클래스 마스크, 혹은 입력 브러시 움직임 목록이 아니라 라인아트 필드입니다. 채워진 어두운 영역은 합법이며, 가장자리 감지, 얇아짐, 임계값 설정, 자동 역전, 감마 변환 또는 프레임당 정규화는 수행되지 않습니다.

네이티브 커버리지는 이진64 정밀도를 유지합니다. `lineArtRasterPreview` 와 `renderLineArtControlMap` 은 `round((1 - coverage) * 255)` 를 각 RGB8 채널에 대해 255로 계산합니다. 커버리지 0/0.5/1 는 따라서 흰색/128-회색/검은색을 생성합니다. 빈 영역은 불투명한 흰색이며, 투명하지 않습니다. 매우 작은 양의 커버리지는 흰색으로 반올림될 수 있으므로, 제어 결과에도 원래 `coverage` 와 `inkPixels` (커버리리 > 0) 가 함께 포함됩니다. 이것들은 미리보기 양자화와 독립적으로 약한 작성된 줄을 독립적으로 유지합니다. `inkPixels` 를 임계값 처리된 에지 맵으로 해석하지 마십시오.

<a id="staticdynamic-editing"></a>

## 정적/동적 편집

```cpp
Document document;
document.extent = {2, 2};
DocumentEditor editor(document);
auto assetResult = editor.insertLineArtAsset(
    {"lines.frame0", {2, 2}, {0.0, 0.5, 1.0, 0.0}});
LineArtLayer layer;
layer.properties = {"lines", "Line Art"};
layer.source = StaticSource{"lines.frame0"};
auto layerResult = editor.insertLineArtLayer(layer);
auto editResult = editor.setLineArtSample("lines.frame0", 1, 0, 0.75);
auto map = renderLineArtControlMap(document, "lines", 0);
// 출력을 사용하기 전에 연산 결과와 map.ok()를 확인한다.
```

정적 콘텐츠는 타임라인 전체에 걸쳐 하나의 자산을 유지합니다. 동적 콘텐츠는 `KeyframedSource{}`와 `insertLineArtLayer(layer, {{0,"a"},{1,"b"}})` 또는 `setKeyframedSource`를 사용합니다; 참조 샘플링은 기존 유지 방식만 사용하는 타임라인 규칙을 따릅니다. 이것들은 `StaticBitmap` 및 `DynamicBitmap`에 매핑됩니다. 각 프레임 자산은 고유한 네이티브 차원을 가지고 있습니다. 시간 보간은 도입되지 않습니다.

`setLineArtSample` 를 개별 픽셀에 사용하고 `replaceLineArtAsset` 를 대량 업데이트에 사용하십시오. 모든 편집은 원자적으로 유효성 검사와 롤백을 수행하며, 수정 사항 및 파일 단위의 트랜잭션도 포함됩니다. 여러 프레임/레이어에 의해 공유되는 자산은 모든 참조에 대해 변경되며, 독립적인 편집이 필요한 경우 별도의 자산을 생성하십시오. 새로 크기 조정된 커버리리 배열에 0.0 를 채워 빈 흰색 필드를 생성합니다. `BitmapEditor` 는 이 형식의 스칼라 자산을 바인딩할 수 없습니다. 새 도메인 코드는 Qt 의존성이 없으며, 미리보기는 기존 렌더링 스택을 재사용합니다.

제어 출력은 `ControlNetSettings::enabled`, 타임라인 및 프레임 범위를 따르며, 표시 가시성, 불투명도, 변환 및 블렌딩을 무시합니다. 일반적인 아트웍 구성은 ControlNet 레이어를 제외하며, 명시적인 레이어 미리보기는 여전히 사용 가능합니다. 출력 할당 기본값은 최대 16 Mi 픽셀 ( RGB8, 이진64 커버리지 및 u8 존재: 할당기 오버헤드 이전 13 바이트/샘플) 입니다. `lineArtRasterPreview` 는 잘못된 입력 또는 예산 위반에 대해 invalid_argument / length_error 를 던지며, `renderLineArtControlMap` 는 오류 메시지를 반환합니다.

<a id="files-and-boundaries"></a>

## 파일 및 경계

패키지 0.16.0 는 7 자산 및 레이어 대안을 가지며, 소비자의 재빌드를 요구합니다. 포맷 1.10 는 자산 태그 6, 키프레임이 적용된 콘텐츠 태그 4, 레이어 역할 태그 4를 추가합니다. 자산은 id, i32 너비/높이, u64 개수 및 리틀엔디안 f64 커버리지를 저장합니다. 레이어는 일반적인 운동 데이터 이전에 공유 ControlNet 활성화/ modelId / modelRevision /스케일/가이드 필드를 저장합니다. 이전 포맷은 라인 아트를 거부하며, 기존 1.0 – 1.9 데이터는 읽을 수 있습니다. 알 수 없는 태그와 잘못된 소스 종류 안전하게 거부한다 입니다.

`SerializationLimits::maximumLineArtSamples`는 모든 자산에 대해 64 Mi 샘플로 기본값으로 설정됩니다. 카운트, 사용 가능한 바이트 및 주소 공간 경계는 할당 전에 확인됩니다. 스냅샷 및 SQLite 작업 파일 편집은 원본 범위를 유지하고, 변경되지 않은 레코드를 재사용하며, SQLite 스키마 1를 유지합니다. PSD와 타임라인 교환은 지원되지 않는 ControlNet 의미 체계를 거부하고, 타임라인 패키지의 고아 라인아트 자산을 포함하며, 아무런 알림 없이를 평탄화하거나 폐기하는 대신 이를 비활성화합니다.

이 API는 저작된 컨디셔닝 데이터를 제공합니다. 이미지-라인 주석자를 실행하거나 벡터를 추적하거나 확산 모델을 실행하거나 특정 모델의 입력 규칙과의 호환성을 주장하지 않습니다. 정규 출력은 흰색 바탕에 검은 잉크이며, 모델별 변환은 명시적인 소비자 어댑터에 해당합니다.

자세한 레이어 및 개체 필드는 다음을 통해 원자적으로 편집할 수도 있습니다.
모든 동적 소스 상태를 포함하는 [공통 매개변수 API](CONTROLNET_PARAMETERS.md).
