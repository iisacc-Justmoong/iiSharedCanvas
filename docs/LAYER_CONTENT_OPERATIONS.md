# 레이어 콘텐츠와 출력 연산 명세

상태: C++ 네이티브 API와 Python 문서 API가 제공하는 동작 및 제한이다.

## 소유와 편집

`FillLayerContent`는 단색, 여러 정지점의 그라디언트 또는 문서가 소유한
래스터 자산의 반복 패턴을 표현한다. 자산 경로는 저장하지 않는다. 반복
원점은 레이어 로컬 좌표에 있으며 레이어 변환이 전체 채우기에 적용된다.
패턴은 가장 가까운 원본 픽셀을 반복한다. 없는 자산이나 잘못된 정지점은
검증에서 거부한다. 그라디언트는 선형·방사형과 정지점 2~256개를 제공하고
색과 알파는 기존 래스터 그라디언트 규칙으로 계산한다.

`AdjustmentLayerContent`는 색 조정 설정을 저장하고 동일 그룹에서 해당
레이어 아래에 이미 합성된 결과에 적용한다. 통과 그룹은 부모 배경을 이용하고
격리 그룹은 자신의 중간 결과를 이용한다. 범위와 마스크, 불투명도를 적용한다.

`LayerStyleStack`은 그림자·내부 그림자·광선·내부 광선·외곽선·단색 오버레이·
베벨을 보관한다. 각 효과의 색, 불투명도, 반경과 이동을 보존하며 검증은 문서 모델이 담당한다. 외부
효과를 스택 순서대로 그린 후 채우기, 내부 효과를 순서대로 계산한다.
광선과 그림자는 Gaussian으로 흐리며 외곽선은 알파의 확장으로 계산한다.
베벨은 단순한 방향성 가장자리 음영이다. 이는 명시된 네이티브 규칙이며
다른 편집기의 모든 효과 또는 색 관리 결과와의 일치는 보장하지 않는다.
`LayerStyleEvaluator`는 명시적으로 전달된 픽셀과 설정에서 일반 알파 합성 결과를 만든다.
실행 객체나 함수 포인터를 입력받지 않는다.
독립 평가에는 호출자가 투명한 여백을 제공한다. 프레임 렌더러는 샘플링
격자에 맞춘 후광을 계산해 인접 타일의 효과가 이어지게 한다. 원본 알파는
채우기 불투명도가 0이어도 효과를 정의하며 전체 불투명도는 합성 시 적용한다.

편집 가능한 콘텐츠와 스타일은 `.iisc` 1.25에 보존하고 문서 이력으로 되돌린다.
기존 문서 버전은 읽을 수 있으며 새 기능을 표현하지 못하는 이전 버전으로의
저장은 조용한 정보 손실 대신 실패한다. 자산과 공유 레이어의 잠금은 기존
문서 편집 정책을 따른다. `DocumentHistory::insertLayerWithAsset`으로 콘텐츠와
정적 비트맵 레이어를 함께 만들고 `replaceProceduralContent`로 설정을 바꾼다.
마스크와 스타일의 완전한 외형 값은 `setLayerAppearance`로 게시한다.
외형 이력은 불변 스냅샷을 별도로 보관하고 실제 보유량을 예산에 반영한다.
파일에 바인딩된 동일 값 요청도 외부 작성자의 변경을 확인하며 거부 시 이력을 유지한다.

## 마스크

`LayerMaskOperations`와 `VectorMaskOperations`는 적용할 래스터 또는 벡터
마스크 슬롯의 교체·제거를 명시적 입력에서 계산한다. 벡터 경로 자체의 편집은
기존 `VectorEditor`에 맡긴다. 저장 가능한 결과는 기존 레이어 외형과 문서
편집·이력 경로로 게시한다. 참조 대상의 종류가 맞지 않거나 잠금이 변경을
금지하면 기존 문서와 이력은 바뀌지 않는다. 마스크의 활성화, 반전, 밀도,
페더, 변환 및 연결 속성을 보존한다.

