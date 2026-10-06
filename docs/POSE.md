<a id="native-full-body-and-expression-dense-pose-controlnet-layers-0140"></a>

# 네이티브 전신 및 표정 밀도 포즈 ControlNet 레이어 ( 0.14.0 )

`PoseLayer` 는 소유된 `PoseAsset` 를 참조하는 전용 ControlNet 레이어입니다. PoseAsset 는 뷰포트 와 최대 64 명의 독립적으로 식별된 사람을 포함하며, 베이킹된 미리보기나 일반적인 벡터 경로를 아닌 편집 가능한 관절/면 기하학을 저장합니다. `layerRole` 는 ControlNet 를 반환하고 `controlNetKind` 는 포즈를 반환합니다. 독립적인 4-방식 종류는 소스에 따라 StaticVector 또는 DynamicVector 입니다. `ContentKind::Pose` 는 저장된 도메인 자산을 식별합니다.

<a id="complete-native-topology"></a>

## 완전한 네이티브 토폴로지

네이티브 토폴로지 V1 는 사람**당 **590 앵커를 포함하며, **523 면 앵커**를 포함합니다. 배열은 안정적인 이름 지정된 그룹 순서를 가지며, `poseAnchors` 를 통해 노출됩니다.

|부분|개수|세부 정보|
| --- | ---: | --- |
|본체| 25 |BODY_25 주문: 코, 목, 어깨, 팔꿈치, 손목, 중간 엉덩이, 엉덩이, 무릎, 발목, 눈, 귀, 발가락 및 뒤꿈치|
|왼쪽/오른손|21 각각|손목 플러스 4 앵커는 조절 손잡이 /index/middle/ring/little finger에 따라 제공되며, 조인트 이름은 PoseHandJoint를 통해 확인할 수 있습니다.|
|얼굴 윤곽| 33 |이미지 왼쪽 템플을 턱을 통해 이미지 오른쪽 템플까지|
|눈썹|17 각각|독립적인 눈썹|
|코| 26 |9 브리지 및 17 등고선 앵커|
|각 눈| 68 |17 상단 뚜껑, 17 하단 뚜껑, 17 크리즈, 16 아이리스 링 및 하나의 동공 앵커|
|입에 넣다| 143 |25는 상·하부 외부 및 내부 입술용 각각; 17 상부 및 17 하부 치아; 9 혀 앵커|
|볼/이두부| 51 |17 각각 왼쪽 볼, 오른쪽 볼 및 이마용|
|비음주름|25 각각|왼쪽/오른쪽 표현 접힘을 분리합니다|
|눈가 윤곽|25 각각|왼쪽/오른쪽 피부 윤곽 분리|

좌/우란 사람의 해부학적 측면을 의미하며, 카메라 측면을 절대 의미하지 않습니다. 면 호는 이미지 왼쪽에서 이미지 오른쪽으로 이어지며, 홍채는 이미지 오른쪽 점에서 시작하여 y-하향 좌표계에서 시계 방향으로 이어집니다. 중립 템플릿은 해부학적 엄지 방향을 반영하며, 정면 자세와 독립적으로 편집 가능한 면 곡선을 사용하며, 해부학적으로 보정된 3D 모델을 사용하지 않습니다. `makeNeutralPosePerson(id)` 는 호출자가 즉시 편집할 수 있도록 모든 590 가시 앵커를 제공합니다. 맨한 PosePerson 는 앵커를 누락으로 초기화하는 집계입니다.

<a id="coordinates-and-identity"></a>

## 좌표 및 정체성

각 `PoseAnchor` 는 정규화된 뷰포트 x/y, 선택적 상대 z, [0,1]의 신뢰도, 명시적인 누락/가시/폐쇄 상태 및 편집자 잠금을 소유합니다. 좌표는 [-16,16]에서 유한해야 하며, 한계가 설정된 캔버스 밖 관절을 허용합니다. z 는 동일한 상대 스케일을 사용하며, 3D 재구정의 계측치가 아닙니다. 0,0 는 유효한 가시 앵커로 남으며, 누락은 0 좌표를 절대 의존하지 않습니다. 자산 내 사람의 id 는 비어있지 않고 고유해야 하며, 이름과 trackId 는 선택 사항입니다. 트랙 id 는 프레임 자산 간 동일한 사람을 연결할 수 있습니다. Person.enabled 이 출력을 제어합니다.

