# iiSharedCanvas .iisc logical format draft

Status: Draft 1 logical contract. Serialization is not implemented in Phase 0.

## Identity

- Extension: .iisc
- Media type: application/vnd.iisacc.ii-shared-canvas+zip
- Model version: major 1, minor 0
- Integer byte order for future binary records: little-endian
- Raster channel contract in memory: 32-bit ARGB as defined by iiPaintEngine

The planned physical package is ZIP. A reader must locate data by manifest
references, never by relying on ZIP entry order.

## Logical package

~~~text
mimetype
manifest.json
assets/
  raster/
    <asset-id>.<bitmap-extension>
  vector/
    <asset-id>.json
preview/
  thumbnail.png
~~~

mimetype contains the media type as plain UTF-8. preview is optional and never
authoritative. Assets not referenced by manifest are allowed during editing but
may be removed by an explicit compact operation.

## Manifest shape

~~~json
{
  "format": "com.iisacc.ii-shared-canvas",
  "version": {"major": 1, "minor": 0},
  "canvas": {"width": 1920, "height": 1080},
  "timeline": {
    "frameRate": {"numerator": 24, "denominator": 1},
    "frameCount": 48
  },
  "assets": [
    {
      "id": "paint",
      "kind": "raster",
      "path": "assets/raster/paint.png"
    },
    {
      "id": "logo",
      "kind": "vector",
      "path": "assets/vector/logo.json"
    }
  ],
  "layers": [
    {
      "id": "paint-layer",
      "name": "Paint",
      "visible": true,
      "opacity": 1.0,
      "blendMode": "source-over",
      "transform": [1, 0, 0, 1, 0, 0],
      "source": {"type": "static", "asset": "paint"}
    },
    {
      "id": "logo-animation",
      "name": "Logo animation",
      "visible": true,
      "opacity": 1.0,
      "blendMode": "source-over",
      "transform": [1, 0, 0, 1, 0, 0],
      "source": {
        "type": "keyframed",
        "kind": "vector",
        "keyframes": [
          {"frame": 0, "asset": "logo"},
          {"frame": 24, "asset": "logo-next"}
        ]
      }
    }
  ]
}
~~~

## Vector asset shape

~~~json
{
  "viewport": {"width": 64, "height": 64},
  "paths": [
    {
      "commands": [
        {"op": "M", "p": [0, 0]},
        {"op": "L", "p": [64, 0]},
        {"op": "Q", "c": [64, 32], "p": [64, 64]},
        {"op": "C", "c1": [48, 64], "c2": [16, 64], "p": [0, 64]},
        {"op": "Z"}
      ],
      "fill": {"argb": "FFFF0000"},
      "stroke": {"argb": "FF000000", "width": 1.0}
    }
  ]
}
~~~

Only M, L, Q, C, and Z are part of model version 1. Unknown operations fail
closed. ARGB strings are exactly eight uppercase hexadecimal digits.

## Layer order and evaluation

layers are stored bottom-to-top. Each layer has exactly one source.

A static source refers to one raster or vector asset. A keyframed source
declares raster or vector and contains at least one keyframe. Its first frame is
zero, positions are strictly increasing, and all referenced assets match the
declared kind.

Version 1 uses hold sampling only. At frame F, the selected asset is the last
keyframe whose frame is less than or equal to F. Frames are valid in the
half-open interval [0, frameCount).

## Raster and brush contract

Raster entries use an iiPaintEngine-supported bitmap encoding. Exact allowed
codecs will be frozen when the serializer is implemented and tested.

Brush input is not a manifest content kind. The brush output is stored as a
raster asset after iiPaintEngine commits it. No trajectory, pressure point
sequence, curve, dab sequence, or replay command belongs in an .iisc package.

## Compatibility

- A reader accepts the current major and a minor no newer than it implements.
- A newer major or minor fails closed until a migration is implemented.
- A writer emits one canonical representation for its implemented version.
- Migration must preserve raster pixels and vector geometry. It must never
  silently rasterize vector assets or discard animation.

These rules match the current in-memory validation contract. The physical ZIP
rules become normative only when Phase 1 round-trip and corruption tests exist.
