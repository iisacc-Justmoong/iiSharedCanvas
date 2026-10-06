<a id="ip-adapter-image-embedding-layers"></a>

# IP -어드앱터 이미지 임베딩 레이어

패키지 0.23.0, 네이티브 모델 1.17. 새로운 종속성이 없습니다. 이 모듈은 이미 계산된 이미지 임베딩을 저장하고 해당 이미지의 타임라인 상태를 선택합니다. CLIP, 로드 가중치, 프로젝트 임베딩, 어텐션 프로세서 또는 확산을 수행하지 않습니다.

<a id="data-and-identity"></a>

## 데이터와 신원

- `IpAdapterAsset`는 ID, 디스크립터, 조건 텐서 및 선택적 명시적 무조건 텐서를 보유하고 있습니다. 하나의 자산은 하나의 이미지 상태이며, batch= 1. 다수의 독립적인 참조는 여러 층으로 표현될 수 있으며, 하위 소비 측 추론은 융합을 소유합니다.
- `IpAdapterTensor`는 행‐대소수점이며32 `[tokenCount, channelCount]`입니다. 양의 차원은 정확히 개수와 일치합니다. 음수 값과 1보다 큰 크기는 유효합니다; NaN /Inf는 유효하지 않습니다. Equality는 바이너리32 비트를 비교하고, 서명된0 편집을 보존합니다. 양자화, 정규화 또는 숨겨진 스케일링이 발생하지 않습니다.
- `IpAdapterEmbeddingStage::EncoderPooled`는 단일 토큰 인코더 벡터를 식별합니다; `EncoderHiddenStates`는 토큰 시퀀스를 식별합니다; `ProjectedTokens`는 선택된 어댑터의 투사에 의해 이미 변환된 이미지 프롬프트 토큰을 식별합니다. 아키텍처 전반에 걸쳐 보편적인 토큰/채널 차원이 하드코딩되지 않습니다.
- 설명자는 인코더 id/수정본, 어댑터 id/수정본, 기본 모델 호환성 id, 및 전처리 id 를 요구합니다. 수정본에는 불변 커밋 또는 가중치 해시를 사용해야 하며, 전처리 id 는 확대/축소/정규화 및 적용 가능한 경우 선택된 숨겨진 상태 레이어를 식별해야 합니다. 이들은 프로듀서 선언이며 이 라이브러리가 모델 가중치를 검사하거나 인증했다는 주장이 아닙니다.
- 레이어의 `control.modelId/modelRevision`는 어댑터를 식별하고 자산과 일치해야 합니다. 동적 상태는 전체 디스크립터와 형태를 공유하여 아무런 알림 없이의 프레임 변경이 임베딩 공간을 변경하는 것을 방지합니다. 소비자는 또한 설명자를 실제 인코더, 프로젝션 및 확산 모델과 일치시해야 합니다.

