# FileFormat Plugin Adapter Contract

Section 3.1 of the [design policy](../design/DESIGN_POLICY.md) requires that
FileFormat Plugins remain thin adapters, and section 8 of the LOD policy
requires read orchestration to move out of them before tiling and LOD work
begins. This document records the current state, the target shape, and the
migration.

## Rule

A FileFormat Plugin normalizes file-format arguments, invokes a reader, hands
the result to `usdPointCloudAuthoring`, and converts diagnostics. It owns nothing else.

Specifically, a plugin must not contain:

- file access, seeking, or byte-range slicing;
- header, VLR, or EVLR interpretation;
- overflow or truncation validation of source offsets and counts;
- per-record decode loops;
- point-attribute fan-out into authoring buffers;
- attribute-schema construction from a point format;
- `GeoReference` or bounds construction from source metadata;
- camera-distance or viewport-size calculations;
- LOD selection state or LOD naming conventions;
- renderer-specific branching;
- format-specific USD schema definitions.

Everything above is either reader behavior or `usdPointCloudAuthoring` behavior. Placing it
in a plugin duplicates it across every future bundle and puts it outside the
tests that run without an OpenUSD runtime.

## Current State

`pointcloud-las` now delegates file access and LAS decoding to `usdlas::LasReader`,
and both plugins normalize the supported file-format arguments before calling
their shared readers and authoring path. Metadata-only reads use the same
header/VLR contracts without decoding point records.

| Concern | `pointcloud-las` | `pointcloud-laz` |
| --- | --- | --- |
| Source file access | In `usdlas::LasReader` | In `usdLaz` |
| Header and VLR/EVLR reading | In `usdlas::LasReader` | In `usdLaz` |
| Point-data truncation checks | In `usdlas::LasReader` | In `usdLaz` |
| Per-record decode loop | In `usdlas::LasReader` | In `usdLaz` |
| Uses the chunked reader API | Yes | Yes |
| Attribute fan-out into point data | In `usdlas` | In `usdlas` |
| `PointChunk` attribute schema | In `usdPointCloudCore` | In `usdPointCloudCore` |
| `GeoReference` and bounds construction | In `usdlas` | In `usdlas` |
| Stage creation, metrics, authoring, transfer | In `usdPointCloudAuthoring` | In `usdPointCloudAuthoring` |
| `metadataOnly` | Header/VLR inspection | Decoder header inspection |

`UsdGeoLasFileFormat::Read` now constructs a `usdlas::LasReader`, consumes its
point chunks, and projects reader diagnostics onto the stable plugin codes.
It passes the validated point-cloud asset to the shared authoring entry point.

`usdlas::LasReader` already provides exactly that orchestration behind
`LasReadOptions`, a chunk consumer, and typed diagnostics. It is currently
called from `pointcloud-las` and its unit tests. `UsdGeoLasFileFormat::Read` now uses it
the same way that `UsdGeoLazFileFormat::Read` uses `usdlaz::LazReader`.

Both plugins now pass reader output through the shared `usdlas` point-data and
asset builders, then call the layer-level `usdgeo::AuthorPointCloudAsset`
entry point. The plugins no longer own point fan-out, chunk schema
construction, CRS or bounds conversion, stage metrics, or layer transfer.
COPC native-hierarchy tiles reach the layer-level
`usdgeo::AuthorPointCloudTiledAssetWithPayloads` entry point the same way, so
no adapter creates a stage or transfers layer content. Every tiled adapter
names its layer as the owner of the payloads it generates through
`usdgeo::PointCloudPayloadOwner`.

### Consequence

`chunkPointLimit`, `memoryBudgetBytes`, and `range` are now reachable through
the normalized argument request. `isCancelled` remains host-supplied and is
not an asset argument. Non-tiled reads still accumulate data for the current
`UsdGeomPoints` authoring path; tiled reads use `PointStream`, spool by tile,
and reconstruct one tile at a time before payload authoring.

The static adapters consume `SdfLayer::GetFileFormatArguments()`, which is the
argument map OpenUSD stores after layer lookup. Callers that construct a layer
must normalize arguments first and pass the request's canonical map to
`SdfLayer::FindOrOpen`; `Read` is too late to change the layer cache key. The
registered format-specific LOD metadata field is composed by Pcp and mapped
back to the same normalized `lod` argument; all other arguments remain static.

