<a id="reference-conditioning-layer"></a>

# 참조 조건화 레이어

패키지 0.22.0는 네이티브 레퍼런스 자산/레이어 변형을 도입합니다. 소비자는 재구성해야 합니다; 스냅샷 모델은 1.16이며 SQLite 작업 파일 스키마는 1로 유지됩니다.

<a id="meaning"></a>

## 의미

레퍼런스 레이어는 이미지와 레퍼런스 애플리케이션 계약을 추론 어댑터에 제공합니다. 원본 확장은 [레퍼런스 전용 어텐션 컨트롤](https://github.com/Mikubill/sd-webui-controlnet/discussions/1236)와
[AdaIN / attention+ AdaIN](https://github.com/Mikubill/sd-webui-controlnet/discussions/1280). 이러한 모드들은 전용 ControlNet 체크포인트를 로드할 필요가 없습니다. `control.modelId`는 선택 사항이며 비어 있을 수 있습니다; 모델명이 만들어지거나 다운로드되지 않았습니다.

레이어는 SDK 의 ControlNet 조건부 계열에 속하므로 선택, 활성화/가중치/가이드, 타임라인, 지속성 및 내보내기 계약을 공유합니다. 그것은 임베딩이나 학습된 특징 맵 대신 RGB 참조 픽셀을 저장합니다. 그것은 주의 후크, AdaIN , IP -어댑터, 얼굴 신원 매칭, 스타일 추출, VAE 인코딩 또는 모델 추론을 구현하지 않습니다. 소비하는 어댑터는 요청된 모드를 명시적으로 지원해야 하며, 아무런 알림 없이 다른 조건부 기법을 대체해서는 안 됩니다. 모델별 지원 및 유효한 충실도 동작 ( CFG /control-mode 상호작용 포함) 은 어댑터 책임으로 남습니다.

<a id="objects"></a>

## 객체

`ReferenceColor { uint8_t red, green, blue }`는 RGB8를 저장합니다. `ReferenceAsset { id, viewport, colors }`는 양의 치수와 정확히 width*height 행-주요 색상을 보유하고 있습니다. 그것의 네이티브 해상도는 문서 캔버스와 다를 수 있습니다. 모든 채널이 정확히 보존됩니다; 불투명한 검은색은 유효한 참조 내용입니다.

`ReferenceSettings`에는 다음이 포함됩니다.

|필드|계약|
| --- | --- |
|모드|주의(기본값), AdaIN, 또는 AttentionAdaIN|
| styleFidelity |유한 [0,1], 기본값 0.5; 어댑터에서 요청된 참조 스타일 충실도|

주의는 `reference_only`, AdaIN에서 `reference_adain`로, 그리고 AttentionAdaIN에서 `reference_adain+attn`로 매핑됩니다. Fidelity는 불투명도, RGB 필터 또는 픽셀 블렌드가 아닙니다. 알 수 없는 열거형 값, NaN /infinite 충실도 및 범위를 벗어나는 값이 유효하지 않습니다.

`ReferenceLayer { properties, source, control, reference }` 는 공통 ControlNet 설정과 참조 전용 설정을 모두 소유합니다. 역할은 ControlNet 이며, 종류는 Reference 입니다. 이는 라인 제어 레이어가 아닙니다. StaticSource 는 StaticBitmap 를 생성하고, KeyframedSource 는 DynamicBitmap 를 생성합니다. 정수 프레임 소스 변경은 유지 방식만 사용하는 로만 유지되며 0프레임부터 시작합니다. 참조 설정은 레이어 전체에 적용되며, 이미지 콘텐츠 변경은 키프레임 를 통해만 이루어집니다.

<a id="editing-and-output"></a>

## 편집 및 출력

`makeReferenceAsset(id, opaqueRaster, maxPixels=16Mi)`는 크기 조정, 자르기, 색상 정규화 또는 특징 추출 없이 네이티브 RGB를 복사합니다. 투명 입력은 거부되므로 호출자는 합성 배경을 선택합니다. 잘못된 입력은 invalid_argument를 발생시킵니다; 출력 예산을 초과하면 할당 전에 length_error를 발생시킵니다. 집계 구조는 준비된 RGB 이미지를 직접 지원합니다.

insertReferenceAsset, replaceReferenceAsset, insertReferenceLayer, setReferenceSample 및 setReferenceSettings를 DocumentEditor를 통해 사용하십시오. 일반적인 setKeyframedSource와 setControlNetSettings도 적용됩니다. 잘못된 편집은 상태/수정을 유지합니다. 파일에 바인드된 편집은 동기식으로 커밋되며, 레이어 설정 중 하나를 변경하면 변경되지 않은 이미지 레코드가 재사용됩니다.

`referenceRasterPreview`  불투명한  RGB  픽셀을 반환합니다. `renderReferenceControlMap` 는 픽셀을 반환하며, 정확한 색상, `reference` 설정, `control` 설정 및 메시지/확인 상태를 반환합니다. 현재 프레임 이미지를 선택하며 활성화/프레임 범위/타임라인을 준수합니다. 가시성, 불투명도, 아핀 변환 및 운동 표시는 조건을 변경하지 않습니다. 스타일 충실도는 RGB 를 내보낼 때 절대 변경하지 않습니다. 두 출력 모두 16 Mi 픽셀 할당 예산으로 기본값입니다. 직접 미리보기는 invalid_argument 또는 length_error 를 발생시키며, 문서 내보내기는 ok 가 아닌 결과를 반환합니다.

```cpp
Document doc;
doc.extent = {2,1};
DocumentEditor editor(doc);
RasterLayer input = {2,1,{0xffbb8844,0xff226699}};
auto assetResult = editor.insertReferenceAsset(makeReferenceAsset("reference",input));
ReferenceLayer layer;
layer.properties = {"reference-layer", "Style reference"};
layer.source = StaticSource{"reference"};
layer.reference = {ReferenceMode::AttentionAdaIN,0.7};
auto layerResult = editor.insertReferenceLayer(layer);
auto output = renderReferenceControlMap(doc,"reference-layer",0);
// 편집 결과/output.ok()를 확인하고 출력 픽셀과 설정을 어댑터에 전달한다.
```

일반 아트워크/ CanvasItem / PDF 구성은 레이어를 제외합니다; 각 레이어별 명시적인 미리보기가 제공됩니다. PSD와 타임라인 교환은 지원되지 않는 참조 의미를 거부하며, 타임라인 패키지의 고아 참조 자산을 포함합니다. BitmapEditor는 이 전용 데이터 유형을 바인딩할 수 없습니다. 네이티브 스냅샷 및 작업 파일은 RGB, 프레임 참조, 설정 객체 및 정확한 바이너리64의 충실도를 보존합니다.

<a id="verification"></a>

## 검증

ReferenceTest는 정확한 RGB, 불투명한 가져오기, 타입이 지정된 static/dynamic 소스, 모든 3 모드 왕복, 트랜잭션 롤백을 포함한 충실도 경계 및 모드 거부, 잘못된 체크섬 수정 모드/값/태그/카운트 데이터, 직렬화/출력 예산, 작업 파일 재개 및 설정 전용 레코드 재사용을 포함합니다. 설치된 소비자는 동일한 계약을 실행합니다. 실제 참조 기반 생성은 테스트되지 않았습니다.

자세한 레이어 및 개체 필드는 다음을 통해 원자적으로 편집할 수도 있습니다.
모든 동적 소스 상태를 포함하는 [공통 매개변수 API](CONTROLNET_PARAMETERS.md).
