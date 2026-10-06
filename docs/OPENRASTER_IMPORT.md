<a id="openraster-layered-import"></a>

# OpenRaster 계층형 가져오기

`decodeLayeredDocument` 와 `importLayeredDocument` 는 ZIP 콘텐츠 및 `image/openraster` mimetype 으로 OpenRaster ( `.ora` ) 를 인식합니다. 가져오기는 분리된, 검증된 `Document` 를 반환하며, 아카이브 구성원 추출이나 외부 리소스 읽기, 프로세스 호출, 또는 작업 문서 작성을 수행하지 않습니다.

<a id="editable-mapping"></a>

## 편집 가능한 매핑

지원되는 프로파일은 OpenRaster 버전 0.0.1부터 0.0.6까지이며, PNG 래스터 레이어를 포함합니다. 각 소스는 여러 레이어가 하나의 PNG를 참조하더라도 독립적인 `RasterAsset`와 `StaticBitmapLayer`가 됩니다. 이름, 가시성, 불투명도, 부호 있는 정수 오프셋 및 네이티브 PNG 픽셀이 유지됩니다. 레이어 순서가 OpenRaster 상단에서 iiSharedCanvas 하단으로 반전됩니다. ID는 결정론적입니다: `<idPrefix>-asset-0` / `<idPrefix>-layer-0`는 하단 레이어를 식별합니다.

지원되는 합성 작업 매핑은 정확히:

| OpenRaster | iiSharedCanvas |
| --- | --- |
| `svg:src-over` | `SourceOver` |
| `svg:multiply` | `Multiply` |
| `svg:screen` | `Screen` |
| `svg:overlay` | `Overlay` |

기타 블렌드, 마스크, 필터, 텍스트, 벡터 레이어 소스, 알 수 없는 그리기 요소/속성, 그리고 고립되었거나 중립적이지 않은 그룹 안전하게 거부한다. 수입자는 요청된 편집 가능한 레이어에 대해 `mergedimage.png`를 절대 대체하지 않습니다. `svg:dst-out`도 거부되었습니다: `DestinationOut`는 비트맵 브러시 블렌드 열거형이지만, 네이티브 캔버스 문서는 의도적으로 레이어 블렌드로 허용하지 않습니다.

비 루트 그룹은 `isolation="auto"` 를 명시적으로 선언하고 가시적이며 완전히 불투명하며 `svg:src-over` (또는 기본값) 를 사용할 때만 평평하게 만들 수 있습니다. 이 안전한 패스-through 변환은 계층 구조/이름 손실 경고를 방출합니다. 그룹은 기본적으로 OpenRaster 에서 격리되므로 `isolation` 의 누락은 패스-through 로 취급되지 않습니다. 전체 그룹 `x` / `y` 속성은 0.0.6 버전 이전의 버전에서 무시되며 경고가 발생하며, 규격은 다음과 같이 요구합니다: 자식 오프셋에 절대 추가하지 않습니다. 0.0.6 버전 그룹 좌표는 거부됩니다.

루트 스택은 항상 사양의 고정된 격리 렌더링을 가지고 있습니다. 현재 사양에서는 작성자에게 해당 속성을 생략하도록 지시하고 있지만, 가져오기는 기존 작성자로부터 중복된 루트 `name`, `opacity`, `visibility`, `composite-op` 및 `isolation` 속성을 받아들이고 이를 무시하고, 이러한 호환성 처리를 경고로 보고합니다. 알 수 없는 루트 속성은 여전히 안전하게 거부한다입니다.

이미지 제목, 물리적 인쇄 해상도 및 레이어 PNG 텍스트 메타데이터에는 네이티브 문서 필드가 없으며, 존재할 경우 경고가 표시됩니다. 레이어 이름은 유지됩니다. 병합된 미리보기나 썸네일이 누락되면 경고가 표시되며, 두 경우 모두 편집 가능한 변환에 사용되지 않습니다. 네이티브 PNG 색상/심도 변환 경고가 기존 한계가 설정된 비트맵 디코더에서 전파됩니다.

