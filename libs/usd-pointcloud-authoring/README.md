# usdPointCloudAuthoring

## Purpose

`usdPointCloudAuthoring` is the one place shared point-cloud data becomes
OpenUSD. Every format bundle authors through it, so the stage contract holds
across formats by construction rather than by two parallel implementations
being kept in step. It is the only library in the workspace that links OpenUSD.

CMake package `usdPointCloudAuthoring`, target `usdpointcloud::authoring`, C++
namespace `usdgeo`.

> Renamed from `usdGeoUsd` / `libs/usd-geo-usd` after v0.1.0. `usdgeo::usd`
> remains as a deprecated CMake alias and is removed in v0.3.0; see
> [MIGRATION.md](../../docs/compatibility/MIGRATION.md). The C++ namespace and
> the `include/usdgeo/` header path deliberately did not change.

## Responsibilities

- Stage creation with the project's metrics: up axis `Y`, `metersPerUnit` 1.0.
- `UsdGeomPoints` authoring at `/PointCloud`, with positions converted to
  stage-local `float`.
- Geospatial metadata authoring: the `geo:*` namespace for CRS, local origin,
  bounds, linear unit, up axis, and point count.
- Point attribute authoring, including RGB, NIR, GPS time, waveform packet
  fields, and named scalar Extra Bytes columns.
- Metadata-only authoring, for reads that decode no point records.
- `usdLod` hierarchy authoring: `UsdLodRootAPI` and
  `UsdLodScreenSizeHeuristic`, with correctly ordered children and a default
  index.
- Tiled authoring: one deterministic `usdLod` root per tile.
- Payload-backed tile assets: one USDC payload per tile and LOD level, with
  portable relative asset paths, and ownership of the generated payload set
  when the caller names an owner.
- Layer- and stage-level validation, and the typed
  `PointCloudAuthorFailure` a caller maps onto its own codes.

## Non-responsibilities

- LAS- or LAZ-specific decoding. This module never learns a source format.
- Spatial partitioning policy. It authors the tiles it is given; deciding
  *which* tile a point belongs to is the reserved `usdPointCloudTiling`
  module's job.
- Plugin format-argument parsing. It consumes a validated options struct, never
  `SDF_FORMAT_ARGS`.
- Diagnostic code ownership. Codes stay with the calling bundle; this module
  reports a typed failure kind and the bundle projects it onto `LASxxx` /
  `LAZxxx`.
- Rendering, LOD selection, cameras, viewport state, or screen-space math.

## Public API

```text
usdgeo/PointCloudLayer.h
usdgeo/PointCloudCache.h
```

| Group | Entry points |
| --- | --- |
| Stage | `PointCloudLayer::CreateStage`, `PointCloudLayer::AuthorPointCloud` |
| Layer authoring | `AuthorPointCloudAsset` (layer and stage overloads, with and without a `PointCloudAuthorFailure` out-parameter) |
| Metadata only | `AuthorPointCloudMetadata`, `PointCloudSourceMetadata` |
| LOD | `AuthorPointCloudLodAsset` |
| Tiling | `PointCloudTileAsset`, `AuthorPointCloudTiledAsset` |
| Payloads | `PointCloudPayloadOptions`, `AuthorPointCloudTiledAssetWithPayloads` (stage and layer overloads), `PointCloudPayloadOwner` |
| Generated cache | `TryBuildPointCloudCacheLayout`, `TryLoadPointCloudCache`, `TryBuildResolverSourceIdentity` |
| Failure kinds | `PointCloudAuthorFailure` (`InvalidLayer`, `StageCreation`, `StageMetrics`, `PointCloud`) |

Minimal use:

```cpp
#include "usdgeo/PointCloudLayer.h"

usdgeo::PointCloudAuthorFailure failure = usdgeo::PointCloudAuthorFailure::None;
if (!usdgeo::AuthorPointCloudAsset(layer, "/PointCloud", asset, failure)) {
    switch (failure) {
        case usdgeo::PointCloudAuthorFailure::StageCreation: /* map to your code */ break;
        case usdgeo::PointCloudAuthorFailure::StageMetrics:  /* ... */ break;
        default: break;
    }
    return false;
}
```

## Dependencies

`usdgeo::core`, `usdpointcloud::core`, and OpenUSD — specifically `usdGeom`
and `usdLod`, plus the `arch`/`tf`/`gf`/`vt`/`sdf`/`usd` core it pulls in.

**OpenUSD is required.** This is the only `libs/` module for which that is
true, and it is why the root CMake build guards it behind `USDGEO_BUILD_USD`.

## Data flow

