# 네이티브 작업 파일의 타일 저장

iiSharedCanvas Native 0.35.2는 사설 SQLite 작업 파일 형식 2를 사용한다. 정규 교환·스냅샷 `.iisc` 형식은 1.22를 유지한다. Qt API의 기존 작업 파일 형식은 1이며, 새 네이티브 경로와 별도로 유지된다. 하위 SQLite 실행은 iiFileProvider Native 0.11.0의 공개 API가 소유한다.

## 저장 계약

`canvas_records`에는 기존 문서 헤더·레이어·저작 정보와 자산별 필드가 남는다. 편집 가능한 `RasterAsset`와 `ChunkedRasterAsset`의 레코드는 식별자·크기·청크 좌표 등의 기술자만 포함한다. 각 래스터 기술자는 기존 raw ARGB32 크기와 표본 개수를 기록하지만 픽셀 본문을 포함하지 않는다. 비디오 프레임과 비트맵 이외의 자산은 기존 인라인 표현을 유지한다. 별도 타일 판독 콜백은 기술자의 크기·개수·자원 한도를 확인한 뒤 실제 래스터를 구성하며, 최종 문서 전체를 정상 검증한다. 작은 가짜 래스터로 검증을 우회하지 않는다.

픽셀은 `canvas_tiles(asset,column,row,data,digest)`에 little endian ARGB32로 저장한다. 기본 타일은 64×64이며 32px 청크만 32×32 타일을 사용한다. 좌표는 타일 단위의 문서 좌표이며, 음수 청크도 동일한 정렬을 사용한다. 유한 래스터의 가장자리 패딩은 정확히 0이어야 한다. 모든 ARGB 비트가 0인 타일만 생략한다. alpha가 0이어도 RGB가 있으면 저장하며, 픽셀이 모두 0인 청크의 존재는 자산 기술자에 남긴다.

각 타일의 digest는 다음 SHA-256 계약이다. 모든 정수는 little endian uint64이며 음수 좌표는 해당 int64의 비트 패턴을 사용한다.

```text
payloadHash = SHA256(rawTileBytes)
digest = SHA256(
    ['I','I','C','T','I','L','E',0x02]
    || byteLength(assetId) || utf8(assetId)
    || column || row || byteLength(rawTileBytes) || payloadHash
)
```

정상 payload·digest 쌍을 다른 위치나 자산으로 옮겨도 검사를 통과하지 않는다. `canvas_tile_state`의 타일 수와 실제·참조된 타일 수를 대조하여 타일 누락도 거부한다. 미참조 타일, 잘못된 크기·패딩·체크섬, 명시적으로 저장한 0색 타일과 알 수 없는 스키마/버전은 거부한다. 큰 BLOB과 식별자는 소유 버퍼로 읽기 전에 SQL 길이를 검사한다. 최대 타일 읽기는 16 KiB이며, 전체 래스터 할당은 열기·스냅샷 구성에 필요한 검증된 크기에서만 발생한다.

범위 교체는 외부 SQL 트랜잭션 안에서 iiFileProvider의 `compare_exchange_blob_ranges`를 사용한다. 같은 크기의 타일은 달라진 바이트만 쓴다. 새 타일·크기 변경·삭제, 기술자·체크섬·타일 수와 저장 리비전은 같은 동기 트랜잭션에 속한다. 쓰기 통계의 `recordsWritten`에는 변경한 메타데이터 및 타일 행이 포함된다. `payloadBytesWritten`은 논리 본문 바이트이며 SQLite 페이지·저널·인덱스·체크섬·상태 행을 포함하지 않는다.

0.35.11의 문서 공통 이력 타임라인 변경은 현재 형식의 헤더와 저작 정보만 비교·
갱신하고 파일 리비전과 함께 원자적으로 확정한다. 이 경로는 기존 래스터·오디오
본문을 복사하지 않지만 문서 구조의 검증과 레코드 순회는 수행한다. 변경이 없는
요청도 외부 저장 충돌을 확인한다. 구형 형식은 기존 전체 문서 마이그레이션 경로를
사용한다. 결합 레이어·자산 삭제와 이력 한도는 [문서 공통 이력](DOCUMENT_HISTORY.md)을
참조한다.

