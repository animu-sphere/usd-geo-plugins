# Workspace contract

This is the binding structural contract for `usd-pointcloud-plugins`. It fixes module
identities, dependency directions, root responsibilities, artifact naming, and
change invariants. A structural change that contradicts this document must
change this document first.

Status: `usdGeoCore`, `usdGeoCache`, `usdPointCloudCore`,
`usdPointCloudTiling`, `usdPointCloudAuthoring`, `usdLas`, `usdLaz`, `usdCopc`,
and `usdPly` are implemented, as are the `pointcloud-las`, `pointcloud-laz`,
`pointcloud-copc`, and `pointcloud-ply` bundles. Resolver-backed COPC random
access is implemented on `main`; production network transport remains the
active resolver's responsibility, and no resolver implementation is a
build-time dependency of this workspace. The resolver-facing behavior these
modules implement is fixed in
[RESOLVER_SOURCE.md](RESOLVER_SOURCE.md).

## 1. Components

| Identity | Directory | Kind | Status | Responsibility |
| --- | --- | --- | --- | --- |
| `usdGeoCore` | `libs/usd-geo-core` | plain CMake/OpenStrata static library | implemented | Format-independent geospatial values: coordinates and local-origin transforms, spatial bounds, CRS-related values, deterministic tile IDs, normalized cache-key inputs, and the typed diagnostic vocabulary shared across modules. |
| `usdPointCloudCore` | `libs/usd-pointcloud-core` | plain CMake/OpenStrata static library | implemented | Format-independent point-cloud contracts: point attribute definitions and types, `PointChunk` / `PointData` / `PointCloudAsset`, multi-scan `PointCloudScan` / `PointCloudCollection` values, point validation, attribute selection, range and chunk read options, deterministic sampling, and the shared LOD hierarchy and tile value types. |
| `usdLas` | `libs/usd-las` | plain CMake/OpenStrata static library | implemented | LAS parsing and decoding: 1.2-1.4 headers, VLR/EVLR, point formats 0-10, waveform packet metadata, GeoTIFF key records, scalar and vector Extra Bytes, metadata-only reads, and chunked and range-based reads behind `LasReader`. |
| `usdLaz` | `libs/usd-laz` | plain CMake/OpenStrata static library | implemented | LAZ decompression through the vendored laz-perf codec, converted into the shared LAS and point-cloud contracts, with metadata-only reads, chunked reads, and stable diagnostics for unsupported compressed formats. Owns the vendored laz-perf subset. |
| `usdPointCloudAuthoring` | `libs/usd-pointcloud-authoring` | plain CMake/OpenStrata static library | implemented | Shared OpenUSD authoring: `UsdGeomPoints` and geospatial metadata authoring, point-attribute authoring, `usdLod` hierarchy authoring, tiled point-cloud authoring, payload-backed tile assets and the ownership of generated payload files, root layer generation, and stage/layer validation. |
| `pointcloud-las` | `plugins/pointcloud-las` | OpenStrata plugin bundle (`usd-fileformat`) | implemented | OpenUSD `SdfFileFormat` adapter for `.las`: plugin registration, argument normalization, `LasReader` construction, and authoring through the shared library. Owns its `LASxxx` diagnostic codes. |
| `pointcloud-laz` | `plugins/pointcloud-laz` | OpenStrata plugin bundle (`usd-fileformat`) | implemented | The same adapter shape for `.laz`, using `LazReader` and the laz-perf codec integration. Owns its `LAZxxx` diagnostic codes. |
| `usdCopc` | `libs/usd-copc` | plain CMake/OpenStrata static library | implemented (source-backed) | COPC Info and hierarchy validation, project-owned random-access source integration, selective point-data range decoding through the shared LAZ chunk decoder, and native hierarchy streaming. It remains OpenUSD- and transport-independent. |
| `pointcloud-copc` | `plugins/pointcloud-copc` | OpenStrata plugin bundle (`usd-fileformat`) | implemented (resolver-backed read) | Resolver-opened `ArAsset` adaptation, metadata-only and non-tiled reads, and native hierarchy tiled COPC authoring through shared `usdLod`. Remote tiled reads require a local payload directory; source point ranges remain unsupported. |
| `httpresolver` | `tests/plugins/httpresolver` | test-only OpenUSD `ArResolver` fixture | Tier 1 fixture | OpenUSD `ArResolver` test double for `http://memory.copc` and `https://memory.copc`, serving a configured local fixture as an in-memory `ArAsset`. It has no network transport or product bundle manifest, is built only with COPC integration tests, and is excluded from product discovery and release matrices. |
| `usdPointCloudTiling` | `libs/usd-pointcloud-tiling` | plain CMake/OpenStrata static library | implemented | Format-independent fixed-grid partitioning, spill-backed bounded-memory routing, deterministic tile and LOD ordering, validated tile manifest serialization, spool validation, and cleanup contracts. See the [streaming and tiling plan](../roadmap/streaming-and-tiling.md). |
| `usdGeoCache` | `libs/usd-geo-cache` | plain CMake/OpenStrata static library | implemented | Descriptor-based stable cache keys, deterministic USDC root/payload layout, machine-readable lookup states, process-local lookup statistics, the stable cache-decision vocabulary, and entry invalidation. Entries are addressed by a generation key over what the caller asked for and a source identity key over everything read out of the source, so revisions of one source are siblings. The conversion tool owns generation and atomic publication; direct FileFormat adapters reuse committed entries through `USDGEO_CACHE_ROOT`. |
| `usdPly` | `libs/usd-ply` | plain CMake/OpenStrata static library | implemented | PLY 1.0 header inspection, scalar vertex decoding, source filters, and explicit georeference conversion into shared point-cloud assets. |
| `usdAsciiPoints`, `usdE57` | `libs/` | plain libraries | reserved, not implemented | Additional point-cloud readers targeting the same shared contracts. The multi-scan collection contract needed by E57 is implemented in `usdPointCloudCore`; the decoder remains reserved. |
| `pointcloud-ply` | `plugins/pointcloud-ply` | OpenStrata plugin bundle (`usd-fileformat`) | implemented | Thin `.ply` adapter requiring an explicit `epsg` argument and authoring through shared point-cloud contracts. |
| `pointcloud-points-text`, `pointcloud-e57` | `plugins/` | reserved, not implemented | Future point-cloud FileFormat Plugin adapters, in the order fixed by [format support order](../roadmap/format-support-order.md). |