```text
usdpointcloud::PointCloudAsset            (source-space positions + GeoReference)
    | GeoReference::TryToLocal, up-axis conversion, narrowing to float
    v
UsdGeomPoints at /PointCloud  +  geo:* metadata

usdpointcloud::PointLodHierarchy + per-level PointCloudAsset
    | AuthorPointCloudLodAsset
    v
UsdLodRootAPI root + ordered children + UsdLodScreenSizeHeuristic

std::vector<PointCloudTileAsset>
    | AuthorPointCloudTiledAsset            -> one LOD root per tile, in-layer
    | AuthorPointCloudTiledAssetWithPayloads -> one USDC payload per tile/level
    v
root layer referencing the generated payload assets
```

## Error and diagnostic behavior

Nothing throws across the API boundary. Every authoring entry point returns
`bool`; the overloads taking a `PointCloudAuthorFailure&` additionally report
*which* stage failed so a bundle can pick the right stable code without parsing
a message.

Validation is refuse-whole, not best-effort: an asset or hierarchy that fails
`IsValid()` or `ValidatePointLodHierarchy` is rejected before anything is
authored, so a failed call does not leave a partially populated prim or a LOD
root with missing children. OpenUSD's own diagnostics (`TF_RUNTIME_ERROR`) are
emitted by the calling bundle, not here.

## Threading and ownership

Authoring functions take an `SdfLayer*` or `UsdStageRefPtr` owned by the
caller; this module never retains either past the call. Input assets are taken
by `const&` and copied into `VtArray` where OpenUSD requires it, so no caller
buffer is borrowed after the call returns. Nothing returned aliases internal
storage.

There is no internal shared state, but OpenUSD's threading rules apply to the
layer or stage passed in: authoring into one layer from two threads is unsafe,
and callers must not author into a layer another thread is composing.

One caller-side note that belongs to the bundles rather than to this module:
`SdfLayer` reload runs a file format under an outer `SdfChangeBlock`, which is
thread-local state, so a bundle that authors a detached stage and transfers its
content must do so on the same thread that will hand the result back.

The layer overloads build content in an in-memory stage opened with
`UsdStage::LoadNone` and transfer it into the caller's layer. Nothing is
loaded, so a payload arc is never resolved or opened while its layer is being
built, and no layer is renamed to anchor one: the caller's layer keeps its
identifier throughout, and the detached stage never takes a file identity.
Each payload file is built in an in-memory layer and written with
`SdfLayer::Export`, so writing it never registers a layer under the payload
path.