0.35.12의 청크형 무한 캔버스 확장 획은 변경된 픽셀 타일, 청크 존재 기술자,
확장 또는 복원된 헤더, 저작 정보와 저장 리비전을 하나의 트랜잭션으로 확정한다.
파일 쓰기나 이력 예산이 실패하면 이전 영역과 픽셀을 유지한다. 이전 저장 형식의
첫 실제 획은 같은 트랜잭션에서 타일 형식으로 이전한다. 다른 대형 래스터의 픽셀
복제는 피하지만 영역 검증·청크 기술자·동기화 비용은 남는다.

## 단일 자산·레이어 이력의 타일 쓰기

현재 저장 형식과 현재 문서 모델의 단일 자산·레이어 삽입 또는 삭제는
해당 자산의 타일만 조사하고 갱신한다. 다른 자산의 타일을 전체 순회하여
페이로드 크기나 타일 수를 다시 계산하지 않는다. 메타데이터 레코드는 계속
검증·순회한다. 구형 형식의 전환과 일반 문서 편집은 기존 경로를 유지한다.

파일을 열 때 확정한 페이로드 크기에 변경된 메타데이터와 해당 자산 타일의
바이트 차이를 적용하고, 타일 수·저장 리비전과 함께 같은 트랜잭션에서
확정한다. 한도 초과, 잘못된 타일 크기, 개수 범위 오류 또는 외부 저장 충돌은
커밋 전에 거부하고 트랜잭션을 되돌린다. 커밋 결과가 불확실한 저장 오류는
기존 계약대로 파일 바인딩을 해제하고 재열기를 요구한다.

macOS의 `NativeAssetTileScanTest`는 SQLite 전체 스캔 계측의 양성 대조군,
다른 자산의 1,024개 타일이 있는 문서의 삭제, Undo/Redo 및 재열기와
픽셀 보존을 확인한다. 스캔 단계 수는 실행 시간, 디스크 I/O 또는 전체
문서 작업의 성능을 대신하지 않는다.

## 기존 파일과 API

`open`, `readSnapshot`, `readRevision`, `readStorageInfo`는 작업 형식 1과 2를 읽는다. 형식 1을 열거나 무변경 편집하는 것만으로 형식을 올리지 않는다. 첫 실제 변경은 같은 트랜잭션 안에서 형식 2로 옮긴다. 편집 거부·실패는 이전 형식과 문서·리비전을 보존한다. 이전 라이브러리는 알 수 없는 작업 형식 2를 거부하며 원본을 임의 해석하지 않는다.

```cpp
DocumentFileInfo info;
auto inspected = DocumentFile::readStorageInfo(path, info);
// 스키마와 SQLite 무결성을 읽기 전용 트랜잭션에서 확인한다.
// 문서 본문 검증은 readSnapshot 또는 open의 역할이다.

DocumentFile file;
auto opened = file.open(path);
auto upgraded = opened.ok() ? file.upgradeStorage() : opened;
```

`readStorageInfo`는 같은 트랜잭션의 저장 버전과 리비전을 반환하며 실패 시 출력 객체를 보존한다. `storageVersion()`은 현재 바인딩의 버전이고 닫힌 객체는 0이다. `upgradeStorage()`는 명시적으로 사설 저장 형식만 올린다. 픽셀·정규 문서 버전·저작 정보와 바인딩 세대는 바꾸지 않으며, 저장 리비전은 한 번 올린다. 이미 현재 형식이면 무변경이다. 외부 변경·이동·중첩 업그레이드·자원 한도 및 불확실한 커밋 결과는 기존 파일 편집과 같은 거부/연결 해제 경계를 사용한다. 첫 마이그레이션은 전체 자산을 읽어 타일을 구성하는 일회성 작업이다.