## 래스터화와 슬라이스

외부 제어기는 문서와 프레임에서 레이어 타일을 먼저 렌더링하고 그룹의
유효 가시성을 계산한다. `LayerRasterizationPass`는 그 타일, 요청 영역과
가시성 값만 받아 불투명도가 반영된 독립 래스터를 반환한다. 다른 실행
객체를 호출하거나 문서를 수정하지 않는다. 배경 의존 조정, 다른 레이어에
클리핑된 레이어, Blend If와 디졸브 혼합은 독립 출력에서 명시적으로 거부한다.

`LayerSliceDescriptor`는 레이어 또는 전체 합성 결과, 프레임, 잘라낼 영역,
출력 크기와 샘플링을 기술한다. 외부 제어기가 원본을 선택하고 렌더링한 뒤
`LayerSliceBuilder`에 렌더링 결과와 원본의 문서 좌표 영역을 전달한다.
빌더는 제공된 원본의 내부 영역을 최근접 또는 알파를 고려한 선형 보간으로
잘라 출력한다. 영역과 버퍼가 일치하지 않거나 원본 밖으로 요청하면 실패한다.
같은 영역과 해상도로 미리 렌더링했다면 픽셀을 그대로 복사한다. 반환된
독립 래스터의 인코딩과 파일 기록은 호출자가 담당한다.

## 지원 범위와 검증

조정 레이어는 현재 일반 혼합만 지원하며 조정 레이어 자체의 필터·클리핑·
Blend If·스타일과 조정 레이어를 클리핑 기준으로 쓰는 조합은 지원하지 않는다.
PSD 및 타임라인 내보내기는 채우기·조정 콘텐츠를 보존할 수 없어 거부한다.
스타일 후광을 포함한 한 요청은 최대 16,777,216 출력 픽셀과 계산 예산으로
제한한다. 이 한도를 넘는 큰 요청은 호출자가 더 작은 타일로 나누어야 한다.

각 연산 객체는 단일 실행 진입점을 제공하고 외부 의존성과 데이터를 명시적으로
받으며 명시적인 결과 또는 오류를 반환한다. 데이터 객체의 관찰과 동등 비교는
실행 진입점이 아니다. 문서 기록, 연산 순서와 결과 게시의 책임은 호출자에게
있다. C++ 공개 API와 Python API, 소유권, 직렬화와 이력, 중첩 그룹의 합성,
마스크, 타일 경계 및 오류 복구를 테스트한다. Python `CanvasDocument`의
`put_fill`, `put_adjustment`, `set_styles`, `set_mask`, `slice`가 동일한
네이티브 문서와 이력을 사용한다. `slice`는 행 우선 직선 ARGB 정수 배열을
반환하며 출력을 이미지 파일로 인코딩하는 일은 호출자에게 남긴다.

## 호출 예

다음 예는 외부 제어기가 합성 원본을 먼저 구하고 데이터만 전달한다.
`document`와 `descriptor`의 소유권과 수명은 호출자가 관리한다.

```cpp
#include <iiSharedCanvas>
using namespace iiSharedCanvas;

FrameRenderResult buildSlice(const Document &document, const LayerSliceDescriptor &descriptor)
{
    auto rendered = renderFrameRegion(document, descriptor.frame, descriptor.region,
                                      descriptor.outputExtent, descriptor.sampling);
    return LayerSliceBuilder{}(rendered, descriptor.region, descriptor);
}
```

Python에서는 동일한 소유 문서를 생성하거나 디코딩한 뒤 변경과 출력을 요청한다.

```python
from iisharedcanvas import CanvasDocument

with CanvasDocument.raster(64, 64, 0xff808080) as canvas:
    canvas.put_fill("fill", "fill-layer", kind="solid", color=0xff336699)
    canvas.put_adjustment("light", "adjustment", exposure=0.5)
    argb = canvas.slice((0, 0, 32, 32))
    canvas.undo()
```
