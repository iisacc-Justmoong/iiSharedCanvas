<a id="controlnet-detailed-parameter-api"></a>

# ControlNet 상세 매개변수 API

패키지 0.24.0는 Reference 및 IP-Adapter를 포함하여 모든 12 컨디셔닝 종류에 대한 공통 유형 매개변수 워크플로우를 공개합니다. 네이티브 모델은 1.17로 유지됩니다. 이 API는 기존 지속 필드를 편집하고 새로운 연결 태그나 추론 종속성을 도입하지 않습니다.

<a id="query-modify-apply"></a>

## 쿼리, 수정, 적용

`getControlNetParameters(document, layerId)`와 `DocumentEditor::controlNetParameters(layerId)`는 분리된 `ControlNetParametersResult`를 반환한다. `parameters`에 접근하기 전에 `ok()`를 확인한다. `parameters.layer`는 기존의 형식 지정 `Layer` variant이다. `parameters.assets`는 최초 참조 순서대로 모든 고유 원본 자산을 포함한다. 동적 원본은 레이어가 비활성화·숨김 상태이거나 표시 프레임 범위 밖에 있어도 모든 키프레임 상태를 포함한다. 같은 자산의 반복 참조는 한 번만 나타난다.

완전한 콘크리트 골재를 조정하려면 `std::get<T>`/`std::get_if<T>`를 사용하십시오. `controlNetSettings(layer)`는 모든 컨디셔닝 레이어에 대한 공통 설정을 제공합니다. const 오버로드는 읽기 전용 액세스를 지원합니다. 둘 다 아트워크에 대해 null을 반환합니다. 반환된 분리된 사본을 편집해도 문서가 직접 변경되지는 않습니다.

`DocumentEditor::setControlNetParameters(layerId, parameters)` 는 레이어와 ONE 트랜잭션 내의 모든 소스 자산을 검증하고 적용합니다. 식별자, 구체적 타입 및 소스 참조는 변경되지 않아야 합니다. 참조된 모든 자산은 정확히 한 번만 나타내야 하며, 입력 자산 순서는 임의입니다. 레이어 이름, 가시성, 불투명도, 변환, 블렌드 모드, 프레임 범위 및 모션은 조건부 데이터와 함께 변경될 수 있습니다. 소스 변환, ID 변경 및 키프레임 키프레임 토폴로지는 기존 구조 편집기 API 를 사용합니다.

세터는 병합이나 동시 편집 프로토콜이 아닌 전체 교체입니다. 다른 편집 후 스냅샷을 다시 획득하세요. 각 승인된 세터 호출은 하나의 문서 편집을 기록하며, 동일한 값의 재제출도 포함합니다 (`replaceLayer` 의미론과 일치). 다른 레이어와 공유되는 자산은 모든 소유자에게 변경되며, 완전한 문서 검증은 다른 소유자를 무효화할 수 있는 모든 변경을 거부합니다. 별도의 소유가 필요한 경우 구조 API 를 사용하여 독립적인 자산을 만드세요.

<a id="partial-common-settings"></a>

## 부분 공통 설정

`patchControlNetSettings(layerId, ControlNetSettingsPatch)` 는 활성화된 선택 필드만 변경합니다: `enabled`, `modelId`, `modelRevision`, `conditioningScale`, `guidanceStart`, `guidanceEnd` 입니다. 명시적인 false, 0, 빈 문자열은 편집이며, 비활성화 필드는 변경되지 않습니다. 빈/동등한 패치는 편집자 리버전을 증가시키지 않습니다. 최종 설정이 모두 유효성 검증을 받습니다: 스케일은 유한하고 음수가 아니며, 가이드는 0 <= 시작 < 끝 <= 1을 따릅니다. IP - 어댑터는 여전히 비어 있지 않은 어댑터 식별자/버전을 일치시켜야 합니다. 층 바인딩과 자산 설명자를 함께 변경할 때 완전한 매개변수 거래를 사용하세요.

<a id="detailed-field-coverage"></a>

## 상세한 현장 적용 범위

