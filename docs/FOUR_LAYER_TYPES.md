# Four concrete layer/content types (0.28.0)

The artwork model has exactly four concrete C++ alternatives. Each owns its own
content type; there is no mutable static/dynamic source variant inside them.

| Layer | Content | Allowed payload | Frame sampling |
| --- | --- | --- | --- |
| `StaticBitmapLayer` | `StaticBitmapContent` | `RasterAsset` or `ChunkedRasterAsset` | One asset throughout the active range |
| `StaticVectorLayer` | `StaticVectorContent` | `VectorAsset` with native paths/paint | One asset throughout the active range |
| `DynamicBitmapLayer` | `DynamicBitmapContent` | Frame-selected raster or chunked raster assets | Each content key selects pixels |
| `DynamicVectorLayer` | `DynamicVectorContent` | Frame-selected native vector assets | Each content key selects paths/paint |

`content.assetId` belongs to either static content type. `content.frameIndices`
belongs to either dynamic content type. The common source bases only share
reference/index storage. The four content types cannot be substituted for one
another. The document owns full asset payloads and `Document::frames` owns exact
`{layerId, assetId}` keys; content does not duplicate those payloads or keys.
Validation checks every referenced asset against the concrete layer type and
checks the complete increasing frame index against the frame-owned keys.

Static means the same content across the timeline, rather than immutable pixels.
Motion and an existence range are independent of content identity. Dynamic keys
use hold sampling, begin at frame zero, and may be present on every frame. A
one-key dynamic layer retains its dynamic type. `setDynamicFrameContent` commits a
fresh raster/vector payload and the exact-frame key atomically. Editing one
independent asset does not modify other frame assets.

```cpp
Document document;
document.extent = {640, 480};
document.timeline.frameCount = 24;
DocumentEditor editor(document);
editor.insertAsset(RasterAsset{"background", makeRasterLayer(640, 480, 0xff000000U)});
editor.insertLayer(StaticBitmapLayer{{"background-layer"}, StaticBitmapContent{"background"}});
editor.insertDynamicLayer({"animation"}, LayerRepresentation::Bitmap, {{0, "background"}});
editor.setDynamicFrameContent("animation", 1,
    RasterAsset{"frame-1", makeRasterLayer(640, 480, 0xffff0000U)});
const auto *animation = findDynamicBitmapLayer(document, "animation");
// animation->content.frameIndices == {0, 1}
```

## Editing and migration

Use `DocumentEditor::setStaticSource` or `setKeyframedSource` to convert timing.
They replace the concrete layer alternative and its content together, retain
properties, rebuild/remove frame-owned keys, and validate atomically. Failure
restores the previous type, frame records, revision and authorship. File-bound
edits commit synchronously through the existing incremental transaction.

`findStaticBitmapLayer`, `findStaticVectorLayer`, `findDynamicBitmapLayer` and
`findDynamicVectorLayer` return only their exact alternative. `layerKind` returns
`std::optional<LayerKind>` containing one of the four values for artwork.
`layerTiming` and `layerRepresentation` remain independent queries.

This is a breaking C++ model/API change: rebuild against package **0.28.0**, ABI
**0.28**. Replace `BitmapLayer`/`VectorLayer` with the appropriate concrete type and
replace `.source` with `.content`. `layerSource` now returns a detached source
value; assigning to it does not mutate a document. `staticLayerSource` and
`keyframedLayerSource` borrow reference/index storage without allocating.
`makeBitmapLayer`/`makeVectorLayer` convert a source carrier into the correct
concrete aggregate. `setLayerSource` performs aggregate conversion but does not
validate or manage document-owned keys; use the editor for structural edits.

Video and existing ControlNet specializations retain their media/conditioning
contracts. Video reports dynamic bitmap identity; spatial conditioning maps its
timing and representation to the same four visual kinds. IP-Adapter tensor
conditioning has `LayerRepresentation::Embedding` and `layerKind == nullopt`;
it does not add artwork kinds or masquerade as bitmap/vector pixels.

## Persistence and verification

Native **1.19** records store the explicit visual type byte immediately after
artboard membership. Tags 0/1/2/3 mean static bitmap/static vector/dynamic bitmap/
dynamic vector. Tag 255 identifies nonspatial conditioning. Decoders reject an
unknown tag or a declared type that disagrees with source/asset/role, even when
the checksum is valid. Working-file schema remains 1 and stores the same typed
layer record in its existing incremental store.

All legacy 1.0–1.18 versions infer concrete memory types from their existing
source/asset/role fields. Re-encoding an untouched legacy document retains its
version and canonical bytes. An accepted editor change advances to 1.19.

`iiSharedCanvas.FourLayerType` verifies distinct compile-time content types,
all four rendered behaviors, canonical current/legacy round trips, valid-checksum
type corruption rejection, successful/failed concrete conversions, and working
file reopen. Existing per-frame content, rendering, conditioning, layered media,
artboard and installed-package suites cover the migrated paths. The standalone
consumer also runs `iiSharedCanvas.InstalledFourLayerType` against the installed
headers and shared library.

## 정적 레이어의 타임라인 셀

iiSharedCanvas Native 0.35.42의 `staticLayerTimelineRange(document, layer)`는 유효한 정적 레이어에 대해 `firstFrame`과 `lastFrame`이 같은 범위를 반환한다. 시작 위치는 레이어 활성 범위의 첫 프레임이며, 범위가 생략되면 인덱스 0이다. 연결된 자산이 투명해도 셀 한 칸을 차지한다. 동적 레이어, 영상 레이어, 자산을 해석할 수 없는 레이어에는 빈 결과를 반환한다.

편집기는 이 범위로 타임라인 셀을 배치한다. 합성과 재생에서 정적 자산이 보이는 구간은 `LayerProperties::frameRange`가 정한다. 활성 범위가 생략된 레이어는 문서 전 구간에서 같은 자산을 제공한다.

`iiSharedCanvas.Native.DocumentModel`은 여러 프레임의 문서에서 정적 비트맵과 벡터의 셀이 각각 한 칸인지, 활성 범위가 지정된 경우 위치가 맞는지, 활성 범위의 이후 프레임에서도 자산을 해석하는지 검증한다. `NativeCanvas.Relocated`는 설치된 헤더와 라이브러리로 같은 API를 호출한다.

## 새 동적 레이어의 기본 길이

iiSharedCanvas Native 0.35.43의 `defaultDynamicLayerTimelineRange(frameRate)`는 새 동적 레이어의 기본 범위를 반환한다. 범위는 프레임 0에서 시작한다. 프레임 수는 문서의 유리수 프레임레이트에 5초를 곱한 값 이상인 최소 정수이다. 예를 들어 24fps면 120프레임이고 30000/1001fps면 150프레임이다. 유효하지 않은 프레임레이트나 표현 가능한 프레임 수를 초과하는 입력은 빈 결과를 반환한다.

편집기는 반환된 범위를 새 레이어의 `LayerProperties::frameRange`에 저장한다. 문서가 이 범위보다 짧으면 `DocumentEditor::setFrameCount`로 끝을 확장한다. 프레임 0에 빈 초기 자산을 연결하고, 그 뒤의 프레임은 편집할 때 키프레임을 기록할 수 있다. 파일을 여는 경우와 기존 레이어를 가져오는 경우에는 저장된 범위와 키프레임을 사용한다.
