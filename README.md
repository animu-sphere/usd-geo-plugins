# OpenUSD Point Cloud Plugins

[![ost source ci](https://github.com/animu-sphere/usd-pointcloud-plugins/actions/workflows/ost-source-ci.yml/badge.svg?branch=main)](https://github.com/animu-sphere/usd-pointcloud-plugins/actions/workflows/ost-source-ci.yml)

OpenUSD FileFormat Plugins and libraries for point-cloud data. The project
opens LAS, LAZ, local or resolver-backed COPC, and PLY sources through shared
point-cloud contracts for surveying, mapping, scanning, and 3D data-exchange
workflows.

<p align="center">
  <img src="docs/assets/openLASwithHydraStorm.gif" alt="LAS file preview through Hydra Storm" width="640" />
  <br>
  <p align="center"><i>The video shows direct preview of a LAS file through Hydra Storm in usdview.</i></p>
</p>

**What it does**

- Reads LAS 1.2-1.4 and LAZ point records through format-specific plugins.
- Reads local and resolver-backed COPC metadata, hierarchy, and selected byte
  ranges through the active OpenUSD `ArResolver`, without implementing any
  network transport.
- Reads scalar PLY vertex properties through ASCII and binary point streams.
- Authors `UsdGeomPoints`, metadata, OpenUSD 26.08 `usdLod` roots, and
  payload-backed spatial tiles through shared authoring.
- Supports metadata-only inspection, bounded-memory tiled generation, stable
  `LASxxx` / `LAZxxx` / `COPCxxx` diagnostics, and explicit conversion for
  long-running tiled jobs.

Rendering is the consuming application's responsibility. These plugins provide
import and USD authoring, not a point-cloud renderer, and LOD selection stays
with the host application. See the
[tile and LOD contract](docs/architecture/LOD.md).

## Supported Formats

| Extension | Plugin | Current support |
| --- | --- | --- |
| `.las` | `pointcloud-las` | LAS 1.2-1.4, point formats 0-10, CRS, waveform metadata, and Extra Bytes |
| `.laz` | `pointcloud-laz` | Compressed point formats supported by the bundled `laz-perf` adapter |
| `.copc` | `pointcloud-copc` | Local or resolver-backed metadata-only, direct, and native-hierarchy tiled reads |
| `.ply` | `pointcloud-ply` | Scalar vertex reads, explicit CRS, bounded streaming, and payload-backed tiled reads |

Point formats 4 and 5 require LAS 1.3 or newer; formats 6-10 require LAS 1.4.
The LAZ adapter rejects waveform formats because the bundled `laz-perf` codec
does not provide their compressed record decoders.

| Attribute | Status |
| --- | --- |
| XYZ, intensity, return number, number of returns, classification | Authored |
| RGB (formats 2, 3, 7, 8, 10) | Authored |
| GPS time (formats 1, 3, 6-10) | Authored |
| NIR, scan angle, user data, point source ID, classification flags, scanner channel, scan direction, edge of flight line | Authored |
| Waveform packet metadata and external `.wdp` reference (LAS formats 4, 5, 9, 10) | Authored |
| Extra Bytes point attributes | Types 1-30 authored as scalar/vector USD attributes |

Extra Bytes are authored as `geo:<normalized descriptor name>` with the
descriptor scale and offset applied. Scalar types 1-10 become `double[]`,
vector types 11-20 become `double2[]`, and vector types 21-30 become
`double3[]`. Non-finite values and integer values that are not exactly
representable as `double` are rejected. Names are normalized deterministically
to ASCII USD identifiers; original names remain in header metadata. See the
[capability matrix](docs/reference/CAPABILITY_MATRIX.md).

The complete matrix, including VLR, CRS, and authored USD attributes, is in
the [capability matrix](docs/reference/CAPABILITY_MATRIX.md).

The [capability matrix](docs/reference/CAPABILITY_MATRIX.md) is the canonical
source for point formats, attributes, CRS, metadata, and authored USD details.
Source identity, caching, adaptive tiling, real-world I/O baselines, and one
tile-plan representation for sequential and COPC-native planning are
implemented, including conservative resolver-backed cache reuse and recorded
external resolver interoperability. Current work favors COPC stability,
reproducible real-world baselines, temporary-layer ownership, and CI cost
before E57 or other
[format expansion](docs/roadmap/infrastructure-maturity.md). Raster and vector
work belongs to separate repositories under the
[ecosystem strategy](docs/design/ECOSYSTEM_STRATEGY.md).

## Resolver-Backed Sources

Resolver-backed COPC reads operate through the active OpenUSD `ArResolver`.
Network transport, authentication, retries, and raw byte caching are owned by
the selected resolver implementation, not by this repository. The plugins
consume the resolved `ArAsset` and never learn which resolver opened it.

An external resolver is runtime composition: install it alongside these
bundles and register both through `PXR_PLUGINPATH_NAME`.
[`usd-http-resolver`](https://github.com/animu-sphere/usd-http-resolver) is one
compatible implementation, not a required dependency — this repository builds
and tests without it. The repository-local
`tests/plugins/httpresolver` fixture is a Tier 1 test double that serves a local
fixture in memory; it is not a network transport or product bundle.

Generated-USDC cache reuse for resolver-backed sources requires stable source
identity and stays disabled when the resolver cannot supply it, with the reason
reported through a stable category rather than silently. The boundary and the
identity model are in the
[resolver-backed source contract](docs/architecture/RESOLVER_SOURCE.md); what a
remote read actually costs is recorded in the
[resolver read baseline](docs/reference/RESOLVER_BASELINE.md).

## Quick Start

Requirements: CMake 3.23+, a C++17 compiler, OpenUSD 26.08 (the plugin
contract accepts `>=26.08,<27.0`), and OpenStrata 0.22.7 for the pinned
workspace build.

Build and test everything through the pinned OpenStrata workspace:

```powershell
ost configure
ost build
ost test
```

Build and verify a single bundle:

```powershell
ost plugin build .\plugins\pointcloud-las
ost plugin test .\plugins\pointcloud-las --up-to 4
```

Generate a tiled, payload-backed asset explicitly for production processing:

```powershell
usd-pointcloud-convert `
  C:\path\to\sample.las `
  C:\path\to\output\PointCloud.usda `
  --tile-size 128 `
  --memory-limit 1048576
```

The converter is the operational path for long-running tiled generation. It
accepts local `.las`, `.laz`, `.copc`, and `.copc.laz` inputs and
publishes the root layer and a deterministic `PointCloud.usda.manifest`
sidecar after generation completes; static FileFormat tiled reads remain
available for preview and small inputs. The sidecar records normalized
generation arguments and relative payload asset paths. The converter can reuse
committed deterministic USDC cache entries through `--cache-root`; direct
LAS, LAZ, COPC, and PLY FileFormat cache lookup uses `USDGEO_CACHE_ROOT`.

Build and test the libraries with plain CMake — without an OpenUSD runtime,
this covers `usdGeoCore`, `usdPointCloudCore`, `usdLas`, and `usdLaz`:

```powershell
cmake -S . -B build -DUSDGEO_BUILD_TESTS=ON
cmake --build build --config Release
ctest --test-dir build -C Release --output-on-failure
```

The full surface is in [BUILDING.md](docs/guides/BUILDING.md).

## Using the Plugins

Point `PXR_PLUGINPATH_NAME` at the directory holding `plugInfo.json`. A
trailing slash makes OpenUSD search subdirectories, registering both bundles at
once:

```powershell
$env:PXR_PLUGINPATH_NAME = "C:\path\to\pointcloud-las\plugin\resources\"
```

```bash
export PXR_PLUGINPATH_NAME=/path/to/pointcloud-las/plugin/resources/
```

Then open a file with any OpenUSD tool, or reference it from a layer:

```bash
usdview sample.las
usdcat --flatten sample.las -o sample.usda
```

```usda
def "Survey" (
    references = @./sample.las:SDF_FORMAT_ARGS:lod=balanced@
)
{
}
```

The supported compact profiles are `off`, `preview`, `balanced`, and
`quality`. `tile=true` connects the LAS/LAZ/COPC/PLY stream to spill-backed,
payload-backed spatial tiling. `tileSize`, `tileMemoryLimit`, and
`payloadDirectory` control the tiled output. Registration details are in
[INSTALL.md](docs/guides/INSTALL.md); the full argument surface is in the
[file-format argument contract](docs/architecture/FILE_FORMAT_ARGUMENTS.md).

### Previewing without installing

`ost plugin view` prepares the managed OpenUSD runtime and the plugin
environment for a bundle, so a built bundle can be inspected in place:

```powershell
ost plugin view `
  .\plugins\pointcloud-las `
  C:\path\to\sample.las `
  --with .\plugins\pointcloud-laz
```

```bash
ost plugin view \
  ./plugins/pointcloud-las \
  /path/to/sample.las \
  --with ./plugins/pointcloud-laz
```

`--with` makes one session discover both LAS and LAZ. Compact LOD profiles are
passed in the standard USD format-argument suffix:

```powershell
ost plugin view `
  .\plugins\pointcloud-las `
  'C:\path\to\sample.las:SDF_FORMAT_ARGS:lod=balanced' `
  --with .\plugins\pointcloud-laz
```

To run any other USD tool under the same composed environment:

```powershell
ost plugin run .\plugins\pointcloud-las -- usdcat --flatten sample.las -o sample.usda
```

### Authored result

```usda
#usda 1.0
(
    metersPerUnit = 1
    upAxis = "Y"
)

def Points "PointCloud"
{
    point3f[] points = [(0, 0, 0), (1.42, 0.31, -2.07), ...]
    custom uchar[] geo:classification = [2, 2, 6, ...]
    custom int[] geo:intensity = [1024, 981, ...]
    custom uchar[] geo:numberOfReturns = [1, 2, 2, ...]
    custom uchar[] geo:returnNumber = [1, 1, 2, ...]
    custom double3 geo:boundsMin = (0, 0, -84.31)
    custom double3 geo:boundsMax = (512.5, 43.2, 0)
    custom double3 geo:localOrigin = (-8242.5, -34212.5, 12.4)
    custom string geo:linearUnit = "metre"
    custom string geo:upAxis = "Y"
    custom int geo:epsgCode = 0
    custom string geo:wkt = "PROJCS[...]"
    custom uint64 geo:pointCount = 1048576
}
```

Positions are stage-local `float` values. Source coordinates are recovered
from `geo:localOrigin`; the exact expression is in the
[capability matrix](docs/reference/CAPABILITY_MATRIX.md).

## Tiling and LOD Status

Library capability and FileFormat integration are deliberately reported
separately, because they differ today.

Authoring library (`usdPointCloudAuthoring`):

- `usdLod` hierarchy authoring: implemented
- spatial tiled authoring: implemented
- payload-backed tile output: implemented

LAS/LAZ/COPC/PLY FileFormat read path:

- compact non-spatial LOD profiles (`lod=preview|balanced|quality`):
  implemented
- spatial `tile` argument: implemented
- bounded-memory payload generation during file open: implemented
- PLY scalar point reads and payload-backed fixed-grid tiling: implemented

The read path consumes bounded pull-stream chunks, spools points by
source-coordinate tile, and authors one payload-backed level per tile. A
Windows baseline against the supplied 14.6-million-point Shizuoka LAS input
is recorded in [streaming and tiling](docs/roadmap/streaming-and-tiling.md);
payload working-set measurements are included, while broader real-world
dataset coverage remains open.

## Known Limitations

- Tiled reads use bounded chunk delivery and spill points to per-tile temporary
  files. A single Shizuoka LAS RSS/spool/payload baseline is published in the
  [streaming and tiling roadmap](docs/roadmap/streaming-and-tiling.md);
  broader real-world dataset coverage remains open.
- File-format arguments expose normalized attribute selection, chunked and
  range-based reads, compact `lod` profiles, spatial tiling, bounds filters,
  and classification filters. The two filters are evaluated in source
  coordinates before stage-local transforms.
- `metadataOnly` reads author the `/PointCloud` metadata namespace without
  decoding point records: source count, bounds, CRS, and available-attribute
  metadata, but no point positions.
- Extra Bytes support covers types 1-30. Non-finite values and integers not
  exactly representable as `double` are rejected; descriptor names are
  normalized deterministically before USD authoring.
- CRS WKT is retained and EPSG is inferred from explicit WKT or GeoTIFF
  horizontal CRS keys. Conflicting definitions are rejected with a typed
  `ConflictingCrs` diagnostic.
- The deterministic USDC cache is available to the conversion tool through
  `--cache-root`; direct FileFormat lookup reuses committed entries through
  `USDGEO_CACHE_ROOT`. Reuse requires either a stable local filesystem identity
  or a `Stable` resolver identity, and fails closed otherwise. The conversion
  tool publishes entries for local LAS, LAZ, and COPC inputs. Publishing an
  entry against resolver-provided identity is not yet supported.
- HTTP, cloud SDKs, authentication, retries, and raw byte-range caching are
  out of scope; they belong to the resolver implementation.
- Writing LAS, LAZ, or COPC is out of scope; all three plugins export as
  `usda`.

See the [implementation status](docs/roadmap/implementation-status.md) and
[roadmap](docs/roadmap/README.md) for the planned work.

## Status

Latest release: **v0.10.0** — generated-cache decisions explained through a
stable transport-neutral vocabulary, a revision-aware cache layout, the Tier 1
resolver contract gate running in CI on every host, and recorded
interoperability with a released external resolver. The cache layout change is
recorded in [MIGRATION.md](docs/compatibility/MIGRATION.md), along with the
v0.3.0 module and bundle rename. See the
[release record](docs/releases/v0.10.0.md) and
[CHANGELOG.md](CHANGELOG.md).

Next: format-independent depth continues over format count. The nearest open
items are publishing generated cache entries for COPC sources and accepting
resolver-addressable identifiers in the conversion tool, both of which the
[infrastructure maturity roadmap](docs/roadmap/infrastructure-maturity.md)
places before any new format.

Direction is fixed in the [design policy](docs/design/DESIGN_POLICY.md); the
structure is fixed in the
[workspace contract](docs/architecture/WORKSPACE.md).

## Architecture

```text
.las  -> usdLas  ----\
.laz  -> usdLaz  -----+--> usdPointCloudAuthoring --> pointcloud plugins --> stage
.copc -> usdCopc -----/
          ^
          usdGeoCore + usdPointCloudCore
          (shared geospatial and point-cloud contracts)
```

The repository separates format readers, shared validation, USD authoring, and
plugin adapters so the core tests remain independent of an OpenUSD runtime.
The `usdPointCloudTiling` module provides format-independent tile routing and
spool contracts for the streaming authoring path. Readers remain independent
of OpenUSD and the tiling library remains independent of LAS and LAZ; the
plugin adapters connect those pieces through shared APIs. The binding version
of this is
[WORKSPACE.md](docs/architecture/WORKSPACE.md).

## Repository Layout

```text
libs/usd-geo-core/              Geospatial values, transforms, bounds, cache keys, diagnostics
libs/usd-geo-cache/             Source identity, deterministic cache keys, layout, and lookup
libs/usd-pointcloud-core/       Point attribute, chunk, read-option, sampling, and LOD contracts
libs/usd-pointcloud-tiling/     Fixed-grid and adaptive partitioning, spool routing, tile manifests
libs/usd-pointcloud-authoring/  OpenUSD point-cloud, usdLod, and payload authoring
libs/usd-las/                   LAS header, metadata, and point-record reader
libs/usd-laz/                   LAZ chunk reader and laz-perf adapter
libs/usd-copc/                  COPC metadata, hierarchy, and source-backed range reader
libs/usd-ply/                   PLY 1.0 header inspection and scalar vertex decoding
plugins/pointcloud-las/         LAS OpenUSD FileFormat Plugin
plugins/pointcloud-laz/         LAZ OpenUSD FileFormat Plugin
plugins/pointcloud-copc/        COPC OpenUSD FileFormat Plugin
plugins/pointcloud-ply/         PLY OpenUSD FileFormat Plugin
tests/plugins/httpresolver/     Tier 1 resolver fixture, excluded from product discovery
docs/                           See docs/README.md for the documentation index
```

Every module under `libs/` and `plugins/` carries its own `README.md` stating
what it owns and refuses to own; that README is part of the module contract.
See the [module README contract](docs/contributing/MODULE_README_CONTRACT.md).

## Documentation

Start at the [documentation index](docs/README.md).

- [Workspace contract](docs/architecture/WORKSPACE.md) — structure and
  dependency directions
- [Capability matrix](docs/reference/CAPABILITY_MATRIX.md) — what is supported
  today
- [Building and testing](docs/guides/BUILDING.md) ·
  [Installing](docs/guides/INSTALL.md) ·
  [Binary distribution and licensing](docs/guides/DISTRIBUTION.md)
- [Design policy](docs/design/DESIGN_POLICY.md)
- [Tile and LOD contract](docs/architecture/LOD.md) ·
  [Resolver-backed sources](docs/architecture/RESOLVER_SOURCE.md) ·
  [Plugin adapter](docs/architecture/PLUGIN_ADAPTER.md) ·
  [File-format arguments](docs/architecture/FILE_FORMAT_ARGUMENTS.md) ·
  [Point reader](docs/architecture/POINT_READER.md) ·
  [Diagnostics](docs/architecture/DIAGNOSTICS.md)
- [Roadmap](docs/roadmap/README.md) ·
  [Streaming and tiling](docs/roadmap/streaming-and-tiling.md) ·
  [Implementation status](docs/roadmap/implementation-status.md)
- [OpenUSD compatibility](docs/compatibility/OPENUSD.md) ·
  [Migration](docs/compatibility/MIGRATION.md)
- [Release records](docs/releases/README.md) ·
  [Third-party notices](THIRD_PARTY_NOTICES.md)

## License

Project code is licensed under the Apache License 2.0; see [LICENSE](LICENSE)
and [NOTICE](NOTICE). The LAZ adapter incorporates `laz-perf 2.0.0`, which is
distributed under LGPL-2.1, and the `pointcloud-laz` plugin binary therefore
contains LGPL-2.1 code. Redistributing `pointcloud-laz` binaries carries the
obligations described in
[binary distribution](docs/guides/DISTRIBUTION.md); see
[THIRD_PARTY_NOTICES.md](THIRD_PARTY_NOTICES.md) for the applicable
third-party terms. The `pointcloud-las` plugin contains no laz-perf code.
