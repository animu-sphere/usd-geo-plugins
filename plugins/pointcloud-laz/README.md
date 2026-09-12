# pointcloud-laz

## Purpose

Read-only OpenUSD `SdfFileFormat` for compressed LAZ point clouds. The bundle
is a thin adapter: it normalizes file-format arguments, drives
`usdlaz::LazReader`, authors through the shared `usdPointCloudAuthoring`
library, and projects typed diagnostics onto its stable `LAZxxx` codes.

Because `usdLaz` decompresses records and then hands them to `usdLas` for
interpretation, and both bundles author through one entry point, `.laz` and
`.las` produce the same layer shape for equivalent data by construction.

OpenStrata bundle `pointcloud-laz`, CMake target and shared library
`UsdGeoLazFileFormat`, registered `plugInfo.json` type `UsdGeoLazFileFormat`,
format id `laz`.

> Renamed from `geo-laz` after v0.1.0. The bundle directory, shared library,
> and `plugInfo.json` type name all changed; the `laz` format id, the `.laz`
> extension, every `LAZxxx` code, and the authored stage did not. See
> [MIGRATION.md](../../docs/compatibility/MIGRATION.md).

## Supported file extensions

| Extension | Claimed | Notes |
| --- | --- | --- |
| `.laz` | Yes, as the primary file format for `laz` | Compressed only; uncompressed `.las` belongs to [pointcloud-las](../pointcloud-las/README.md) |

## Supported source versions and point formats

LAZ accepts the same LAS versions. The reader clears the compression bit of the
point data record format byte before running the shared LAS header validation.

| LAS version | Status |
| --- | --- |
| 1.0, 1.1 | Rejected |
| 1.2, 1.3, 1.4 | Supported |

| Point format | Status |
| ---: | --- |
| 0-3 | Supported |
| 4, 5 | **Rejected** — the bundled codec provides no compressed waveform record decoder |
| 6-8 | Supported |
| 9, 10 | **Rejected** — same reason |

Once decompressed, attribute coverage is identical to `pointcloud-las`: RGB,
NIR, GPS time, classification flags, scanner channel, and scalar Extra Bytes
types 1-10 are all authored. The authoritative matrix is
[CAPABILITY_MATRIX.md](../../docs/reference/CAPABILITY_MATRIX.md).

## FileFormat arguments

Identical to `pointcloud-las`, with the same names, values, normalization, and
layer-identity participation.

