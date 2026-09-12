# Changelog

All notable changes to this project are documented here.

## [Unreleased]

### Added

- Local `.copc` and `.copc.laz` inputs in `usd-pointcloud-convert`, including
  deterministic generated-cache publication and reuse.
- `PointCloudPayloadOptions::owner`, `PointCloudPayloadOwner`, and a
  layer-level `AuthorPointCloudTiledAssetWithPayloads` overload that reports
  typed diagnostics. A tiled FileFormat read publishes its payloads in
  `<payloadDirectory>/<owner>/<generation>/`, where the owner is a hash of the
  source (relative to the payload directory for local files) and the layer's
  exact file-format arguments. The layout is in the
  [file-format argument contract](docs/architecture/FILE_FORMAT_ARGUMENTS.md#generated-payload-ownership).

### Changed

- Layer authoring builds content in an in-memory stage that loads nothing, and
  payload files are exported from in-memory layers. No authoring call renames
  a caller's layer or gives a scratch layer a file identity any more.
- Tiled FileFormat payloads move from `payloadDirectory/Tile_*.usdc` into the
  layer's own generation directory below it. Generations are staged privately
  and published by rename: identical content reuses the published generation,
  changed content is published beside it, and the superseded one is removed.
- The COPC adapter authors native-hierarchy tiles through the shared
  layer-level entry point instead of creating and transferring its own stage.
- A tiled LAS, LAZ, or COPC read that cannot author reports the reason
  alongside `LAS016`, `LAZ007`, or `COPC007` instead of the bare code. PLY
  already did so under `PLY007`.

### Fixed

- Reading a tiled LAS, LAZ, PLY, or COPC layer again, for example in a new
  process or after the first layer was released, no longer fails because the
  payloads the first read generated already exist.
- Tiled COPC reads no longer warn `Could not open asset` for their own payloads
  when the working directory differs from the source directory.
- A generated-cache hit copies its payloads into a generation named by the
  entry, reusing an intact copy without writing and replacing a damaged one,
  instead of keeping whatever file already sat at the payload path.
- The authoring stream benchmark builds on macOS, where `std::uintmax_t` and
  `std::uint64_t` are distinct types.

### Compatibility

- Payload files written by earlier tiled reads directly into
  `payloadDirectory` are no longer used or touched; a read publishes a new
  generation below them. Delete the old `Tile_*.usdc` files to reclaim the
  space.

## [0.11.0] - 2026-08-27

Composable point-cloud product and packaged geospatial acceptance.

### Added

- Component-owned packaged probes for LAS, LAZ, PLY, and COPC registration and
  stage opening from an installed runtime prefix.
- The Tier 2 HTTP/COPC fixture server and integration runner are installed in
  the aggregate product, with compatibility wrappers retained at their former
  source-tree paths.

### Changed

- The workspace product declares exact release members and shared acceptance
  data for OpenStrata runtime composition.
- Canonical OpenUSD 26.08 pins and generated source CI are refreshed for the
  OpenStrata v0.22.8 release line.

## [0.10.0] - 2026-08-23

Resolver-backed source identity and external resolver interoperability. The
release record is [docs/releases/v0.10.0.md](docs/releases/v0.10.0.md).

### Added

- The resolver-neutral `ResolverAssetIdentity` contract and
  `Stable` / `Unstable` / `Unavailable` classification API in `usdGeoCache`,
  with the OpenUSD-facing adapter centralized in the shared authoring cache
  bridge so no format reader extracts identity itself.
- `usdgeo::cache::CacheDecision`: seven stable, transport-neutral categories
  that explain a cache decision, `CacheDecisionName` as the machine-matchable
  form, fixed `CacheDecisionMessage` constants, and `IdentityDecision` to map an
  identity stability onto its category.
- `HasSupersededIdentityEntry`, which distinguishes a changed validation
  identity from a source never generated before without persisting an
  identifier or a token.
- Cache-decision reporting through `TryLoadPointCloudCache`, projected onto four
  COPC codes: `COPC009` reuse disabled, `COPC010` reuse permitted or taken,
  `COPC011` identity changed, `COPC012` entry invalidated. Every message names
  its exact category.
- `kind: workspace` CI cells on Windows, macOS, and Linux for both lanes, which
  configure the repository root and run its CTest suite. They are what makes the
  Tier 1 resolver contract gate a CI gate.
- `tools/tier2_fixture_server.py`, a loopback origin that honours `Range` and
  logs every request, and `tools/tier2_resolver_integration.py`, the harness
  that composes it with an external resolver and the COPC FileFormat.
- The recorded Tier 2 baseline in
  [docs/reference/RESOLVER_BASELINE.md](docs/reference/RESOLVER_BASELINE.md),
  against `usd-http-resolver` v0.4.0 and the 81 MB Autzen COPC.

### Changed

- Generated cache entries are addressed by a generation key and a source
  identity key rather than one combined key. Revisions of one source are now
  siblings under one generation directory, which is what makes
  `resolver-identity-changed` reportable. The generation key carries caller
  intent; source size, modification time, the resolved georeference, and the
  new `Descriptor::sourceDerived` group - which the conversion tool's tile-plan
  key moved into - carry what was read out of the source.
- `Invalidate` removes an emptied generation directory, so an invalidated cache
  root does not accumulate empty parents.
- Removed standalone `httpresolver` product CI cells. The relocated test double
  is built transitively by the COPC Tier 1 integration test, which the workspace
  cells and the local gate both run.
- Added a shared cache-layout construction entry point so producer and consumer
  tests derive resolver-backed cache entries from the same descriptor contract.

### Fixed

- Windows CTest registrations for the authoring bridge, converter, and
  FileFormat integrations now prepend the OpenUSD imported `lib` and `bin`
  directories and the configured Python runtime to `PATH`, so all linked
  OpenUSD, TBB, and Python DLLs resolve in workspace CI without hiding host
  runtime DLLs.
- The PLY tiled-read integration test now writes payloads beside its fixture,
  keeping the generated payload references relative when a Windows workspace
  and the system temporary directory are on different volumes.
- Resolver cache Tier 1 coverage verifies cache hits, incomplete and corrupted
  entry invalidation, and validation-token changes through cache artifacts
  instead of process-local counters that are not shared across a FileFormat DLL
  boundary on Windows.

### Documentation

- Added the
  [resolver-backed source contract](docs/architecture/RESOLVER_SOURCE.md),
  covering the responsibility boundary, the transport-neutral `SourceIdentity`
  model, identity classification, generated-cache ownership and reuse rules,
  the diagnostics categories, the no-secrets rule, and the Tier 1 / Tier 2 test
  split. Every section is now marked shipped or explicitly not implemented.
- Stated that no resolver implementation is a build-time dependency, and that
  `usd-http-resolver` is one compatible implementation composed at runtime.
- Relocated the repository-local resolver test double to
  `tests/plugins/httpresolver` and documented its exclusion from the product
  surface and release matrix.
- Recorded the cache layout change in
  [MIGRATION.md](docs/compatibility/MIGRATION.md).
- Added an OpenStrata 0.22.2 dogfooding record for the external resolver
  skeleton; it identifies repository setup work, not an OpenStrata defect.

### Compatibility

- A `v0.9.0` cache root is never looked up under the new layout, so the first
  run after upgrading is a miss that regenerates. Cache entries are derived
  data; delete an old root to reclaim the space.
- `StableCacheKey`, `TryBuildLayout`, `Inspect`, `IsCacheHit`, and `Invalidate`
  keep their signatures and meanings. Tooling that enumerated entries with a
  single-level glob needs a second level.
- Existing LAS, LAZ, COPC, and PLY format ids, arguments, authored stage shape,
  and fixed-grid tiling behavior remain compatible with v0.9.0.

### Known limitations

- Nothing publishes a generated cache entry for a COPC source:
  `usd-pointcloud-convert` accepts `.las` and `.laz` local inputs only. Lookup,
  the reuse rules, and the decision diagnostics are complete; a measurable
  generated-cache hit ratio for a remote source waits on COPC generation.
- `usd-pointcloud-convert` does not accept resolver-addressable identifiers.
- The Tier 2 origin is loopback, so the recorded numbers are protocol and
  selectivity numbers rather than latency numbers.
- Raw byte-range caching and its hit ratios belong to the resolver.
- COPC writing and new public USD schemas remain deferred.

## [0.9.0] - 2026-08-15

### Added

- A versioned `TilePlan` contract covering tile identity, bounds, point counts,
  hierarchy relationships, source ranges, depth, and planner identity.
- Tile-plan cache identity through the deterministic
  `TilePlanCacheArguments` and `StableTilePlanKey` contracts.
- A reproducible Windows `usdview` host-responsiveness baseline using a
  payload-backed USGS 3DEP fixture and a documented five-key workload.

### Changed

- Adaptive point-budget planning now reaches the shared tile router through
  the `TilePlan` representation without changing its partitioning behavior.
- COPC native hierarchy nodes and byte ranges are mapped to `TilePlan` rather
  than re-derived through sequential planning.
- Sequential and COPC-native plans share the downstream payload authoring
  path, with equivalent authored output covered by tests.

### Compatibility

- Existing LAS, LAZ, COPC, and PLY format ids, arguments, authored stage shape,
  and fixed-grid tiling behavior remain compatible with v0.8.0.
- Planner identity and version are now explicit cache compatibility inputs;
  changing a planner algorithm invalidates incompatible generated output.

### Known limitations

- The host-responsiveness baseline measures bounded UI dispatch latency and
  working-set pressure; it does not measure renderer frame latency or a
  host-specific input-to-present time.
- Resolver-backed generated-cache reuse still requires a stable validation
  token, and remote range-cache ownership remains out of scope.
- Target payload-byte and spatial-size fallback policies, COPC writing, and
  new public USD schemas remain out of scope.

## [0.8.0] - 2026-08-14

### Added

- Reproducible fixed-grid versus adaptive real-world measurements for LAS,
  LAZ, COPC, and PLY, including tile distributions, RSS, source and spool
  I/O, payload bytes, processing time, and I/O amplification in TSV and JSON.
- A deterministic LAS 1.4 point-format-7 LAZ regression fixture covering RGB
  decoding and optional compressed substreams.

### Changed

- Windows spool authoring now supports fine-grained real-world runs beyond
  the CRT default of 512 simultaneously open files.
- The vendored LAZ reader accepts the compressor version emitted for LAS 1.4
  point formats 6-8 and safely skips absent optional point-format-7 streams.
- Payload authoring validates tile manifests only when a manifest is requested,
  allowing external payload directories for FileFormat reads.

### Compatibility

- Existing LAS, LAZ, COPC, and PLY format ids, arguments, authored stage shape,
  and fixed-grid tiling behavior remain compatible with v0.7.0.
- The v0.8.0 real-world matrix completed for Shizuoka LAS/LAZ, Autzen COPC,
  and Stanford Bunny PLY inputs on Windows with OpenUSD 26.08.

### Known limitations

- Host responsiveness during interactive output use remains unmeasured.
- Broader datasets remain build-local and are not shipped in plugin bundles.
- Target payload-byte and spatial-size fallback policies, COPC writing, and
  new public USD schemas remain out of scope.

## [0.7.0] - 2026-08-13

### Added

- Deterministic point-budget adaptive tiling with minimum and maximum points
  per tile, maximum depth, and typed planning diagnostics.
- Point-budget plan statistics covering point count, tile count, leaf density,
  split count, and reached depth.
- Reproducible cross-format streaming benchmark coverage for LAS, LAZ, COPC,
  and PLY, including a generated COPC fixture and a machine-readable report.

### Changed

- Adaptive planning now provides an additive path alongside the existing
  fixed-grid `tileSize` and `tileMemoryLimit` behavior.
- Tiling and streaming documentation now records the shared benchmark command
  shape and comparable payload, memory, tile, and elapsed-time metrics.

### Compatibility

- Existing LAS, LAZ, COPC, and PLY format ids, arguments, authored stage shape,
  and fixed-grid tiling behavior remain compatible with v0.6.0.
- Adaptive planning reports a typed failure when the requested point budget
  cannot be satisfied rather than silently relaxing the limit.

### Known limitations

- Broader real-world cross-format baselines remain dependent on representative
  local datasets that are not shipped in the repository.
- Target payload-byte and spatial-size fallback policies, COPC writing, and
  new public USD schemas remain out of scope.

## [0.6.0] - 2026-08-12

### Added

- Machine-readable generated-cache lookup states for missing, incomplete, hit,
  and invalid layouts, with stable lookup statistics and diagnostics.
- Cache recovery coverage for corrupt entries and interrupted publications,
  including committed-root and payload validation before reuse.
- Local and resolver-backed cache reuse baselines covering local miss-to-hit
  reuse and conservative resolver behavior when stable identity is absent.

### Changed

- Generated-cache identity now has explicit invalidation and compatibility
  behavior through the transport-independent `SourceIdentity` contract.
- Invalid cache entries are invalidated through the descriptor-derived entry
  path, while resolver-backed reuse remains disabled without a stable
  validation token.

### Compatibility

- Existing LAS, LAZ, COPC, and PLY format ids, arguments, authored stage shape,
  and plugin behavior remain compatible with v0.5.0.
- The generated-USDC cache remains separate from resolver or transport byte
  caches; cache reuse is opt-in through the existing cache configuration.

### Known limitations

- Remote generated-cache reuse requires stable resolver identity metadata and
  remains disabled when that metadata is unavailable.
- Range-cache ownership, adaptive tiling, and new public USD schemas remain
  out of scope.

## [0.5.0] - 2026-08-12

### Added

- Resolver-backed COPC reads through the active OpenUSD `ArResolver`, including
  `ArAsset` random access for local and resolver-provided assets.
- Typed COPC diagnostics for resolver-open failures, range failures, and short
  reads, plus local payload-directory validation for remote tiled reads.
- A transport-independent cache `SourceIdentity` contract and shared local
  filesystem identity construction for authoring and the conversion tool.
- HTTP resolver integration-test coverage on Windows, Linux, and macOS through
  the `httpresolver` plugin and the generated OpenStrata CI matrix.

### Changed

- COPC FileFormat reads now use the active resolver instead of requiring a
  project-owned HTTP transport or direct local-file access.
- Generated cache reuse remains enabled only for stable local filesystem
  identities, avoiding stale output when resolver identity is unavailable.
- Release and compatibility documentation now includes the resolver bundle and
  cross-platform source CI coverage.

### Compatibility

- Existing LAS, LAZ, COPC, and PLY format ids, arguments, authored stage shape,
  and local cache behavior remain compatible with v0.4.0.
- COPC resolver-backed reads require OpenUSD 26.08 and an active resolver that
  can provide the requested asset and byte ranges.

### Known limitations

- The repository does not include a production HTTP client, authentication,
  retries, network caching, or resolver-backed generated-cache identity.
- Remote COPC tiled reads require an absolute local `payloadDirectory`.
- COPC writing, adaptive tiling, and new public USD schemas remain out of
  scope.

## [0.4.0] - 2026-08-12

### Added

- PLY 1.0 scalar vertex decoding for ASCII, binary little-endian, and binary
  big-endian sources through the shared `PointStream` contract.
- Bounded source streaming for PLY with chunk limits, memory budgets, point
  ranges, bounds, classification filters, cancellation, RGB, intensity,
  classification, and generic scalar vertex attributes.
- The `pointcloud-ply` OpenUSD FileFormat Plugin with explicit EPSG input,
  shared point authoring, deterministic cache lookup, and payload-backed fixed-
  grid tiled authoring.
- PLY integration coverage using the checked-in Stanford Bunny corpus and a
  tiled conformance fixture.

### Changed

- Shared point authoring now emits adaptive point widths and `displayColor`
  when RGB data is available, with explicit 8-bit PLY and 16-bit LAS color
  normalization.
- PLY intensity values that are not integral are retained as generic scalar
  attributes without rescanning the source stream for every chunk.

### Fixed

- Tiled payload identifiers and LOD extent validation now remain consistent
  across COPC and PLY authoring.
- Missing LAS sources retain the source-open diagnostic classification.

### Compatibility

- Existing LAS, LAZ, and local COPC format ids, arguments, authored stage shape,
  and plugin behavior remain compatible with v0.3.0.
- PLY sources have no embedded CRS contract and require an explicit `epsg`
  file-format argument. PLY writing and metadata-only reads remain unsupported.

### Known limitations

- PLY faces and non-vertex mesh authoring are out of scope.
- PLY tiled planning uses the shared fixed-grid `tileSize` and
  `tileMemoryLimit` arguments; adaptive planning is deferred.

## [0.3.0] - 2026-08-11

### Added

- Local, read-only COPC metadata and hierarchy validation, selective point-data
  range reads, and native hierarchy point streaming through the shared LAZ
  decoder.
- The `pointcloud-copc` OpenUSD FileFormat Plugin for `.copc` inputs, including
  metadata-only, non-tiled, and hierarchy-backed tiled reads.
- Equivalence coverage across LAS, LAZ, and COPC point streams and authored LOD
  representations.
- Deterministic USDC cache generation for the conversion tool and cache lookup
  for direct LAS, LAZ, and COPC FileFormat reads through `USDGEO_CACHE_ROOT`.

### Changed

- Release products now include the LAS, LAZ, and COPC plugin bundles.
- COPC native hierarchy metadata is mapped to the shared tile and `usdLod`
  authoring contracts.

### Compatibility

- Existing LAS and LAZ format ids, file-format arguments, and authored stage
  shape remain compatible with v0.2.2.
- COPC support is local-only and requires OpenUSD 26.08 through the existing
  `cy2026` / `usd` runtime contract.

### Known limitations

- COPC writing, HTTP range sources, network caching, and new public USD schemas
  remain deferred.
- Writing LAS, LAZ, or COPC is out of scope; all three plugins export as
  `usda`.

## [0.2.2] - 2026-08-06

### Fixed

- Corrected the OpenStrata workspace, plugin manifests, and plugin CMake
  project versions so platform packages and product archives are named
  `0.2.2`.
- Added release metadata validation to the release workflow and documented the
  version update and tag order.

### Compatibility

- No runtime, file-format, or public API behavior changed from v0.2.1.

## [0.2.1] - 2026-08-06

### Added

- Bounds and classification filters for LAS and LAZ reads, normalized into
  canonical file-format arguments and applied in source coordinates.
- EPSG inference from explicit WKT and GeoTIFF horizontal CRS keys, with typed
  conflict diagnostics when CRS definitions disagree.
- An explicit LAS/LAZ conversion tool for tiled, payload-backed generation with
  deterministic manifest output.
- Real-dataset processing, RSS, spool, payload, and payload working-set
  measurements for the documented LAS and LAZ paths.

### Changed

- The explicit conversion tool is now the production entry point for
  long-running tiled generation; static FileFormat tiling remains available for
  preview, compatibility, and small inputs.
- Release and capability documentation now distinguishes implemented paths,
  measured paths, and remaining limitations.

### Fixed

- Cleanup of tiled authoring after cancellation and failure, including spool
  reader closure and payload-directory restrictions.
- Recovery of interrupted conversion transactions, making tiled conversion
  interruption-safe, and deterministic root and manifest publication.

### Known limitations

- Rendering is provided by the consuming OpenUSD application.
- Tiling uses a fixed-grid `tileSize`; adaptive depth and point-budget tile
  planning are not exposed through LAS/LAZ file-format arguments.
- A deterministic USDC cache is not implemented, and broader real-world
  dataset coverage remains open.
- Waveform sample data is retained as packet metadata and external `.wdp`
  references but is not fetched or interpreted.
- Writing LAS or LAZ is out of scope; the plugins export `usda`.

## [0.2.0] - 2026-08-05

### Changed — breaking

- Renamed `libs/usd-geo-usd` to `libs/usd-pointcloud-authoring`. The library id
  and CMake package become `usdPointCloudAuthoring` and the CMake target becomes
  `usdpointcloud::authoring`. `usdgeo::usd` remains as a deprecated alias and is
  removed in v0.3.0.
- Renamed `plugins/geo-las` to `plugins/geospatial-las` and `plugins/geo-laz` to
  `plugins/geospatial-laz`. The OpenStrata bundle names, `plugin/resources/`
  directories, CMake targets, installed shared libraries, and `plugInfo.json`
  type names all move to `geospatial-*` / `UsdGeoLasFileFormat` /
  `UsdGeoLazFileFormat`.
- Renamed the per-bundle header directories and diagnostic namespaces from
  `geolas` / `geolaz` to `usdgeolas` / `usdgeolaz`.

No behavior changed with the rename: the `las` and `laz` format ids, the
`.las` / `.laz` extensions, every `LASxxx` / `LAZxxx` diagnostic code, every
file-format argument, and the authored stage are all unchanged. Consumers must
update `PXR_PLUGINPATH_NAME`, bundle paths, and any script naming the shared
libraries directly. The complete before/after table and checklist are in
[docs/compatibility/MIGRATION.md](docs/compatibility/MIGRATION.md).

### Added

- Scalar LAS Extra Bytes point attributes, authored as `geo:<name>` (`double[]`)
  with descriptor scale and offset applied. Types 1-10 are supported; vector
  types, non-finite values, and integers not exactly representable as `double`
  are rejected.
- GeoTIFF CRS VLR parsing: key directory, double parameters, and ASCII
  parameters are parsed and retained on the LAS header.
- The remaining LAS 1.4 point attributes, including NIR, classification flags,
  scanner channel, scan angle, user data, and point source ID.
- LAS point formats 4, 5, 9, and 10 with the waveform packet metadata contract
  and the external `.wdp` reference.
- Metadata-only LAS and LAZ reads: `Read(metadataOnly=true)` authors the
  `/PointCloud` metadata namespace — source count, bounds, CRS, and
  available-attribute metadata — without decoding point records.
- A chunked and range-based reader API behind the shared `PointReadOptions`
  contract, with a memory budget and a cancellation callback.
- Normalized file-format arguments, with a canonical argument map that
  participates in layer identity. `attributes`, `chunkPointLimit`,
  `memoryBudgetBytes`, `rangeFirstPoint`, `rangePointCount`, and `lod` are
  reachable; unknown, out-of-range, and not-yet-implemented arguments are
  rejected with typed diagnostics.
- Shared LOD contracts in `usdPointCloudCore`: `PointTileId`, `PointLodItem`,
  `PointLodHierarchy`, `PointTile`, their validation invariants, and typed
  diagnostics.
- Deterministic, versioned fixed-stride point sampling. The algorithm, version,
  and target count are normalized cache-key inputs.
- OpenUSD 26.08 `usdLod` authoring: `UsdLodRootAPI` and
  `UsdLodScreenSizeHeuristic` on a single non-tiled root.
- Compact `lod` file-format profiles (`off`, `preview`, `balanced`, `quality`)
  reachable from both LAS and LAZ reads.
- Spatial tiled authoring with deterministic per-tile `usdLod` roots, through
  the authoring API.
- Payload-backed tiled authoring: one USDC payload per tile and LOD level, with
  portable relative asset paths, through the authoring API.
- Bounded-memory LAS and LAZ pull-stream integration for spill-backed,
  payload-backed tiled authoring, including spool cleanup and rollback on
  failure.
- Typed diagnostics (`usdgeo::Diagnostic`, `Severity`, `DiagnosticCode`) in
  `usdGeoCore`, emitted by both readers and projected onto the stable plugin
  code prefixes.
- Endian-safe LAS point decoding.
- A `README.md` for every module under `libs/` and `plugins/`, and the
  [module README contract](docs/contributing/MODULE_README_CONTRACT.md) that
  makes it part of the module contract.
- [docs/architecture/WORKSPACE.md](docs/architecture/WORKSPACE.md), the binding
  structural contract for module identities, dependency directions, artifact
  naming, and change invariants.
- [docs/roadmap/streaming-and-tiling.md](docs/roadmap/streaming-and-tiling.md),
  the plan for bounded-memory streaming: the `PointStream` interface,
  spill-backed fixed-grid tiling, spool and payload contracts, the spatial
  file-format argument surface, the pull-request sequence, testing
  requirements, and the definition of done.
- [docs/guides/BUILDING.md](docs/guides/BUILDING.md) and
  [docs/guides/INSTALL.md](docs/guides/INSTALL.md).
- [docs/compatibility/MIGRATION.md](docs/compatibility/MIGRATION.md).
- Tile and LOD contract fixing OpenUSD 26.08 `usdLod` as the only public LOD
  representation, with the target namespace, validation invariants, sampling
  and cache-key rules, payload policy, and the test matrix.
- Plugin adapter contract recording the thin-adapter rule and its migration.
- File-format argument contract covering syntax, candidate arguments,
  validation and normalization rules, layer identity, and cache-key
  participation.
- ADR-0003 proposing a sequence for dynamic file format support, with the open
  questions that must be answered before it is accepted or rejected.
- Standing design policy covering contracts, LAS/LAZ scope, tiling, streaming,
  caching, diagnostics, binary safety, testing, and licensing.
- Capability matrix with the exact point format, attribute, VLR, CRS, and
  authored USD matrices.
- Diagnostics contract describing the migration from string errors to typed
  diagnostics.
- Binary distribution document covering the LGPL-2.1 obligations for
  `geospatial-laz`.
- OpenUSD compatibility statement for the pinned runtime and tested platforms.
- Roadmap entries for COPC, PLY, delimited text point formats, E57, GeoTIFF and
  DEM elevation, and COG.

### Changed

- Both FileFormat Plugins now meet the thin-adapter rule. `geospatial-las` runs
  on `usdlas::LasReader`, both plugins normalize arguments before reading, and
  both author through the shared `usdgeo::AuthorPointCloudAsset` entry point.
  Point fan-out, chunk-schema construction, CRS and bounds conversion, stage
  metrics, and layer transfer no longer live in a plugin.
- Reorganized `docs/` by responsibility — `architecture/`, `reference/`,
  `guides/`, `design/`, `adr/`, `compatibility/`, `contributing/`, `roadmap/`,
  `releases/` — and added [docs/README.md](docs/README.md) as the index.
  `development-policy.md`, `supported-formats.md`, `distribution.md`, and
  `roadmap/library-architecture.md` moved to `design/DESIGN_POLICY.md`,
  `reference/CAPABILITY_MATRIX.md`, `guides/DISTRIBUTION.md`, and
  `architecture/WORKSPACE.md`.
- Documentation now reports authoring-library capability and direct LAS/LAZ
  FileFormat read capability separately, because they differ: tiled and
  payload-backed authoring exist in the library, and no file-format argument
  reaches them yet.
- The README no longer claims that Extra Bytes point attributes are
  unimplemented, or that payload packaging is entirely unimplemented. Both
  claims were wrong.
- The README distinguishes the latest release from the current branch.
- Design policy section 5 replaces the open tile/LOD plan with the `usdLod`
  standing policy: no repository-specific LOD schema, tiling separate from LOD,
  deterministic sampling, renderer-driven selection, and payload behavior
  measured rather than assumed.
- Design policy records file-format arguments and layer identity as a design
  principle.
- Roadmap phase table reflects what shipped: phase 2 is complete, and phase 4
  is split into the completed authoring work (4a) and the not-started
  streaming work (4b).
- OpenUSD compatibility statement records the `usdLod` and payload surface now
  in use, and that `metadataOnly` reads are supported rather than refused.

### Fixed

- LAS Extra Bytes validation for non-finite values and for 64-bit integers that
  cannot be represented exactly as `double`.
- Metadata-only attribute reporting and empty-point-cloud handling.
- Payload asset-path portability across a moved output directory.
- Tiled LOD authoring edge cases.
- Aliased point-sampling output.
- Reader range and diagnostic accounting.
- LAS waveform field layout.
- LAS legacy attribute authoring.
- Big-endian LAS decoding.

### Known limitations

- Rendering is provided by the consuming OpenUSD application.
- Peak RSS and payload working-set measurements for real-world tiled datasets
  are not yet published; the generated-corpus benchmark is included in the
  release verification record.
- Bounds and classification filters remain unavailable and are rejected with
  typed diagnostics.
- GeoTIFF keys are parsed and retained but are not interpreted, and EPSG codes
  are not inferred when the WKT CRS VLR is absent.

## [0.1.0] - 2026-08-01

### Added

- Shared geospatial values, transforms, bounds, and point-cloud contracts.
- LAS 1.2-1.4 header, VLR/EVLR, WKT CRS, and point-record support.
- LAZ chunk decoding through the bundled `laz-perf` adapter.
- OpenUSD FileFormat Plugins for LAS and LAZ.
- Machine-readable diagnostics for invalid or unsupported input.
- Apache License 2.0 project licensing and third-party notices.

### Known limitations

- Rendering is provided by the consuming OpenUSD application.
- Tile/LOD streaming and a USDC cache are not included in this release.
- OpenUSD is a required runtime dependency for the USD targets.

[0.1.0]: https://github.com/animu-sphere/usd-pointcloud-plugins/releases/tag/v0.1.0
[0.2.0]: https://github.com/animu-sphere/usd-pointcloud-plugins/releases/tag/v0.2.0
[0.2.1]: https://github.com/animu-sphere/usd-pointcloud-plugins/releases/tag/v0.2.1
[0.2.2]: https://github.com/animu-sphere/usd-pointcloud-plugins/releases/tag/v0.2.2
[0.3.0]: https://github.com/animu-sphere/usd-pointcloud-plugins/releases/tag/v0.3.0
[0.4.0]: https://github.com/animu-sphere/usd-pointcloud-plugins/releases/tag/v0.4.0
[0.5.0]: https://github.com/animu-sphere/usd-pointcloud-plugins/releases/tag/v0.5.0
[0.6.0]: https://github.com/animu-sphere/usd-pointcloud-plugins/releases/tag/v0.6.0
[0.7.0]: https://github.com/animu-sphere/usd-pointcloud-plugins/releases/tag/v0.7.0
[0.8.0]: https://github.com/animu-sphere/usd-pointcloud-plugins/releases/tag/v0.8.0
[0.9.0]: https://github.com/animu-sphere/usd-pointcloud-plugins/releases/tag/v0.9.0
[Unreleased]: https://github.com/animu-sphere/usd-pointcloud-plugins/compare/v0.9.0...HEAD