<a id="expression-deformation"></a>

## 표현 변형

`PoseExpression` 는 안정적인 id/이름, [0,1] 가중치, 그리고 이름 지정된 그룹과 인덱스로 주소를 가진 희소 `PoseAnchorDelta` 엔트리를 소유합니다. 각 델타는 dx/dy/dz 를 가집니다. 평가 는 작성된 앵커에 가중 델타를 추가하지만 수정하지 않으며, 여러 타겟은 결합됩니다. 이는 명시적인 기하학을 통해 미소, 턱 열림, 입 닫힘, 혀/이빨 노출, 눈 감기, 눈썹 움직임, 동공/홍채 움직임 및 비대칭 표정을 지원합니다. 타겟은 추론된 감정 분류기가 아닌 작성된 변형입니다.

각 사람은 128 표현 대상을 허용합니다; 대상은 최대 590 고유 앵커를 지정할 수 있습니다. 중복 주소, 알 수 없는 그룹, 잘못된 인덱스/가중치 및 결합된 좌표 오버플로는 거부됩니다. 앵커 잠금을 설정하면 잠금이 해제될 때까지 대상 편집기 이동을 방지합니다. 잠금은 표현식 평가를 고정하지 않으며, 도매 자산 교체는 의도적인 하위 수준 작업입니다.

```cpp
PoseAsset asset;
asset.id = "pose-neutral";
asset.viewport = {1024, 1024};
asset.people = {makeNeutralPosePerson("person-1")};
PoseExpression smile;
smile.id = "smile"; smile.name = "Smile"; smile.weight = 0.6;
smile.deltas = {
    {PoseGroup::MouthOuterUpper, 0, 0, -0.01, 0},
    {PoseGroup::MouthOuterUpper, 24, 0, -0.01, 0},
};
asset.people[0].expressions.push_back(smile);
editor.insertPoseAsset(asset);
PoseLayer layer;
layer.properties = {"pose-control", "Pose control"};
layer.source = StaticSource{"pose-neutral"};
editor.insertPoseLayer(layer);
editor.setPoseExpressionWeight("pose-neutral", "person-1", "smile", 1.0);
auto map = renderPoseControlMap(document, "pose-control", currentFrame);
```

`setPoseAnchor` 는 주소를 가진 하나의 앵커를 편집하며, `replacePoseAsset` 는 검증과 원자적 롤백을 통해 완전한 사람 컬렉션을 대체합니다. 파일 기반 편집은 동기적으로 지속됩니다. `setControlNetSettings` 는 의미론적 및 포즈 레이어 모두에 적용됩니다. 동적 삽입은 KeyframedSource 와 프레임-0우선 배치 방식을 사용하며, 일반 소스 변환/키프레임 API 는 여전히 사용 가능합니다. 프레임 콘텐츠는 hold 샘플링을 사용합니다. 자동 IK, 신체 부위 부모 연결 또는 표현 추론은 포함되지 않습니다: 관절과 얼굴 움직임은 직접 작성하거나 희소 표현 타겟을 통해 작성됩니다.

<a id="rendering-and-openpose-projection"></a>

## 렌더링 및 OpenPose 프로젝션

네이티브 상세 프로필은 전체 신체/손 연결성과 모든 얼굴 윤곽을 그립니다. 출력은 PoseAsset 뷰포트 해상도의 불투명한 검은 배경 조건부 비트맵 입니다. control.enabled, person.enabled, confidence, occlusion 옵션과 소스 프레임 범위를 준수합니다. 레이어 표시 변환, 가시성과 불투명도는 모델 입력에 포함되지 않습니다. 일반 캔버스는 ControlNet 콘텐츠를 생략하며, 명시적인 레이어 미리보기는 여전히 표시 변환을 지원합니다. `poseVectorPreview` 는 invalid_argument 를 던지며, 유효한 프레임 렌더링은 유효한 값을 제공합니다.

2 내보내기 경로는 의도적으로 구분됩니다:

