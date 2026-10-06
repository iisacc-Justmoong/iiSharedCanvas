<a id="canny-and-scribble-controlnet-layers-0170--format-111"></a>

# Canny와 Scribble ControlNet 레이어 ( 0.17.0 / 포맷 1.11 )

이 문서는 3 픽셀 기반 라인 컨트롤을 다룹니다. `isLineControlNetLayer(layer)`는 Line Art, Canny, Scribble 및 (0.18.0 이후) MLSD를 깊이, 포즈 또는 의미 구간을 그룹화하지 않고 식별합니다. MLSD는 편집 가능한 벡터 세그먼트를 사용합니다; MLSD .md를 참조하십시오. 모두 독립적인 `ControlNetKind` 아이덴티티와 정상적인 공유 설정을 유지합니다.

|유형|출처|네이티브 값|제어 출력|
| --- | --- | --- | --- |
| LineArtLayer | LineArtAsset |F64 커버리지 [0,1]|흰색 위에 검은 잉크, 부드러운 레벨 유지|
| CannyLayer | CannyAsset |U8 마스크, 정확히 0 또는 1|불투명한 검은색 위에 흰색 가장자리|
| ScribbleLayer | ScribbleAsset |U8 마스크, 정확히 0 또는 1|불투명한 검은색 위에 흰색 스트로크|

이진 소스 타입은 공개 집계 필드 `id`, 양수 `viewport` 및 `std::vector<std::uint8_t> mask` 를 포함하며, 정확히 너비 * 높이 행 우선 샘플을 포함합니다. 0 는 배경을 의미하고 1 은 선을 의미합니다. 기본 크기의 필드가 0 로 채워지면 빈 검은색입니다. 값 2 – 255 는 유효하지 않습니다: 이는 이진 논리 필드이며 0 와 255를 포함하는 이미지 바이트 배열이 아닙니다. 자산에 삽입하기 전에 들어오는 0/255 이미지 바이트를 명시적으로 변환하십시오. 배경에 투명성을 사용하지 마십시오 또는 원시 입력/포인터 궤적을 스트로크로 저장하지 마십시오.

2 소스 유형은 별칭이 아니며 서로 또는 래스터 /Line Art 데이터를 대체할 수 없습니다. 검증은 잘못된 소스 종류, 누락된 자산, 잘못된 차원 및 비이진 샘플을 거부합니다. 바이너리 마스크 검증/렌더링의 공유 구현은 공개 기본 클래스가 아니라 내부 세부 사항입니다.

<a id="authoring-and-timeline"></a>

## 저작 및 타임라인

```cpp
Document document;
document.extent = {2, 2};
DocumentEditor editor(document);
auto a = editor.insertCannyAsset({"edges", {2, 2}, {0, 1, 1, 0}});
CannyLayer edges;
edges.properties = {"edges.layer", "Canny"};
edges.source = StaticSource{"edges"};
auto b = editor.insertCannyLayer(edges);
auto c = editor.setCannySample("edges", 0, 0, 1);
auto output = renderCannyControlMap(document, "edges.layer", 0);
// a/b/c 결과와 output.ok()를 확인한다.
```

Scribble은 해당 `insertScribbleAsset`, `replaceScribbleAsset`, `insertScribbleLayer`, `setScribbleSample`, `renderScribbleControlMap` API를 제공합니다. Canny는 대량 편집을 위해 `replaceCannyAsset`도 제공합니다. Set-sample API는 u8 논리 값을 허용합니다; 호출자는 검증되지 않은 더 넓은 입력을 좁혀서는 안 됩니다. 거부된 편집은 상태/수정을 유지합니다. 바인딩된 DocumentEditor 작업은 DocumentFile를 통해 동기식으로 지속됩니다. 공유 소스 편집은 모든 참조에 영향을 미칩니다; 독립적인 콘텐츠를 위해 별도의 자산을 생성하십시오.

