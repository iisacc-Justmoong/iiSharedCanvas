<a id="mlsd-line-segment-controlnet-0180--format-112"></a>

# MLSD 라인-세그먼트 ControlNet ( 0.18.0 / 형식 1.12 )

`MlsdSegment`, `MlsdAsset` 및 `MlsdLayer`는 편집 가능한 직선 기하학을 보존합니다. MLSD는 라인 컨트롤 패밀리(`isLineControlNetLayer`)에 속하며 자체 `ControlNetKind::Mlsd`와 `ContentKind::Mlsd`를 보유하고 있습니다. Binary Canny/Scribble 마스크는 그 기하학을 대체할 수 없습니다. 정적 레이어와 키프레임이 적용된 MLSD 레이어는 각각 `StaticVector`와 `DynamicVector`입니다.

<a id="geometry"></a>

## 기하학

각 세그먼트는 자산 내에 비어 있지 않은 고유 ID를 가지고 있으며, 정규화된 x1/y1/x2/y2 엔드포인트, [0,1]에 대한 신뢰도, 그리고 활성화 플래그. 모든 숫자는 유한합니다. X는 오른쪽으로 달리고, Y는 아래로 달립니다. 엔드포인트는 달라야 하지만, 교차점, 서로 다른 ID를 가진 일치하는 선, 그리고 분리된 세그먼트는 합법입니다. 신뢰도는 그레이스케일 밝기가 아니라 선택을 제어합니다. 비활성화된 세그먼트는 계속 유지됩니다.

자산은 자신의 id, 양의 네이티브 뷰포트 및 정렬된 segment 벡터를 소유합니다. 빈 벡터는 유효한 빈 장면입니다. 최대 100000 개의 segment 가 자산당 허용되며, 직렬화 limits 는 문서 전체에 1000000 개의 segment 로 기본값으로 설정됩니다. 그것을 재구성하려면 detector model, source photo, 임의의 곡선 또는 입력 이벤트 replay 가 필요하지 않습니다. 엔드포인트는 이진64 이며 정확한 라운드 트립을 견뎌냅니다.

<a id="rendering-and-edits"></a>

## 렌더링 및 편집

`renderMlsdControlMap` 출력은 일회성 불투명한 흰색 직선 segment 와 선택된 segment 객체들을 소스 순서로 포함합니다. 래스터 엔드포인트는 `coordinate * (extent - 1)` 에서 반올림되어 표준화된 0/1 가 첫 번째 및 마지막 픽셀 중심을 해결합니다. Bresenham 스텝핑은 결정론적이며, 안티앨리어싱, 임계값 처리 또는 프레임당 정규화가 없습니다. 서브픽셀 기하학은 작은 해상도에서 하나의 픽셀로 축소될 수 있지만 자산에서는 유지됩니다. 선 방향은 사선 래스터 라인의 결재 기준을 변경할 수 있으며, OpenCV 픽셀 동등성에 대한 주장은 없습니다. 벡터 디스플레이 미리보기는 기존 렌더러를 통해 검은 배경과 흰색 스트로크를 그립니다; 그 안티앨리어싱은 컨트롤 맵과 다를 수 있습니다.

`MlsdRenderOptions` 는 minimumConfidence (기본값 0 ), maximumPixels (기본값 16 Mi 픽셀) 과 maximumRasterSteps (기본값 64 Mi 픽셀 방문) 을 포함합니다. 신뢰도를 만족하는 활성화된 세그먼트만 출력됩니다. 래스터 할당/그리기 전에 출력 크기와 중복을 포함한 방문의 합이 확인됩니다. 이 작업은 많은 긴 세그먼트가 동일한 작은 캔버스와 공유되더라도 제한됩니다. `mlsdRasterPreview` 는 invalid_argument / length_error 를 던지며, 컨트롤 맵 API 은 실패 시 메시지를 반환합니다. `mlsdVectorPreview` 는 경로 생성 전에 유효성을 검사합니다.

