# Streaming and Spatial Tiling

This document records the completed first-generation approach to consuming
large point clouds without accumulating the complete cloud in memory and
turning the result into payload-backed `usdLod` tile assets.

Status: **complete for fixed-grid tiling**. The `PointStream` contract, LAS,
LAZ, COPC, and PLY connections,
spill-backed routing, and one-tile-at-a-time payload authoring are implemented.
The production path for long-running tiled generation is now an explicit
conversion tool; FileFormat-triggered generation remains a compatibility path
and is not the target operational interface. The converter publishes a
deterministic `<root>.manifest` sidecar containing normalized generation
arguments and the relative payload asset list. It also publishes a
deterministic `tiles.manifest` beside the payload assets containing each tile
ID, LOD level, source bounds, point count, and portable payload path.
Generated-corpus RSS measurement and explicit benchmark paths are implemented.
Real-input and payload working-set baselines are recorded below; a broader
real-world matrix remains open. Adaptive point-budget planning, tile
statistics, and fixed-grid compatibility coverage are implemented; a
fixture-based cross-format payload and memory comparison is implemented;
broader real-world baselines remain open in the
[infrastructure maturity roadmap](infrastructure-maturity.md). The streaming
benchmark also reports source bytes, spool bytes written and read, payload
bytes, and effective I/O amplification. For local LAS, LAZ, COPC, and PLY,
source bytes are counted at the actual reader boundary; generated input
reports its logical generated-point byte volume. Payload bytes are the
materialized payload size used as the output-write baseline. Resolver
range-read accounting remains a separate future measurement. The v0.8.0 I/O
observability work is complete, and v0.9.0 completed host-responsiveness
measurement alongside convergence of the sequential planner and the COPC
native hierarchy onto one tile-plan representation. What `main`
implements today is in
[implementation status](implementation-status.md) and
[capability matrix](../reference/CAPABILITY_MATRIX.md).

The opt-in `usdCopc_runtime_streaming_probe` provides the first runtime
streaming experiment without changing the production pipeline. It accepts a
local COPC and a requested hierarchy level, selects point-data nodes in stable
tile order, decodes only those ranges, and reports hierarchy size, selected
points, selected range bytes, and actual source bytes read as TSV. Build it
with `USDGEO_BUILD_BENCHMARKS=ON` and run:

```powershell
usdCopc_runtime_streaming_probe C:\path\to\sample.copc --level 3
```

This measures the source-side portion of the runtime-streaming question.
The TSV summary also includes `selected_tile_ids` and
`selected_tile_ranges`. The comma-separated values are aligned by position:
tile identities use `L<level>/<x>/<y>/<z>`, while ranges use
`<offset>:<byte-size>`. This makes a deterministic source-read selection
available for comparison with host observations. Host LOD policy, OpenUSD
payload composition, and renderer loading remain outside the probe and are
still research work.

The OpenUSD composition portion is measured separately by the
`usdPointCloudAuthoring_runtime_lod_payload_probe` target. It creates a
deterministic three-LOD payload-backed fixture and reports authored payload
arcs, loaded LODs after `LoadNone`, a selective LOD load, and `LoadAll` point
counts as TSV. Build it with `USDGEO_BUILD_BENCHMARKS=ON` and run:

```powershell
usdPointCloudAuthoring_runtime_lod_payload_probe `
    --output C:\path\to\runtime-lod-payload\PointCloud.usda