Terrain, raster, and vector contracts belong to future repository candidates:
`usd-raster-plugins` and `usd-vector-plugins`. They are not reserved modules
or active roadmap phases in this repository; their ownership is defined in the
[ecosystem strategy](../design/ECOSYSTEM_STRATEGY.md).

None of the `libs/` modules is a plugin: none has a `plugInfo.json`, none
performs plugin registration, and only `usdPointCloudAuthoring` exposes OpenUSD
types. Only create a reserved directory when its first tested capability is
implemented; the table describes ownership boundaries, not a requirement to
scaffold empty modules.

`.csv` and `.json` are generic extensions. The bundles that would claim them
never do so by default; a host opts in through an explicit file-format
selection, and the readers require the arguments they need instead of
inferring a layout.

## 2. Dependency directions

Allowed today:

```text
usdPointCloudCore      -> usdGeoCore
usdGeoCache            -> usdGeoCore
usdLas                 -> usdGeoCore, usdPointCloudCore
usdLaz                 -> usdLas, usdPointCloudCore
usdCopc                -> usdLas, usdLaz, usdPointCloudCore
usdPly                 -> usdGeoCore
usdLaz                 -> laz-perf (private, vendored codec implementation)
usdPointCloudAuthoring -> usdGeoCore, usdPointCloudCore
usdPointCloudAuthoring -> OpenUSD (usdGeom, usdLod)
pointcloud-las         -> usdLas, usdPointCloudAuthoring, OpenUSD
pointcloud-laz         -> usdLaz, usdPointCloudAuthoring, OpenUSD
pointcloud-copc        -> usdCopc, usdPointCloudAuthoring, OpenUSD
```

Reserved future directions:

```text
usdPointCloudTiling    -> usdGeoCore, usdPointCloudCore
pointcloud-las         -> usdPointCloudTiling
pointcloud-laz         -> usdPointCloudTiling
any format reader      -> usdGeoCore, usdPointCloudCore
any format bundle      -> usdPointCloudAuthoring
```

Forbidden:

```text
usdGeoCore             -> anything (OpenUSD, a reader, a codec, PROJ, GDAL)
usdPointCloudCore      -> OpenUSD, LAS/LAZ, or usdLod schema types
usdLas or usdLaz       -> OpenUSD, tiling policy, payload generation,
                          plugin registration
usdPointCloudTiling    -> LAS/LAZ parsing, OpenUSD, plugin registration
usdPointCloudAuthoring -> LAS/LAZ decoding, spatial partitioning policy,
                          plugin argument parsing, renderer implementation
any plugin bundle      -> another plugin bundle
any dependency cycle
```

The readers must not depend on the tiling or OpenUSD authoring modules, and the
tiling module must not depend on LAS or LAZ. The plugin adapters are what
compose readers, tiling, and OpenUSD authoring.

Each `libs/*/openstrata.library.yaml` gives the plain library a workspace
identity and CMake package/target. A bundle declares the edge in its manifest;
`ost plugin build/test/package` resolves and executes it.

| Identity | CMake package | CMake target |
| --- | --- | --- |
| `usdGeoCore` | `usdGeoCore` | `usdgeo::core` |
| `usdPointCloudCore` | `usdPointCloudCore` | `usdpointcloud::core` |
| `usdPointCloudAuthoring` | `usdPointCloudAuthoring` | `usdpointcloud::authoring` |
| `usdPointCloudTiling` | `usdPointCloudTiling` | `usdpointcloud::tiling` |
| `usdGeoCache` | `usdGeoCache` | `usdgeo::cache` |
| `usdLas` | `usdLas` | `usdlas::core` |
| `usdLaz` | `usdLaz` | `usdlaz::core` |

## 3. Source boundaries

```text
plugins/pointcloud-las/src/UsdGeoLasFileFormat.cpp
    thin SdfFileFormat integration: argument normalization, reader call,
    authoring call, diagnostic projection

plugins/pointcloud-las/include/usdgeolas/UsdGeoLasDiagnostics.h
    the bundle's stable LASxxx codes

plugins/pointcloud-laz/src/UsdGeoLazFileFormat.cpp
    the same integration for .laz

plugins/pointcloud-laz/include/usdgeolaz/UsdGeoLazDiagnostics.h
    the bundle's stable LAZxxx codes

libs/usd-las/
    LAS header, VLR/EVLR, point-record decoding, attribute fan-out,
    GeoReference and bounds construction, LasReader orchestration

libs/usd-laz/
    laz-perf isolation and the equivalent LazReader orchestration

libs/usd-copc/
    COPC metadata, hierarchy, and local or resolver-backed range decoding
    through the shared LAZ decoder and an OpenUSD-independent source contract

libs/usd-pointcloud-core/
    the point schema, chunk contracts, read options, sampling, LOD values
    every reader and the authoring library share

libs/usd-geo-core/
    format- and USD-independent geospatial values and diagnostics

libs/usd-geo-cache/
    format- and USD-independent cache identity, layout, lookup, and invalidation

libs/usd-pointcloud-authoring/
    OpenUSD authoring, shared by every format bundle
```

The C++ namespaces are `usdgeo`, `usdpointcloud`, `usdlas`, `usdlaz`, `usdcopc`,
and the per-bundle `usdgeolas` / `usdgeolaz` / `usdgeocopc`. Directory names, CMake target names, and
C++ namespaces are deliberately not required to be identical: the external
bundle name uses the explicit `geospatial` term while the internal C++ prefix
stays `UsdGeo` to avoid excessively long symbols. The authoring library keeps
the `usdgeo` namespace and the `include/usdgeo/` header path it had as
`usdPointCloudAuthoring`.

Plugin C++ sources, `plugInfo.json`, format fixtures, and the per-bundle
diagnostic tables belong to the bundle. Each bundle must remain buildable
through `ost` as an independent bundle; the root CMake build is an additional
supported path.

## 4. Root responsibilities

The repository root owns composition, not module implementation:

- the plain CMake build that wires `libs/` and `plugins/` together;
- workspace-wide version and OpenStrata platform/profile selection;
- `openstrata.ci.yaml` and generated CI;
- shared licensing, third-party notices, documentation, and release records;
- cross-format equivalence tests and aggregate packaging.

## 5. Authored stage contract

Every read using the shared authoring contract produces, for `lod=off`:

```text
/PointCloud             UsdGeomPoints
```

