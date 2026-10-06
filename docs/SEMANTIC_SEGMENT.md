<a id="controlnet-semantic-segment-layers-0130"></a>

# ControlNet 의미 세그먼트 레이어 ( 0.13.0 )

`Layer` 는 전용 `SemanticSegmentLayer` 대안을 소유합니다. 도메인 계층 구조는 레이어 -> ControlNet 역할 -> SemanticSegment 종류입니다. 이는 구성이며, 임의의 메타데이터가 첨부된 비트맵 비트맵이 아닙니다. `layerRole()` 는 아트워크를 조건부에서 분리합니다; `controlNetKind()` 는 전문화된 제어를 식별합니다. 이전 타이밍/표현 축은 여전히 독립적입니다: 시맨틱 레이어는 출처에 따라 StaticBitmap 또는 DynamicBitmap 입니다.

<a id="object-contract"></a>

## 객체 계약

|물건|분야와 목적|
| --- | --- |
| `ControlNetSettings` |`enabled`, 모델 식별자/수정, 비음수 유한 조건화 척도, 정규화된 가이던스 시작/끝|
| `SemanticTaxonomy` |안정적인 분류 ID, 버전, 출처 URI, 클래스 컬렉션|
| `SemanticClass` |안정적인 숫자 ID, 고유 머신 키, 표시 이름, 설명, 카테고리, 선택적 부모, 외부 데이터셋 ID, 별칭, 정확한 모델-팔레트 ARGB 색상|
| `SemanticRegion` |안정적인 비영점 ID, 클래스 참조, 고유 ID 마스크 ARGB 색상, 이름, 설명, 선택적 인스턴스 ID/신뢰도, 주석 출처, 생성기, 소스 참조, 명명된 속성|
| `SemanticSegmentation` |분류학, 영역, 명시적 무효 정체성 색상 및 무효 조건 색상|
| `SemanticRegionGeometry` |프레임당 픽셀 수, 좁은 정수 경계, 픽셀 중심 중심점; 존재하지 않는 영역은 0 기하학을 갖습니다.|
| `SemanticControlMapResult` |정확한 컨디셔닝 픽셀, 고밀도 클래스 ID, 영역 ID, 유효한 픽셀 플래그, 영역 기하학 및 오류 메시지|

클래스 ID 는 분류학에 국한되며 0일 수 있습니다. @number@ 지역 ID 와 선택적 인스턴스 ID 는 0 이 아닙니다. 여러 지역은 하나의 클래스를 참조할 수 있으며, 동일한 객체를 설명할 때 여러 지역은 인스턴스 id 를 공유할 수 있습니다. 영역은 여러 개의 연결되지 않은 구성 요소를 포함할 수 있으며, 영역 식별자는 연결성을 주장하지 않습니다. 동일한 영역 정의는 모든 프레임 마스크에 적용되며 일부 프레임에서는 누락될 수 있습니다. 부모 클래스는 순환이 없는 계층 구조를 형성합니다. 속성은 주석을 설명하며 텍스트 프롬프트에 자동으로 삽입되거나 지시사항으로 실행되지 않습니다.

<a id="identity-colors-versus-model-colors"></a>

## 아이덴티티 색상과 모델 색상 비교

식별 마스크는 소유된 조밀한 `RasterAsset`이다. 각 불투명 픽셀은 영역의 정확한 `maskColor` 또는 `voidMaskColor`이다. 2명의 사람은 같은 사람 클래스를 참조하면서 서로 다른 빨간색·초록색 식별 색상을 가질 수 있다. 조건 내보내기는 두 영역을 해당 클래스의 `controlColor`로 다시 칠한다. 이로써 서로 다른 색상을 다른 의미 클래스로 간주하지 않으면서 독립적인 객체 편집을 유지한다. 대상 팔레트가 요구하면 출력 클래스 색상은 반복될 수 있지만 영역 색상은 고유해야 한다. 근사 매칭·알파 혼합·안티앨리어싱·손실 마스크 가져오기·자동 의미 추론은 없다.