```

The probe verifies OpenUSD stage and payload-loading semantics only. It does
not identify a host's camera-driven LOD decision, renderer visibility, frame
latency, or renderer memory savings. Those measurements remain host- and
render-delegate-dependent research.

## 1. Problem statement

Chunked decoding reduces decoder buffer size. The tiled path additionally
spools points by source-coordinate tile and reconstructs one tile at a time
before writing its payload, so it does not retain the complete point cloud in
the authoring stage. Generated, LAS, and LAZ inputs can be measured with the
explicit benchmark target; a broader real-world dataset matrix is still
outstanding.

### Conversion streaming and runtime streaming

This document covers conversion streaming only:

```text
large input -> bounded-memory processing -> USD payload generation
```

Its objectives are memory use independent of input size, safe long-running
processing, recoverable state after failure or cancellation, and deterministic
output.

Runtime streaming is a different problem:

```text
camera / host LOD decision -> required tile -> range request -> decode -> render
```

It involves OpenUSD composition, payload loading, the host application, and
renderer LOD decisions, so a FileFormat Plugin cannot own it. It is tracked as
a research theme in the
[infrastructure maturity roadmap](infrastructure-maturity.md) and does not
change the contracts below.

The target pipeline must:

- avoid storing the complete point cloud in memory;
- preserve all selected point attributes;
- produce deterministic results;
- support cleanup after failures;
- preserve stable diagnostics;
- support LAS first and LAZ second;
- remain reusable by future COPC and E57 bundles.

## 2. Target pipeline

```text
LAS / LAZ reader
    | PointChunk
attribute selection and validation
    | PointChunk
spatial partitioning
    | tile spool
LOD sampling
    | tile level data
payload writer
    | tile assets
root usdLod authoring
```

The initial implementation favors correctness, deterministic output, and
bounded memory over sophisticated spatial indexing.

## 3. Streaming interface

A pull-based C++17 interface in `usdPointCloudCore` provides natural
backpressure and keeps buffer ownership explicit:

```cpp
class PointStream {
public:
    virtual ~PointStream() = default;

    virtual PointStreamStatus ReadNext(
        PointChunk& chunk,
        PointData& data,
        Diagnostic& diagnostic) = 0;
};
```

Reader-specific factory functions return this interface:

```cpp
std::unique_ptr<PointStream> OpenLasPointStream(...);
std::unique_ptr<PointStream> OpenLazPointStream(...);
```

`PointStream` is the second consumer of the existing `PointReadOptions`
contract, not a replacement for it; see
[point reader architecture](../architecture/POINT_READER.md).

## 4. Spill-backed fixed-grid tiling

```text
PointStream
    |
TileRouter
    |
per-tile memory buffers
    | threshold reached
temporary tile spool files
    | end of stream
LOD sampling and payload authoring
    |