Native `readSnapshot`과 CLI의 완전 검증은 작업 파일을 읽기 전용 임시 복사본으로 검사한다. 운영체제가 제공한 임시 디렉터리의 상위 경로가 링크인 경우(macOS의 `/var` 등), 선택된 부모를 정규 경로로 해석한 뒤 FileProvider가 생성·잠금·정리한다. 사용할 수 없는 부모는 오류로 반환하며 출력 문서를 바꾸지 않는다. 링크 추적을 허용하는 범위는 호출자가 선택한 임시 부모이고, 실제 백업 파일의 접근과 잠금 검증은 FileProvider의 기존 계약을 따른다.

0.35.11부터 SQLite 백업 복사의 기본 시간 제한은 `DefaultNativeSnapshotTimeoutMs`
(300,000ms)이다. Native 호출자는 `DocumentFile::readSnapshot(path, result, limits,
timeoutMs)`로 양의 밀리초 값을 지정할 수 있다. 0과 음수는 `InvalidArgument`를
반환하며 기존 출력을 보존한다. 백업의 잠금/시간 초과는 `Conflict`, 저장 예산
초과는 `LimitExceeded`로 전달한다. 이 제한은 백업 복사에만 적용되며 파일 열기,
전체 디코딩과 검증까지 포함하는 전체 실행 시간 제한이나 취소 기능은 아니다.
제한은 SQLite 백업 단계 사이에서 확인하므로 진행 중인 단일 파일 I/O가 지연되면
해당 시스템 호출을 중단하거나 엄격한 벽시계 시간 상한을 보장하지 않는다.
기존 3인자 API와 Native 내부의 읽기 경로도 같은 기본 제한을 사용한다.

0.35.2는 검증이 끝난 임시 소유자의 문서를 분리 결과로 옮기고 소유자를 닫는다. 이때 래스터 전체를 두 번째로 복제하지 않는다. 픽셀을 한 번 메모리에 구성하는 비용, 임시 SQLite 복사본, 호출자가 이미 보유한 출력과 SQLite 내부 할당은 여전히 필요하다. 검증이나 메모리 할당 실패 시 호출자의 이전 출력은 보존한다. 원본 파일이나 WAL을 수정하지 않으며 정규 IISC 입력 경로의 동작도 바꾸지 않는다.

## 에이전트용 CLI

네이티브 빌드는 기본적으로 `iisc-working`을 함께 설치한다. 브라우저·Qt·QML이나 Python 런타임을 요구하지 않는다. Windows의 wide 인자는 표준 파일시스템 변환으로 UTF-8 API에 전달한다.

```sh
iisc-working inspect artwork.iisc
iisc-working verify artwork.iisc --maximum-bytes 1073741824 --snapshot-timeout-ms 600000
iisc-working upgrade-storage artwork.iisc --dry-run
iisc-working upgrade-storage artwork.iisc
```

`inspect`는 스키마/저장 메타데이터만 확인하여 `verified:false`를 반환한다. `verify`는 분리된 읽기 전용 스냅샷을 완전히 검증한다. `--dry-run`은 완전한 검증 후 형식 전환 여부만 보고한다. 이 세 경로는 원본 파일 바이트를 보존한다. 검증 전후 버전·리비전이 달라지면 충돌을 반환한다. 실제 `upgrade-storage`만 쓰기 가능한 소유자를 열어 업그레이드를 요청한다. 이 명령의 `changed`는 저장 형식 변경이며 저널 설정 변경이나 일반 파일 메타데이터의 변경 여부가 아니다.

각 실행은 `schemaVersion:1`인 JSON 객체 하나를 출력한다. `ok`, `command`, `path`, `storageVersion`, `revision`, `verified`, `dryRun`, `wouldChange`, `changed`, `code`, `message`를 제공한다. 종료 코드는 성공 0, 파일/검증 실패 1, 잘못된 명령·인자 2이다. `--help`만 사람용 사용법을 출력한다. `maximum-bytes`는 완전한 문서 읽기·업그레이드 한도이며 메타데이터 전용 `inspect`의 SQLite 무결성 검사 예산은 아니다.

