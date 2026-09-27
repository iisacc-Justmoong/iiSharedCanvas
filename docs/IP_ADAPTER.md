# IP-Adapter image embedding layers

Package 0.23.0, native model 1.17. No new dependencies. This module stores already
computed image embeddings and selects their timeline state. It does not run CLIP,
load weights, project embeddings, attach attention processors or perform diffusion.

## Data and identity

- `IpAdapterAsset` owns an id, descriptor, conditional tensor and optional explicit
  unconditional tensor. One asset is one image state, batch=1. Multiple independent
  references can be represented by multiple layers; downstream inference owns fusion.
- `IpAdapterTensor` is row-major float32 `[tokenCount, channelCount]`. Positive
  dimensions exactly match count. Negative values and magnitudes above one are
  valid; NaN/Inf are invalid. Equality compares binary32 bits, preserving signed-zero
  edits. No quantization, normalization or hidden scaling occurs.
- `IpAdapterEmbeddingStage::EncoderPooled` identifies a one-token encoder vector;
  `EncoderHiddenStates` identifies a token sequence; `ProjectedTokens` identifies
  image prompt tokens already transformed by the selected adapter's projection.
  No universal token/channel dimensions are hardcoded across architectures.
- The descriptor requires encoder id/revision, adapter id/revision, base model
  compatibility id and preprocessing id. Use immutable commits or weight hashes for
  revisions; preprocessing id must identify resize/crop/normalization and the chosen
  hidden-state layer where applicable. These are producer declarations, not an
  assertion that this library has inspected or authenticated the model weights.
- The layer's `control.modelId/modelRevision` identify the adapter and must match
  the asset. Dynamic states share the complete descriptor and shape, preventing a
  frame change from silently changing the embedding space. The consumer must also
  match the descriptor against its actual encoder, projection and diffusion model.

Official IP-Adapter uses pooled image embeddings in its base implementation and
hidden states in Plus. Both are projected before attention conditioning. Their
unconditional paths differ: zeros before projection for base, encoder output from
zero pixels for Plus. Consequently this module never invents an unconditional
branch or equates it with zero projected tokens. See the
[official implementation](https://github.com/tencent-ailab/IP-Adapter/blob/main/ip_adapter/ip_adapter.py).

## Layer and export semantics

`IpAdapterLayer` belongs to the shared conditioning hierarchy (`LayerRole::ControlNet`,
`ControlNetKind::IpAdapter`). This organizational role does not mean IP-Adapter is a
ControlNet neural network. It uses image-based attention conditioning.

Static sources retain one embedding state. Keyframed sources use hold-only selection
from frame zero. `LayerRepresentation::Embedding` and `StaticEmbedding`/
`DynamicEmbedding` distinguish nonspatial tensors from the existing four bitmap/vector
artwork kinds. Generic artwork factories reject Embedding; use `insertIpAdapterLayer`.

`exportIpAdapterEmbeddings` returns owned exact tensors, descriptor and control
settings for the selected frame. It honors enabled, timeline and layer frame range.
Display visibility, opacity and transforms do not change inference tensors. Scale
and guidance schedule are returned as metadata for the inference consumer to apply.
By default export requires an unconditional branch for CFG. Set `requireUnconditional`
to false only when the consuming path does not require one; an existing branch is
still exported. Missing unconditional data is never fabricated. `maximumValues`
limits the total copied scalars across both branches before allocation.

Embedding layers have no spatial image preview: `renderFrameLayerTiles` succeeds
with `spatial=false` and an empty tile list. Batch rendering preserves their metadata;
artwork composition and vector/PDF output omit conditioning layers. PSD and timeline
interchange fail closed rather than losing embedding data. Bitmap editing cannot bind
an embedding asset. No source image is silently retained or reconstructed from tensors.

## Editing example

```cpp
IpAdapterAsset asset;
asset.id = "reference-embedding";
asset.descriptor = {IpAdapterEmbeddingStage::EncoderPooled,
    "encoder-id", "encoder-weight-sha", "adapter-id", "adapter-weight-sha",
    "diffusion-compatibility-id", "preprocessing-contract-v1"};
asset.conditional = {1, 3, {-0.5F, 0.25F, 1.5F}}; // Illustrative shape, not a model size.
asset.unconditional = IpAdapterTensor{1, 3, {0.0F, 0.0F, 0.0F}};
// Supply actual producer-computed values for the model and declared stage.
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

Check every edit/result before consuming it. `setIpAdapterValue` indexes branch,
token and channel. Asset replacement edits shape/provenance/branches atomically;
`setControlNetSettings` edits the layer binding and schedule. A rejected edit leaves
state, version and revision unchanged. File-bound edits commit synchronously.

## Persistence and bounds

Native 1.17 uses asset tag 14, content/role tags 12, little-endian binary32 and explicit
shape/presence fields. Global value budgets sum both branches across every asset;
all descriptor strings count against string budgets. Decode checks shape, budgets,
remaining bytes and vector capacity before allocation, then validates finite values
and model bindings. Unknown enum values and old version headers fail closed.
Stored tensors and dynamic states round-trip without a model runtime. Settings-only
commits reuse unchanged asset records. See [FORMAT.md](FORMAT.md).

Detailed layer and object fields can also be edited atomically through the
[common parameter API](CONTROLNET_PARAMETERS.md), including every dynamic source state.
