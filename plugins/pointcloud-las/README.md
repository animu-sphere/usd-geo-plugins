# pointcloud-las

## Purpose

Read-only OpenUSD `SdfFileFormat` for LAS point clouds. The bundle is a thin
adapter: it normalizes file-format arguments, drives `usdlas::LasReader`,
authors through the shared `usdPointCloudAuthoring` library, and projects typed
diagnostics onto its stable `LASxxx` codes. It owns no decoding and no
authoring of its own.

OpenStrata bundle `pointcloud-las`, CMake target and shared library
`UsdGeoLasFileFormat`, registered `plugInfo.json` type `UsdGeoLasFileFormat`,
format id `las`.

> Renamed from `geo-las` after v0.1.0. The bundle directory, shared library,
> and `plugInfo.json` type name all changed; the `las` format id, the `.las`
> extension, every `LASxxx` code, and the authored stage did not. See
> [MIGRATION.md](../../docs/compatibility/MIGRATION.md).

## Supported file extensions

| Extension | Claimed | Notes |
| --- | --- | --- |
| `.las` | Yes, as the primary file format for `las` | Uncompressed LAS only; `.laz` belongs to [pointcloud-laz](../pointcloud-laz/README.md) |

## Supported source versions and point formats

| LAS version | Status |
| --- | --- |
| 1.0, 1.1 | Rejected (`LAS005`) |
| 1.2, 1.3, 1.4 | Supported |

| Point format | LAS version | Status |
| ---: | --- | --- |
| 0-3 | 1.2-1.4 | Supported |
| 4, 5 | 1.3-1.4 | Supported, including waveform packet metadata |
| 6-8 | 1.4 only | Supported |
| 9, 10 | 1.4 only | Supported, including waveform packet metadata |

VLR and EVLR records are parsed; WKT CRS, GeoTIFF key records, and Extra Bytes
descriptors are interpreted. Scalar Extra Bytes types 1-10 are authored as
`geo:<name>` (`double[]`) with descriptor scale and offset applied.

The authoritative matrix — every attribute, VLR, CRS source, and authored USD
property — is
[CAPABILITY_MATRIX.md](../../docs/reference/CAPABILITY_MATRIX.md).

## FileFormat arguments

Arguments use the standard `SDF_FORMAT_ARGS` encoding and are normalized into a
canonical map that participates in layer identity.

