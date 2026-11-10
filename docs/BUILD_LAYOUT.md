# 빌드 및 리소스 배치

Windows SDK의 `min`·`max` 매크로가 활성화된 소비자에서도 문서와 타임라인의
추가 위치 상수가 치환되지 않도록 보호한다. Qt 및 네이티브 테스트 빌드에 각각
해당 공개 헤더의 컴파일 전용 검사를 두었다. 실제 Windows 빌드는 수행하지 않았다.

기본 CMake 빌드와 컴파일된 라이브러리·실행 파일·테스트는 프로젝트의
`build/`에 둔다. `CMakePresets.json`과 로컬 IDE Debug 설정도 이 경로를 사용한다.
생성된 보고서나 수동 실험의 입력·출력은 빌드 트리에 저장하지 않는다.

테스트 입력과 일회성 작업 공간은 `resources/`의 예약된 `.iisacc-` 하위
디렉터리에 둔다. 네이티브 빌드의 `IISC_TEST_WORK_DIR` 계약과 수명은
[NATIVE.md](NATIVE.md)를 따른다. 다른 작업이 소유하거나 참조하는 디렉터리는
삭제하지 않는다. 이전 빌드에서 출처가 불명확한 문서·스크립트·데이터는
`resources/.recovery/`에 보존하고 Git에서 제외한다.
설치 패키지에는 공개 리소스만 포함하며 어느 깊이에서든 `.recovery`, `.state`,
`.iisacc-*` 디렉터리와 파일은 제외한다. 설치된 iiFileProvider Python 바인딩이
있는 환경에서 `python3 tests/ResourceInstallPolicyTest.py`를 실행하면 독립된
CMake 설치 구성으로 공개 리소스는 유지되고 이 작업 자료는 제외되는지 검증한다.
실제 SDK의 설치 결과도 `iiSharedCanvas.ResourcesInstall` 테스트가 확인한다.

Qt 없이 네이티브 API를 빌드할 때는 `IISHAREDCANVAS_BUILD_QT_API=OFF`를
사용한다. Qt API는 별도 구성이다. 빌드 또는 단위 테스트의 성공은 GUI 앱,
모델 추론, 다른 운영체제나 설치된 제품의 실행 검증을 뜻하지 않는다.
