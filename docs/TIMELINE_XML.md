<a id="layered-timeline-xml-interchange"></a>

# 레이어드 타임라인 XML 교환

`exportTimelineInterchange`는 네이티브 문서를 편집 가능한 미디어 기반 타임라인 패키지로 내보냅니다. 그 2와 XML 표현은 동일한 시각적 레이어를 설명하고, 노출 및 독립적인 오디오 클립을 보유합니다; 두 경우 모두 평평한 영화나 믹스가 아닙니다. 패키지는 또한 `source.iisc`를 유지합니다, 왜냐하면 XML 교환은 네이티브 편집 가능한 벡터, 청크, 문서 및 출처 모델을 대체할 수 없기 때문입니다.

`timeline.xml` 는 애플 7 XML ( `xmeml` 버전 5 )을 사용하여 해당 방언을 가져오는 애플리케이션에 적합하며, 프리미어 프로 및 DaVinci 리졸브를 포함합니다. `timeline.fcpxml` 는 애플 FCPXML 1.9을 사용하여 파이널 컷 프로 및 호환되는 리졸브 가져오기를 위해 intended 합니다. 이것들은 서로 다른 형식이며, 상호 교환 가능한 파일 이름 확장명이 아닙니다. 적합한 파일을 타겟 편집기의 XML /타임라인 가져오기 명령을 통해 가져옵니다. PNG 파일 중 하나를 가져오면 해당 노출만 가져옵니다.

<a id="timeline-and-layer-mapping"></a>

## 타임라인 및 레이어 매핑

원본 프레임 속도는 인코딩 전에 감소됩니다. 정확한 정수 속도와 정확한 정수 × 1000/1001 속도는 두 파일 모두에서 표현될 수 있습니다. 임의의 소수점 속도는 `2997/100` 와 같은 소수점 근사값을 포함하며, `UnsupportedFeature` 를 반환합니다; 그들은 방송 속도에 가까운 값으로 절대로 반올림되지 않습니다. FCPXML 시간은 정수 산술을 사용한 감소된 유리수 초를 사용합니다. 전통적인 편집 위치는 정수 프레임, 명목상 `timebase`, 및 해당 `ntsc` 플래그를 사용합니다. 두 시퀀스는 모두 0프레임에서 시작하여 전체 네이티브 지속 시간을 유지하며 드롭되지 않는 타임코드 표시를 사용하며, 표시 번호는 지속 시간을 변경하지 않습니다.

각 네이티브 레이어는 아래에서 위로 순서대로 고유한 레거시 비디오 트랙을 가지고 있습니다. 각 고정 노출은 노출 전·사이·후의 간격을 포함한 정확한 시작·끝 위치를 포함하는 별도의 클립입니다. 표준 레거시 XML에는 휴대용 트랙 이름 요소가 없기 때문에 클립에 이름이 보존됩니다. 숨겨진 레이어와 해당 클립은 유지되고 비활성화되며, 삭제되지 않습니다. 그들의 원본 PNG는 픽셀을 유지하여 편집자가 나중에 활성화할 수 있도록 합니다.

FCPXML 는 타이밍의 기초로 하나의 전체 지속 시간 기본 간격을 사용합니다. 레이어 클립은 네이티브 하단에서 상단 순서로 1 에서 N까지의 양의 레인을 사용하여 그 위에 연결됩니다. 각 연결된 `clip` 는 시간 없는 PNG 자산에 대한 `video` 를 참조합니다. 자산은 `duration="0s"` 를 가지며 프레임 지속 시간이 없는 별도의 이미지 형식을 가지며, 래퍼와 비디오는 노출 지속 시간을 가집니다. 스틸은 잘못하여 일반적인 유한 지속 시간 `asset-clip` 비디오 파일로 선언되지 않습니다. 빈 타임라인은 여전히 기본 간격을 통해 전체 지속 시간을 유지합니다.

각 렌더링된 PNG 는 직선이고 프리멀티플라이드 알파가 없는 전체 캔버스 뷰포트 입니다. 레이어 변환과 벡터 경로는 해당 레어 픽셀로 렌더링되지만, 레어 불투명도, 가시성, 블렌드 모드는 별도의 타임라인 속성으로 유지됩니다. 그것들은 PNG 에 두 번째로 적용되지 않습니다. `source.iisc` 는 네이티브 벡터 경로와 변환을 유지하며, PNG 편집은 벡터 편집 가능성을 유지하지 않습니다. XML 형식은 자체적으로 네이티브 개인 메타데이터를 보존하지 않습니다.

|네이티브 블렌드|레거시 `compositemode`|FCPXML `adjust-blend` 모드|
| --- | --- | --- |
| SourceOver | `normal` | `0` |
|곱하다| `multiply` | `4` |
|스크린| `screen` | `10` |
|을 놓다| `overlay` | `14` |

