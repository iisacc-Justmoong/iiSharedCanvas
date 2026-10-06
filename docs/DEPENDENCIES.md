<a id="dependency-review"></a>

# 의존성 검토

<a id="macos-release-compatibility-2026-09-09"></a>

## macOS 릴리스 호환성(2026-09-09)

부동 소수점 메타데이터 변환 사용
[fast_float   8.2.10](https://github.com/fastfloat/fast_float/releases/tag/v8.2.10), 2026-06-14에 출시되었습니다. MIT 라이선스가 상위 공급 측 라이선스 옵션에서 선택됩니다. 비밀이고 수정되지 않은 177,375바이트 단일 헤더는 별도의 런타임 의존성이 없으며, 공식 릴리스 디지스트는 `third_party/fast_float/UPSTREAM.md` 에 기록되고 공지사항은 SDK 와 함께 설치됩니다. 유지되는 상위 공급 측 는 로케일 독립적인 `from_chars` 의미를 구현합니다. 이는 Qt 없는 도메인 모델을 보존하고 macOS 12를 지원하며, 컴파일러의 libc++ 부동소수점 오버로드가 사용 불가능합니다. 정수 변환은 변경되지 않았습니다. 표준 iostream 변환도 평가되었지만, 현재 libc++ 런타임 에서 표현 가능한 아정수 값을 실패로 나타냅니다. 회귀 사례는 지수/아정수 값, 범위 오류, 잘못된 접미사, 쉼표 십진 로케일 내의 NUL, 그리고 인용된 공백을 포함합니다.

macOS 12 릴리스 빌드는 워크스페이스의 기존 고정 소스(커밋 `6f8a0cdd24a0dc6cce9dac4a7679da784ab124ea`) 에서 libzip 1.11.4 를 사용하고, 시스템 zlib 로 정적으로 빌드하며 선택적 압축 백엔드를 비활성화합니다. 시스템 CommonCrypto 백엔드는 활성화되어 있으므로 테스트가 암호화된 ZIP 픽스처 를 생성하고 거부 여부를 확인할 수 있습니다. 이는 기존 암호화되지 않은 저장/ DEFLATE 만 있는 ORA 계약에 부합하며 호스트의 최신 Homebrew 라이브러리에 대한 런타임 의존성을 제거합니다. 생성된 패키지를 `libzip_DIR` 로 전달하고 애플리케이션 번들에 BSD-3 -조항 공지사항을 보존합니다.

<a id="psd-export-2026-09-03"></a>

## PSD 내보내기(2026-09-03)

프레임‐0 라이터는 이미 연결된 것을 재사용합니다.
[Qt PDF 페인트 디바이스](https://doc.qt.io/qt-6/qpdfwriter.html) 를 내장 벡터 소스에, 그리고 캐시된 레이어/ 합성 픽셀에 대한 네이티브 렌더러를 위해 사용한다. PDF 직렬화기, 벡터 래스터화기, JavaScript / Python 런타임 또는 추가 링크 의존성이 도입되지 않는다. Qt 의 기존 라이선싱 및 배포 경계는 변경되지 않는다. PSD 구조는 다음을 따른다.
[Adobe 사양](https://www.adobe.com/devnet-apps/photoshop/fileformatashtml/)이며, 유지 관리된 구현 및 픽스처와 독립적으로 검사됩니다.

외부 작성자 검토는 고려되었습니다.
[ag-psd](https://github.com/Agamnentzar/ag-psd), 유지 관리된 MIT JavaScript 리더/라이터. 그 핵심 런타임 종속성은 기본64-js와 pako이며, 여기서 사용하려면 추가로 JavaScript 런타임 /bridge가 필요하고 여전히 네이티브 캔버스‐투캐시 매핑이 필요합니다. 레이어를 렌더링하지 않으며, 합성가 스스로 변경됩니다. 그것의 배치/링크된 레이어 구현은 새로운 배포 종속성이 아니라 유용한 호환성 기준을 제공합니다. 이전에 검토된 BSD-3 -Clause
[PhotoshopAPI](https://github.com/EmilDohne/PhotoshopAPI) 는 훨씬 더 큰 C++20 의존성 그래프와 벡터 배치 레이어 데이터에 대한 지원되지 않음/경고 경로를 가지며, 이를 채택하는 것은 여기에 필요한 특정 PDF 스마트 오브젝트 매핑을 제거하지 못한다. 기존 psd_sdk 리더 제한은 여전히 가져오기에 관련이 있으며, SDK 를 내보내기에 사용한다는 주장은 아니다. 그것의
[공개 내보내기 API](https://github.com/MolecularMatters/psd_sdk/blob/master/src/Psd/PsdExport.h)는 평면 픽셀 레이어와 메타데이터/복합물을 노출하지만, 내장된 스마트 오브젝트 저작 표면은 없습니다. 따라서 네이티브 캔버스‐to‐PDF / PSD 매핑은 기존 코덱 위에 작은 프로젝트 소유 도메인 어댑터로 남아 있습니다.

Python [psd-tools](https://github.com/psd-tools/psd-tools) 는 내보낸 스마트 오브젝트를 인식하고, 내장된 PDF 페이로드/UUID 를 해결하며 첫 프레임 픽셀을 검사하는 선택적 개발 오라클로만 사용된다. 그것의 MIT 패키지 및 Python / Pillow / NumPy /attrs 의존성은 연결되지 않았으며, `install.sh` 에 의해 설치되지 않았고, 라이브러리, 명령줄 도구, 일반 CTest 또는 소비자에게 필요하지 않다. 선택적 오라클은 또한 [pypdf](https://github.com/py-pdf/pypdf), 유지되는 BSD-3 -조항, 순수 Python PDF 리더를 사용하여 실제 경로 연산자와 래스터 이미지 X 오브젝트의 부재를 확인한다. 그것의 코어는 Python 3.11+ 에서 추가 패키지를 필요로 하지 않으며, 암호화/이미지/폰트 부가 기능은 사용되지 않는다. 검사된 번들 복사본은 대략 3.3 MB 를 차지한다. 두 개발 파서는 배송 런타임 의존성 그래프 밖에 있다.

내보내기 CLI는 iiFileProvider::Database에 위임되며, 이는 SQLite의
[온라인 백업 API](https://www.sqlite.org/backup.html) 를 통해 일관된 개인 스냅샷을 읽기 전용 작업 파일 연결에서 획득하여, 커밋된 WAL 콘텐츠를 포함합니다. 작성자 `DocumentFile` 소유자만 임시 복사본이 열립니다. 이는 소스 데이터베이스의 저널 모드를 변경하지 않고 검토된 플랫폼 SQLite 의존성을 재사용하며, 셸 데이터베이스 도구는 시작되지 않습니다.

<a id="layer-preserving-document-import-2026-09-03"></a>

## 레이어 보존 문서 가져오기 ( 2026-09-03 )

- [libzip](https://libzip.org/) 는 비공개 CMake 타겟 `libzip::zip` 를 통해 OpenRaster ZIP 리더를 공급합니다. ZIP 에 대한 활성적으로 유지되는 [BSD-3 조항 라이선스](https://libzip.org/license/)에 따른 의존성입니다. 패키지는 `find_package(libzip 1.7.3 CONFIG REQUIRED)` 를 사용합니다; 이는 API 최소값이며, 오래된 보안 패치 수준을 배포하라는 권장 사항이 아닙니다. 검토된 호스트는 1.11.4 ( Homebrew 버전 1 )을 제공하며, 전체 케그에 약 1.2 MB 를 차지합니다. 이 프로젝트는 아무것도 다운로드하거나 벤더링하지 않습니다.
- 상위 공급 측 의 [빌드 문서](https://libzip.org/guides/building/) 는 zlib 를 필요로 하며, 선택적 추가 코덱/암호화를 허용합니다. 최소 빌드는 이미 검토된 zlib 만 필요합니다. 검사된 호스트의 동적 라이브러리는 시스템 bzip2 과 패키지 제공 xz/zstd 에도 연결되며, 배포 애플리케이션은 실제 런타임 의존성 클로저와 제 3 자 공지를 패키징해야 합니다. ORA 어댑터는 암호화되지 않은 저장된/ DEFLATE 엔트리를만 허용하며, 추출 API 를 절대 호출하지 않고, 이름/개수/확장된 바이트를 독립적으로 제한합니다. 설치된 CMake 패키지는 정적 및 공유 소비자를 위해 libzip 를 해결합니다.
- 배포된 libzip 1.11.4 [파일명 리더](https://github.com/nih-at/libzip/blob/v1.11.4/lib/zip_io_util.c) 는 내장된 파일명 NUL 를 공백으로 정규화하며, 원본 이름 API 에도 적용됩니다. 한계가 설정된 클래식- ZIP 엔벨로프 사전 검사 는 libzip 가 이를 보게 되기 전에 바이트 정밀 중앙/로컬 이름을 유효성 검사합니다. 또한 ZIP64 와 멀티 디스크 레이아웃을 거부하며 디렉토리 개수를 제한합니다. 일반적인 공백 포함 이름은 계속 지원됩니다. 이는 관찰된 의존성 동작을 위한 표적 우회책이며, 두 번째 ZIP 코덱이 아닙니다; 압축, 페이로드 파싱 및 CRC 검증은 libzip 에 남아있습니다. [OPENRASTER_IMPORT .md](OPENRASTER_IMPORT.md)를 참조하세요.
- [OpenRaster 레이아웃](https://www.openraster.org/baseline/file-layout-spec.html) 와 [레이어 의미론](https://www.openraster.org/baseline/layer-stack-spec.html) 은 기존 Qt XML / PNG 프라임티브를 통해 재사용 가능한 캔버스 도메인 매핑을 정의합니다. 애플리케이션 전용 브릿지, 쉘 unzip, 사설 Qt ZIP API 또는 두 번째 XML 라이브러리는 도입되지 않습니다. 격리된 그룹은 독립적인 네이티브 레이어와 동등하지 않으며, 지원되지 않는 합성 안전하게 거부한다 입니다.
- PSD 파싱은 [Adobe 파일 명세](https://www.adobe.com/devnet-apps/photoshop/fileformatashtml/) 를 따르며, 한계가 설정된 v1 / 8비트 RGB 픽셀 레이어 하위 집합을 위해 수행됩니다. 기존 zlib 는 ZIP 채널 압축을 처리하며, 새로운 압축 구현은 없습니다. zlib API 최소값은 `uncompress2` 를 위해 1.2.9 입니다. 이는 소비된 압축 바이트를 확인합니다. 별도의 유지되는 파서가 먼저 검토되었습니다: [psd_sdk](https://github.com/MolecularMatters/psd_sdk) 는 BSD-2 -절이며 최근 상위 공급 측 유지보수가 있었으나, [메모리 파일 리더](https://raw.githubusercontent.com/MolecularMatters/psd_sdk/master/src/Psd/PsdMemoryFile.cpp) 는 단수 확인으로만 원시 `memcpy` 를 보호하고, [동기 리더](https://raw.githubusercontent.com/MolecularMatters/psd_sdk/master/src/Psd/PsdSyncFileReader.cpp) 는 읽기 실패를 전파하지 않습니다. 직접 도입은 유지되는 포크 없이는 이 API 의 복구 가능한 잘못된 입력 및 할당 한계 계약을 충족하지 못합니다. 라이브러리 내 매퍼는 대신 모든 한계가 설정된 필드를 확인하고 구현되지 않은 그리기 의미론을 거부합니다. 이는 작은 파서가 모든 Photoshop 동작을 구현한다는 주장이 아닌 특정 의존성 결함 검토입니다.
- [PhotoshopAPI](https://github.com/EmilDohne/PhotoshopAPI) 는 BSD-3 -절이며 C++20 이나, 그 의존성 그래프에는 OpenImageIO, libdeflate, Eigen, fmt, UUID, 메모리 매핑 및 SIMD/문자열 도우미가 포함됩니다. Python [psd-tools](https://github.com/psd-tools/psd-tools) 는 MIT 이며 유지되나, Python, Pillow, NumPy 및 attrs(또는 추가 선택적 합성 패키지)를 도입합니다. 정의된 네이티브 픽셀 레이어 하위 집합에 대해 두 표면 모두 정당화되지 않으므로, 두 표면 모두 제품에서 연결되거나 실행되거나 다운로드되지 않습니다. 선택적 개발 전용 PSD 내보내기 검증은 위에서 설명되어 있습니다.

캔버스 모델과 `.iisc` 형식은 변함이 없습니다. 계층형 리더는 분리된 네이티브 값을 생성하며, 변환기는 기존의 `DocumentFile` 생성 경계를 사용합니다. 새 모듈은 어떠한 소비자 제품에도 의존하지 않습니다.

<a id="media-interchange-2026-09-03"></a>

## 미디어 교환 ( 2026-09-03 )

- 기존 Qt Gui/Core는 이미지 코덱, XML 토큰화, 경로 기하학, 색상 변환, PDF 도면 및 프로세스 I/O를 제공합니다. 유지 관리되는 [이미지 플러그인 API](https://doc.qt.io/qt-6/qtimageformats-index.html)는 선택적 TIFF/WebP/HEIC/JP2 지원을 런타임에서 발견합니다. 두 번째 이미지 프레임워크는 연결되거나 공급되지 않았습니다. 옵션 플러그인은 Qt와 제3자 라이선스를 유지하며, 애플리케이션은 유지 관리된 플러그인과 해당 공지사항만 배포해야 합니다.
- zlib는 표준 호환 SVGZ 압축 및 PNG 청크 CRC 검증을 위한 소규모 추가 링크 의존성이며, 플랫폼/패키지 `ZLIB::ZLIB` 대상을 사용합니다. 상위 공급 측 [프로젝트](https://zlib.net/)는 유지 관리되며 허용된 zlib 라이선스를 사용합니다. DEFLATE /gzip 및 체크섬을 제공하며, 손글씨 압축은 추가되지 않습니다. 설치된 플랫폼 라이브러리는 번들 포크나 서비스 없이 재사용됩니다.
- FFmpeg/ffprobe는 비디오 및 확장된 비트맵 코덱을 CMake 링크 의존성이 아닌 애플리케이션이 선택하는 선택적 **런타임 실행 파일**로 제공한다. 상위 공급 측는 유지보수되는 [릴리스](https://ffmpeg.org/download.html)와 안정적인 [CLI 문서](https://ffmpeg.org/ffmpeg.html)를 공개한다. 실행 파일은 인수 배열, 한계가 설정된 파이프와 실행 기한을 사용하여 호출하며 네트워크 프로토콜은 사용하지 않는다. 이 저장소에서 바이너리 다운로드, 설치 또는 재배포는 발생하지 않는다. 조사한 호스트의 FFmpeg 9.0.1 설치 접두사 크기는 의존 라이브러리를 제외하고 52 MB이다. 배포하는 코덱의 규모는 이 어댑터보다 상당히 크다. [라이선스는 구성에 따라 달라진다](https://ffmpeg.org/legal.html). 기본은 LGPL-2.1-or-later이며 선택적 구성 요소는 GPL을 요구할 수 있고 nonfree 빌드는 재배포 제한이 있다. 호스트 테스트 빌드는 GPL과 버전 3을 활성화한다. 실행되었다는 사실은 소비자의 배포 번들이 배포 또는 코덱 특허 의무를 충족한다는 근거가 아니다.
- 편집 가능한 SVG 는 Qt 의 XML 와 경로 원시 함수에 대한 캔버스 도메인 매핑입니다. Qt SVG 는 렌더링되지만 편집 가능한 장면을 노출하지 않습니다. NanoSVG 는 검토되었습니다 ([상위 공급 측](https://github.com/memononen/nanosvg)); 의도적으로 제한된 허용적인 파서 아무런 알림 없이 는 지원되지 않는 구조를 제거하고 경로를 입방 곡선으로 정규화합니다. 이는 이 라이브러리의 안전하게 거부하는 import 와 선형/이차 항등 계약과 충돌합니다. 새로운 SVG 파서 의존성이 채택되지 않습니다. 제한된 지원되는 SVG 어휘는 명시적으로 문서화되고 테스트됩니다.

Qt PDF 및 SVG 래스터 화기는 선택적으로 배포된 이미지 플러그인이며, 추가 연결된 Qt 모듈이 아닙니다. 래스터 임포트는 편집 가능한 PDF 객체도 완전한 SVG 기능 충실도도 광고하지 않습니다. `importBitmap` 는 PSD 평탄화된 합성 만 디코딩하며, 별도의 `importLayeredDocument` 리더는 명시적으로 문서화된 PSD 래스터 레이어 하위 집합을 지원합니다. EPS / AI, XCF, KRA, PSB 및 기타 지원되지 않는 레이어 형식은 편집 가능한 임포트로 광고되지 않습니다.

<a id="sqlite-for-write-through-document-files-2026-09-03"></a>

## 쓰기 문서 파일용 SQLite ( 2026-09-03 )

SQLite 는 iiFileProvider 의 개인 저장소 의존성으로, `DocumentFile` 가 자체 소유의 데이터베이스 인터페이스를 통해 사용합니다. iiPaintEngine 는 유일한 페인팅 의존성으로 남으며, iiSharedCanvas 에 의존하지 않습니다. SQLite C API 는 iiFileProvider 에 고유하며, 캔버스 공개 헤더는 SQL 또는 SQLite 타입을 노출하지 않습니다. 제공자 CMake 는 플랫폼/패키지 제공 `SQLite::SQLite3` 타겟을 사용하며, 새 CMake 에서는 `SQLite3::SQLite3` 로 명명됩니다. 서버, 서비스, Qt SQL 플러그인, 다운로드 또는 벤더 포크는 추가되지 않습니다. macOS 에서 발견은 관련 없는 프레임워크 내장 SQLite 복사본보다 SDK /라이브러리 헤더를 우선적으로 선호합니다. 그것은 SQLite 발견 바깥에서 소비자의 프레임워크 검색 정책을 변경하지 않습니다. 오래된 캐시된 헤더/라이브러리 선택을 교체할 때 새 CMake 설정을 사용하세요.

- 유지 관리: 상위 공급 측 [릴리스 히스토리](https://www.sqlite.org/changes.html) 는 계속되는 유지 관임을 보여줍니다. 유지 관리되는 벤더 런타임 를 선호하십시오. 3.26 최소값은 권장되는 런타임 고정점이 아닌 API /헤더 호환성 바닥입니다.
- 라이선스: 상위 공급 측는 제공된 라이브러리를 [퍼블릭 도메인](https://www.sqlite.org/copyright.html)에 전용하며, 상업적 재배포를 포함합니다. iiSharedCanvas의 자체 AGPL-3.0 -only 라이선스는 변경되지 않았습니다.
- 크기: 상위 공급 측의 날짜가 지정된 [풋프린트 예시](https://www.sqlite.org/footprint.html)는 1 MB 아래에 있습니다; 실제 플랫폼 빌드와 선택적 기능은 다를 수 있습니다. 이 빌드는 기존 네이티브 라이브러리를 연결하며, 확장을 활성화하거나 두 번째 사본을 배포하지 않습니다. 교차 컴파일된 소비자는 대상의 SQLite를 제공해야 합니다.
- 범위: SQLite는 잠금, 페이지 쓰기, 롤백 및 내구성 커밋을 제공합니다. iiSharedCanvas는 문서 레코드 매핑 및 편집 경계만 제공합니다. 원자 저장 엔진을 재구현하거나 전체 직렬화된 캔버스를 반복적으로 교체하면 회피 가능한 유지보수나 쓰기 증폭이 추가됩니다.

작업 파일은 DELETE 롤백 저널링, `synchronous=EXTRA`, 및 `fullfsync=ON` 를 사용합니다. 지연 체크포인트링과 달리 완료된 변경 사항은 편집이 반환되기 전에 메인 파일에 있습니다. 충돌 복구를 위해 일시적 `-journal` 파일이 필요하며 파일이 사용 중일 때는 제거하지 않아야 합니다. 보장 사항은 기본 파일 시스템이 잠금과 동기화를 존중하는지에 달려 있으며 SQLite 의
[원자적 커밋 가정](https://www.sqlite.org/atomiccommit.html) 및
[동기 모드](https://www.sqlite.org/pragma.html#pragma_synchronous).

증분 [BLOB API](https://www.sqlite.org/c3ref/blob_write.html) 는 동일한 크기의 레코드에서 변경된 바이트 스패ンを 기록합니다. SQLite 는 여전히 물리적 페이지를 저널링/기록합니다. 4바이트의 논리적 픽셀 편집은 4 바이트의 물리적 장치 I/O 를 주장하는 것이 아닙니다.

<a id="layered-timeline-interchange-2026-09-03"></a>

## 계층형 타임라인 교환 ( 2026-09-03 )

타임라인 패키지는 기존 Qt Core XML / URL / JSON 프리미티브와 검토된 PNG 라이터를 사용합니다. 새로 연결된 라이브러리, Python 런타임, NLE 플러그인, 온라인 변환 서비스 또는 실행 파일이 필요하지 않습니다.

- [OpenTimelineIO](https://github.com/AcademySoftwareFoundation/OpenTimelineIO) 는 어댑터를 구현하기 전에 검토되었습니다. 그의 C++ 타임라인 코어와 Python 바인딩은 [Apache-2.0](https://github.com/AcademySoftwareFoundation/OpenTimelineIO/blob/main/LICENSE.txt)하에 유지됩니다. 검토된 [PyPI 릴리스](https://pypi.org/project/opentimelineio/0.18.1/) 는 0.18.1입니다. 그의 macOS64 CPython 3.12 휠은 대략 1.24 MB 압축되어 있으며, 소스 아카이브는 대략 2.92 MB 입니다; 이 수치들은 Python 런타임 를 제외하며 설치 발자국 측정값이 아닙니다. 상위 공급 측 의 [네이티브 빌드 의존성](https://github.com/AcademySoftwareFoundation/OpenTimelineIO/blob/main/src/deps/CMakeLists.txt) 에는 Imath, RapidJSON 와 minizip-ng 이 포함되어 있으며, Python 바인딩은 추가로 pybind11를 사용합니다. 코어를 재사용하면 이 라이브러리의 정수 프레임, 유리 비율 캔버스 모델 옆에 두 번째 지속된 편집 모델이 추가로 도입됩니다.
- OTIO 의 인터체인지 [어댑터는 Python 플러그인](https://github.com/AcademySoftwareFoundation/OpenTimelineIO/blob/main/docs/tutorials/adapters.md)이며, C++ FCPXML 작성기 API 가 아닙니다. 분리 배포된 Apache-2.0 [FCP 7 어댑터](https://github.com/OpenTimelineIO/otio-fcp-adapter) 와 [FCP X 어댑터](https://github.com/OpenTimelineIO/otio-fcpx-xml-adapter) 는 여러 트랙, 간격 및 중첩을 지원하지만, 둘 다 오디오/비디오 효과 지원 불가임을 명시적으로 표시합니다. 그들의 일반 모델 매핑은 따라서 이 계약의 편집 가능 레이어 불투명도와 합성 를 구현하지 않습니다. 그들의 1.0.0 Python 휠은 각각 대략 28 KB 와 19 KB 압축되어 있으며, OTIO 에 추가됩니다. 검토된 FCP 7 저장소는 2026 유지 관리가 있으며, FCP X 저장소의 최신 푸시는 6 월 2024입니다. 특히, [OTIO -플러그인 0.18.0 는 FCP X 를 포함 배터리 세트](https://github.com/OpenTimelineIO/OpenTimelineIO-Plugins/releases/tag/v0.18.0)에서 제거했습니다. 현재 메타패키지는 8 개의 다른 어댑터를 설치하며, FCP X 를 별도로 선택하고 유지 및 테스트할 필요성을 제거하지 않습니다.
- 직접 XML 생성은 이미 정의된 캔버스 도메인 매핑인 레이어 순서, static/hold-keyframed 노출 범위, 캔버스 크기의 PNG 미디어, 가시성, 불투도 및 문서화된 블렌드 하위 집합으로 제한됩니다. Qt 는 XML 이스케이프 및 파일 URL 인코딩을 처리합니다. 이는 대체 일반 편집 타임라인 엔진이나 무한한 XML 파서가 아닙니다. OTIO 는 여전히 합리적인 선택적 개발 오라클 또는 더 넓은 편집 교환을 위한 미래 의존성으로 남아있지만, 자동으로 설치되거나 실행되지 않습니다. 패키지의 네이티브 `source.iisc` 백업은 기존 직렬화기 를 사용합니다. 검증 또는 직렬화 전에 소스 크기의 임시 데이터를 생성하면, 할당 없는 사전 검사 는 소스 자산, 래스터 청크, 벡터 명령어, 이름, 소유된 키 및 중첩된 생성 메타데이터를 스냅샷 메모리 할당량에 대해 보수적으로 전액 청구합니다. 그는 압축이나 인코딩 후 파일 크기 확인에 의존하지 않고, 호출자는 더 큰 문서에 대해 한도를 명시적으로 높일 수 있습니다.
- 2개 형식 어댑터가 필요하다. Adobe의 현재 [Premiere 가져오기 문서](https://helpx.adobe.com/premiere/desktop/organize-media/import-files/migrate-from-final-cut-pro-x.html)는 직접적인 `.fcpxml` 가져오기를 명시적으로 제외한다. 레거시 Final Cut XML과 현대 FCPXML를 서로 교환 가능한 것으로 취급해서는 안 된다. 레거시 출력은 Apple의 [Final Cut XML 참조](https://developer.apple.com/library/archive/documentation/AppleApplications/Reference/FinalCutPro_XML/Elements/Elements.html)를 따른다. Final Cut 출력은 Apple의 [FCPXML 문서 모델](https://developer.apple.com/documentation/professional-video-applications/creating-fcpxml-documents)과 버전 지정 DTD를 따른다. Apple DTD는 검증 참조이며 새로운 런타임 의존성이나 함께 포함한 구성요소가 아니다. 설치된 Final Cut Pro 애플리케이션은 선택적인 독립 `xmllint` 검증에 사용할 `FCPXMLv1_9.dtd`를 제공한다. 스키마 유효성만으로 대상 앱의 가져오기·정지 미디어 시간·혼합 렌더링·색상 관리 동등성·앱별 트랙 UI를 입증하지 못한다.

OTIO 코드, 어댑터 코드, Apple DTD 또는 NLE 바이너리가 배포 라이브러리에 복사되지 않습니다. 기존 Qt 및 PNG 배포/라이선스 의무는 변함이 없습니다. `.iisc` 문서는 여전히 권위가 있으며, 교환 패키지는 편집 레이어와 노출 타이밍을 보존하면서 수신 NLE에서 네이티브 벡터 경로 편집을 주장하지 않습니다.

<a id="audio-timeline-and-pcm16-wav-090"></a>

## 오디오 타임라인 및 PCM16 WAV ( 0.9.0 )

새로운 연결된 런타임 의존성이 도입되지 않습니다. 기존 Qt 코어 파일과 XML 유틸리티는 한계가 설정된 I/O 와 이스케이프를 구현합니다. PCM16 컨테이너 프레임링은 Microsoft 의 [RIFF 사양](https://learn.microsoft.com/en-us/windows/win32/xaudio2/resource-interchange-file-format--riff-)을 따르는 작은 도메인 어댑터입니다. 그것은 압축된 오디오 디코딩, 리샘플링 또는 DSP 를 구현하지 않습니다. 일반적인 코덱 라이브러리 또는 다른 FFmpeg 프로세스 의존성은 바이트 정밀 PCM16 교환을 개선하지 않으면서 배포 및 유지 관리 비용을 추가합니다. 압축 형식은 명시적으로 지원되지 않습니다. 기존 선택적 FFmpeg 영화 코덱은 이 WAV 자산의 가져오기나 내보내기에 호출되지 않습니다. 알 수 없는 WAV 부속 조각은 검증된 범위 내에서 건너뛰고 누락된 메타데이터로 보고됩니다. OpenTimelineIO 는 애플리케이션의 네이티브 지속성 매핑이나 이중 XML 오디오 번역을 제거하지 않으므로 기존 의존성 결정이 그대로 유지됩니다.

<a id="iifileprovider-authorship-0100"></a>

## iiFileProvider 저자 ( 0.10.0 )

사용자가 요청한 의존성은 공개 의존성이며 버전 0.5.0이 필수이다. 이 라이브러리와 동일한 AGPL-3.0-only에 따라 활발히 유지보수되는 형제 SDK이며 기존 Qt 6 Core 런타임을 사용한다. 네트워크 활동, 인증 SDK, 스레드 또는 다른 저장 엔진 없이 작은 값/JSON 계약을 추가한다. 재사용은 파일 라이브러리마다 계정 검증과 토큰 필터링이 달라지는 것을 방지한다. 집합체 배치 변경은 SOVERSION 0.10 및 정확한 패키지 일치와 함께 제공되므로 소비자는 이 버전을 기준으로 재빌드해야 한다.

설치된 iiSharedCanvas 포함 디렉터리는 비시스템 디렉터리이므로, 선택된 스테이징 패키지가 이전 ABI의 전역 헤더보다 우선합니다.

<a id="storage-ownership-review"></a>

## 스토리지 소유권 검토

iiFileProvider 0.5는 이제 기존 퍼블릭 도메인 SQLite 종속성을 비공개로 소유하고 있습니다. 프로덕션 캔버스와 CLI 대상은 SQLite와 직접 연결되지 않으며, 원시 SQL 손상 테스트는 여전히 연결됩니다. 파일 API는 새로운 아카이브/코덱 의존성 없이 기존 Qt Core를 사용합니다. 방향: iiSharedCanvas -> iiFileProvider -> Qt Core/ SQLite . 제공자에는 캔버스 유형이 없습니다.

네이티브 캔버스 비디오/모션 (0.11.0)은 종속성을 추가하지 않으며, 합리적인 프레임 선택, 변환 보간 및 버전별 필드는 표준 C++23를 사용합니다. 네이티브 비디오 가져오기는 기존 옵션인 FFmpeg 프로세스 어댑터를 재사용합니다; 저장된 네이티브 프레임은 이를 사용하지 않고 렌더링한 뒤 다시 열 수 있습니다. 검토된 파일 제공자인 paint-engine, Qt, zlib 및 libzip 경계는 변경되지 않았습니다.

Bitmap processing adds no dependencies or runtime executables; its operations use C++23 and the existing raster and brush value types.