| Argument | Values | Effect |
| --- | --- | --- |
| `lod` | `off` (default), `preview`, `balanced`, `quality` | `off` authors one `UsdGeomPoints` prim; the others author a single non-tiled `usdLod` root with fixed-stride levels and a screen-size heuristic |
| `attributes` | comma-separated supported names; `xyz` implicit | Authors only the listed attributes |
| `chunkPointLimit` | positive integer | Maximum points delivered per reader chunk |
| `memoryBudgetBytes` | positive integer | Caps the reader's point and record buffers |
| `rangeFirstPoint` | unsigned index | First source point to author |
| `rangePointCount` | unsigned count; `0` means all remaining | Number of source points to author |
| `bounds` | `minX,minY,minZ,maxX,maxY,maxZ` | Inclusive source-coordinate bounds filter |
| `classification` | comma-separated values `0`-`255` | Keeps only matching LAS classifications; values are sorted and deduplicated |
| `tile` | `true` | Routes the pull stream into source-coordinate tile payloads |
| `tileSize` | positive source units | Fixed-grid tile width and depth |
| `tileMemoryLimit` | positive bytes | Per-tile spool buffer limit |
| `payloadDirectory` | path | Directory for generated USDC payloads; each layer publishes its payloads in a directory of its own below it, so reading the layer again never collides with other layers or files; see [payload ownership](../../docs/architecture/FILE_FORMAT_ARGUMENTS.md#generated-payload-ownership) |

```bash
usdcat "sample.las:SDF_FORMAT_ARGS:lod=balanced&attributes=xyz,rgb"
```

Recognized but **rejected** with `LAS017`, because their shared contracts are
not implemented: `lodLevels`, `lodPointCounts`, `lodRatios`, `lodThresholds`, `sampling`,
`originMode`, `upAxis`. An unknown key is rejected rather than ignored, so a
typo is distinguishable from a default. Filters apply to source coordinates and
do not change metadata-only reads.

A static `SdfFileFormat::Read` runs after layer lookup and cannot repair a
non-canonical identifier, so a host constructing a layer directly must
normalize first and pass the canonical map to `SdfLayer::FindOrOpen`. Full
rules: [FILE_FORMAT_ARGUMENTS.md](../../docs/architecture/FILE_FORMAT_ARGUMENTS.md).

## Authored OpenUSD result

A Y-up stage at one metre per unit with `UsdGeomPoints` at `/PointCloud`:

```usda
def Points "PointCloud"
{
    point3f[] points = [(0, 0, 0), (1.42, 0.31, -2.07), ...]
    custom int[] geo:intensity = [1024, 981, ...]
    custom uchar[] geo:classification = [2, 2, 6, ...]
    custom double3 geo:localOrigin = (-8242.5, -34212.5, 12.4)
    custom double3 geo:boundsMin = (0, 0, -84.31)
    custom double3 geo:boundsMax = (512.5, 43.2, 0)
    custom string geo:wkt = "PROJCS[...]"
    custom uint64 geo:pointCount = 1048576
}
```

Positions are stage-local `float` relative to `geo:localOrigin`; source
coordinates are `sourceUpAxisTransform(localPosition) + geo:localOrigin`.

`Read(metadataOnly=true)` authors the same `geo:*` metadata namespace — source
count, bounds, CRS, and available attributes — without decoding point records,
so the stage has no point positions.

`WriteToFile` reports an unsupported operation; `WriteToString` delegates to
USDA so an imported layer stays inspectable with `usdcat`.

### What the four support levels mean here

| Capability | Status |
| --- | --- |
| LAS reader (`usdLas`) | Point formats 0-10, waveform metadata, scalar Extra Bytes, chunked and range reads, metadata-only reads |
| Authoring library (`usdPointCloudAuthoring`) | `usdLod` roots, spatial tiled roots, payload-backed tile assets |
| Reachable from a direct `.las` read | Everything above, including spatial tiling and payload generation |
| Lower-level API only | Advanced bounded-memory measurement and failure-injection coverage |

Remaining streaming work is tracked in
[streaming and tiling](../../docs/roadmap/streaming-and-tiling.md).

## Plugin discovery and installation

```text
lib/UsdGeoLasFileFormat.dll        # or .so / .dylib
plugin/resources/pointcloud-las/plugInfo.json
openstrata.plugin.yaml
```

Point `PXR_PLUGINPATH_NAME` at the directory holding `plugInfo.json`; a
trailing slash makes OpenUSD search subdirectories and register both bundles at
once:

```powershell
$env:PXR_PLUGINPATH_NAME = "C:\path\to\pointcloud-las\plugin\resources\"
```

Full instructions: [INSTALL.md](../../docs/guides/INSTALL.md).

## Build and test

```powershell
ost plugin build .\plugins\pointcloud-las
ost plugin doctor .\plugins\pointcloud-las
ost plugin test .\plugins\pointcloud-las --up-to 4
ost plugin package .\plugins\pointcloud-las
```

Preview a file without installing anything:

```powershell
ost plugin view .\plugins\pointcloud-las C:\path\to\sample.las `
  --with .\plugins\pointcloud-laz
```

The CTest integration test `pointcloudLas_integration` is built only by the
workspace build, because a per-bundle `ost plugin build` does not define
`USDGEO_BUILD_TESTS`:

```powershell
ost configure
ost build
ost test
```

The bundle manifest declares `tests/fixtures/conformance.las` as its smoke
fixture. `ost plugin test --up-to 4` therefore exercises discovery, `usdcat`
read, and Python stage open instead of skipping L3 and L4. Additional generated
and corpus inputs remain under `tests/fixtures/` and `tests/corpus/`.

## Source layout

```text
src/UsdGeoLasFileFormat.cpp              thin SdfFileFormat entry point
include/usdgeolas/UsdGeoLasFileFormat.h  class and format tokens
include/usdgeolas/UsdGeoLasDiagnostics.h the bundle's stable LASxxx codes
plugin/resources/pointcloud-las/         plugInfo.json and its .in template
cmake/OpenStrataPlugin.cmake             shared OST plugin CMake helpers
tests/                                   integration coverage, fixtures, corpus
docs/DIAGNOSTICS.md                      the LASxxx code table
```

`cmake/OpenStrataPlugin.cmake` is shared: the `pointcloud-laz` bundle includes
it from here rather than carrying a copy.

## Runtime dependencies

- OpenUSD, declared as `>=26.08,<27.0` and validated against 26.08. The plugin
  library must be able to load the OpenUSD libraries it was built against.
- The `usd-stage-read` OpenStrata capability.
- `usdlas::core` and `usdpointcloud::authoring`, linked statically.
- No third-party codec. This bundle contains **no laz-perf code**.

## Licensing

Apache-2.0, like the project. Because no laz-perf code is linked in, this
bundle carries no LGPL-2.1 obligation — unlike
[pointcloud-laz](../pointcloud-laz/README.md). See
[LICENSE](../../LICENSE), [NOTICE](../../NOTICE), and
[DISTRIBUTION.md](../../docs/guides/DISTRIBUTION.md).

## Known limitations

- Tiled reads spool points and reconstruct one tile at a time before payload
  authoring; large-corpus memory measurement remains open.
- Extra Bytes types 1-30 are supported, including vectors. Non-finite values
  and integers not exactly representable as `double` are rejected, and
  descriptor names are normalized to deterministic USD-safe names.
- CRS WKT is retained and EPSG is inferred from explicit WKT or GeoTIFF
  horizontal CRS keys. Conflicting definitions are rejected with a typed
  `ConflictingCrs` diagnostic.
- Waveform sample data is not fetched; packet metadata and the `.wdp` reference
  are retained for deferred loading.
- Decoding assumes a little-endian host.
- Writing LAS is out of scope.

## Compatibility

| Item | Value |
| --- | --- |
| Declared OpenUSD range | `>=26.08,<27.0` |
| Validated OpenUSD | 26.08 |
| OpenStrata CLI | 0.22.0 |
| OpenStrata platform / profile | `cy2026` / `usd` |
| C++ standard | C++17 |
| Hosted CI | Windows 2022 (L0-L4), macOS 15 arm64 (L0-L5), Ubuntu 24.04 (L0-L5) |

Full statement: [OPENUSD.md](../../docs/compatibility/OPENUSD.md). Breaking
name changes: [MIGRATION.md](../../docs/compatibility/MIGRATION.md).

## Contracts and status

- [Diagnostics: the LASxxx table](docs/DIAGNOSTICS.md)
- [Capability matrix](../../docs/reference/CAPABILITY_MATRIX.md)
- [Plugin adapter contract](../../docs/architecture/PLUGIN_ADAPTER.md)
- [File-format argument contract](../../docs/architecture/FILE_FORMAT_ARGUMENTS.md)
- [Tile and LOD contract](../../docs/architecture/LOD.md)
- [Workspace contract](../../docs/architecture/WORKSPACE.md)
- [Roadmap](../../docs/roadmap/README.md)