The shared authoring bridge also owns the optional direct cache lookup. When
`USDGEO_CACHE_ROOT` is set, adapters build the same source-and-request
descriptor used by the converter after metadata inspection. A committed cache
hit transfers the cached root into the requested layer and rebases payload
paths to the requested payload directory; a miss or invalid cache entry leaves
the existing reader-to-authoring flow unchanged. Cache storage is host
configuration, not a file-format argument.

## Target Contract

```cpp
bool UsdGeoLasFileFormat::Read(
    SdfLayer* layer,
    const std::string& resolvedPath,
    bool metadataOnly) const
{
  usdpointcloud::PointReadRequest request;
  std::vector<usdgeo::Diagnostic> diagnostics;
  if (!usdpointcloud::MakeReadRequest(
      layer->GetFileFormatArguments(), request, diagnostics)) {
    ReportDiagnostics(diagnostics);
    return false;
  }

  usdpointcloud::PointCloudAsset asset;
  usdlas::LasReadFailure failure = usdlas::LasReadFailure::None;
  if (!usdlas::ReadPointCloud(
      resolvedPath, request.readOptions, request.attributes,
      "LAS CRS unavailable; inspect VLR metadata", asset, failure,
      diagnostics)) {
    ReportDiagnostics(diagnostics);
    return false;
  }

  return usdgeo::AuthorPointCloudAsset(
      layer, "/PointCloud", asset);
}
```

Two new pieces carry the work the plugins hold today:

- a reader entry point per format that takes normalized read controls and
  returns a validated point-cloud asset with typed diagnostics;
- `usdgeo::AuthorPointCloudAsset`, which owns the attribute fan-out, the
  attribute schema, `GeoReference` and bounds, stage metrics, prim authoring,
  and later the `usdLod` hierarchy.

`AuthorPointCloudAsset` is the single authoring entry point every present and
future bundle shares. Tiled streams and LOD assets use adjacent shared
authoring entry points; the plugin keeps the same reader-to-authoring boundary.
See the
[tile and LOD contract](LOD.md).

The plugin keeps only argument normalization, the reader call, the authoring
call, and the projection of typed diagnostics onto its stable `LASxxx` /
`LAZxxx` prefixes. See the
[diagnostics contract](DIAGNOSTICS.md).

## Migration

1. [x] Move `pointcloud-las` onto `usdlas::LasReader`, deleting the plugin's file
  access, metadata reading, truncation checks, and decode loop. The LAZ plugin
  already shows the shape.
2. [x] Move the shared tail into the reader and authoring libraries:
  attribute fan-out and LAS metadata conversion live in `usdlas`, chunk
  schema construction lives in `usdPointCloudCore`, and stage metrics and
  layer transfer live in `usdgeo::AuthorPointCloudAsset`. Both plugins call
  the shared path.
3. [x] Normalize file-format arguments in the plugin and pass supported read
  options to the reader. `isCancelled` remains host-supplied and is not part of
  the layer identifier.
4. [x] Implement `metadataOnly` through the same header and VLR contracts,
  returning metadata without decoding point records. Both plugins author the
  metadata namespace through `usdgeo::AuthorPointCloudMetadata`.
5. [x] Author LOD through the same authoring call. Compact `lod` profiles reach
  `usdgeo::AuthorPointCloudLodAsset` for both plugins.
6. [x] Add the request/result entry points (`MakeReadRequest`,
  `ReadPointCloud`) so the plugin body reduces to the target contract above.
  The LAS and LAZ readers now own point accumulation, attribute selection, and
  point-cloud asset construction; the adapters retain argument normalization,
  metadata or tiled orchestration, USD authoring, and diagnostic projection.
7. [x] Route a `PointStream` through spatial tiling and payload authoring when
  the [streaming and tiling](../roadmap/streaming-and-tiling.md) work lands.
  The plugin body does not grow: tiles arrive through the same result and are
  handed to the same authoring entry point.

Steps 1 and 2 removed the duplication that would otherwise be copied into every
new bundle in the [workspace contract](WORKSPACE.md). They were prerequisites
for tiling and LOD work, not cleanup to be done afterwards.

## Tests

Plugin integration tests verify authored output, not orchestration. As
behavior moves into the readers and `usdPointCloudAuthoring`, coverage moves with it into
tests that run without an OpenUSD runtime.

The invariant the plugin tests keep enforcing is that LAS and LAZ produce the
same layer shape for equivalent data. That equivalence becomes structural once
both call one reader contract and one authoring entry point, rather than being
maintained by two parallel implementations.