StaticSource는 하나의 자산을 보유합니다; KeyframedSource는 기존 보유 샘플링을 통해 자산을 선택합니다, 예를 들어. `insertCannyLayer(layer, {{0,"a"},{1,"b"}})`와 키프레임이 적용된 소스. 두 유형 모두 `StaticBitmap`와 `DynamicBitmap`를 지원합니다. 프레임은 서로 다른 네이티브 범위를 사용할 수 있습니다. 프레임 범위/타임라인 및 제어가 활성화되어 존중됩니다. 이진 마스크에는 시간 보간이 적용되지 않습니다.

제어 출력은 불투명 RGB32 픽셀과 정확한 네이티브 이진 `mask` 를 반환합니다. 이 출력에는 크기 조정, 부드럽게 만들기, 임계값 처리, 반전 또는 정규화가 없습니다. 표시 불투명도, 가시성, 변환 및 블렌딩은 조건부 데이터를 변경하지 않습니다. 일반 작업은 ControlNet 레이어를 제외합니다; 명시적 레이어 미리보기는 사용할 수 있습니다. 컬러 BitmapEditor 브러시는 타입화된 마스크에 바인딩할 수 없습니다.

래스터 미리보기는 잘못된 입력/예산에 대해 예외를 발생시키고, 렌더링 제어 맵은 오류 메시지를 반환합니다. 기본 출력 제한은 16 Mi 픽셀이며, 밀집 할당 전에 확인됩니다 (4 바이트 RGB + 1 바이트 마스크 픽셀당, 할당기 오버헤드를 제외합니다). 직렬화 는 각 종류를 독립적으로 64 Mi 총 샘플로 제한하며, 할당 전에 남은 바이트 및 주소 공간 한계로 확인됩니다 (maximumCannySamples / maximumScribbleSamples).

<a id="persistence-and-scope"></a>

## 지속성과 범위

패키지 0.17.0 는 9 자산과 9 레이어 변형을 가지며, 소비자를 다시 빌드해야 합니다. 포맷 1.11 는 Canny/Scribble 자산 태그 7/8 와 콘텐츠/역할 태그 5/6를 추가합니다. id 이후, 자산은 i32 차원, u64 개수 및 원시 u8 논리 샘플을 포함합니다. 레이어는 운동 전 공통 ControlNet 설정을 저장합니다. 더 오래된 형식은 이러한 형식을 거부합니다. 기존 1.0 – 1.10 기록은 읽을 수 있습니다. 네이티브 스냅샷과 SQLite 작업 파일의 왕복은 형식, 설정, 타임라인 및 마스크를 보존하며, 변경되지 않은 기록은 재사용됩니다. SQLite 스키마는 1상태를 유지합니다. PSD 와 타임라인 상호 교환은 지원되지 않는 조건부 콘텐츠를 거부하며, 타임라인 패키지 내 고아 바이너리 자산도 포함되지만 아무런 알림 없이 아무런 알림 없이 삭제하지 않습니다.

이 구현은 작성/저장된 조건 개체에 대한 것입니다. Canny 감지기를 실행하거나 사진에서 낙서를 파생하거나 확산 추론을 수행하지 않습니다. 마치 추출이 발생한 것처럼 검출기 임계값이 유지되지 않습니다.

검정색 배경/흰색 선 규칙은 원래 ControlNet 조정 경로를 따릅니다(갤러리 미리 보기는 별도로 반전됩니다).
- https://github.com/lllyasviel/ControlNet/blob/main/gradio_canny2image.py
- https://github.com/lllyasviel/ControlNet/blob/main/gradio_scribble2image.py
데이터 계약은 모델별 생성 품질을 암시하지 않습니다.

자세한 레이어 및 개체 필드는 다음을 통해 원자적으로 편집할 수도 있습니다.
모든 동적 소스 상태를 포함하는 [공통 매개변수 API](CONTROLNET_PARAMETERS.md).