공식 IP -Adapter는 기본 구현에서 풀링된 이미지 임베딩을 사용하고 Plus에서는 숨겨진 상태를 사용합니다. 두 경우 모두 주의 조건화 전에 투사됩니다. 그들의 무조건 경로는 다릅니다: 기본은 투사 전 0이며, 플러스는 0 픽셀에서 인코더 출력됩니다. 따라서 이 모듈은 무조건적인 분기를 발명하거나 이를 0 투사 토큰과 동일시하지 않습니다. 보십시오
[공식 구현](https://github.com/tencent-ailab/IP-Adapter/blob/main/ip_adapter/ip_adapter.py).

<a id="layer-and-export-semantics"></a>

## 계층 및 내보내기 의미론

`IpAdapterLayer` 은 공유 조건부 계층 구조 ( `LayerRole::ControlNet` ,  `ControlNetKind::IpAdapter` ) 에 속합니다. 이 조직적 역할은 IP - 어댑터가 ControlNet 신경망이라는 것을 의미하지 않습니다. 이미지 기반 주의력 조건부 방식을 사용합니다.

정적 소스는 하나의 임베딩 상태를 유지합니다. 키프레임이 적용된 소스는 0프레임에서 유지 방식만 사용하는 선택을 사용합니다. `LayerRepresentation::Embedding`은 비공간 텐서를 나타내며 `layerKind`는 `nullopt`를 반환한다. 네 가지 비트맵/벡터 아트워크 종류에 임베딩 종류를 추가하지 않는다. 일반 아트워크 공장은 임베딩을 거부합니다; `insertIpAdapterLayer`를 사용하십시오.

`exportIpAdapterEmbeddings`는 선택한 프레임에 대해 소유권을 가진 정확한 텐서, 기술자와 제어 설정을 반환한다. enabled, 타임라인과 레이어 프레임 범위를 따른다. 표시 가시성, 불투명도와 변환은 추론 텐서를 바꾸지 않는다. 스케일과 guidance 일정은 추론 소비자가 적용할 수 있도록 메타데이터로 반환한다. 기본적으로 내보내기에는 CFG를 위한 무조건 분기가 필요하다. 소비 경로가 이를 요구하지 않는 경우에만 `requireUnconditional`를 false로 설정한다. 기존 분기는 여전히 내보낸다. 누락된 무조건 데이터를 임의로 만들어 내지 않는다. `maximumValues`는 할당 전에 양쪽 분기에서 복사하는 스칼라의 총수를 제한한다.

임베딩 레이어에 공간 이미지 미리보기가 없습니다: `renderFrameLayerTiles`는 `spatial=false`와 빈 타일 목록으로 성공합니다. 배치 렌더링은 메타데이터를 보존합니다; 아트워크 구성 및 벡터/ PDF 출력은 컨디셔닝 레이어를 생략합니다. PSD와 타임라인이 안전하게 거부한다를 교환하여 임베딩 데이터를 잃지 않습니다. 비트맵 편집은 임베딩 자산을 바인딩할 수 없습니다. 소스 이미지가 아무런 알림 없이를 보존하거나 텐서에서 재구성하지 않았습니다.

<a id="editing-example"></a>

## 편집 예시

```cpp
IpAdapterAsset asset;
asset.id = "reference-embedding";
asset.descriptor = {IpAdapterEmbeddingStage::EncoderPooled,
    "encoder-id", "encoder-weight-sha", "adapter-id", "adapter-weight-sha",
    "diffusion-compatibility-id", "preprocessing-contract-v1"};
asset.conditional = {1, 3, {-0.5F, 0.25F, 1.5F}}; // 설명을 위한 형태이며 모델 크기가 아니다.
asset.unconditional = IpAdapterTensor{1, 3, {0.0F, 0.0F, 0.0F}};
// 모델과 선언된 단계에 대해 생산자가 실제로 계산한 값을 제공한다.
DocumentEditor editor(document);
auto inserted = editor.insertIpAdapterAsset(std::move(asset));
IpAdapterLayer layer;
layer.properties = {"reference-layer", "Image embedding"};
layer.source = StaticSource{"reference-embedding"};
layer.control.modelId = "adapter-id";
layer.control.modelRevision = "adapter-weight-sha";
auto linked = editor.insertIpAdapterLayer(std::move(layer));
auto tensors = exportIpAdapterEmbeddings(document, "reference-layer", 0);
```

사용하기 전에 모든 편집/결과를 확인하십시오. `setIpAdapterValue`는 브랜치, 토큰 및 채널을 색인합니다. 자산 교체는 형태/소산/분지를 원자적으로 편집합니다; `setControlNetSettings`는 레이어 바인딩 및 스케줄을 편집합니다. 거부된 편집은 상태, 버전 및 리비전을 그대로 유지합니다. 파일에 바인드된 편집은 동기식으로 커밋됩니다.

<a id="persistence-and-bounds"></a>

## 지속성과 경계

Native  1.17  는 자산 태그  14, 콘텐츠/역할 태그  12, 리틀엔디안 바이너리32 와 명시적인 모양/존재 필드를 사용합니다. 전역 값 예산은 모든 자산에 걸쳐 두 가지 가지 모두를 합산하며, 모든 설명자 문자열은 문자열 예산에 포함됩니다. 할당 전에 디코드 검사가 모양, 예산, 남은 바이트 및 벡터 용량을 확인한 다음 유한한 값과 모델 바인딩을 유효성 검사합니다. 알 수 없는 열거형 값과 이전 버전 헤더  안전하게 거부한다 . 모델  런타임 없이 저장된 텐서와 동적 상태  왕복 변환 . 설정만 있는 커밋은 변경되지 않은 자산 기록을 재사용합니다. [FORMAT .md](FORMAT.md)를 참조하세요.

자세한 레이어 및 개체 필드는 다음을 통해 원자적으로 편집할 수도 있습니다.
모든 동적 소스 상태를 포함하는 [공통 매개변수 API](CONTROLNET_PARAMETERS.md).
