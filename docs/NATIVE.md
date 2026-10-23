# Qt 없는 문서·편집 패키지

네이티브 BitmapCodec은 iiImageCodec과 FileProvider를 사용한다. PNG, JPEG, BMP,
TGA, WebP, TIFF를 쓰고 GIF와 PSD의 합성 이미지를 읽는다. 독립 레이어의 PSD는
LayeredDocumentCodec이 담당하며, 한글 레이어 이름, 알파, 27개 혼합 모드를 보존한다.
벡터 출력은 GUI 초기화 없이 PDF 1.4 경로와 알파를 생성하여 PSD Smart Object에 포함한다.

OpenRaster는 메모리 내 ZIP 검증, 제한된 deflate와 CRC, 외부 접근 없는 XML 파싱을
사용한다. 경로 이탈, 링크, 중복 이름, DTD, 과도한 확장 크기를 거부한다. 표현할 수
없는 그룹 합성은 오류이며 중립 pass-through 그룹의 평탄화는 경고이다. 파일 생성과
원자적 교체는 FileProvider가 수행한다. 캔버스는 sRGB 픽셀을 사용하며 지원하지 않는
RGB ICC 프로파일은 iiImageCodec의 네이티브 변환으로 sRGB 작업 공간에 맞춘다.
PSD 입력의 지원 범위를 벗어난 ICC·합성 구성은 명시적으로 거부한다. NativeMedia 테스트는 독립 ORA 픽스처, 손상된
아카이브, 투명 픽셀, 한글 PSD 레이어와 벡터 PDF 포함을 검사한다.

`IISHAREDCANVAS_BUILD_QT_API=OFF`는 Qt 탐색 전에 네이티브 빌드 경로를 선택한다.
설치 소비자는 `find_package(iiSharedCanvasNative CONFIG REQUIRED)`와
`iiSharedCanvas::Native`를 사용한다. 이 패키지의 `Core` 별칭도 같은 네이티브 타깃이다.

기존 표준 C++ 문서·래스터/벡터/타일·편집기·검증·직렬화를 재사용하며,
저자 메타데이터와 SQLite·파일 I/O는 iiFileProvider Native가 소유한다.
문서 파일은 기존 canonical SQLite 레코드·해시·동기 트랜잭션 형식을 유지한다.
일반 탐색과 렌더링은 저장 리비전을 바꾸지 않는다. 브러시 입력 궤적은 문서에 저장하지 않는다.

`insertChunkedRasterAsset`은 검증한 무한 캔버스 청크 자산을 편집 트랜잭션에 삽입한다.
무한 캔버스의 `renderFrameRegion`과 타일 요청은 저장된 표시 범위 밖의 음수 좌표도
읽을 수 있다. 출력과 좌표 한도를 검사하며 저장된 원점·크기·리비전을 바꾸지 않는다.
유한 캔버스의 요청은 기존 문서 표시 범위 안으로 제한한다.

네이티브와 기존 Qt 패키지는 서로 다른 메타데이터 ABI이다. 하나의 실행 파일에서
두 ABI를 섞어 연결해서는 안 된다. 소비자는 해당 exported target의 정의와 헤더를 함께 사용한다.
기존 Qt 미디어/Quick 어댑터는 명시적인 레거시 패키지로 유지한다.

`iiSharedCanvas.Native.Contracts`는 Qt 없이 생성·편집·거부/예외 롤백·읽기 전용
스냅샷·재열기·저자 메타데이터·직렬화 손상 검사를 실행한다. 설치 소비자도 같은 파일을
exported target만으로 빌드한다. 컴파일 성공과 실제 파일 왕복 결과를 구분하여 기록한다.