`PointCloudPayloadOptions::owner` decides where payload files go. Left empty,
the payload directory is exclusive: payloads are written straight into it and
any existing payload path refuses the write, which is what
`usd-pointcloud-convert` relies on. Set, typically to
`PointCloudPayloadOwner(resolvedPath, payloadDirectory, arguments)`, payloads
go to `<directory>/<owner key>/<generation>/`. A generation is staged in a
private directory and published by renaming it into place, so a published one
is never modified: a failure removes only the staging directory, identical
content reuses the published generation, and changed content is published
beside it before the superseded one is removed. Nothing outside the owner's
directory is touched. The layout is stated in the
[file-format argument contract](../../docs/architecture/FILE_FORMAT_ARGUMENTS.md#generated-payload-ownership).

`PointCloudPayloadOptions::spoolDirectory` optionally names the exclusive
working directory for tiled stream spools. When it is empty, the authoring
library creates a `.spool` directory below the payload directory, scoped by
the payload owner when one is present. The directory contains a marker before
spooling starts; a later invocation removes a marked directory left by an
interrupted run, and successful or failed authoring removes it on exit. An
existing directory without the marker is refused so authoring never deletes
unrelated files.

## Coordinate-space assumptions

Input positions are **source-space** `usdgeo::Vec3d`. This module applies
`GeoReference::TryToLocal` — the local-origin translation and the source-`Z`
to stage-`Y` up-axis conversion — and narrows to `float` for `points`.
Absolute source precision survives only through the `double3`
`geo:localOrigin`, which is authored alongside. Source coordinates are
recovered as `sourceUpAxisTransform(localPosition) + geo:localOrigin`.

Spatial tile identity is **not** recomputed here; a `PointTile` arrives with
its id already fixed in source horizontal coordinates, so an up-axis change
does not renumber tiles.

## Build and test

Requires an OpenUSD runtime. The plain CMake build only adds this module when
`USDGEO_BUILD_USD` is on, which happens automatically when `pxr_ROOT` or
`OpenUSD_ROOT` is defined:

```powershell
ost configure
ost build
ctest --test-dir build/cy2026-windows-x86_64-py313-usd -C Release `
  -R usdPointCloudAuthoring_unit --output-on-failure
```

`ost test` runs it as part of the workspace suite. See
[BUILDING.md](../../docs/guides/BUILDING.md).

## Known limitations

- Positions are authored as `float`; absolute precision is preserved only via
  `geo:localOrigin`.
- The prim path is `/PointCloud` and the stage is always Y-up at one metre per
  unit, regardless of the source CRS units.
- `UsdLodOverrideAPI` is not authored. It is expected to come from a stronger
  layer.
- Sampling is fixed-stride, inherited from `usdPointCloudCore`.
- Payload working-set behavior is unmeasured: the library emits payloads, but
  no claim is made that a non-selected LOD child's payload stays unloaded.
- A superseded generation is removed on a best-effort basis. On a platform
  that refuses to delete an open file, such as Windows, files another stage
  still holds stay until a later read of the same layer removes them.
- Generating payloads needs write access to the payload directory even when
  the content turns out unchanged; only a cache hit whose copy is already
  published reads a directory without writing to it.
- `AuthorPointCloudTiledAssetFromStream` consumes a pull stream, spools points
  by source tile, and reconstructs one tile at a time before payload authoring.
  The router overload also accepts a planned `PointBudgetTileRouter`, allowing
  adaptive leaf plans to use the same bounded spool and payload path.
  Spools live in the caller-owned `spoolDirectory` or an owner-scoped
  `.spool` directory below the payload directory; marked leftovers are
  recovered on the next invocation.
- Large-corpus RSS measurement for generated, LAS, and LAZ inputs is available
  through the explicit `usdPointCloudAuthoring_stream_benchmark` target. It is
  disabled by default;
  configure with `-DUSDGEO_BUILD_BENCHMARKS=ON`, build the `cy2026` / `usd`
  target, and run, for example:

  ```powershell
  cmake --preset cy2026-windows-x86_64-py313-usd `
    -DUSDGEO_BUILD_BENCHMARKS=ON
  ost build --target cy2026 --profile usd --config Release
  Invoke-Expression (ost env cy2026 --profile usd --shell powershell | Out-String)
  & .\build\cy2026-windows-x86_64-py313-usd\libs\usd-pointcloud-authoring\benchmarks\usdPointCloudAuthoring_stream_benchmark.exe `
    --points 1000000 --chunk-points 65536 --tile-size 128 `
    --memory-limit 1048576
  ```

  For a real input, pass `--input` and optionally override format detection
  with `--format las` or `--format laz`:

  ```powershell
  & .\build\cy2026-windows-x86_64-py313-usd\libs\usd-pointcloud-authoring\benchmarks\usdPointCloudAuthoring_stream_benchmark.exe `
    --input .\plugins\pointcloud-las\tests\fixtures\conformance.las `
    --chunk-points 65536 --tile-size 128 --memory-limit 1048576
  ```

  The benchmark reports elapsed time, RSS delta, the generated `.usdc` payload
  count as tile count, sampled peak spool file bytes, payload bytes, and process
  write bytes. This benchmark authors one payload level per tile. The process
  write counter is Windows-specific; it is reported as zero on other platforms.
  Windows RSS uses the working set; Linux and macOS use the current resident set
  when the platform API is available.
  A Windows baseline using the supplied Shizuoka `08NF2330.las` dataset
  processed 14,574,030 points in 32.4375 seconds at 449,296 points/s. It
  produced 12 payloads with a 351.16 MiB RSS delta, 1.004 GiB sampled peak
  spool size, 420.68 MiB of payload output, and 1.415 GiB of process writes.
  The broader real-dataset matrix and payload working-set measurement remain
  open; the full command and interpretation are recorded in the
  [streaming and tiling roadmap](../../docs/roadmap/streaming-and-tiling.md).
- Only the `Z` source / `Y` stage up-axis pair is supported.

## Planned work

- Payload working-set measurement across the supported scene and render
  delegates.
- Broader representative real-dataset RSS, spool, and payload measurements.

These items are specified in
[streaming and tiling](../../docs/roadmap/streaming-and-tiling.md).

## Contracts

- [Workspace contract](../../docs/architecture/WORKSPACE.md)
- [Tile and LOD contract](../../docs/architecture/LOD.md)
- [Plugin adapter contract](../../docs/architecture/PLUGIN_ADAPTER.md)
- [Capability matrix](../../docs/reference/CAPABILITY_MATRIX.md)
- [OpenUSD compatibility](../../docs/compatibility/OPENUSD.md)