```cpp
Document doc;
doc.extent = {512, 512};
DocumentEditor editor(doc);
auto a = editor.insertMlsdAsset({"room.lines", {512, 512}, {
    {"ceiling", 0.1, 0.2, 0.9, 0.2, 1.0, true},
    {"wall", 0.1, 0.2, 0.1, 0.9, 0.95, true}
}});
MlsdLayer layer;
layer.properties = {"mlsd", "MLSD"};
layer.source = StaticSource{"room.lines"};
auto b = editor.insertMlsdLayer(layer);
auto c = editor.setMlsdSegment("room.lines", "wall",
    {"wall", 0.15, 0.2, 0.15, 0.9, 0.95, true});
auto output = renderMlsdControlMap(doc, "mlsd", 0);
// a/b/c와 output.ok()를 확인한다.
```

`replaceMlsdAsset`는 대량 추가/제거/재오더링을 지원합니다. `setMlsdSegment`는 기존 세그먼트를 업데이트하면서 해당 ID를 유지합니다. 편집기 리비전 및 파일 트랜잭션을 포함한 모든 편집은 원자적으로 검증 및 롤백됩니다. 공유된 자산 편집은 모든 참조에 영향을 미칩니다; 독립적인 콘텐츠를 위해 별도의 자산을 생성하십시오. `KeyframedSource{}`와 `insertMlsdLayer(layer, {{0,"a"},{1,"b"}})` 또는 `setKeyframedSource`는 기존 유지 방식만 사용하는 프레임 선택을 가능하게 합니다. 각 소스는 고유한 뷰포트를 유지할 수 있습니다. 암시적 엔드포인트 보간이 없습니다.

제어 출력은 control.enabled, 프레임 범위 및 타임라인을 존중합니다. 디스플레이 가시성, 불투명도, 변환 및 블렌드 모드를 무시합니다. 일반 아트워크는 ControlNet 레이어를 제외하며, 레이어별 미리보기는 계속 제공됩니다. 공유된 ControlNet 모델 아이덴티티/강도/가이던스 설정이 레이어에 지속됩니다.

<a id="native-storage-and-boundaries"></a>

## 네이티브 스토리지 및 경계

패키지 0.18.0 는 10 자산과 레이어 대안을 가지며, 소비자 재빌드를 요구합니다. 네이티브 형식 1.12 는 자산 태그 9 와 콘텐츠/역할 태그 7를 추가합니다. 자산은 id, i32  너비/높이, u32  섹션 수, 그리고 각 섹션의 문자열 id, 5  f64  좌표/신뢰도 값, 및 활성화 u8  불리언을 저장합니다. 할당 전에 카운트, 남은 바이트 및 maximumMlsdSegments  가 확인됩니다. 형식화된 불리언 값, 알 수 없는 태그, 유효하지 않은 기하 구조 및 지원되지 않는 이전 버전 안전하게 거부한다 . 이전 1.0 – 1.11  문서들은 읽을 수 있습니다. SQLite  작업 파일 스키마는 1  그대로 유지되며, 변경되지 않은 기하 구조 기록은 재사용되고 섹션 편집은 동기적으로 지속됩니다. PSD /타임라인 교환은 MLSD  의미론의 손실을 거부하며, 타임라인 패키지 내 고아 MLSD  자산도 포함됩니다.

이는 자동 MLSD 모델 추론이 아니라, 작성된 기하학/제어‐맵 구현입니다. 화이트-세그먼트-온-블랙 규칙은 공식 ControlNet MLSD 주석기를 따르며, 각 검출된 엔드포인트 쌍을 선으로 그립니다: https://github.com/lllyasviel/ControlNet/blob/main/annotator/mlsd/__init__.py 모델 다운로드, 검출기 임계값 또는 생성된 이미지 품질은 암시되지 않습니다.

자세한 레이어 및 개체 필드는 다음을 통해 원자적으로 편집할 수도 있습니다.
모든 동적 소스 상태를 포함하는 [공통 매개변수 API](CONTROLNET_PARAMETERS.md).