root usdLod layer
```

Input points are not guaranteed to be ordered by tile, so keeping every tile
completely in memory would merely move the existing memory problem. External
sorting is powerful but unnecessarily complex for a first implementation.
Per-tile spool files are the practical first bounded-memory design.

The fixed-grid path in `usdPointCloudTiling` uses:

- a fixed 2D grid;
- source horizontal coordinates;
- a fixed tile size;
- per-tile memory thresholds;
- temporary spool files;
- fixed-stride LOD samples;
- deterministic asset naming.

The same library also exposes deterministic point-budget planning. A
`PointStreamFactory` planner performs a bounds pass and a cell-count pass,
retaining counts rather than all point records; its leaf plan is consumed by a
`PointBudgetTileRouter` and the shared payload authoring path. The conversion
tool uses this adaptive path with `--max-points-per-tile`,
`--min-points-per-tile`, and `--max-depth`. Complex octrees, target payload-byte
fallbacks, and COPC-specific hierarchy optimization remain deferred.

### Coordinate policy

Spatial partitioning uses source horizontal coordinates; OpenUSD authoring
continues to use stage-local coordinates. This keeps tile identity stable
across stage up-axis conversion and preserves the meaning of projected CRS
coordinates. See [ADR 0001](../adr/0001-coordinate-model.md).

## 5. Spool positioning and I/O amplification

Spool-backed tiling is a reasonable bounded-memory conversion design, and it is
the current one:

```text
PointStream -> tile router -> temporary spool -> payload authoring
```

It is not treated as the final form of streaming. Its main cost is I/O
amplification, because a large conversion moves the same data several times:

```text
source read
decoded point traffic
spool write
spool read
USD payload write
```

The project therefore evaluates designs on two axes rather than one. Memory
bounded is necessary but not sufficient; I/O bounded is measured as well.

The spool is not removed or redesigned on suspicion. It is measured on real
data first, and changed only where measurement shows it is the bottleneck. The
instrumentation for that decision — source bytes read, spool bytes written,
spool bytes read, payload bytes written, and effective I/O amplification — is
`v0.8.0` work in the
[infrastructure maturity roadmap](infrastructure-maturity.md).

Deterministic point-budget planning helps here for a structural reason: the
tile plan is fixed before any payload is authored, so spool volume and write
patterns are known in advance rather than discovered during authoring. Any
future reduction in spool traffic builds on that property.

## 6. Temporary spool requirements

The spool system must define:

- a binary schema version;
- the selected attribute layout;
- source and stage coordinate representation;
- file naming;
- flush thresholds;
- a maximum open file count;
- retry and failure behavior;
- cleanup behavior;
- detection of incomplete spools;
- deterministic iteration order.

Temporary output is isolated in a dedicated working directory and removed on
successful completion unless a debug-retention option is enabled.

## 7. Payload output requirements

The payload writer produces:

- one payload asset per tile and LOD level, or another documented
    deterministic grouping;
- relative and portable asset paths;
- stable prim paths;
- root `usdLod` metadata;
- a source fingerprint and the generation settings;
- tile bounds and point counts;
- no partially published root asset after failure;
- explicit ownership of the payload files, so a FileFormat read can read the
  layer again without colliding with its earlier payloads, another layer's, or
  a user's files.

The conversion tool writes the versioned tile manifest only after all payloads
have been authored successfully. Entries are sorted by tile ID and LOD, and
portable payload paths, duplicate tile/LOD entries, bounds, and point counts
are validated before publication. Cache materialization preserves the tile
manifest beside the cached payloads.

FileFormat reads publish their payloads in a directory owned by the layer, as
a generation that is staged privately and renamed into place, so a failed or
interrupted read never modifies payloads a root already references; the
layout is in the
[file-format argument contract](../architecture/FILE_FORMAT_ARGUMENTS.md#generated-payload-ownership).
The plugins never write the root layer of a FileFormat read to disk.

Final output is committed atomically where the filesystem allows it. The
authored representation stays inside the existing
[tile and LOD contract](../architecture/LOD.md); streaming changes how assets
are produced, not what they look like.

## 8. Initial FileFormat arguments

The existing compact `lod` profiles remain. Spatial streaming is introduced
with a small, explicit argument set:

```text
tile=true
tileSize=<source units>
tileMemoryLimit=<bytes or profile>
payloadDirectory=<relative or generated path>
```

Not every internal tuning parameter is exposed in the first public contract.
Profiles map to internal defaults; explicit advanced arguments are added only
after real data testing. Argument normalization, layer identity, and cache-key
participation follow the existing
[file-format argument contract](../architecture/FILE_FORMAT_ARGUMENTS.md).

These arguments remain supported for static FileFormat reads, preview, and
small inputs. Production generation should use the conversion tool so that
progress, cancellation, retries, cleanup, and root-last publication are
explicit process operations rather than side effects of layer open or
recomposition.

### Conversion tool contract

The first tool surface reuses the same normalized options and authoring path:

```text
usd-pointcloud-convert <input> <output-root>
    --tile-size <source units>
    --memory-limit <bytes>
    --payload-directory <directory>
    --attributes <comma-separated names>
