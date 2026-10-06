<a id="native-canvas-timeline-package-command-line"></a>

# 네이티브 캔버스 타임라인 패키지 명령줄

설치된 `iisc-export-timeline` 유틸리티는 `exportTimelineInterchange` 를 통해 완전한 원본 캔버스 타임라인을 내보냅니다. 각 캔버스 레이어는 별도의 타임라인 트랙으로 유지되며, 원본 hold-key 구간은 클립으로 유지됩니다. 출력은 `timeline.xml` (전통적인 최종 컷 프로 XML ), `timeline.fcpxml` , `media/*.png` , `manifest.json` 및 편집 가능한 `source.iisc` 스냅샷이 포함된 새 디렉토리입니다. PNG 트랙은 교환 미디어로 렌더되며, 소스 스냅샷은 원래 래스터 /vector 자산과 원본 편집 정보를 유지합니다.

```sh
iisc-export-timeline drawing.iisc delivery-package
iisc-export-timeline "drawing with spaces.iisc" "delivery package" --name "Scene 01"
iisc-export-timeline --name "Scene 01" -- -input.iisc -delivery-package
iisc-export-timeline --help
```

`--name TEXT` 는 2 경로 전후에 한 번씩 또는 `--` 전에 나타날 수 있으며, 기본값은 `iisc Timeline` 입니다. 비어 있지 않은 값에는 Unicode, 공백 및 앞쪽 대시(-) 가 포함될 수 있습니다. `--` 는 옵션 파싱을 종료하므로 대시 경로가 사용 가능하게 유지됩니다. `--help` 와 `-h` 는 단독으로 허용됩니다. 알 수 없는 옵션, 중복 이름, 누락된 값 또는 경로, 추가 경로 및 URL 은 사용 오류입니다. 출력은 파일 이름 확장자가 필요하지 않습니다. 덮어쓰기 옵션은 없습니다: 기존 파일, 디렉터리 및 심볼릭 링크는 항상 보존되며, 목적지의 부모 디렉터리가 이미 존재해야 합니다. 도구는 누락된 부모 트리 생성을 수행하지 않습니다.

종료 상태는 성공적인 내보내기/도움말인 `0`, 입력/변환/I/O 오류인 `1`, 사용 오류인 `2` 입니다. 성공 시 표준 출력에 패키지 목적지를 보고합니다. 실패 및 변환 경고는 표준 오류에 기록됩니다. Qt 는 사용자 경로/옵션을 받지 않으며 명시적으로 구성되지 않은 경우 오프스크린 플랫폼을 사용합니다. 이 CLI 는 편집기 애플리케이션, 네트워크 액세스 또는 코덱 다운로드를 호출하지 않습니다.

<a id="shared-read-only-native-input"></a>

## 공유된 읽기 전용 네이티브 입력

PSD와 타임라인 내보내기는 동일한 사설 `tools/IiscInput` 로더를 사용합니다. 해당 계약은 네이티브 미디어 제한과 최대 레이어 수만을 적용하며, PSD나 타임라인 형식에 대한 의존성이 없습니다. 입력 형식은 파일 이름이 아니라 콘텐츠에서 감지됩니다. 바이너리 `.iisc` 스냅샷은 한계가 설정된 읽기와 `decodeIisc`를 사용합니다.

원본 SQLite 작업 파일은 저작자 `DocumentFile::open` API에게 전달되지 않습니다. 대신, `SQLITE_OPEN_READONLY` 소스 연결과 SQLite의 기존 온라인 백업 API는 커밋된 체크포인트가 없는 WAL 데이터를 포함한 일관된 스냅샷을 생성합니다. URI 또는 불변 모드가 사용되지 않습니다. 해당 개인 백업은 정규 스키마, 다이제스트 및 문서 검증을 위해 `DocumentFile`로만 열립니다. 실시간 데이터베이스의 원시 파일 시스템 복사본은 사용되지 않습니다.

백업은 의도된 출력 옆에 있는 개인 `.iisc-input-XXXXXX` 디렉터리에 저장되어 있으며, 성공 또는 실패 시 제거됩니다. 원본 데이터베이스와 WAL 페이로드는 재작성되지 않으며, 저널 모드는 변함이 없고, 체크포인트가 강제되지 않습니다. SQLite는 일반 공유 메모리 리더 회계를 업데이트할 수 있습니다. 읽을 수 없는 스냅샷은 명시적으로 실패합니다: 바쁜 타임아웃은 250 ms이며, 복사는 한계가 설정된 페이지 배치를 사용하고, 전체 복사 마감 시간는 30초입니다.

입력 파일과 정규 파일 WAL /journal/shared-memory 사이드카는 입력 바이트 예산을 공유합니다. 사이드카 심볼릭 링크는 거부됩니다. `page_count * page_size`와 백업의 결과 크기는 입력 및 디코딩 바이트 제한 모두에 부합해야 합니다. 네이티브 디코딩은 캔버스 픽셀, 총 래스터 저장 용량 및 레이어 카운트 경계도 적용됩니다. 공개 패키지 작성자는 새 디렉터리를 게시하기 전에 전체 내보내기를 검증하고, 실패 시 개인 스테이징 디렉터리를 정리합니다.

<a id="verification"></a>

## 검증

`TimelineInterchangeCliTest` 는 실제 실행 파일을 실행하고 XML 문서, 디코딩 가능한 PNG 미디어, 매니페스트 지속 시간 및 정확한 정통 네이티브 스냅샷 보존을 확인합니다. 애니메이션 비트맵 및 벡터 레이어, Unicode/공백/대시 경로를 포함한 순서, 이름이 지정된 시퀀스, 헤드리스 사용, 읽기 전용 작업 파일, 커밋된 WAL 데이터 및 활성 WAL 작성기, SHA-256 소스 보존, 목적지 충돌 및 별칭, 누락된 부모, 잘못된 형식의 네이티브 입력, 지원되지 않는 프레임 속도, 유효하지 않은 XML 메타데이터 및 임시 디렉터리 정리를 포함합니다. 순서 이름 확인은 레거시 `sequence/name`, FCPXML `project@name` 및 매니페스트 필드를 대상으로 합니다. 그들은 macOS `QProcess` 분해 Unicode 인수를 CLI 이 그들을 받기 전에 처리합니다; CLI 는 받은 이름을 유지합니다. `PsdExportCliTest` 는 공유 로더의 기존 프레임 회귀 워크플로우, 오버라이트 및 소스 사이드카 보호를 포함하여0 PSD 회귀 시트를 유지합니다.

공유 로더는 종속성을 추가하지 않습니다: 두 도구 모두 이미 검토된 비공개 SQLite 종속성과 공개 iiSharedCanvas API를 재사용합니다. 보다
[SQLite의 온라인 백업 API](https://sqlite.org/backup.html) 및 패키지 형식의 교환 제한에 대한 라이브러리 문서.

지속된 `.iisc` 1.4 오디오 트랙은 동일한 명령으로 자동으로 내보내집니다. 결과 패키지에는 WAV 소스와 PNG 미디어가 포함되어 있습니다. Premiere Pro/Resolve에서 `timeline.xml`를 가져오거나 Final Cut Pro에서 `timeline.fcpxml`를 가져옵니다. 오디오 가져오기/첨부는 `importAudioWav` 및 `DocumentEditor::insertAudioAsset` / `insertAudioTrack`를 통해 사용할 수 있으며, CLI는 저장된 전체 문서를 소비하고 입력을 변경하지 않습니다.
