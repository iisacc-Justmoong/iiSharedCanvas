<a id="one-canvas-for-images-vectors-video-and-motion-graphics"></a>

# 이미지, 벡터, 비디오 및 모션 그래픽을 위한 하나의 캔버스

패키지 0.11.0는 `Document`를 시각 작업 4가지 종류 전체의 공유되며 렌더링 가능하고 영속 저장되는 소유자로 삼는다. 이미지는 `RasterAsset`(또는 희소 `ChunkedRasterAsset`)를 유지하며 벡터는 편집 가능한 `VectorAsset` 경로를 유지한다. 네이티브 `VideoAsset`는 같은 크기의 ARGB 표시 프레임을 순서대로 저장하며 자체 양의 유리수 `FrameRate`를 가진다. 자체 완결형이므로 렌더링, 샘플링, 바이너리 스냅샷과 작업 파일 다시 열기에 미디어 경로나 디코더가 필요하지 않다.

`Layer`는 네 가지 정적/동적 비트맵/벡터 타입과 `VideoLayer` 및 전용 조건부 역할입니다. 각 레이어는 하나의 콘텐츠 종류를 유지합니다. 비트맵는 비디오를 참조할 수 없습니다. 비디오 레이어에는 비디오 자산을 참조하는 하나의 `StaticSource`가 있어야 합니다. 기존 프레임 소유 비트맵/벡터 키는 홀드 샘플링을 유지합니다. 비디오는 이미지 레이어 모음으로 위장되지 않습니다.

<a id="video-placement-and-timing"></a>

## 비디오 배치 및 타이밍

`VideoPlayback::sourceInFrame` 는 포함형이며 `sourceOutFrame` 는 배제형입니다; 부재한 아웃 포인트는 소유된 프레임의 수를 의미합니다. 빈 또는 범위 밖의 트림은 거부됩니다. 클립은 `LayerProperties::frameRange.firstFrame` 에서 시작하거나, 범위가 설정되지 않은 경우 문서 프레임 0 에서 시작합니다. 포함형 레이어 존재 범위 항상 재생을 제한합니다. 문서 프레임 `f` 의 소스 프레임은 다음과 같습니다:

```
sourceInFrame + floor((f - layerStart) * sourceRate / documentRate)
```

이 라이브러리는 정수 곱과 비교를 사용하여 정확히 평가하며, 전체 너비 uint32 유리율에도 적용됩니다; 영구 타이밍을 부동 초로 변환하지 않습니다. 트리밍된 소스가 종료된 후, `VideoEndBehavior::Transparent`는 하위 레이어를 표시합니다; `Hold`는 레이어가 끝날 때까지 최종 트리밍된 프레임을 유지합니다. 재생은 자산 비율에서 전방됩니다. 암시적인 루프, 광학 흐름, 역재생 또는 속도 램프 해석이 없습니다.

`videoFrameIndexAt`는 선택적 소스 프레임 인덱스를 반환하고, `resolveVideoFrameAt`는 읽기 전용 소유 프레임을 반환합니다. 가시성은 소스 타이밍에 영향을 주지 않습니다. 두 함수 모두 문서를 수정하지 않습니다.

<a id="motion-graphics"></a>

## 모션 그래픽

모든 시각 레이어에는 `LayerProperties::motion`가 있으며, 이는 `MotionKeyframe`의 순서가 지정된 벡터입니다. 각 키는 절대 문서 `frame`와 `MotionValue`, 그리고 해당 발신 세그먼트의 보간을 소유합니다. 키는 문서 타임라인 내에 엄격히 증가해야 하며, 첫 번째 키는 0에 있을 필요가 없습니다. 첫 번째 키 이전이나 마지막 키 이후의 샘플링은 해당 엔드포인트를 유지합니다.

`MotionValue`는 위치, 비균일 스케일, 앵커, 각도 회전 및 불투명도를 노출합니다. 값은 유한해야 하며, 불투명도는 `[0, 1]`에 있어야 합니다. 제로 및 음수 스케일이 허용됩니다. 회전은 언래핑 각도를 사용하므로, 0에서 720는 2의 완전한 회전을 의미합니다. Hold, Linear 및 SmoothStep ( `t*t*(3-2*t)` )가 지원됩니다. 각 키는 모든 모션 채널을 함께 제어합니다.

샘플링된 변환은:

```
baseTransform * T(position + anchor) * R(rotation) * S(scale) * T(-anchor)
```

샘플링된 불투명도는 기본 층 불투명도에 운동 불투명도를 곱한 값입니다. 빈 운동 벡터는 정적 변환과 불투명성을 보존합니다. 자산 스위치 키와 속성 애니메이션은 독립적이며 동일한 이미지 또는 벡터 레이어에서 사용할 수 있습니다. `sampleLayerAt`는 평가된 가시성, 변환 및 불투명도를 반환합니다. 비유한 구성 변환에 대한 오버플로는 렌더러에 의해 거부됩니다.

이 기능은 편집 가능한 벡터를 저장된 래스터 프레임으로 변환하지 않고도 이미지, 벡터 및 비디오 그래픽을 이동·회전·스케일링·페이드하는 것을 지원합니다. 텍스트 레이아웃, 경로 변환, 마스크, 효과, 중첩된 구성 및 표현은 이 모델 확장에 포함되지 않습니다.