레거시 불투명도는 `opacity` 에서 0 – 100 스케일에서 독립적인 FCPXML 효과입니다. `adjust-blend amount` 는 0 – 1 스케일에서 사용됩니다. 지원되지 않는 모드 안전하게 거부한다 입니다. 레거시 애플 요소 카탈로그는 Overlay 를 이전 목록에서 생략하지만, 현재 프리미어/리졸브 상호 운용성 문서에는 Overlay 지원이 나열되어 있습니다. 발생한 `overlay` 토큰은 따라서 타겟 편집기 검증을 필요로 하며, FCPXML 는 명시적인 문서화된 숫자 매핑을 가집니다. 다른 편집기의 색상 관리 및 합성 구현은 여전히 다른 픽셀을 생성할 수 있습니다. FCPXML 는 SDR Rec.709 시퀀스와 sRGB 소스 이미지 오버라이드를 선언하며 편집기의 현재 프로젝트나 이미지 프로파일 가정에 의존하지 않습니다.

레거시 정적 **소스** 의 `file` 와 `file/media/video` 지속 시간은 시퀀스를 덮는 전체 분으로, 애플의 정적 이미지 예시와 같습니다. 시퀀스 지속 시간, `clipitem` 지속 시간, 및 소스/타임라인 편집 위치는 프레임 기반을 유지합니다. PNG 소스와 클립은 명시적으로 `alphatype=straight` 와 `stillframe=TRUE` 를 선언합니다. FCPXML 는 일반적인 알파 타입 속성의 동등한 것이 없으며, 알파는 PNG 미디어에 의해 전달됩니다. 전체 캔버스 비디오 요소에서 공간적 컨포름은 안전하게 거부됩니다.

<a id="audio-timeline-mapping"></a>

## 오디오 타임라인 매핑

네이티브 PCM16 모노/스테레오 오디오는 WAV 파일 패키지를 통해 참조됩니다. 8,000 에서 192,000 Hz 범위의 소스 샘플 레이트는 유지되며, 두 시퀀스 모두 48 kHz 스테레오 출력을 선언하므로 수신 편집자는 정상적인 출력 샘플 레이트 변환을 수행할 수 있습니다. 소스 WAV 바이트는 재샘플링, 정규화 또는 믹싱되지 않습니다. 소스 미디어 지속 시간은 시퀀스의 비디오 타임베이스와 독립적으로 인터리브된 PCM 샘플 프레임 수와 소스 샘플 레이트에서 유도됩니다.

레거시 `media/audio` 는 원래 순서로 네이티브 트랙을 포함합니다. 스테레오 클립은 2 링크된 모노 채널 클립으로 변환되며, 서로 다른 `sourcetrack` 인덱스, 공유된 `file` 및 일치하는 `link/groupindex` 값을 사용합니다. 스테레오를 사용하는 네이티브 트랙은 2 개의 인접한 레거시 트랙을 사용하며, 모노 클립은 첫 번째 트랙을 사용합니다. 명시적인 오디오 팬 값은 스테레오 소스 채널 1 를 왼쪽에, 채널 2 를 오른쪽에 배치하며, 모노 클립은 중앙에 배치됩니다. 소스 `file` 는 16비트 심도, 샘플 레이트, 채널 수, 레이아웃 및 소스 채널 레이블을 포함합니다. 트랙과 그 클립은 모두 무음/활성화 상태를 가집니다. 클립 `in/out` 값은 소스 프레임 위치이며, `start/end` 는 독립적인 시퀀스 프레임 위치입니다.

FCPXML는 정확한 샘플 클록 재생 시간과 `hasAudio="1"`, `hasVideo="0"`, `audioChannels`, `audioRate` 값을 가진 유한 재생 시간의 오디오 전용 `asset` 리소스를 사용한다. 각 네이티브 클립은 주 gap 아래의 음수 lane -1부터 -N에 있는 `asset-clip`이 된다. `start`는 원본 오프셋이며, `offset`과 `duration`은 정확한 유리수 시퀀스 시간을 사용한다. 스테레오 구성요소는 원본 채널 `1,2`를 `L,R`로 명시적으로 매핑한다. 음소거 트랙과 개별 비활성 클립도 `enabled="0"` 상태로 유지한다. 네이티브 트랙 묶음은 lane 순서와 합친 트랙/클립 라벨로 표현한다. 수신 편집기는 역할 이름을 정규화할 수 있다. 빈 오디오 트랙은 네이티브 스냅샷과 매니페스트에 유지하며, 트랙 없는 FCPXML 타임라인에는 빈 lane 객체가 없다.