```

For adaptive point-budget tiling, replace `--tile-size` with
`--max-points-per-tile <count>` and optionally add
`--min-points-per-tile <count>` and `--max-depth <depth>`. Adaptive arguments
are conversion-tool arguments; `max-depth` is limited to 31 and local input
identity is checked between planning and payload authoring. FileFormat-triggered
reads continue to expose the fixed-grid contract above.

The tool owns temporary output, cancellation, failure cleanup, and final
root-last publication. It must not duplicate LAS or LAZ decoding. It also
publishes a deterministic `<root>.manifest` sidecar after payload generation
and before the root layer; the sidecar records normalized generation settings
and the relative output asset list. Source content fingerprinting and resume
support remain subsequent steps and do not change the reader contracts.
An internal transaction marker lets the next invocation remove incomplete
temporary output after an interrupted process, while completed root and
manifest pairs are left intact.

## 9. Out of scope for the first streaming release

- adaptive octrees;
- density-aware tile sizing;
- multithreaded decode and authoring;
- distributed processing;
- COPC hierarchy preservation;
- renderer-specific page scheduling;
- automatic cache eviction.

Two of these have since moved into the plan. Deterministic point-budget
planning shipped in `v0.7.0`, and COPC hierarchy preservation shipped in
`v0.9.0`: rather than re-deriving a partition that COPC already carries, its
native hierarchy becomes a tile plan through the same representation the
sequential planner produces. The remaining items stay out of scope.

## 10. Implementation sequence

Each step is one pull request. Behavior changes are not mixed into the rename
and documentation steps that precede them.

| Step | Title | Scope |
| --- | --- | --- |
| 1 | Rename point-cloud authoring and geospatial plugin modules | Done: directory, bundle, CMake target, manifest, `plugInfo.json`, CI, and test renames with no behavior change. |
| 2 | Synchronize documentation with current implementation | Done: capability status, unreleased change record, renamed references, and a `README.md` for every module. |
| 3 | Add point-stream and spatial-tiling contracts | `PointStream`; tile keys and configuration; tile router; spool interface and schema; the deterministic output contract; tests that run without an OpenUSD runtime; `libs/usd-pointcloud-tiling/README.md`. The FileFormat Plugins are not connected yet. |
| 4 | Connect LAS streaming to tiled payload authoring | LAS `PointStream`; source-coordinate tile routing; spool generation; tile payload output; root `usdLod` output; bounded-memory tests; cleanup and failure tests. LAS is first so tiling issues are not mixed with LAZ codec issues. |
| 5 | Connect LAZ streaming to tiled payload authoring | LAZ `PointStream`; an output contract equivalent to LAS; codec-specific diagnostics; cross-format consistency tests. |
| 6 | Enable spatial tiled reads through FileFormat arguments | Enable the spatial tile argument; define output and cache paths; connect profiles to tiling configuration; update plugin READMEs and root documentation; add integration tests with plugin discovery. |
| 7 | Add explicit conversion tooling | Add a thin LAS/LAZ command-line entry point over the shared stream and authoring APIs; publish the root layer last; add cancellation, cleanup, and deterministic-output tests. |
| 8 | Adopt narrow dynamic FileFormat composition | Format-specific LOD metadata fields map to the normalized `lod` argument after generated assets, cache lookup, manifest identity, and operational recovery stabilized. Other fields and raw source re-generation during recomposition remain out of scope. |

## 11. Testing requirements

### Unit tests

Each module owns tests for its own contract:

- `usdPointCloudCore`: stream ownership and validation;
- `usdPointCloudTiling`: tile keys, negative coordinates, boundaries,
  deterministic ordering, spill thresholds;
- `usdPointCloudAuthoring`: asset paths, prim paths, LOD metadata, payload
  round trips;
- `usdLas`: chunk continuity and attribute preservation;
- `usdLaz`: codec chunk continuity and error mapping.

### Integration tests

- LAS direct non-tiled read;
- LAZ direct non-tiled read;
- metadata-only read;
- tiled LAS output;
- tiled LAZ output;
- payload paths after moving the output directory;
- interrupted generation cleanup;
- empty point cloud;
- negative tile coordinates;
- invalid Extra Bytes names;
- point attributes preserved across spool round trips;
- deterministic output from repeated runs.

### Memory tests

At least one generated large-corpus test verifies that peak buffering is
bounded by configuration rather than by total point count. CI does not need a
truly massive file, but the test must generate enough points and tiles to force
repeated spill and flush behavior.

### Benchmarks

Track at least:

```text
LAS decode points per second
LAZ decode points per second
peak resident memory
spool write volume
payload authoring time
tile count
root layer size
time to first usable LOD asset
```

Benchmarks may initially run outside required PR CI, but their commands and
datasets must be documented.

`v0.8.0` extends this set with I/O counters, so that amplification is visible
rather than inferred:

```text
source bytes read
spool bytes written
spool bytes read
payload bytes written
effective I/O amplification
```

It also adds a fixed-grid versus adaptive comparison on the same input,
reporting points per tile distribution, payload bytes per tile, total payload
bytes, tile count, tree depth, peak RSS, spool bytes, source read bytes, total
processing time, and `usdview` open time.

v0.9.0 measured interaction with the generated output using the reproducible
payload-backed `usdview` fixture, and recorded the workload, method, and
baseline alongside the existing open-time and working-set measurements.

The comparison needs inputs whose density genuinely varies, because uniform
fixtures cannot distinguish the two strategies:

```text
forest         high density
buildings      high / medium density
roads          medium density
open terrain   low density
```

Datasets that cannot be redistributed are referenced through corpus provenance
records with reproducible download and run commands, as the Shizuoka and USGS
3DEP baselines below already do.

#### Real LAS baseline

On Windows, the supplied Shizuoka dataset `08NF2330.las` was measured with
`--chunk-points 65536`, `--tile-size 128`, and `--memory-limit 1048576`.
The input contained 14,574,030 points and completed successfully:

| Metric | Result |
| --- | ---: |
| Elapsed time | 32.4375 s |
| Decode and author throughput | 449,296 points/s |
| Tile count | 12 |
| RSS delta | 351.16 MiB |
| Sampled peak spool file bytes | 1.004 GiB |
| Payload bytes | 420.68 MiB |
| Root and payload output bytes | 420.68 MiB |
| Process write bytes | 1.415 GiB |

This is a single LAS baseline for the tiled authoring path, not a payload
working-set measurement across scene or render delegates. The benchmark removes
its temporary output directory after reporting.

The full source tile is not checked in, but it can be reverse-looked-up from
the corpus provenance and downloaded into the build tree. The Virtual
Shizuoka source is recorded in
`plugins/pointcloud-las/tests/corpus/virtual-shizuoka-2019/PROVENANCE.md`:

```powershell
$sourceDir = '.\\build\\real-data-source'
$archive = "$sourceDir\\08NF2330.zip"
New-Item -ItemType Directory -Force $sourceDir | Out-Null
Invoke-WebRequest `
    'https://virtual-shizuoka.s3.ap-northeast-1.amazonaws.com/2019/LP/Ground/08/NF/23/08NF2330.zip' `
    -OutFile $archive