`--snapshot-timeout-ms`는 `verify` 또는 `upgrade-storage --dry-run`에서만 쓰며
양의 `int` 범위 값이 필요하다. 이는 위 SQLite 백업 복사 제한을 선택할 뿐, 나머지
검증 단계 전체의 소요 시간을 제한하지 않는다.

## 검증과 남은 전환

독립적인 Python hashlib/struct.pack 기대값으로 타일 digest를 대조하며, 저장 예산을 넘긴 명시/자동 마이그레이션은 스키마·원본 파일 바이트·픽셀·리비전 보존을 검사한다. `WorkingFileTilesTest`는 기존 0.34.1 공개 API가 생성하여 `resources/fixtures/legacy-working-v1.tsv`에 보존한 고정 형식 1 레코드, 유한/음수 청크·32px 청크, 숨은 RGB·빈 청크, 비디오, 작은 쓰기량, 읽기 전용 스냅샷, 명시/자동 마이그레이션·무변경/거부·외부 충돌, 타일 누락·이동·미참조·잘못된 크기·체크섬·수·미지원 버전을 검사한다. `NativeWorkingFileCli.cmake`는 JSON, 원본 해시 보존, 예산 실패, dry-run, 실제 업그레이드, 재실행과 손상 파일 거부를 검사한다. 같은 검사를 설치 패키지의 독립 소비자에서도 실행한다.

`WorkingSnapshotAllocationTest`는 세 래스터와 마스크 참조를 가진 작업 파일을 독립적으로 읽고, 숨은 RGB·마스크·원본 바이트·실패 시 기존 출력과 실제 WAL의 최신 커밋을 검사한다. 일반 빌드에서는 읽기 호출 스레드의 C++ `operator new` 누적량에 한 개 문서 크기와 부대 비용에 대한 회귀 한도를 적용하고 세 할당 지점의 실패를 주입한다. 이 계측은 SQLite의 C 할당, 다른 스레드, 파일 공간과 프로세스 RSS를 포함하지 않는다. AddressSanitizer 빌드는 할당 가로채기 방식이 달라 비용·실패 주입 검사를 생략하고, 읽기와 WAL의 안전성 및 예산 실패 시 출력 보존을 검사한다.

이 형식 전환은 D28 전체 완료가 아니다. Native 0.35.1의 파일 바인딩 비트맵 편집은 같은 크기의 영역 변경에 대해 실제로 닿은 타일만 확인하고 갱신한다. 트랜잭션과 측정 경계는 [파일 영역 저장](BITMAP_FILE_REGIONS.md)에 설명한다. 일반 `DocumentFile::edit`, 크기 변경, 이전 문서 모델의 전환은 전체 임시 문서를 사용한다. 새 청크의 실 메모리 할당, 청크 존재 기술자의 전체 갱신, 앱 통합·일반 구조 명령·최종 대형 워크플로우와 다른 플랫폼의 실행 검증도 별도 범위이다.

회귀 자료는 원본 `resources/`에 보관하고 Native 설치본에도 함께 공급한다. 설치 소비자는 `iiSharedCanvasNative_RESOURCE_DIR`에서 자료를 찾는다. Qt 없는 Native 소스와 설치 소비자 검사의 임시 자료는 검증된 별도 `resources/` 작업 공간에 둔다. 링크가 있는 임시 부모도 그 작업 공간 안의 독립 자료로 재현한다.
작업 파일 CLI 회귀는 매 실행마다 고유한 하위 디렉터리를 만들고 그 안의 파일만
정리한다. 기존 테스트 출력 디렉터리에 같은 이름의 사용자 파일이 있어도 삭제하지 않는다.
