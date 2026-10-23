# 네이티브 게시 메타데이터

패키지 0.32.0, ABI 0.32, `.iisc` 형식 1.22부터 `Document::publicationMetadata`는
iisacc 공통 게시 데이터를 캔버스 내부에 보존한다. 제목, 설명, 캔버스별 캡션과
대체 텍스트, 게시 순서, 커버 및 게시 설정은 하나의 `.iisc`만으로 복원된다.
별도 게시 포맷이나 메타데이터 동반 파일을 요구하지 않는다.

공개 헤더는 `Metadata/PublicationMetadata.h`이다. 새 모델은 표준 C++23의 문자열,
벡터, 맵과 `variant<string, bool>`만 사용하며 Qt, UI 및 전송 API를 참조하지 않는다.
모든 소비자가 같은 공개 모델과 직렬화 계약을 사용한다.

## 모델과 편집

`PublicationMetadata`의 `title`과 `description`은 문서 전체의 게시 제목과 설명이다.
`canvases`는 `CanvasPublicationMetadata`의 순서 있는 목록이며 `artboardId`,
`caption`, `alternativeText`를 보존한다. 목록 순서는 아트워크 쌓기 순서와 독립이다.
빈 ID는 문서의 루트 캔버스를 참조하며, 아트보드를 추가해도 이 참조는 유효하다.
`coverArtboardId`는 커버 아트보드 ID이며 빈 값은 지정하지 않은 커버 또는 루트이다.

`settings`는 문자열 또는 불리언 값을 보존한다. 콘텐츠 분류, 주제, 소프트웨어,
태그, 라이선스, 콘텐츠 표시, 게시 시각, 예약 시각, 댓글 허용 범위, 반응·댓글 수
표시, 멘션, 출처·기여자 등의 공통 설정을 담을 수 있다. 소비자는 해석하지 않는
키를 보존하며 UI 레이블과 유효한 설정 선택지는 소비자 어댑터에서 변환한다.
인증 토큰과 네트워크 작업 상태는 게시 메타데이터가 아니다.

```cpp
iiSharedCanvas::PublicationMetadata metadata;
metadata.title = "Publication title";
metadata.description = "Description";
metadata.coverArtboardId = "artboard-id";
metadata.canvases = {{"artboard-id", "Caption", "Alternative text"}};
metadata.settings["adult"] = false;
metadata.settings["license"] = std::string("All rights reserved");
iiSharedCanvas::DocumentEditor editor(document);
const auto result = editor.setPublicationMetadata(std::move(metadata));
```

`setPublicationMetadata`와 `clearPublicationMetadata`는 검증된 문서 편집이다.
실제 변경은 문서 리비전·저자 이력을 갱신하며 같은 값을 다시 설정하면 무변경이다.
유효하지 않은 참조는 이전 메타데이터·형식·리비전을 보존하고 거부한다.
아트보드 삭제는 해당 게시 항목을 제거하고 커버 참조를 남은 게시 항목으로 옮긴다.
화면 전환·미리보기·입력 중인 UI 초안은 이 API를 호출하는 문서 저장 동작과 구분한다.

## 저장과 호환성

바이너리 스냅샷의 아트보드 뒤에 메타데이터 유무, 제목·설명·커버, 캔버스 목록,
정렬된 설정 맵을 기록한다. 설정 태그 0은 문자열, 1은 불리언이다.
작업용 저장소는 기존 스키마 1의 독립 `PublicationMetadata` 레코드(종류 12)를
사용한다. 메타데이터 변경에 미디어 레코드를 다시 기록하지 않는다.
트랜잭션·체크섬·직렬화 제한·읽기 전용 스냅샷에도 동일한 계약이 적용된다.

1.0~1.21 파일은 메타데이터 없음으로 읽으며 기존 바이트 재인코딩을 유지한다.
메타데이터가 있는 문서를 1.21 이하로 기록하면 오류이다. 편집 API는 필요한 형식을
1.22로 승격한다. 구버전 리더는 새 형식을 거부하므로 조용한 메타데이터 손실이 없다.

`SerializationLimits`의 공통 메타데이터 제한을 사용한다. 기본 문자열별 제한은
16 MiB, 전체 문자열 예산은 64 MiB, 메타데이터 항목은 합산 65,536개이다.
UTF-8, 중복 설정 이름, 알 수 없는 타입·불리언 태그, 중복·누락된 캔버스 참조,
설정의 빈 이름, 컬렉션의 크기와 페이로드 길이를 양방향으로 검증한다.
외부 FileProvider 분류 필드의 64 KiB 제한을 적용하지 않는다.

`Document` 공개 집계의 크기가 바뀌므로 소비자와 그 문서를 값으로 전달하는 SDK도
0.32.0을 사용해 다시 빌드해야 한다. 검증은 스냅샷 단일 파일 복사, 작업 파일
닫기·재개방, 편집 원자성, 기존 형식 왕복, 미디어 보존 및 설치된 Core 소비자에서 수행한다.
소스·설치 소비자 검사는 실제 링크한 FileProvider 디렉터리를 로더 경로에 우선 배치하여
다른 SDK가 포함한 오래된 동명 라이브러리로 검증 결과가 바뀌지 않도록 한다.