Expand-Archive $archive -DestinationPath $sourceDir -Force

$envScript = ost env cy2026 --profile usd --shell powershell | Out-String
Invoke-Expression $envScript
& '.\\build\\cy2026-windows-x86_64-py313-usd\\libs\\usd-pointcloud-authoring\\benchmarks\\usdPointCloudAuthoring_stream_benchmark.exe' `
    --input "$sourceDir\\08NF2330.las" --format las `
    --chunk-points 65536 --tile-size 128 --memory-limit 1048576
```

The same lookup is available for the USGS 3DEP source LAZ in
`plugins/pointcloud-las/tests/corpus/usgs-3dep-2020/PROVENANCE.md`. A local
reproduction of the Shizuoka command on 2026-08-05 processed 14,574,030 points
in 31.6717 seconds, produced 12 tiles, and reported 368,668,672 bytes of RSS
delta, 1,078,481,220 peak spool bytes, 441,114,026 payload bytes, and
1,519,604,759 process write bytes.

The official Shizuoka LP resource provides LAS archives, not LAZ. For a
same-source codec comparison, the LAS can be recompressed into a build-local
LAZ with `laspy` and `lazrs`, without thinning or changing the point records.
The derived LAZ is not an official provider artifact. The same benchmark run
on that derived file processed 14,574,030 points in 35.5156 seconds, produced
12 tiles, and reported 374,370,304 bytes of RSS delta. Peak spool, payload,
root output, and process write bytes were respectively 1,078,481,220,
441,114,026, 441,117,880, and 1,519,604,759.

The same benchmark executable accepts `las`, `laz`, `copc`, and `ply` input and
reports a machine-readable `format=` field alongside the shared tile, memory,
spool, and payload metrics. PLY has no embedded CRS contract, so its command
must include an explicit EPSG code:

```powershell
& '.\\build\\cy2026-windows-x86_64-py313-usd\\libs\\usd-pointcloud-authoring\\benchmarks\\usdPointCloudAuthoring_stream_benchmark.exe' `
    --input '.\\plugins\\pointcloud-ply\\tests\\fixtures\\conformance.ply' `
    --format ply --epsg 26910 --chunk-points 2 --tile-size 1 --memory-limit 1024