| Argument | Values | Effect |
| --- | --- | --- |
| `lod` | `off` (default), `preview`, `balanced`, `quality` | `off` authors one `UsdGeomPoints` prim; the others author a single non-tiled `usdLod` root with fixed-stride levels and a screen-size heuristic |
| `attributes` | comma-separated supported names; `xyz` implicit | Authors only the listed attributes |
| `chunkPointLimit` | positive integer | Maximum points delivered per decoder chunk |
| `memoryBudgetBytes` | positive integer | Caps the decoder's point and record buffers |
| `rangeFirstPoint` | unsigned index | First source point to author |
| `rangePointCount` | unsigned count; `0` means all remaining | Number of source points to author |
| `bounds` | `minX,minY,minZ,maxX,maxY,maxZ` | Inclusive source-coordinate bounds filter |
| `classification` | comma-separated values `0`-`255` | Keeps only matching LAS classifications; values are sorted and deduplicated |
| `tile` | `true` | Routes the pull stream into source-coordinate tile payloads |
| `tileSize` | positive source units | Fixed-grid tile width and depth |
| `tileMemoryLimit` | positive bytes | Per-tile spool buffer limit |
| `payloadDirectory` | path | Directory for generated USDC payloads; each layer publishes its payloads in a directory of its own below it, so reading the layer again never collides with other layers or files; see [payload ownership](../../docs/architecture/FILE_FORMAT_ARGUMENTS.md#generated-payload-ownership) |

```bash
usdcat "sample.laz:SDF_FORMAT_ARGS:lod=preview"
```

One codec-specific caveat: the bundled laz-perf exposes no compressed point
seeking, so `rangeFirstPoint` avoids *delivering* unselected points but still
*decodes* every chunk that precedes the range. The argument controls output,
not work.

Recognized but **rejected** with `LAZ008`: `lodLevels`, `lodPointCounts`,
`lodRatios`, `lodThresholds`, `sampling`, `originMode`, `upAxis`. Unknown keys
are rejected rather than ignored. Filters apply to source coordinates and do
not change metadata-only reads. Full rules:
[FILE_FORMAT_ARGUMENTS.md](../../docs/architecture/FILE_FORMAT_ARGUMENTS.md).

## Authored OpenUSD result

The same layer shape `pointcloud-las` produces: a Y-up stage at one metre per
unit with `UsdGeomPoints` at `/PointCloud`, stage-local `float` positions
relative to `geo:localOrigin`, and the `geo:*` metadata namespace. Keeping that
equivalence is the invariant both bundles' integration tests enforce.

`Read(metadataOnly=true)` inspects the compressed header without decoding point
records and authors the metadata namespace alone.

`WriteToFile` reports an unsupported operation; `WriteToString` delegates to
USDA.

### What the four support levels mean here

| Capability | Status |
| --- | --- |
| LAZ reader (`usdLaz`) | Compressed formats 0-3 and 6-8, chunked reads, metadata-only reads, sequential range reads |
| Authoring library (`usdPointCloudAuthoring`) | `usdLod` roots, spatial tiled roots, payload-backed tile assets |
| Reachable from a direct `.laz` read | Everything the reader supports, plus compact `lod` profiles, spatial tiling, and payload generation |
| Lower-level API only | Advanced bounded-memory measurement and failure-injection coverage |

Remaining streaming work is tracked in
[streaming and tiling](../../docs/roadmap/streaming-and-tiling.md).

## Plugin discovery and installation

```text
lib/UsdGeoLazFileFormat.dll        # or .so / .dylib
plugin/resources/pointcloud-laz/plugInfo.json
openstrata.plugin.yaml
```

```powershell
$env:PXR_PLUGINPATH_NAME = "C:\path\to\pointcloud-laz\plugin\resources\"
```

A trailing slash makes OpenUSD search subdirectories, so pointing at a parent
directory containing both bundles registers `.las` and `.laz` together. Full
instructions: [INSTALL.md](../../docs/guides/INSTALL.md).

## Build and test

```powershell
ost plugin build .\plugins\pointcloud-laz
ost plugin doctor .\plugins\pointcloud-laz
ost plugin test .\plugins\pointcloud-laz --up-to 4
ost plugin package .\plugins\pointcloud-laz
```

```powershell
ost plugin view .\plugins\pointcloud-laz C:\path\to\sample.laz `
  --with .\plugins\pointcloud-las
```

The CTest integration test `pointcloudLaz_integration` is built only by the
workspace build (`ost configure && ost build && ost test`), because a
per-bundle `ost plugin build` does not define `USDGEO_BUILD_TESTS`.

This bundle's CMake includes `../pointcloud-las/cmake/OpenStrataPlugin.cmake`
rather than carrying its own copy, so the `pointcloud-las` directory must be
present in the tree to configure it.

The bundle manifest declares `tests/fixtures/conformance.laz` as its smoke
fixture. `ost plugin test --up-to 4` therefore exercises discovery, `usdcat`
read, and Python stage open instead of skipping L3 and L4. Additional generated
and corpus inputs remain under `tests/fixtures/` and `tests/corpus/`.

## Source layout

```text
src/UsdGeoLazFileFormat.cpp              thin SdfFileFormat entry point
include/usdgeolaz/UsdGeoLazFileFormat.h  class and format tokens
include/usdgeolaz/UsdGeoLazDiagnostics.h the bundle's stable LAZxxx codes
plugin/resources/pointcloud-laz/         plugInfo.json and its .in template
tests/                                   integration coverage, fixtures, corpus
docs/DIAGNOSTICS.md                      the LAZxxx code table
```

## Runtime dependencies

- OpenUSD, declared as `>=26.08,<27.0` and validated against 26.08.
- The `usd-stage-read` OpenStrata capability.
- `usdlaz::core` and `usdpointcloud::authoring`, linked statically.
- **`laz-perf 2.0.0`**, vendored and compiled into `usdLaz`, which is linked
  into this plugin's shared library. This is the only third-party codec in the
  workspace.

## Licensing

Project code is Apache-2.0. **`laz-perf 2.0.0` is LGPL-2.1 and is compiled into
this plugin binary**, so the shipped `UsdGeoLazFileFormat` library is a
combined work and LGPL-2.1 section 6 applies to its distribution.

Every distribution including this bundle must provide the LGPL-2.1 text, the
project `LICENSE` and `NOTICE`, `THIRD_PARTY_NOTICES.md` naming laz-perf with
its version and commit, a prominent statement that laz-perf is used under
LGPL-2.1, the complete corresponding laz-perf source for the exact version
built, and a means for the recipient to relink against a modified laz-perf.

The obligations, the link model, and the pre-publication checklist are in
[DISTRIBUTION.md](../../docs/guides/DISTRIBUTION.md). Read it before shipping a
binary. [pointcloud-las](../pointcloud-las/README.md) contains no laz-perf code
and carries none of this.

## Known limitations

- Compressed waveform formats 4, 5, 9, and 10 are rejected.
- Decoding is sequential; a point range does not skip decode work.
- Tiled reads spool points and reconstruct one tile at a time before payload
  authoring; large-corpus memory measurement remains open.
- A decode failure inside a chunk is reported as `LAZ003` even when its cause
  is a LAS record condition, because record interpretation is delegated to the
  LAS reader.
- Extra Bytes types 1-30 are supported, including vectors. Non-finite values
  and integers not exactly representable as `double` are rejected, and
  descriptor names are normalized to deterministic USD-safe names.
- CRS WKT is retained and EPSG is inferred from explicit WKT or GeoTIFF
  horizontal CRS keys. Conflicting definitions are rejected with a typed
  `ConflictingCrs` diagnostic.
- Decoding assumes a little-endian host.
- Writing LAZ is out of scope.

## Compatibility

| Item | Value |
| --- | --- |
| Declared OpenUSD range | `>=26.08,<27.0` |
| Validated OpenUSD | 26.08 |
| OpenStrata CLI | 0.22.0 |
| OpenStrata platform / profile | `cy2026` / `usd` |
| C++ standard | C++17 |
| Vendored codec | laz-perf 2.0.0 (LGPL-2.1) |
| Hosted CI | Windows 2022 (L0-L4), macOS 15 arm64 (L0-L5), Ubuntu 24.04 (L0-L5) |

Full statement: [OPENUSD.md](../../docs/compatibility/OPENUSD.md). Breaking
name changes: [MIGRATION.md](../../docs/compatibility/MIGRATION.md).

## Contracts and status

- [Diagnostics: the LAZxxx table](docs/DIAGNOSTICS.md)
- [Capability matrix](../../docs/reference/CAPABILITY_MATRIX.md)
- [LAZ codec decision (ADR 0002)](../../docs/adr/0002-laz-codec.md)
- [Binary distribution and licensing](../../docs/guides/DISTRIBUTION.md)
- [Plugin adapter contract](../../docs/architecture/PLUGIN_ADAPTER.md)
- [File-format argument contract](../../docs/architecture/FILE_FORMAT_ARGUMENTS.md)
- [Workspace contract](../../docs/architecture/WORKSPACE.md)
- [Roadmap](../../docs/roadmap/README.md)