- `exportOpenPoseKeypoints` : BODY_25 의 픽셀 좌표 x/y/신뢰도 배열, 좌/우 손 21, 및 얼굴 70 순서입니다. 70 얼굴은 샘플 네이티브 윤곽을 앵커링합니다: 두 번째 점마다 윤곽; 눈썹은 네 번째마다; 다리를 0/3/5/8 인덱스; 코 윤곽은 네 번째마다; 각 눈 윗부분은 0/5/11/16 그리고 아랫부분은 11/5; 입술 바깥쪽 윗부분은 0/4/. ../24 그리고 아랫부분은 20/16/. ../4; 입술 안쪽 윗부분은 0/6/. ../24 그리고 아랫부분은 18/12/6; 오른쪽과 왼쪽 동공은 68 와 69에 있습니다. 결측 앵커는 0 확신을 방출합니다. 이것은 구조화된 C++ 데이터로 하위 소비 측 JSON /텐서 어댑터에 대한 것입니다.
- `PoseRenderProfile::OpenPose` : COCO18 본체 + 21+21 손 + 70 얼굴 래스터 투영, 최대 130는 활성화된 전체 인물당 앵커를 방출합니다. 이 래스터에서는 발 관절과 조밀한 얼굴 디테일이 생략되었습니다. NativeDetailed는 모든 것을 유지합니다.
  590. 결과 카운터와 경고가 예측 손실을 공개합니다; 네이티브 지속성
  항상 모든 기하학, 깊이, 잠금 및 표현 정의를 유지합니다.

원본 [OpenPose 출력 계약](https://github.com/CMU-Perceptual-Computing-Lab/openpose/blob/master/doc/02_output.md) 및 [70- 포인트 면 레이아웃](https://github.com/CMU-Perceptual-Computing-Lab/openpose/blob/master/include/openpose/face/faceParameters.hpp)는 교환 인덱싱을 고정합니다. 래스터 색상/연결성은 OpenPose 스타일 프로젝션을 따르지만, **는 픽셀과 동일하지 않으며,**는 모델의 주석자와 동일하지 않습니다. 원본
[ControlNet 바디/핸드 주석기](https://github.com/lllyasviel/ControlNet/blob/main/annotator/openpose/util.py)는 자체 도면 폭/블렌딩을 사용합니다. 소비자는 호환 가능한 모델을 선택하고 추론 품질을 검증해야 합니다; 이 SDK는 다운로드도 실행도 하지 않습니다.

점 반경/선 폭은 ( 0,64]; 출력은 기본적으로 16 Mi-pixel 예산으로 설정됩니다. 조밀한 얼굴은 개별 편집을 위해 적절한 출력 해상도 또는 줌이 필요합니다. 신뢰 임계값 이하이거나 배제된 차단 상태인 앵커는 래스터 출력에서 삭제되지 않고 생략됩니다. 프로젝션 카운터는 참여 슬롯/앙코를 계산하며, 잘라낸 래스터 픽셀은 계산하지 않습니다.

<a id="persistence-and-evidence"></a>

## 지속성과 증거

패키지 0.14 는 자산과 레이어 변형 ABI 를 변경합니다. 스냅샷 1.8 는 네이티브 PoseAsset 태그 4, 포즈 소스 종류 2 와 ControlNet 레이어 역할 태그 2를 추가합니다. 작업 SQLite 스키마는 1 를 유지하며, 이 버전화된 자산/레이어 기록을 재사용합니다. 더 오래된 스냅샷은 읽을 수 있으며, 1.7 보다 이전인 안전하게 거부한다 하에 포즈 데이터를 저장합니다. 변경되지 않은 포즈 자산은 증분 쓰기에 의해 재사용될 수 있습니다. 직렬화 는 문서 전체의 사람, 표현 및 델타를 각각 제한하며 할당 전에 최소 페이로드 크기를 확인합니다.

PSD 와 XML 타임라인 상호작용은 포즈 레이어를 거부하며, 타임라인 내보내기 또한 모든 유산 벡터 캐스트 전에 참조되지 않은 PoseAssets 를 거부합니다. 일반적인 비트맵 / PDF 아트웍 출력은 ControlNet 레이어를 제외합니다. 포즈 테스트는 전체 토폴로지, 실제 표현/눈 렌더링, 누락, 편집 롤백, 네이티브 스냅샷, 동적 소스, 작업 파일, 예산 및 설치된 패키지 소비를 모두 다룹니다. 그것들은 실제 ControlNet 추론 또는 얼굴 추적 평가로 구성되지 않습니다.

자세한 레이어 및 개체 필드는 다음을 통해 원자적으로 편집할 수도 있습니다.
모든 동적 소스 상태를 포함하는 [공통 매개변수 API](CONTROLNET_PARAMETERS.md).