The stage is Y-up with one meter per unit. Positions are stage-local `float`
values relative to `geo:localOrigin`; source coordinates are recovered through
that origin. Compact LOD profiles author a single non-tiled `usdLod` root at
the same path. The tiled and payload-backed shapes the authoring library can
also produce, and the exact metadata authored on each prim, are fixed in
[LOD.md](LOD.md) and
[CAPABILITY_MATRIX.md](../reference/CAPABILITY_MATRIX.md).

Spatial partitioning uses source horizontal coordinates while OpenUSD
authoring uses stage-local coordinates. This keeps tile identity stable across
stage up-axis conversion and preserves the meaning of projected CRS
coordinates. See [ADR 0001](../adr/0001-coordinate-model.md).

## 6. Artifact naming and versioning

Per-bundle artifacts use OpenStrata's target-qualified convention:

```text
pointcloud-las-<version>-<target>.tar.zst
pointcloud-laz-<version>-<target>.tar.zst
pointcloud-copc-<version>-<target>.tar.zst
```

The installed shared libraries are `UsdGeoLasFileFormat`, `UsdGeoLazFileFormat`,
and `UsdGeoCopcFileFormat`, and the registered `plugInfo.json` type names match
them. Until a real need for independent release cadences appears, every bundle
and plain library mirrors the repository-root `VERSION`. Git tags use `vX.Y.Z`.

## 7. Change invariants

Every structural or format change preserves these invariants:

1. Public APIs use project-owned value types and standard-library types.
2. Source coordinates and stage-local coordinates use distinct names and
   explicit transforms.
3. Readers return deterministic, validated intermediate data and never author
   USD directly.
4. File-format arguments are normalized before reader or cache lookup, and
   normalized arguments participate in layer identity. See
   [FILE_FORMAT_ARGUMENTS.md](FILE_FORMAT_ARGUMENTS.md).
5. Unknown source metadata is preserved or reported; it is never silently
   discarded.
6. Tile, LOD, and cache contracts are shared, while format-specific spatial
   indexes remain private.
7. Spatial tiling and level of detail stay separate concepts; neither collapses
   into the other.
8. `UsdLodRootAPI`, Hydra, cameras, viewport state, and screen-space math never
   appear in a reader or a plugin adapter. See [LOD.md](LOD.md).
9. A new format reaches USD through `PointCloudAsset` and the shared authoring
   entry point, not by copying LAS assumptions into a new writer.
10. Manifest and CMake dependency declarations change together.
11. Plugin registration changes include a discovery test.
12. Third-party revision and license changes update both notices and package
    verification.
13. A change that modifies a module contract updates that module's `README.md`
    in the same pull request. See
    [MODULE_README_CONTRACT.md](../contributing/MODULE_README_CONTRACT.md).
14. Write support is deferred until read behavior and preservation rules are
    stable.
15. No resolver implementation is a build-time dependency. The workspace
    contains no CMake dependency, submodule, vendored transport library,
    resolver-specific include, or link dependency on one; external resolvers
    compose at runtime. See [RESOLVER_SOURCE.md](RESOLVER_SOURCE.md).
16. Transport-specific concepts do not enter shared contracts. Resolver
    validation tokens are opaque, and credentials, authorization headers,
    signed URLs, and tokens are never persisted into manifests, cache
    descriptors, or diagnostics.

## 8. Build and packaging

- Use OpenStrata manifests for libraries and plugin bundles.
- Keep the root CMake build working without `ost` for local development.
- Keep large dependencies optional and scoped to the owning target.
- Test pure libraries without requiring an OpenUSD runtime where possible.
    `usdGeoCore`, `usdPointCloudCore`, `usdLas`, `usdLaz`, `usdCopc`, and `usdPly`
        build and test with no OpenUSD runtime; `usdPointCloudAuthoring` and all
        plugin bundles require one.
- Validate plugin bundles with the pinned OpenStrata `cy2026` / `usd` runtime.

## 9. CI and verification contract

`openstrata.ci.yaml` is the source of truth; the GitHub workflow is generated
by `ost ci generate github`. The declared PR matrix runs every production
bundle on every host, plus a `kind: workspace` cell per host on both lanes.

The two cell kinds exist for different reasons. A bundle cell builds one
plugin from `plugins/pointcloud-<x>/CMakeLists.txt`, a standalone `project()`
that never declares `USDGEO_BUILD_TESTS`, so it cannot compile the repository's
test suite or the Tier 1 resolver test double. A workspace cell configures the
repository root, where `USDGEO_BUILD_TESTS` defaults to `ON`, and runs its
CTest suite. The workspace cells are therefore what makes the Tier 1 resolver
contract gate a CI gate rather than a local-only one:

| Host | Target | Bundle cells | Workspace cell |
| --- | --- | --- | --- |
| Windows 2022 x86_64 | cy2026 / USD | L0-L4 | `ost build` + `ost test` |
| macOS 15 arm64 | cy2026 / USD | L0-L5 | `ost build` + `ost test` |
| Ubuntu 24.04 x86_64 | cy2026 / USD | L0-L5 | `ost build` + `ost test` |

The required local gate is:

```text
ost configure
ost build
ost test
ost plugin build plugins/pointcloud-las
ost plugin build plugins/pointcloud-laz
ost plugin build plugins/pointcloud-copc
ost plugin test plugins/pointcloud-las --up-to 4
ost plugin test plugins/pointcloud-laz --up-to 4
ost plugin test plugins/pointcloud-copc --up-to 4
```

The LAS, LAZ, and PLY bundles declare OST smoke fixtures and run the L3
`usdcat.read` and L4 `python.stage_open` checks. The `pointcloud-copc` bundle
declares none, so those two levels skip for it and its L2 discovery check is
the extent of its bundle-cell coverage; its functional coverage is the Tier 1
integration test the workspace cells run. Declaring a checked-in COPC smoke
fixture is open work. The test-only `httpresolver`
bundle has no standalone fixture and no bundle cell; it is built transitively by
the COPC Tier 1 integration test, which the workspace cells and the local gate
both run. The COPC bundle follows the same runtime matrix as LAS and LAZ.

The gate must stay passable without any external resolver repository. From
`v0.10.0`, repository-local resolver contract tests (Tier 1) are the required
gate on every host and both lanes, carried by the workspace cells above.
Cross-repository integration against an external resolver implementation
(Tier 2) is composed at runtime and recorded separately; see
[RESOLVER_SOURCE.md](RESOLVER_SOURCE.md) §6 and
[RESOLVER_BASELINE.md](../reference/RESOLVER_BASELINE.md).

## 10. Delivery status

| Milestone | Boundary | Status |
| --- | --- | --- |
| v0.1.0 | shared geospatial and point-cloud contracts, LAS and LAZ readers, FileFormat Plugin integration | released 2026-08-01 |
| v0.2.0 | typed diagnostics, LAS 1.4 attributes and waveform formats, GeoTIFF key parsing, Extra Bytes, file-format arguments, shared authoring, `usdLod`, and metadata-only reads | released 2026-08-05 |
| v0.2.1 | bounded-memory streaming, spill-backed tile routing, conversion tooling, cache generation and lookup in the conversion tool | released |
| v0.3.0 | local COPC reader and FileFormat integration, native hierarchy streaming, shared LOD authoring, and all-bundle CI smoke coverage | released |
| v0.4.0 | PLY point decoding, adaptive display attributes, shared authoring, and payload-backed tiled reads | released 2026-08-12 |
| v0.5.0 | project-owned random access and resolver-backed COPC without transport coupling | released 2026-08-12 |
| v0.6.0 | source identity, cache hardening, diagnostics, recovery, and reuse baselines | released 2026-08-12 |
| v0.7.0 | point-budget-aware adaptive tiling | released 2026-08-13 |
| v0.8.0 | real-world measurement and I/O observability | released 2026-08-14 |
| v0.9.0 | TilePlan convergence and interactive validation | released 2026-08-15 |
| v0.10.0 | resolver-backed source identity, generated-cache decision diagnostics, Tier 1 as a CI gate, and recorded external resolver interoperability | released 2026-08-23 |

Current work and acceptance gaps are tracked in
[roadmap/implementation-status.md](../roadmap/implementation-status.md).

## Composed-runtime packaging (OST v0.22.8 dogfood)

The aggregate membership is explicitly pinned by `workspace.release_members` in
`openstrata.toml`. Component-owned acceptance probes are installed under
`share/usd-pointcloud-plugins/probes` through file-specific `workspace.install_data` mappings.
They exercise the packaged payload and are not runtime dependencies. Probe output
is caller-owned evidence and must not modify the immutable composed prefix.

Package producers need the aggregate payload projection correction found during
OST v0.22.8 dogfooding; stock ost 0.22.7 products advertise install sources absent
from their archive inventory. CI source builds remain pinned to released 0.22.7.