네이티브 트랙 및 클립 게인이 dB에 추가되어 하나의 편집 가능한 클립 조정이 됩니다: 레거시 `Audio Levels/Level`는 선형 진폭인 `10^(gainDb/20)`를 인코딩하고, FCPXML `adjust-volume`는 `dB` 접미사를 사용하여 합계를 전달합니다. 개별 값과 음소거 컨트롤은 `source.iisc`에서 별도로 편집할 수 있습니다. XML 클립 이름에는 네이티브 트랙과 클립 레이블이 모두 포함되어 있으며, 레거시 로깅 정보와 FCPXML 메타데이터도 별도의 레이블을 유지합니다.

샘플 정확한 네이티브 트리밍은 비디오 프레임으로 반올림되어서는 안 됩니다. 레거시 `subframeoffset` 는 충분히 지정된 크로스 편집기 샘플 클록 계약을 갖지 않습니다. 따라서 패키지는 필요할 때 최소한의 WAV 접두사를 제외하고 소스 오프셋을 정확한 비디오 프레임에 재매핑합니다. 소스 샘플 레이트 `s` 와 비디오 레이트 `n/d` 에 대해 정렬 양자 `s*d/gcd(n,s*d)` 샘플 프레임입니다. 제외된 접두사는 이 양자에 대한 네이티브 오프셋의 나머지이며, 나머지 XML 소스 오프셋은 NTSC 레이트를 포함하여 정확합니다. 네이티브 오프셋은 WAV 접두사 트리밍과 XML 오프셋의 합입니다. 후원 소스 핸들이 사용 가능하게 유지되며, 전체 원본 PCM 는 `source.iisc` 에 그대로 남아 있습니다. 2 버전의 매니페스트는 모든 3 오프셋을 십진 문자열로 기록합니다. 프라이빗 작성기는 정렬되지 않은 오프셋을 조용히 수정하는 대신 거부합니다. 오디오 전용 문서와 같은 전체 지속 시간의 기본 간격은 시각적 레이어를 요구하지 않습니다.

<a id="bounds-and-publication"></a>

## 경계 및 출판

XML 미디어 URL 은 절대 `file:` URL 이며, Qt 의 URL 인코더로 인코딩된 후 XML 작성기에 의해 이스케이프 처리됩니다. 그들은 최종 게시된 디렉토리를 참조하며, 임시 스테이지 디렉토리를 결코 참조하지 않습니다. ASCII 이름이 아닌 이름, 공백, 해시 기호, 앰퍼샌드 기호, 퍼센트 기호는 그대로 유지됩니다. 완료된 패키지를 이동하면 수신 편집기에서 미디어를 다시 연결해야 할 수 있습니다. XML 작성기는 외부 미디어를 복사하거나 가져오지 않으며, 네트워크 위치는 출력되지 않습니다.

프라이빗 XML 작성기는 양의 차원/프레임 속도/지속 시간을, UTF-8/XML 안전 이름, 유한 범위 내의 불투명도, 안전한 상대 미디어 경로, 유효한 미디어 참조를 가진 순차적 중첩되지 않는 범위 내 클립을 유효성 검사합니다. 오디오는 채널 레이아웃, 샘플 속도, 소스 범위, 유한한 한계가 설정된 이득, 정확한 표현 가능한 소스 오프셋을 확인된 정수 산술을 사용하여 유효성 검사합니다. 탐색, 절대 미디어 경로, NUL /제어 문자, 유효하지 않은 UTF-8 , 지원되지 않는 블렌드, 지원되지 않는 속도 안전하게 거부한다 입니다. 리소스와 클립 식별자는 사용자 텍스트에서 XML ID 를 파생하는 대신 로컬에서 생성됩니다.

두 XML 바이트 배열은 하나의 집계 출력 한도를 공유합니다. 문자열/ URL 할당 전에 보수적인 초기 크기 확인이 선행되며, 한계가 설정된 출력 장치는 실제 직렬화된 바이트 수를 강제합니다. 정확한 크기 한도는 허용됩니다. 할당 실패, 작성자 오류, 및 한도 실패는 부분적인 XML 를 반환하지 않습니다. 주변 패키지 내보내기 도구는 렌더링, 미디어, 클립/레이어, 네이티브 스냅샷, 및 전체 패키지 한도를 각각 별도로 강제하고 원자적으로 새 디렉토리를 게시합니다. 기존 출력 디렉토리를 덮어쓰거나 네이티브 소스 문서를 변형하지 않습니다.

<a id="verification-and-source-references"></a>

## 검증 및 출처 참조

`TimelineXmlWriterTest`는 실제 출력된 문서를 `QXmlStreamReader`로 독립적으로 파싱한다. 시간/gap·이름과 URL 왕복 변환·아래에서 위로의 트랙/lane·숨긴 클립·4개 혼합 방식 전체·별도 불투명도·straight alpha·시간과 무관한 PNG 리소스·유리수 비율 약분·빈 타임라인·미지원 입력·합산 출력 경계를 검사한다. 오디오 테스트는 연결된 스테레오/모노 채널·NTSC 비율의 원본 자르기·독립 gap·비활성/음소거 클립·합산 gain·샘플 클록 재생 시간·오디오 검증 실패를 다룬다. 성공한 테스트는 추가 스키마 검사를 위해 `build/test-output/`에 XML 픽스처를 기록한다. 형식에 맞는 XML 테스트 자체는 모든 상용 편집기에서 가져오기 성공이나 동일한 렌더링 출력을 입증하지 않는다.

