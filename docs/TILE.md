<a id="tile-controlnet-layer"></a>

# 타일 ControlNet 레이어

패키지 0.21.0는 전용 타일 자산/레이어 변형을 추가하고 소비자에게 재구축을 요구합니다. 네이티브 모델 버전은 1.15이며, SQLite 작업 파일 스키마는 여전히 1입니다.

<a id="meaning-and-data-contract"></a>

## 의미와 데이터 계약

ControlNet 타일은 공간 이미지 컨텍스트를 사용하여 세부 재생을 안내합니다. 그
[공식 타일 설명](https://github.com/lllyasviel/ControlNet-v1-1-nightly#controlnet-11-tile)는 로컬 컨텍스트 컨디셔닝 및 디테일 교체를 설명하며, 별도의 타일 디퓨전 워크플로와의 사용을 포함합니다. 이 SDK 모델은 RGB가 참조합니다. 누락된 세부 사항을 추론하거나 확산/업스케일링 알고리즘을 선택하지 않습니다.

- `TileColor { uint8_t red, green, blue }`는 정확한 인코딩된 RGB8 값을 저장합니다.
- `TileAsset { id, viewport, vector<TileColor> colors }`는 양의 치수와 정확히 width*height 색상을 행-major 공간 순서로 보유합니다. 검은색은 유효한 콘텐츠입니다. 알파, 마스크, 반복 패턴 또는 숨겨진 셔플 연산이 없습니다.
- `TileLayer { properties, source, control }` 는 TileAsset 만 참조하며 `LayerRole::ControlNet` 와 `ControlNetKind::Tile` 를 포함합니다. 이는 라인 제어 레이어가 아닙니다. StaticSource 는 StaticBitmap 입니다. KeyframedSource 는 DynamicBitmap 입니다. 유지 방식만 사용하는 원천 선택은 정수 프레임에서 수행되며 첫 번째 키프레임 는 0입니다.

참조 이미지는 캔버스와 다른 네이티브 해상도를 사용할 수 있습니다. 그 픽셀은 저자 형태로 저장되며, 모델별 크기 조정, 흐림/다운샘플링, 색상 변환, 타일 추론, 겹침 블렌딩 및 출력 병합은 명시적인 소비 어댑터에 속합니다. 렌더링/열 때 암시적 전처리가 발생하지 않습니다.

<a id="preparing-and-editing"></a>

## 준비 및 편집

`makeTileAsset(id, opaqueRaster, maxPixels=16Mi)`는 공간 레이아웃, 세부 사항 또는 해상도를 변경하지 않고 소스 RGB를 복사합니다. 투명 입력은 거부되므로 호출자는 합성 배경을 명시적으로 선택합니다. 형식이 올르지 않거나 빈 ID가 invalid_argument를 던지고, 출력 예산 오버플로가 색상을 할당하기 전에 length_error를 던집니다. 준비된 RGB 데이터에 대해서도 직접 골재 구축이 지원됩니다.

검증된 편집을 위해 `insertTileAsset`, `replaceTileAsset`, `insertTileLayer`, `setTileSample`, `setKeyframedSource` 및 `setControlNetSettings`를 사용하십시오. 자산 교체는 ID를 보존합니다. 잘못된 좌표, 차원, 참조 및 설정은 상태/수정을 변경하지 않습니다. 바인드된 DocumentEditor 작업은 DocumentFile에 동기식으로 커밋됩니다. BitmapEditor는 전용 Tile 자산을 바인딩할 수 없습니다. 공통 모델 ID/수정, 조건화 척도 및 안내 간격이 적용됩니다.

```cpp
Document doc;
doc.extent = {2,2};
DocumentEditor editor(doc);
RasterLayer input = {2,2,{0xffff0000,0xff00ff00,0xff0000ff,0xffffffff}};
auto assetResult = editor.insertTileAsset(makeTileAsset("reference",input));
TileLayer layer;
layer.properties = {"detail-control", "Tile reference"};
layer.source = StaticSource{"reference"};
auto layerResult = editor.insertTileLayer(layer);
auto full = renderTileControlMap(doc,"detail-control",0);
auto region = renderTileControlRegion(doc,"detail-control",0,{1,0,1,2});
// 데이터를 사용하기 전에 각 편집 결과와 출력의 ok()를 확인한다.
```

<a id="full-and-region-output"></a>

## 전체 및 지역 출력

`tileRasterPreview` 는 불투명한 RGB 미리보기를 생성합니다. `renderTileControlMap` 는 전체 현재 프레임 참조와 `region={0,0,width,height}` 를 반환합니다. `renderTileControlRegion` 는 네이티브 자산 픽셀의 `TileRegion { x,y,width,height }` 를 수락하고 해당 직사각형만 복사합니다. 결과물은 `pixels`, 정확한 `colors`, 소스 `region`, 및 실패 `message` 를 소유합니다. 중첩된 요청은 동일한 소스 좌표에 대해 동일한 색상을 반환합니다. 영역 차원은 양수여야 하며 완전히 범위 내에 있어야 하며, 클리핑, 패딩, 반복 또는 크기 조절은 수행되지 않습니다. 범위 산술은 과도한 직사각형을 거부하기 위해 확장된 합을 사용합니다.

두 문서는 Honor Control.enabled, 레이어 프레임 범위 및 타임라인을 내보냅니다. 디스플레이 가시성, 불투명도, 아핀 변환 및 움직임은 제어 픽셀을 변경하지 않습니다. 기본 16 Mi 픽셀 예산이 요청된 출력에 적용되므로, 전체 이미지에 필요한 것보다 작은 영역을 더 적은 예산으로 읽을 수 있습니다. 지역 내보내기가 임시 전체 이미지 출력을 할당하지 않습니다. 내보내기가 실패하면 OK가 아닌 결과가 반환됩니다; 직접 미리보기에서는 invalid_argument 또는 length_error가 발생합니다.

일반 아트워크/ CanvasItem / PDF 구성은 ControlNet 레이어를 제외합니다; 명시적인 레이어별 미리보기는 디스플레이 속성와 함께 제공됩니다. PSD와 타임라인 교환은 지원되지 않는 Tile 의미론을 거부하며, 타임라인 패키지에 포함된 orphan Tile 자산을 포함합니다. 스냅샷 및 작업 파일 왕복은 모든 픽셀과 소스 키프레임를 보존합니다. 변경되지 않은 이미지 레코드는 제어 전용 편집에 재사용됩니다.

<a id="verification-boundary"></a>

## 검증 경계

TileTest는 전체/영역의 정확한 RGB·겹침 일관성·작은 크롭 예산·잘못되거나 오버플로 크기의 영역·불투명 가져오기·형식 지정/static/dynamic 원본·제어/표시 분리·거부 시 롤백·유효한 체크섬을 가진 스냅샷의 버전/tag/개수 실패·작업 파일 재열기와 레코드 재사용·미지원 외부 형식 내보내기를 검증한다. 설치 패키지 소비자도 같은 테스트를 실행한다. 실제 확산·초해상도 품질·타일 출력 혼합은 이 SDK 객체 및 내보내기 검증의 범위 밖이다.

자세한 레이어 및 개체 필드는 다음을 통해 원자적으로 편집할 수도 있습니다.
모든 동적 소스 상태를 포함하는 [공통 매개변수 API](CONTROLNET_PARAMETERS.md).