<a id="untrusted-input-contract"></a>

## 신뢰할 수 없는 입력 계약

검토된 libzip 는 ZIP 파싱, 엄격한 일관성 검사, 압축 해제, 그리고 CRC 검증을 수행합니다. 한계가 설정된 클래식 ZIP 종료 레코드/디렉토리/로컬 이름 사전 검사 는 libzip 1.11.4 가 이를 공백으로 정규화하기 전에 내장된 NUL 을 추가로 거부합니다. 그는 의존성이 디렉토리를 할당하기 전에 원시 UTF-8 이름, 로컬/중앙 바이트 정확 일치, 디렉토리 범위 및 엔트리 예산을 확인합니다. ZIP64 및 분할/멀티 디스크 아카이브는 명시적으로 지원되지 않으며, 추가 필드 또는 압축 페이로드 파서가 재구현되지 않습니다. 아카이브 이름의 유효한 공백은 계속 지원됩니다.

레이어 스택이 참조하지 않는 항목을 포함하여 모든 항목을 EOF까지 읽어 체크섬을 검증한다. `STORED`와 `DEFLATED` 멤버만 허용한다. 첫 번째 물리적 로컬 파일과 첫 번째 인덱스 항목은 모두 압축되지 않은 `mimetype`여야 한다. 고정된 외피 검사를 통해 중앙 디렉터리의 순서를 바꾸어 첫 파일 요구사항을 우회하는 것을 막는다. 암호화된 항목, 중복 이름, 잘못된 UTF-8 이름, 안전하지 않은 상대 경로, 심볼릭 링크, 특수 파일, 잘못된 형식의 XML, DTD, 엔터티, 처리 지시문, 알 수 없는 버전, 누락된 이미지 참조 및 지원하지 않는 렌더링 의미는 부분 문서를 반환하기 전에 거부한다.

`maxInputBytes` 는 아카이브를 제한하며, `maxArchiveEntries` 는 그 디렉토리를 제한하며, `maxXmlDepth` 는 XML 중첩을 제한하며, `maxLayers` 는 편집 가능한 레이어를 제한하며, `maxPixelsPerFrame` 는 캔버스와 개별 PNG 영역을 모두 제한합니다. `maxDecodedBytes` 는 선언된 확장 아카이브 바이트의 합계와 누적 유지 래스터 픽셀을 보수적으로 제한하며, 반복 PNG 참조를 위한 독립적으로 복사된 자산도 포함됩니다. PNG 무결성과 크기는 `decodeBitmap` 를 통해 확인되며 확장된 코덱은 비활성화됩니다. 아카이브 경로는 파일 시스템에 대해 해결되지 않습니다. 실패는 항상 빈 문서를 반환합니다.

<a id="verification"></a>

## 검증

`OpenRasterImportTest`는 검토된 libzip 라이터와 공개 네이티브 비트맵 코덱을 사용하여 메모리에 아카이브를 생성합니다. 정확한 금색 렌더링 픽셀, 네이티브 `.iisc` 인코딩/디코딩 보존, 이름/순서/오프셋/불투명도/가시성/블렌드, 명시적인 패스스루 그룹 처리, 그리고 손상되었거나 지원되지 않는 아카이브와 자원 제한 위반에 대한 거부를 검증합니다.

사양 참조:

- [OpenRaster 레이어 스택 사양](https://www.openraster.org/baseline/layer-stack-spec.html)
- [OpenRaster 파일 레이아웃 사양](https://www.openraster.org/baseline/file-layout-spec.html)
- [libzip 문서](https://libzip.org/documentation/)

종속성 유지보수, 라이선스 및 발자국 검토는 `DEPENDENCIES.md`에 기록됩니다. 이 리더는 한계가 설정된 서브셋을 명시적으로 구현하며, 전체 OpenRaster 베이스라인 편집이나 임의의 레이어드 포맷 호환성을 구현하지 않습니다.