& '.\\build\\cy2026-windows-x86_64-py313-usd\\libs\\usd-pointcloud-authoring\\benchmarks\\usdPointCloudAuthoring_stream_benchmark.exe' `
    --input 'C:\\path\\to\\source.copc' --format copc `
    --chunk-points 65536 --tile-size 128 --memory-limit 1048576
```

The checked-in benchmark tests cover LAS, LAZ, PLY, and a generated COPC
fixture. The generated COPC case validates the shared authoring path with
compressed point data; a larger real-world COPC baseline remains dataset
dependent because the repository does not ship a large COPC corpus. The
`usdPointCloudAuthoring_stream_benchmark_cross_format` test runs all four
inputs with the same chunk, tile, and memory settings and writes
`benchmark-cross-format/cross-format.tsv` with fixed-grid and adaptive rows
for every format. The report includes comparable point count, tile count, tile
point and payload distributions, tree depth, peak RSS, source and spool I/O,
payload bytes, I/O amplification, and elapsed time. Use the same command shape
with a representative local source when collecting the larger real-world
comparison matrix; the checked-in fixtures do not close that gate.

For repeatable real-data comparisons, use
`tools/compare_real_world_tiling.ps1`. It runs the same source through both
strategies, records the source size and SHA-256, preserves the normalized
benchmark settings, and writes a TSV report plus a JSON companion beside it:

```powershell
& '.\\tools\\compare_real_world_tiling.ps1' `
    -Benchmark '.\\build\\cy2026-windows-x86_64-py313-usd\\libs\\usd-pointcloud-authoring\\benchmarks\\usdPointCloudAuthoring_stream_benchmark.exe' `
    -InputPath '.\\build\\real-data-source\\08NF2330.las' `
    -Format las -ChunkPoints 65536 -TileSize 128 `
    -MemoryLimitBytes 1048576 -MaxPointsPerTile 1000000 `
    -MinPointsPerTile 1000 -MaxDepth 16 `
    -Report '.\\build\\real-data-source\\shizuoka-fixed-adaptive.tsv'
```

The TSV is intended for comparison tables and the JSON companion is intended
for provenance-preserving archival. Repeat the command with the corresponding
`-Format` and `-Epsg` (required for PLY) for each available corpus. The
harness does not claim that a fixture or a single real source closes the
broader uneven-density baseline gate.

For a four-format run, `tools/compare_real_world_matrix.ps1` accepts the
checked-in `tools/real-world-tiling.example.json` manifest. Paths in that
manifest are build-local and the source files must first be prepared from the
provenance records above and the Stanford Bunny provenance record. Use:

```powershell
& '.\\tools\\compare_real_world_matrix.ps1' `
    -Benchmark '.\\build\\cy2026-windows-x86_64-py313-usd\\libs\\usd-pointcloud-authoring\\benchmarks\\usdPointCloudAuthoring_stream_benchmark.exe' `
    -Manifest '.\\tools\\real-world-tiling.example.json' `
    -Report '.\\build\\real-data-source\\real-world-tiling-matrix.tsv'
```

The runner writes one report per manifest entry and aggregate TSV/JSON output.
The aggregate rows and JSON `manifest_datasets` preserve optional source URL,
repository, license, and attribution fields. With `-ContinueOnError`, an input
failure is preserved in the JSON `failures` array instead of being mistaken for
a successful comparison.

