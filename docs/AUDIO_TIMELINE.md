<a id="persisted-audio-timeline"></a>

# 지속되는 오디오 타임라인

문서 형식 1.4 는 `Document::audioAssets` 와 `Document::audioTracks` 를 추가합니다. 이들은 별도의 `TimelineProject` 모델과 독립적인 공식적인 공개 집계 데이터입니다. 시각적 `Layer` 은 비트맵 /벡터 변형으로 남으며, `AudioTrackLayer` 는 문서 타임라인 좌표에서 오디오 클립을 소유하고 이미지 렌더러로 전달되지 않습니다.

<a id="source-and-timing-contract"></a>

## 소스 및 타이밍 계약

`AudioAsset` 는 서명된 16비트 인터리브 PCM, 하나 또는 2 채널, 8000 – 192000 Hz 를 소유합니다. 적어도 하나의 완전한 샘플 프레임을 포함해야 합니다. 입체음향 샘플 프레임은 왼쪽과 오른쪽 샘플을 포함합니다. 자산 ID 는 시각적 및 오디오 자산 컬렉션 모두에서 고유합니다.

`AudioClip`은 오디오 자산 하나를 참조한다. `startFrame`와 `durationFrames`는 정수 문서 프레임이다. 반열린 구간 `[startFrame, startFrame + durationFrames)`는 재생 시간이 양수이고 `Document::timeline.frameCount` 안에 들어가야 한다. `sourceOffsetSamples`는 인터리브된 스칼라 샘플이 아니라 채널별 샘플 프레임을 센다. 원본 범위에는 최소 `ceil(durationFrames * frameRate.denominator * sampleRate / frameRate.numerator)`개의 샘플 프레임이 있어야 한다. `audioSampleFrameCount()`는 검사된 정수 연산으로 이 정확한 유리수 계산을 구현하며, 잘못된 비율이나 오버플로는 `nullopt`를 반환한다.

트랙은 저장된 순서대로 정렬됩니다. 한 트랙 내의 클립은 시작 프레임별로 정렬되어야 하며 겹칠 수 없습니다. 별도의 트랙이 자유롭게 겹칠 수 있습니다. 트랙은 비어 있을 수 있고 공백이 포함될 수 있습니다. 트랙 ID는 시각적 레이어와 오디오 레이어 모두에서 고유합니다. 클립 ID는 모든 오디오 트랙에서 고유합니다.

트랙과 클립 `gainDb` 는 -96 에서 +24 dB 사이의 유한한 값입니다. 게인은 데시벨로 더합니다. 트랙 `muted` 또는 클립 `enabled == false` 를 만들면 해당 클립을 들을 수 없게 합니다. 무음 처리된 데이터와 비활성화된 데이터는 여전히 동일한 소스, 참조 및 타이밍 검증을 따르므로 다시 활성화하는 것은 안전합니다. 이 지속된 계약은 장치 재생, 리샘플링, 믹싱, 효과, 페이드, 또는 파형 렌더링을 제공하지 않습니다.

<a id="structural-editing"></a>

## 구조적 편집

소유한 소스를 관리하려면 `DocumentEditor::insertAudioAsset`, `replaceAudioAsset` 및 `removeAudioAsset`를 사용하세요. 비활성화되거나 음소거된 클립을 포함하여 모든 클립에서 사용되는 소스를 삭제하면 `AssetReferenced`가 반환됩니다. 교체는 해당 ID를 유지하며 모든 클립의 소스 범위를 유효한 상태로 유지해야 합니다.

`insertAudioTrack`, `replaceAudioTrack`, `moveAudioTrack` 및 `removeAudioTrack`를 사용하여 레이어를 관리합니다. 트랙 교체는 ID를 유지하면서 이름, 게인, 음소거 플래그 및 전체 클립 컬렉션을 편집합니다. 트랙을 제거하면 해당 클립이 제거되고 재사용 가능한 오디오 자산이 유지됩니다.

`insertAudioClip`, `replaceAudioClip`, 및 `removeAudioClip` 는 트랙 id 로 작동합니다. 클립 교체는 그 id 를 보존하고 이름, 소스, 타임라인 구간, 트리밍, 게인, 또는 활성화 상태를 편집합니다. 삽입과 교체는 클립을 시작 프레임으로 정렬하며, 중복은 여전히 전체 작업을 거부합니다. 직접적인 집계 트랙 삽입 또는 교체는 이미 정준화된 클립 순서를 요구합니다.

모든 편집은 참조, 소스 범위, 및 전체 문서를 검증합니다. 실패한 편집은 문서를, 형식 버전을, 및 편집자 수정을 보존합니다. 동일한 교체는 무작위 작업입니다. 오디오 삽입은 이전 문서를 1.4형식으로 업그레이드합니다. 기존 프레임 수 및 프레임 속도 편집도 오디오 범위를 검증합니다. 파일 기반 편집은 시각 편집과 동일한 동기 `DocumentFile` 트랜잭션을 사용합니다. 안정적인 id 와 const/mutable `findAudioAsset`, `findAudioTrack`, 및 `findAudioClip` 헬퍼를 사용하세요; 컬렉션 변형은 유지된 포인터를 무효화할 수 있습니다.

예를 들어, 지원되는 WAV에서 최소 1초 분량의 소스 오디오를 포함한 1초 오디오 타임라인을 생성합니다. 편집은 클립을 패딩하는 아무런 알림 없이 대신 짧은 입력을 거부합니다:

```cpp
#include <iiSharedCanvas.h>
#include <stdexcept>
#include <utility>

iiSharedCanvas::Document makeAudioTimeline(const std::string &wavPath)
{
    using namespace iiSharedCanvas;
    AudioImportOptions options;
    options.assetId = "dialogue-source";
    auto imported = importAudioWav(wavPath, options);
    if (!imported.ok()) {
        throw std::runtime_error(imported.result.message);
    }

    Document document;
    document.extent = {1920, 1080};
    document.timeline = {{24, 1}, 24};
    DocumentEditor editor(document);
    auto result = editor.insertAudioAsset(std::move(imported.asset));
    if (!result.ok()) {
        throw std::runtime_error(result.message);
    }
    AudioTrackLayer track;
    track.id = "dialogue-track";
    track.name = "Dialogue";
    track.clips.push_back({"dialogue-clip", "Opening", "dialogue-source",
                           0, 24, 0, -3.0, true});
    result = editor.insertAudioTrack(std::move(track));
    if (!result.ok()) {
        throw std::runtime_error(result.message);
    }
    return document;
}
```

<a id="interchange-boundary"></a>

## 교환 경계

타임라인 교환 어댑터는 지속된 오디오 데이터를 시각 트랙과 함께 변환합니다. 그 패키지와 애플리케이션 호환성 계약은 `TIMELINE_INTERCHANGE.md`에 문서화되어 있습니다. `source.iisc`는 권위 있는 오디오 집계를 유지합니다; 편집기 XML와 생성된 WAV 미디어는 파생된 교환 출력입니다. 지원되지 않는 투영은 아무런 알림 없이가 오디오를 폐기하는 대신 보고되어야 합니다.

오디오 모델과 정수 타이밍 검증은 외부 의존성을 도입하지 않습니다. `DocumentAudioTest`는 모델 제약, 합리적 반올림 및 오버플로, 크로스 트랙 겹침, 소스 트림 경계, 롤백, 삭제 안전, 트랙 순서, 클립 편집 및 포맷 마이그레이션을 다룹니다.