분류 체계는 선택된 모델이 기대하는 팔레트를 명시적으로 기술해야 합니다. 모든 ControlNet와 호환된다고 주장되는 내장 팔레트는 없습니다. 원본 세분화 모델의 예제는 라벨 ID를 팔레트를 통해 매핑합니다; 기본을 참조하십시오.
[ControlNet 분할 모델 카드](https://huggingface.co/lllyasviel/control_v11p_sd15_seg). SDK는 모델을 다운로드하지 않으며 세분화/인페어런스 모델을 실행하지도 않습니다.

<a id="creation-editing-and-frame-behavior"></a>

## 생성, 편집 및 프레임 동작

```cpp
SemanticSegmentation semantics;
semantics.taxonomy.id = "my-scene";
semantics.taxonomy.version = "1";
SemanticClass person;
person.id = 1; person.key = "person"; person.name = "Person";
person.controlColor = 0xffaabbcc; // 실제 모델의 팔레트 값을 제공한다.
semantics.taxonomy.classes.push_back(person);
SemanticRegion region;
region.id = 10; region.classId = 1; region.maskColor = 0xffff0000;
region.name = "Left person"; region.instanceId = 100;
semantics.regions.push_back(region);

SemanticSegmentLayer layer;
layer.properties = {"segments", "Semantic segmentation"};
layer.source = StaticSource{"identity-mask"}; // 해당 RasterAsset을 먼저 삽입한다.
layer.segmentation = semantics;
editor.insertSemanticSegmentLayer(layer);
auto controlMap = renderSemanticControlMap(document, "segments", currentFrame);
```

애니메이션을 위해 `source = KeyframedSource{}` 를 설정하고 프레임 배치를 `insertSemanticSegmentLayer` 에 공급합니다. Frame-0  콘텐츠는 필수이며, 다음 키가 나올 때까지 콘텐츠는 유지됩니다. 참조된 모든 마스크는 동일한 차원을 가져야 합니다. `setStaticSource`  /  `setKeyframedSource`  의 변경 타이밍을 잃지 않고 조정합니다. `setSemanticSegmentation`  와  `setControlNetSettings`  는 정의와 제어 매개변수를 원자적으로 업데이트합니다. 기존 자산 교체는 참조하는 모든 의미론적 마스크를 유효하게 검증합니다. 거절된 편집은 상태/버전을 보존하며,  `DocumentFile` 에 대해서도 마찬가지입니다. `isSemanticMaskAsset`  는 편집자가 범주별 소유권을 감지할 수 있게 합니다. `BitmapEditor`  는 이러한 마스크에 연성 브러시 결합을 거부합니다 (의미론적 소유권이 추가된 후 기존 결합 포함);  `replaceRasterPixels` 를 사용하여 정확한 픽셀을 사용하세요. 마스크 색상과 정의를 함께 변경하려면 새 마스크 자산과  `replaceLayer` 를 사용하여 유효한 교체를 구성한 다음 사용하지 않는 이전 자산을 제거합니다.

<a id="rendering-and-coordinate-contract"></a>

## 렌더링 및 계약 조정

`renderSemanticControlMap`  는 현재 소스 프레임을 샘플링하고 네이티브 마스크 해상도에서 불투명 클래스 색상을 방출합니다. 타임라인/범위와  `control.enabled` 를 존중합니다. 표시 가시성, 불투명도, 운동 및 변환은 적용되지 않습니다: 출력 좌표는 명시적으로 로컬 마스크 좌표입니다. 기본 16 Mi-pixel 예산은 출력 할당량을 제한하며, 호출자는 `maxPixels` 을 제공할 수 있습니다. 0유효한 픽셀 버퍼는 빈 공간과 유효한 클래스 ID 를 구별합니다. 기하학은 픽셀 중심 (x+0.5,y+0.5 )을 사용하며, 정의 순서로 선언된 모든 영역을 덮습니다. 클래스/지역 조회는 안정적인 id 로 이용 가능합니다.

일반 프레임 합성·CanvasItem 아트워크 타일·PDF 아트워크 출력은 ControlNet 레이어를 제외한다. 명시적인 레이어별 렌더링은 여전히 식별 색상 프리뷰를 제공하며 ControlNet 역할을 담는다. 표시 프리뷰는 변환될 수 있으므로 무손실 조건 맵으로 사용해서는 안 된다. 모델 배율과 가이던스 필드는 소비하는 추론 런타임을 위한 저장된 의도이다.

<a id="persistence-and-limits"></a>

## 끈기와 한계

스냅샷은 `.iisc` 1.7를 필요로 하며, 작업 파일 레코드는 정확히 동일한 버전의 레이어 확장을 재사용합니다; SQLite 스키마는 1로 유지됩니다. 모든 클래스/지역/제어 필드는 의미론 전용 증분 편집을 포함하여 유지됩니다. 스냅샷 1.0 – 1.6는 여전히 읽을 수 있습니다. 이전 버전 안전하게 거부한다에서 의미론적 콘텐츠를 저장합니다. 패키지 ABI 0.13는 새로운 레이어 대안을 위해 소비자가 재구성해야 합니다.

문서 유효성 검사는 레이어당 최대 65,536 개의 클래스와 1,048,576 개의 영역을 허용합니다. 직렬화 는 문서 전체의 클래스, 영역, 별칭 및 속성을 각각 독립적으로 제한하며, 기존 문자/컨테이너/픽셀 예산도 함께 제한합니다. 할당 전에 사용 가능한 바이트에 대해 수집 크기를 검증하여 디코딩합니다. 알 수 없는 역할/출처 태그, 사이클, 중복된 id/키/마스크 색상, 잘못된 클래스 참조, 잘못된 신뢰도/스케일/범위 및 매핑되지 않은 마스크 픽셀은 거부됩니다. 빈 색상과 모든 마스크/모델 팔레트 색상은 불투명해야 합니다.

PSD 와 타임라인 XML 에서 클래스, 인스턴스, 출처 및 제어 데이터를 아무런 알림 없이  대신 시맨틱 레이어를 거부합니다. 편집 가능한 상호 교환을 위한 네이티브 문서와 모델 입력을 위한 전용 정확한 맵을 사용하십시오. 자동 분할, 모델 팔레트 발견, GPU  추론 및 라이브 소스 캡처는 이 레이어의 계약 범위를 벗어납니다.

0.14.0이후, ControlNetSettings는 ControlNet / ControlNet .h에 거주하며 PoseLayer와 공유됩니다. 이 헤더는 기존 시맨틱 API 소비자를 위해 이를 포함합니다.

자세한 레이어 및 개체 필드는 다음을 통해 원자적으로 편집할 수도 있습니다.
모든 동적 소스 상태를 포함하는 [공통 매개변수 API](CONTROLNET_PARAMETERS.md).