The first local matrix run on 2026-08-14 completed for the 14,574,030-point
Shizuoka LAS and its 118,588,494-byte derived LAZ, and for the 35,947-vertex
Stanford Bunny PLY. The public 10,653,336-point Autzen COPC source was
metadata-readable but the vendored laz-perf point-format-7 decoder terminated
with a Windows access violation on the first point-data chunk. It is therefore
recorded as a reproducible reader blocker, not as a successful COPC baseline;
the broader four-format gate remains open until a native real COPC run passes.
The Autzen file is obtained from the public `PDAL/data` distribution, whose
repository license is CC BY 4.0; preserve the source URL, the PDAL repository
license, and the upstream attribution recorded in the example manifest before
redistributing the data or derived assets. This repository does not check in
the file or include it in plugin bundles.

#### Payload working-set measurement through OST plugin view

The pinned OpenUSD runtime exposes `Storm` as its Hydra renderer. The available
scene and render paths can be measured consistently with the Windows
process-tree sampler:

For a reproducible payload-backed smoke fixture, use the public-domain USGS
3DEP 4,096-point LAS subset described in
`plugins/pointcloud-las/tests/corpus/usgs-3dep-2020/PROVENANCE.md`. The recipe
keeps generated USD and USDC files in the build tree rather than checking in
platform-dependent binary output:

```powershell
$envScript = ost env cy2026 --profile usd --shell powershell | Out-String
Invoke-Expression $envScript
& '.\tools\prepare_usdview_fixture.ps1' `
    -Converter '.\build\cy2026-windows-x86_64-py313-usd\tools\usd-pointcloud-convert\usd-pointcloud-convert.exe' `
    -OutputDirectory '.\build\usdview-fixture-usgs-4096'
```

The default recipe settings produce a deterministic adaptive `usdLod` root
with multiple payloads while keeping the fixture fast enough for local
startup checks. The generated root can then be measured directly:

```powershell
python tools/measure_usd_working_set.py `
    --bundle .\\plugins\\pointcloud-las `
    --with-bundle .\\plugins\\pointcloud-laz `
    --fixture C:\\path\\to\\shizuoka-full-PointCloud.usda `
    --mode view --renderer Storm

python tools/measure_usd_working_set.py `
    --bundle .\\plugins\\pointcloud-las `
    --with-bundle .\\plugins\\pointcloud-laz `
    --fixture C:\\path\\to\\shizuoka-full-PointCloud.usda `
    --mode record --renderer Storm `
    --output C:\\path\\to\\shizuoka-full-payload-storm.png
```

For the v0.9.0 host-responsiveness workload, use `--mode interactive`. The
sampler synchronously probes the usdview UI thread with `WM_NULL`, sends
`Home`, `Left`, `Right`, `Up`, and `Down` at the configured interaction
interval, samples working set during the sequence, and closes or boundedly
cleans up usdview afterward:

```powershell
python tools/measure_usd_working_set.py `
    --bundle .\\plugins\\pointcloud-las `
    --with-bundle .\\plugins\\pointcloud-laz `
    --fixture .\\build\\usdview-fixture-usgs-4096-interactive\\PointCloud.usda `
    --mode interactive --renderer Storm `
    --interaction-interval-ms 500 --interval-ms 50 `
    --post-interaction-seconds 3