|종류|편집 가능한 객체 데이터|특정 레이어 데이터/출력 API|
| --- | --- | --- |
|의미론적 세그먼트|`RasterAsset.pixels`: 치수 및 정확한 식별 색상|분류 ID/버전/소스; 클래스의 ID/키/이름/설명/범주/부모/팔레트/외부 ID/별칭; 지역의 클래스, 마스크 색상, 인스턴스, 신뢰도, 출처, 생성기, 소스, 속성; 무효 색상|
|포즈|뷰포트; 사람 ID/이름/트랙/활성화; 모든 신체, 손, 얼굴, 눈, 입 앵커(좌표, z, 자신감, 가시성, 잠금); 표현식 ID/이름/중량/델타|`PoseRenderOptions`: 프로필, 신뢰도 필터, 폐색, 포인트 반경, 선 너비, 출력 예산|
|깊이|뷰포트 및 모든 정규화된 근접 샘플 [0,1]|정확한 깊이 데이터는 디스플레이 불투명도/변환과 무관하게 유지됩니다.|
|라인 아트|뷰포트 및 모든 적용 범위 샘플 [0,1]|준비된 적용 범위, 암시적 추출 없음|
|Canny|뷰포트 및 모든 바이너리 에지 샘플|준비된 에지 마스크, 암시적 검출기 없음|
|낙서|뷰포트 및 모든 이진 스트로크 샘플|준비된 스트로크 마스크, 포인터/브러시 궤적 없음|
| MLSD |뷰포트; 세그먼트 ID, 끝점, 신뢰도, 활성화된|`MlsdRenderOptions`: 최소 신뢰도, 픽셀 및 래스터 단계 예산|
|노멀 맵|뷰포트; 서명된 XYZ 단위 정상 및 유효성|`NormalMapRenderOptions`: XYZ/ZYX 주문, flipY, 출력 예산|
||뷰포트 및 RGB 샘플 셔플|명시적 준비 UV 매핑은 `makeShuffleAsset`를 통해 계속 사용 가능|
|타일|뷰포트 및 RGB 샘플|기존 전체 이미지 및 한계가 설정된 영역 내보내기 API|
|참조|뷰포트 및 RGB 샘플|주의/AdaIN/결합 모드 및 스타일 충실도|
|IP-Adapter|설명자 단계, 인코더/어댑터 ID 및 개정판, 기본 모델 및 전처리 ID; 토큰/채널 차원; 모든 상태에 대한 조건부 및 선택적 무조건 float32 값|어댑터 바인딩 및 공통 규모/일정; `IpAdapterExportOptions`는 CFG 요구 사항 및 출력 예산을 제어합니다.|

모든 기존 타입화된 insert/replace/sample/anchor/expression API 는 공개적으로 유지됩니다. 렌더링/내보내기 옵션 구조는 기존 출력 함수에 제공되며 레이어 매개변수로 영속화되지 않습니다. SDK 가 실행하지 않는 전처리 알고리즘 (예: Canny 추출 임계값) 은 비효율적인 설정으로 발명되지 않습니다. 준비된 픽셀/기하/텐서 데이터는 네이티브 객체 계약입니다.

<a id="atomic-examples"></a>

## 원자적 예

의미 영역 ID와 해당 마스크를 함께 변경합니다.

```cpp
auto result = editor.controlNetParameters("segments");
if (!result.ok()) { /* present result.message */ return; }
auto parameters = std::move(*result.parameters);
auto &semantic = std::get<SemanticSegmentLayer>(parameters.layer);
const auto oldColor = semantic.segmentation.regions.at(0).maskColor;
const auto newColor = 0xff336699U; // 다른 영역이나 빈 공간과 충돌해서는 안 된다.
semantic.segmentation.regions.at(0).maskColor = newColor;
for (auto &asset : parameters.assets) {
    auto &mask = std::get<RasterAsset>(asset).pixels;
    for (auto &pixel : mask.pixels) if (pixel == oldColor) pixel = newColor;
}
auto applied = editor.setControlNetParameters("segments", std::move(parameters));
// applied.ok(), applied.changed, applied.path와 applied.message를 확인한다.
```

지정되지 않은 설정을 바꾸지 않고 공통 매개변수를 변경합니다.

```cpp
ControlNetSettingsPatch patch;
patch.conditioningScale = 0.65;
patch.guidanceStart = 0.1;
patch.guidanceEnd = 0.85;
auto applied = editor.patchControlNetSettings("pose", patch);
```

하나의 포즈 자산 스냅샷을 통해 세부 표현과 여러 앵커를 변경하거나 동일한 방식으로 IP-Adapter 모델 바인딩 및 모든 참조 상태 설명자를 변경합니다. 라이브 문서를 직접 변경할 필요는 없습니다.

<a id="validation-and-persistence"></a>

## 검증 및 지속성

거부된 편집은 레이어/에셋, 형식 버전, 작성 정보와 편집기 리비전을 보존한다. 의미 팔레트 일관성, 자세 앵커/표정, 이진 마스크, 단위 법선, 유한한 텐서와 IP-Adapter 프레임 간 호환성을 포함한 모든 타입별 불변 조건을 계속 적용한다. 잘못된 참조, 중복/누락된 에셋, 작품 데이터와 만료된 파일 바인딩은 명시적으로 실패한다. 성공한 파일 연결 편집은 기존 `DocumentFile` 트랜잭션을 통해 동기적으로 커밋한다. 공통 항목만 바꾸는 패치에서는 변경되지 않은 에셋 레코드를 재사용한다. 테스트는 스냅샷과 작업 파일 양쪽의 12개 타입 전체, 여러 상태의 원자적 편집과 롤백을 검사한다.
