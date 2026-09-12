# OpenUSD Compatibility

This document states which runtimes the plugins are built and tested against,
and what they require from a host application.

## Declared Contract

All plugin manifests declare:

```yaml
runtime:
  openusd: ">=26.08,<27.0"
```

`pointcloud-las`, `pointcloud-laz`, `pointcloud-copc`, and `pointcloud-ply` are
`usd-fileformat` bundles and require the `usd-stage-read` capability. The
`tests/plugins/httpresolver` fixture is an external-dependency-free Tier 1 test
double, not part of the product surface. Production network transport is
provided by a separately installed resolver such as
[`usd-http-resolver`](https://github.com/animu-sphere/usd-http-resolver).

| Item | Value |
| --- | --- |
| Validated OpenUSD version | 26.08 |
| Accepted range | `>=26.08,<27.0` |
| OpenStrata CLI | 0.22.7 |
| OpenStrata platform / profile | `cy2026` / `usd` |
| C++ standard | C++17 |
| CMake | 3.23 or newer |

A 27.x runtime is outside the declared range. Raising the upper bound requires
rebuilding and re-running the plugin integration tests against that runtime.

## Tested Platforms

| Platform | Runner | Coverage |
| --- | --- | --- |
| Windows x86_64 | `windows-2022` | Plugin build and integration tests |
| Linux x86_64 | `ubuntu-24.04` | Plugin build and integration tests |
| macOS arm64 | `macos-15` | Plugin build and integration tests |

The core libraries (`usdGeoCore`, `usdPointCloudCore`, `usdLas`, `usdLaz`,
`usdCopc`)
build and test with plain CMake and no OpenUSD runtime. Only `usdPointCloudAuthoring` and
the plugin bundles require OpenUSD.

## OpenUSD Surface Used

The plugins depend on a small, stable part of the API:

- `SdfFileFormat`, `SdfLayer`, and `SdfFileFormat::FindByExtension("usda")`
- `UsdStage::CreateInMemory` with `UsdStage::LoadNone`, then
  `SdfLayer::TransferContent` into the layer being read; payload files are
  built in in-memory layers and written with `SdfLayer::Export`
- `UsdGeomPoints`, `UsdGeomSetStageUpAxis`, `UsdGeomSetStageMetersPerUnit`
- `TfType` registration through `TF_REGISTRY_FUNCTION` and
  `SDF_DEFINE_FILE_FORMAT`
- `VtArray`, `GfVec3f`, and `GfVec3d` for authored values
- `SdfPayload` for the payload-backed tile assets the authoring library emits
- `ArResolver`, `ArResolvedPath`, and `ArAsset` for resolver-backed COPC byte
  access; the plugin adapts this source to its OpenUSD-independent reader
  contract

No Hydra scene index is used.

## LOD Surface

Tile and LOD authoring binds to the OpenUSD 26.08 `usdLod` schemas, inside
`usdPointCloudAuthoring` only:

| Schema | Use | Status |
| --- | --- | --- |
| `UsdLodRootAPI` | Applied to each prim whose children are LOD items | Authored whenever `lod` is not `off` |
| `UsdLodScreenSizeHeuristic` | Authored once and referenced by LOD roots | Authored whenever `lod` is not `off` |
| `UsdLodOverrideAPI` | Consumed in tests and offline renders, normally authored in a stronger layer | Not authored |

The exact property names are taken from the schema definitions in the build and
are verified against the pinned runtime.

The compact `lod` profiles reachable from a LAS, LAZ, or COPC read author a single
non-tiled `usdLod` root. The authoring library additionally supports per-tile
roots and payload-backed LOD children, which no file-format argument reaches
yet; see [LOD.md](../architecture/LOD.md) and
[streaming and tiling](../roadmap/streaming-and-tiling.md).

This raises the effective floor for LOD support:

```text
OpenUSD < 26.08:
    Reader libraries may still build where practical.
    Standard LOD authoring is unavailable.

OpenUSD >= 26.08:
    usdLod-based authoring is enabled.
```

No fallback LOD representation is maintained for older runtimes, and no
repository-specific LOD schema is published. See the
[tile and LOD contract](../architecture/LOD.md) and the
[design policy](../design/DESIGN_POLICY.md).

## Dynamic FileFormat Surface

| Mechanism | Impact |
| --- | --- |
| `SDF_FORMAT_ARGS` handling | No new API surface; the plugins parse and normalize arguments themselves |
| `PcpDynamicFileFormatInterface` | Used by LAS, LAZ, and COPC for format-specific LOD prim metadata fields; adds a Pcp dependency and recomposition behavior |

The plugin manifests declare `pc_las_lod`, `pc_laz_lod`, or `pc_copc_lod` in
`SdfMetadata`. Each field maps to the existing normalized `lod` argument and
accepts `off`, `preview`, `balanced`, or `quality`. Other generation arguments
remain static `SDF_FORMAT_ARGS`. See [ADR-0003](../adr/0003-dynamic-file-format.md).

## Host Expectations

- The host discovers plugins through `PXR_PLUGINPATH_NAME`; see
  [INSTALL.md](../guides/INSTALL.md).
- `Read` requires a writable layer. `Read(metadataOnly=true)` is supported and
  authors the `/PointCloud` metadata namespace — source count, bounds, CRS, and
  available-attribute metadata — without decoding point records.
- Authored stages use up axis `Y` and `metersPerUnit` 1.0 regardless of the
  source CRS units. The source-to-stage relationship is recorded in the
  `geo:*` attributes documented in
  [capability matrix](../reference/CAPABILITY_MATRIX.md).
- The host selects the active LOD. The plugins author the hierarchy,
  heuristics, and default index; they never read a camera or a viewport.
- The host is responsible for rendering. These plugins author stages; they do
  not provide a point-cloud render delegate.
- Remote COPC requires the active resolver to provide efficient random-access
  reads. HTTP support, authentication, retries, redirects, and raw byte-range
  caching remain resolver responsibilities. Generated-USDC cache reuse is
  disabled unless a stable local filesystem identity is available; extending
  reuse to stable resolver-provided identity is `v0.10.0` work.
- An external resolver is runtime composition, not a build-time dependency.
  These bundles carry no CMake dependency, submodule, vendored transport
  library, or link dependency on any resolver implementation, and they do not
  detect which resolver opened an asset. `usd-http-resolver` is one compatible
  implementation; register it alongside these bundles through
  `PXR_PLUGINPATH_NAME`.
- The repository's `tests/plugins/httpresolver` fixture is a Tier 1 integration-
  test double only; it does not provide network transport or production HTTP
  behavior and is excluded from the product matrix.

The boundary is stated in full in the
[resolver-backed source contract](../architecture/RESOLVER_SOURCE.md).