```

The sampler launches `usdview.cmd` or `usdrecord.cmd` through `ost plugin run`
and reports the stage open time, total process elapsed time, and peak working
set of the complete runtime process tree. In `view` mode, the command uses
`--quitAfterStartup`; `stage_open_seconds` is parsed from usdview's timing
output, while `elapsed_seconds` covers the complete process lifetime. The
working-set measurement includes the OpenUSD runtime and renderer process,
which is the relevant working set for an actual plugin view or render session.
For the full
Shizuoka payload root generated with tile size 128, a local run on 2026-08-05
reported:

| Path | Peak tree working set | Peak child process | Total time | Renderer |
| --- | ---: | ---: | ---: | --- |
| `usdview` scene/view | 688,746,496 B | 668,323,840 B | 0.837116 s | Storm |
| `usdrecord` headless render | 592,355,328 B | 572,211,200 B | not reported | HdStormRendererPlugin |

The fixture contained 12 payloads totaling 204,694,049 bytes, with a 4,262-byte
root layer. Both runs returned zero.

#### Failure and interruption cleanup validation

The authoring unit tests cover cancellation before and during spool reads,
stream failure without layer mutation, and rollback when payload authoring
fails. The converter integration test covers invalid attribute failure,
stale transaction recovery, unsafe payload paths, and an orphan transaction
without a state file.

Process-level interruption is reproduced with the full Shizuoka source by
`tools/usd-pointcloud-convert/test_interruption.ps1`. It waits for the
transaction state file, force-terminates the converter, then retries the same
output workspace and checks the published root, manifest, payloads, and
removal of transaction and temporary artifacts:

```powershell
ost plugin run .\\plugins\\pointcloud-las `
    --with .\\plugins\\pointcloud-laz -- `
    powershell.exe -NoProfile -ExecutionPolicy Bypass -File `
    C:\\dev\\usd-pointcloud-plugins\\tools\\usd-pointcloud-convert\\test_interruption.ps1 `
    -Converter C:\\dev\\usd-pointcloud-plugins\\build\\cy2026-windows-x86_64-py313-usd\\tools\\usd-pointcloud-convert\\usd-pointcloud-convert.exe `
    -Fixture C:\\dev\\usd-pointcloud-plugins\\build\\real-data-source\\08NF2330.las `
    -TestRoot C:\\dev\\usd-pointcloud-plugins\\build\\real-data-source\\interruption-test
```

The local run on 2026-08-05 reported `interrupted_exit=forced`,
`recovered_exit=0`, and `payload_count=12`. This validates the process-level
recovery boundary; graceful SIGINT cancellation remains covered by the
authoring cancellation path and converter cleanup checks.

#### Reproducible thinned-corpus measurements

The checked-in thinned corpora provide a small, repeatable LAS/LAZ matrix for
regression checks. These runs used `--chunk-points 65536`, `--tile-size 128`,
and `--memory-limit 1048576` under the pinned `ost` `cy2026` / `usd` runtime:

| Dataset | Format | Points | Elapsed (s) | RSS delta (bytes) | Peak spool (bytes) | Payload (bytes) | Output (bytes) | Process writes (bytes) |
| --- | --- | ---: | ---: | ---: | ---: | ---: | ---: | ---: |
| virtual-shizuoka-2019 | LAS | 4,096 | 0.139237 | 23,597,056 | 306,104 | 155,586 | 159,440 | 471,156 |
| virtual-shizuoka-2019 | LAZ | 4,096 | 0.115338 | 27,959,296 | 306,104 | 155,586 | 159,440 | 471,156 |
| usgs-3dep-2020 | LAS | 4,096 | 0.696362 | 26,419,200 | 321,104 | 227,220 | 249,986 | 605,192 |
| usgs-3dep-2020 | LAZ | 4,096 | 0.289793 | 30,547,968 | 321,104 | 227,220 | 249,986 | 605,192 |

Run the benchmark after `ost build` by activating the managed environment with
`ost env cy2026 --profile usd --shell powershell`, then pass one corpus path to
`usdPointCloudAuthoring_stream_benchmark.exe` with the options above. These
fixtures validate the measurement path and cross-format output equivalence;
they do not replace the full-size real-dataset baseline or payload working-set
measurement.

## 12. Definition of done

The first streaming phase is complete when:

- LAS and LAZ can be consumed through `PointStream`;
- point data is partitioned without accumulating the complete cloud;
- tile buffers spill at a configured threshold;
- payload-backed tile assets are generated deterministically;
- a root `usdLod` asset references the generated payloads;
- selected attributes survive the full pipeline;
- failures do not leave a valid-looking partial root asset;
- temporary files are cleaned up;
- memory use is bounded by tile and spool configuration;
- direct FileFormat arguments can enable the path;
- all affected module READMEs and architecture documents are updated.

## 13. Related open contracts

Extra Bytes descriptor names are currently rejected when they are not valid USD
identifiers, which fails whole files for names such as `temperature (C)` or
`Height above ground`. The normalization contract — invalid characters replaced
with `_`, a prefix for names beginning with a digit, deterministic collision
suffixes, the original descriptor name retained as metadata, and an optional
strict mode — must be defined before Extra Bytes support is widened further. It
is independent of streaming and can land in any order relative to steps 3-6.