Final Cut Pro가 설치되면, 번들된 1.9 DTD는 FCPXML 픽스처를 독립적으로 검증할 수 있으며, 런타임 종속성을 추가하지 않습니다:

```sh
xmllint --noout --nonet --dtdvalid \
  'file:///Applications/Final%20Cut%20Pro.app/Contents/Frameworks/Interchange.framework/Versions/A/Resources/FCPXMLv1_9.dtd' \
  build/test-output/timeline-writer.fcpxml
```

오디오 픽스처 는 `build/test-output/timeline-audio-writer.fcpxml` 이며, 그 레거시 대응물은 `timeline-audio-writer.xml` 입니다. 두 픽스처 도 설치된 FCPXML 1.9 DTD 와 애플이 발행한 레거시 v5 DTD 를 모두 통과합니다. 스탠다드 라이브러리 개발 오라클 `tests/verify_timeline_interchange.py` 는 완전한 패키지 매니페스트, WAV 채널/샘플 수 및 해시, 정확한 소스 매핑, 게인, 음소거 상태, 연결된 레거시 채널 및 부정 FCPXML 레인을 독립적으로 확인합니다. 그와 같은 명제를 최종 컷 애플리케이션의 재수출에 적용할 수 있으며, 스키마 확인만으로는 검증된 애플리케이션의 수입과 구별됩니다.

2026-09-05에서 최종 컷은 오디오 전용 24 프레임 속도 / 5초 패키지를 수입하여 FCPXML 1.14를 재수출했습니다. 독립적인 오라클은 부정 2 레인, 3 클립, 정확한 소스와 타임라인 트리밍, 활성화/음소거 상태, -9 dB 와 -3 dB 편집 가능한 게인, 스테레오 채널 매핑 및 테스트 라이브러리에 복사된 동일한 WAV 바이트를 확인했습니다. 최종 컷은 사용자 정의 오디오 역할을 표준화하고 사용자 정의 클립 메타데이터를 생략하면서 합쳐진 표시 이름을 유지했습니다. 별도의 네이티브 레이블, 게인 컨트롤 및 ID 는 `source.iisc` 와 매니페스트에서 여전히 권위적입니다. 이 오디오 애플리케이션 확인은 프리미어 또는 리졸브 애플리케이션 통과를 확립하지 않습니다.

어댑터는 새로운 XML 런타임 를 도입하는 대신 기존 Qt XML / URL 작성기를 사용합니다. 2 작은 도메인에서 스키마 매핑은 여전히 별개이므로 하나의 방언의 호환성 규칙이 다른 방언을 재정의하지 않습니다.

권위 있는 스키마와 타이밍 참조는 Apple의 것입니다.
[레거시 XML DTD](https://developer.apple.com/library/archive/documentation/AppleApplications/Reference/FinalCutPro_XML/DTD/DTD.html),
[레거시 요소 카탈로그](https://developer.apple.com/library/archive/documentation/AppleApplications/Reference/FinalCutPro_XML/Elements/Elements.html),
[레거시 타이밍 및 오디오 게인 규칙](https://developer.apple.com/library/archive/documentation/AppleApplications/Reference/FinalCutPro_XML/Topics/Topics.html),
[정지 이미지 예시](https://developer.apple.com/library/archive/documentation/AppleApplications/Reference/FinalCutPro_XML/Applications/Applications.html),
[FCPXML 참조](https://developer.apple.com/documentation/professional-video-applications/fcpxml-reference),
[형식 설명](https://developer.apple.com/documentation/professional-video-applications/format), 그리고 [수치 혼합 매핑](https://developer.apple.com/documentation/professional-video-applications/adjust-blend). 정지 이미지 구조는 실제 Final Cut 내보내기와도 비교되었습니다.
[OpenFCPXMLKit의 ImageSample 고정구](https://github.com/TheAcharya/OpenFCPXMLKit/blob/main/Tests/FCPXML%20Samples/FCPXML/ImageSample.fcpxml). 현재 대상 지원은 다음과 같이 설명됩니다
[Adobe의 XML 교환 테이블](https://helpx.adobe.com/premiere/desktop/organize-media/transfer-files/supported-elements-for-final-cut-pro-x.html)와 [DaVinci Resolve 참조 매뉴얼](https://documents.blackmagicdesign.com/UserManuals/DaVinci_Resolve_21_Reference_Manual.pdf).
