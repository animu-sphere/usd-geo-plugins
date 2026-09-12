# usdPointCloudCore

## Purpose

`usdPointCloudCore` defines the format-independent point-cloud contracts every
reader produces and the authoring library consumes: the attribute schema, the
chunk and asset shapes, read options, deterministic sampling, and the shared
LOD value types, and multi-scan collection values. It is the seam that lets
LAS and LAZ — and later COPC, PLY, and E57 — reach OpenUSD through one path
instead of one writer per format.

CMake package `usdPointCloudCore`, target `usdpointcloud::core`, C++ namespace
`usdpointcloud`.

## Responsibilities

- Point attribute definitions: `PointAttributeType`, `PointAttribute`.
- `PointChunk` — point count, bounds, and the attribute schema for a delivery.
- `PointData` — the parallel per-attribute arrays, including RGB, NIR, GPS
  time, waveform packet fields, and named scalar Extra Bytes columns.
- `PointCloudAsset` — a `GeoReference`, bounds, chunk, and data together.
- `PointCloudScan` — a named asset and its row-major rigid scan-to-collection
  pose.
- `PointCloudCollection` — shared georeference and bounds for multiple scans;
  collection bounds must contain every pose-transformed scan bound.
- `MakePointChunk`, which derives a chunk schema from populated `PointData`.
- Point data validation: array lengths agree, values are finite, the schema
  matches what is populated.
- `PointRange` and `PointReadOptions` — the chunk limit, memory budget, source
  range, and cancellation callback every reader honours.
- File-format argument parsing and normalization, including `LodProfile` and
  attribute selection (`ParseFileFormatArgumentString`,
  `NormalizeFileFormatArguments`, `SelectPointDataAttributes`).
- Deterministic, versioned fixed-stride sampling: `PointSamplingOptions`,
  `SamplePointData`, `BuildPointLodAssets`, `MakeSamplingCacheArguments`.
- Shared LOD and tile value types: `PointTileId`, `PointSourceRange`,
  `PointLodItem`, `PointLodHierarchy`, `PointTile`, and their validation.

## Non-responsibilities

- LAS- or LAZ-specific binary decoding. No format knows its own layout here.
- Spatial partitioning, tile persistence, or spill-to-disk policy — reserved
  for `usdPointCloudTiling`.
- OpenUSD APIs, `usdLod` schema types, or stage authoring.
- FileFormat Plugin adapters, `SDF_FORMAT_ARGS` encoding, or plugin
  registration. This module consumes an already-parsed key/value map; it never
  sees Sdf argument syntax.

## Public API

```text
usdpointcloud/PointCloud.h           attributes, chunks, data, assets, scans, read options
usdpointcloud/Lod.h                  tile ids, LOD items, hierarchies, validation
usdpointcloud/Sampling.h             deterministic sampling and LOD asset building
usdpointcloud/FileFormatArguments.h  argument parsing, normalization, attribute selection
```

Minimal use:

```cpp
#include "usdpointcloud/PointCloud.h"
#include "usdpointcloud/Sampling.h"

usdpointcloud::PointReadOptions options;
options.chunkPointLimit = 65536;
options.memoryBudgetBytes = 64u * 1024u * 1024u;
options.range.firstPoint = 0;   // zero pointCount means "all remaining"

// after a reader fills `data` and `bounds`:
const auto chunk = usdpointcloud::MakePointChunk(data, bounds);
if (!chunk.IsValid() || !data.IsValid()) {
    // reject rather than author a partial cloud
}
```

## Dependencies

`usdgeo::core` only. OpenUSD is **not** required, and no OpenUSD header may
enter this module — including the `usdLod` schema types, which is why the LOD
value types here are plain structs rather than schema wrappers.

## Data flow

```text
format reader
    | LasPoint / decoded records
    v
PointData  (per-attribute arrays, source-space Vec3d positions)
    | MakePointChunk
    v
PointChunk (+ GeoReference, SpatialBounds)  ->  PointCloudAsset
    | SamplePointData / BuildPointLodAssets
    v
PointLodHierarchy + per-level PointCloudAsset
    v
usdPointCloudAuthoring

E57 Data3D scans
  | per-scan asset, bounds, attributes, and pose
  v
PointCloudCollection
```

Sampling preserves source order and applies the same selected indices to every
populated attribute, so a level is fully described by a source range plus the
algorithm and its version.

## Error and diagnostic behavior

Value types expose total `noexcept` `IsValid()` predicates. Validation entry
points (`ValidatePointLodHierarchy`, `ValidatePointTile`) append
`usdgeo::Diagnostic` records to a caller-owned vector and return `bool`;
they never throw and never write to a stream. A hierarchy that fails
validation is refused whole — the authoring library does not emit a partial
LOD root.

## Threading and ownership

All types own their buffers by value (`std::vector`). Nothing returns a
pointer or a reference into internal storage, so there is no borrowed-buffer
lifetime to track: a `PointData` handed to a consumer is either copied or
moved, and the consumer owns what it receives. There is no global or shared
mutable state. `PointReadOptions::isCancelled` is a caller-supplied
`std::function` that a reader may invoke from the thread driving the read; the
caller must keep whatever it captures alive for the duration of that read and
make it safe to call from that thread.

## Coordinate-space assumptions

`PointData::positions` are **source-space** `usdgeo::Vec3d`, already scaled and
offset by the reader but not yet translated by the local origin or converted to
the stage up axis. `PointChunk::bounds` and `PointCloudAsset::bounds` follow the
same space as the positions they describe. The conversion to stage-local
`float` happens only in `usdPointCloudAuthoring`, through
`GeoReference::TryToLocal`. Spatial partitioning, when it lands, also operates
on source horizontal coordinates so tile identity survives an up-axis change.

## Build and test

Builds and tests with plain CMake and **no OpenUSD runtime**:

```powershell
cmake -S . -B build -DUSDGEO_BUILD_TESTS=ON
cmake --build build --config Release
ctest --test-dir build -C Release -R usdPointCloudCore_unit --output-on-failure
```

## Known limitations

- Extra Bytes columns are `std::vector<double>` per descriptor name. Vector
  Extra Bytes types have no representation here.
- Attribute selection is a fixed set of known names; there is no general
  user-defined attribute registry.
- Sampling is fixed-stride only. Density-aware and spatially-aware selection
  are not implemented.
- `PointData` is a struct of named arrays rather than a generic attribute map,
  so adding an attribute is a source change in this module.
- The whole-cloud shape (`PointCloudAsset`) still assumes the data fits in
  memory; format readers expose the bounded-memory path through `PointStream`.
- E57 decoding and multi-scan OpenUSD authoring are not implemented yet; the
  collection types only establish the format-independent contract.

## Planned work

- Attribute-preservation guarantees across a spool round trip.
- Spool-backed tiled consumption and payload authoring. These are specified in
[streaming and tiling](../../docs/roadmap/streaming-and-tiling.md).

## Contracts

- [Workspace contract](../../docs/architecture/WORKSPACE.md)
- [Point reader architecture](../../docs/architecture/POINT_READER.md)
- [File-format argument contract](../../docs/architecture/FILE_FORMAT_ARGUMENTS.md)
- [Tile and LOD contract](../../docs/architecture/LOD.md)
- [Capability matrix](../../docs/reference/CAPABILITY_MATRIX.md)