<a id="editing-example"></a>

## 편집 예시

```cpp
#include <iiSharedCanvas.h>
using namespace iiSharedCanvas;

Document document;
document.extent = {1920, 1080};
document.timeline = {{24, 1}, 120};
DocumentEditor editor(document);
auto imported = importVideoAsset("clip.mov", "footage");
if (!imported.ok()) { /* report imported.result */ }
else {
    auto inserted = editor.insertVideoAsset(std::move(imported.asset));
    if (inserted.ok()) {
        VideoLayer layer{{"video", "Footage"}, StaticSource{"footage"}};
        layer.properties.frameRange = LayerFrameRange{0, 119};
        MotionKeyframe first;
        MotionKeyframe last;
        last.frame = 119;
        last.value.position = {120, 0};
        layer.properties.motion = {first, last};
        auto result = editor.insertLayer(std::move(layer));
        // 결과를 확인한다. 파일에 연결된 편집기는 반환 전에 커밋한다.
    }
}
auto frame = renderFrame(document, 60);
auto snapshot = encodeIisc(document);
```

`insertVideoAsset` , `replaceVideoAsset` , `setVideoPlayback` 및 `setLayerMotion` 는 커밋 전에 유효성을 검사합니다. 대체는 자산 id 를 보존하며, 이름 변경/이동/제거는 기존 일반 자산 및 레이어 방법을 사용합니다. 참조된 미디어는 제거될 수 없습니다. 무효한 변경은 모델 상태, 형식 버전 및 리비전을 보존하며, 동일한 미디어/재생/모션 대체는 리비전을 진행시키지 않습니다. `setLayerMotion(id, {})` 는 애니메이션을 제거합니다. 기존 문서는 새 필드를 추가하는 편집 시 업그레이드됩니다. 직접 집계 작성기는 `validate` 를 직접 호출해야 합니다.

<a id="persistence-limits-and-interoperability"></a>

## 지속성, 한계 및 상호 운용성

`.iisc` 1.6는 비디오 프레임, 레이트, 트림/엔드 동작 및 모든 편집 가능한 모션 키를 보존합니다. 리더는 1.0 – 1.5 지원을 유지하고, 구형 리더는 1.6를 거부합니다. 작업 파일 스키마 1는 변경되지 않았습니다: 새 필드가 버전화된 자산/레이어 레코드에 살아 있습니다. 변경되지 않은 비디오 페이로드는 속성 편집 중에 건너뛰어집니다. 소스와 설치된 소비자는 픽셀 렌더링, 바이너리 라운드 트립, 즉시 파일 편집 및 재개방을 수행합니다.

`SerializationLimits::maximumTotalVideoFrames`는 기본값이 262144이며, `maximumTotalMotionKeyframes`는 1048576입니다. 비디오 픽셀도 `maximumTotalRasterPixels`에 포함됩니다. 인코딩과 디코딩 모두 이러한 예산을 적용합니다; 디코드는 할당하기 전에 남은 바이트에 대한 컬렉션 카운트를 확인합니다.

`importVideoAsset` 는 검토된 한계가 설정된 FFmpeg 가져오기 경로를 재사용하여 하나씩 분리된 네이티브 자산을 반환합니다. `importVideo` 는 이전 프레임 소유 비트맵 문서 API 를 유지합니다. 비디오 가져오기는 8비트 디스플레이 RGB 를 일정한 샘플링된 비율로 수행하며, 원래 압축 패킷, HDR 정밀도, 가변 프레임 타임스탬프 및 소스 오디오는 유지되지 않습니다. 기존 경고는 이러한 변환을 공개합니다. 오디오는 독립적인 `AudioAsset` / `AudioTrackLayer` 필드에 유지됩니다.

`exportVideo` 는 비디오와 모션을 포함한 완전한 혼합 캔버스를 렌더링합니다. 네이티브 `.iisc` 는 편집 가능성을 유지합니다. PSD 와 타임라인 XML / FCPXML 어댑터는 현재 네이티브 비디오/모션에 대해 `UnsupportedFeature` 대신 아무런 알림 없이 을 반환하여 그들의 의미를 떨어뜨리지 않습니다. 그들의 기존 비트맵 /vector hold-key 동작은 변경되지 않았습니다.

새로운 런타임 또는 링크 의존성이 추가되지 않았습니다. 이는 C++23 패키지/ ABI 리비전으로 SOVERSION 0.11 와 정확한 버전 발견이 있으며, 소비자는 0.11.0에 대해 다시 빌드해야 합니다. 기존 비트맵 /vector 소스 초기화기는 필드 순서를 유지하지만, 철저한 방문자는 추가된 비디오 대안을 처리해야 합니다.

Qt  Quick  `CanvasItem`  는 역 브러시 좌표에 샘플링된 변환을 사용하므로 움직이는  래스터  를 편집하면 현재 프레임의 표시된 위치 아래의 픽셀이 변경됩니다. 비동기 렌더러는 동일한 비디오/운동 평가기를 공유합니다.  PDF  내보내기는 요청된 문서 프레임을 샘플링하고 불투명한 벡터를 경로로 유지하며 선택된 비디오 프레임을 이미지로 임베드합니다; 편집 가능한 운동을 보존하지 않습니다.
