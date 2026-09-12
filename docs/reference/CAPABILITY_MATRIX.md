# Capability Matrix

This document describes what the readers accept and author **today**. It is
the authoritative support matrix; the README summarizes it. Planned coverage
is in the [design policy](../design/DESIGN_POLICY.md) and the
[roadmap](../roadmap/README.md).

Status vocabulary:

- **Supported**: decoded, tested, and authored where an authoring path exists.
- **Read only**: parsed and retained, but not converted into USD attributes.
- **Foundation**: an implementation slice is tested, but the complete format
  read path is not available yet.
- **Planned**: not implemented.
- **Rejected**: refused with a diagnostic instead of being read partially.
The tiled, payload-backed authoring path is connected to direct LAS and LAZ
reads through the spatial file-format arguments. The explicit conversion tool
uses the same reader and authoring contracts for long-running generation.

## PLY

| Capability | Status | Notes |
| --- | --- | --- |
| PLY 1.0 header inspection | Supported | ASCII, binary little-endian, and binary big-endian headers are parsed through vendored tinyply; declared elements and scalar/list properties are retained |
| Scalar vertex decoding | Supported | `usdply::OpenPointStream` decodes scalar `x`, `y`, `z`, intensity, RGB, and generic scalar vertex properties into shared point chunks and applies source bounds, classification, range, cancellation, chunk, and memory controls with bounded source reads |
| PLY FileFormat Plugin | Supported | Thin `pointcloud-ply` adapter requires an explicit `epsg` argument, applies optional unit and axis arguments, authors shared `UsdGeomPoints`, reads committed generated-USDC cache entries, and connects payload-backed fixed-grid tiled reads; coverage includes a thinned Stanford Bunny corpus |

## COPC

| Capability | Status | Notes |
| --- | --- | --- |
| LAS 1.4 COPC Info VLR validation | Supported | Implemented in `usdCopc`; requires point formats 6-8, a first-position 160-byte Info VLR, and a hierarchy VLR |
| Local hierarchy page validation | Supported | Root and child pages are read depth-first with range, alignment, and repeated-page checks |
| COPC point-data decoding | Supported | Local hierarchy ranges are decoded as LAZ chunks through `usdlaz::DecodeLazChunk`; bounds, classification, and attribute selection use the shared point-cloud contracts |
| COPC FileFormat Plugin | Supported | `pointcloud-copc` provides local metadata-only, non-tiled, and native hierarchy tiled reads; tiled output is payload-backed `usdLod`, while source point ranges are rejected because hierarchy order is spatial |
| Local COPC conversion and generated-cache publication | Supported | `usd-pointcloud-convert` accepts `.copc` and `.copc.laz`, reuses the COPC point stream for fixed-grid or adaptive tiled generation, and publishes entries compatible with local COPC FileFormat cache lookup |
| Resolver-backed COPC reads | Supported | The plugin adapts an `ArAsset` opened through the active `ArResolver` to the project-owned random-access source; remote tiled reads require an absolute local `payloadDirectory`. Transport, authentication, retries, and raw byte caching belong to the resolver |
| Resolver-backed generated-cache lookup | Supported | COPC extracts resolver-neutral identity through the shared adapter and reuses a committed entry only for `Stable` identity. Incomplete and corrupted entries are invalidated, and a changed validation token regenerates rather than hitting the superseded entry. Recorded against an external resolver in the [resolver read baseline](RESOLVER_BASELINE.md); the contract is the [resolver-backed source contract](../architecture/RESOLVER_SOURCE.md) |
| Resolver-backed generated-cache generation | Supported | `usd-pointcloud-convert` accepts resolver-addressable COPC identifiers, opens them through the active resolver, and publishes/reuses entries only for `Stable` identity; unstable and unavailable identities convert without cache publication |
| Generated-cache decision diagnostics | Supported | `usdgeo::cache::CacheDecision` publishes seven stable, transport-neutral categories; COPC projects them onto `COPC009`-`COPC012` and every message names its category |

## LAS Versions

| Version | Status | Notes |
| --- | --- | --- |
| 1.0 | Rejected | `unsupported LAS version` |
| 1.1 | Rejected | `unsupported LAS version` |
| 1.2 | Supported | 227-byte minimum header, legacy point count |
| 1.3 | Supported | 235-byte minimum header, legacy point count |
| 1.4 | Supported | 375-byte minimum header, 64-bit point count, EVLRs |

LAZ accepts the same versions. The LAZ reader clears the compression bit of the
point data record format byte before header inspection and then applies the
same LAS validation for formats 0-3 and 6-8. The bundled codec does not decode
compressed waveform formats 4, 5, 9, and 10, so those LAZ inputs are rejected.

## Point Data Record Formats

| Format | LAS version | Status | Decoded fields |
| ---: | --- | --- | --- |
| 0 | 1.2-1.4 | Supported | XYZ, intensity, returns, classification |
| 1 | 1.2-1.4 | Supported | Format 0 plus GPS time |
| 2 | 1.2-1.4 | Supported | Format 0 plus RGB |
| 3 | 1.2-1.4 | Supported | Format 0 plus GPS time and RGB |
| 4 | 1.3-1.4 | Supported | Format 1 plus waveform packet metadata |
| 5 | 1.3-1.4 | Supported | Format 3 plus waveform packet metadata |
| 6 | 1.4 only | Supported | XYZ, intensity, returns, classification, GPS time |
| 7 | 1.4 only | Supported | Format 6 plus RGB |
| 8 | 1.4 only | Supported | Format 7 fields plus NIR |
| 9 | 1.4 only | Supported | Format 6 plus waveform packet metadata |
| 10 | 1.4 only | Supported | Format 8 plus waveform packet metadata |

Formats 6-10 are accepted only in LAS 1.4. A file that declares format 6, 7,
or 10 in LAS 1.2 or 1.3 is rejected.

A record length shorter than the format minimum (20, 28, 26, 34, 57, 63, 30,
36, 38, 59, and 67 bytes for formats 0 through 10) is rejected. A longer
record length is accepted when its trailing bytes are declared by an Extra
Bytes VLR.

## Point Attributes

| Attribute | Source formats | Status | USD attribute |
| --- | --- | --- | --- |
| XYZ position | all | Supported | `points` (`point3f[]`, stage-local) |
| Intensity | all | Supported | `geo:intensity` (`int[]`) |
| Return number | all | Supported | `geo:returnNumber` (`uchar[]`) |
| Number of returns | all | Supported | `geo:numberOfReturns` (`uchar[]`) |
| Classification | all | Supported | `geo:classification` (`uchar[]`) |
| RGB | 2, 3, 7, 8, 10 | Supported | `geo:red`, `geo:green`, `geo:blue` (`int[]`) |
| GPS time | 1, 3, 6, 7, 8, 9, 10 | Supported | `geo:gpsTime` (`double[]`) |
| Classification flags | 6-10 | Supported | `geo:classificationFlags` (`uchar[]`) |
| Scanner channel | 6-10 | Supported | `geo:scannerChannel` (`uchar[]`) |
| Scan direction flag | all | Supported | `geo:scanDirectionFlag` (`uchar[]`) |
| Edge of flight line | all | Supported | `geo:edgeOfFlightLine` (`uchar[]`) |
| User data | all | Supported | `geo:userData` (`uchar[]`) |
| Scan angle / rank | all | Supported | `geo:scanAngle` (`int[]`) |
| Point source ID | all | Supported | `geo:pointSourceId` (`int[]`) |
| NIR | 8, 10 | Supported | `geo:nir` (`int[]`) |
| Waveform descriptor index | 4, 5, 9, 10 | Supported | `geo:waveformDescriptorIndex` (`uchar[]`) |
| Waveform data offset | 4, 5, 9, 10 | Supported | `geo:waveformDataOffset` (`uint64[]`) |
| Waveform packet size | 4, 5, 9, 10 | Supported | `geo:waveformPacketSize` (`uint[]`) |
| Return point waveform location | 4, 5, 9, 10 | Supported | `geo:returnPointWaveformLocation` (`float[]`) |
| Waveform parameters | 4, 5, 9, 10 | Supported | `geo:waveformXt`, `geo:waveformYt`, `geo:waveformZt` (`float[]`) |
| External waveform data flag | 4, 5, 9, 10 | Supported | `geo:waveformDataExternal` (`uchar[]`) |
| External waveform data file | 4, 5, 9, 10 | Supported | `geo:waveformDataFile` (`string`) |
| Extra Bytes | all | Types 1-30 supported when integer values are exactly representable as `double` | `geo:<normalized descriptor name>` (`double[]`, `double2[]`, or `double3[]`), with per-component descriptor scale and offset applied |

Positions are decoded as `double` in source coordinates using the header scale
and offset, then translated by the local origin and converted to the stage up
axis before being written as `float3`. Extra Bytes values are decoded from the
appended point-record data and written after applying their per-component
descriptor scale and offset. Types 1-10 are scalar, 11-20 are two-component,
and 21-30 are three-component. Non-finite values and integer values that
cannot be represented exactly as `double` are rejected.
Descriptor names are normalized deterministically to ASCII USD identifiers:
non-alphanumeric characters become `_`, a leading digit gains a leading `_`,
an empty name becomes `extra`, and collisions with built-in or prior names gain
`_2`, `_3`, and so on. The original descriptor names remain in the LAS header
metadata.

## VLR and EVLR

| Record | Status | Notes |
| --- | --- | --- |
| Any VLR | Read only | User ID, record ID, description, raw payload retained |
| Any EVLR (LAS 1.4) | Read only | Same fields, 64-bit length |
| `LASF_Projection` 2112 (WKT) | Supported | Extracted into `geo:wkt` |
| `LASF_Projection` 34735-34737 (GeoTIFF) | Supported | Key directory, double parameters, and ASCII parameters are parsed |
| `LASF_Spec` 4 (Extra Bytes) | Supported | Descriptor type, options, name, limits, scale, offset, and description are parsed |
| Waveform packet descriptors | Read only | Raw descriptors retained; per-point packet metadata is decoded |

## CRS

| Source | Status |
| --- | --- |
| WKT VLR / EVLR | Supported |
| GeoTIFF `KeyDirectoryTag` | Supported | Structured key directory is retained and horizontal EPSG keys are resolved |
| `GeoDoubleParamsTag` / `GeoAsciiParamsTag` | Parsed | Structured parameter arrays and text are retained on the LAS header |
| EPSG inference | Supported | Explicit WKT EPSG or GeoTIFF `ProjectedCSTypeGeoKey` / `GeographicTypeGeoKey` is authored as `geo:epsgCode` |
| Conflicting CRS detection | Supported | Conflicting WKT and GeoTIFF codes, or duplicate same-kind GeoTIFF codes, are rejected with `ConflictingCrs` |

When no WKT record is present, the plugins author a placeholder string in
`geo:wkt` stating that the CRS is unavailable. `geo:epsgCode` is populated from
GeoTIFF keys when available and is `0` when no EPSG code can be resolved.
`geo:projJson` is currently always empty.

## Authored USD

All three plugins produce the same layer shape.
The provisional public meaning and compatibility policy of every authored
`geo:*` attribute is defined in the
[point-cloud metadata contract](POINTCLOUD_METADATA.md).

| Item | Value |
| --- | --- |
| Prim path | `/PointCloud` |
| Prim type | `UsdGeomPoints` |
| Stage up axis | `Y` |
| Stage metersPerUnit | `1.0` |
| Local origin | Source-space minimum of the header bounds |
| Point widths | `UsdGeomPoints.widths`, constant value set to approximately one five-thousandth of the stage-local bounds diagonal |
| Display colors | `primvars:displayColor`, vertex interpolation when RGB data is available |

Dataset metadata authored on the prim:

| Attribute | Type | Meaning |
| --- | --- | --- |
| `geo:epsgCode` | `int` | EPSG code, `0` when unknown |
| `geo:wkt` | `string` | Source WKT, or an unavailability note |
| `geo:projJson` | `string` | Reserved; currently empty |
| `geo:linearUnit` | `string` | Linear unit name, `metre` by default |
| `geo:upAxis` | `string` | Stage up axis used for the conversion |
| `geo:localOrigin` | `double3` | Source-space origin subtracted from points |
| `geo:boundsMin` | `double3` | Stage-local bounds minimum |
| `geo:boundsMax` | `double3` | Stage-local bounds maximum |
| `geo:pointCount` | `uint64` | Point count authored from the header |

Source coordinates are reconstructed as
`sourceUpAxisTransform(localPosition) + geo:localOrigin`, where the source up
axis is `Z` and the stage up axis is `Y`, so
`source = (x + originX, -z + originY, y + originZ)`.

## Tile and LOD

Authoring-library capability and what a direct LAS, LAZ, or COPC read reaches are
reported separately, because they differ.

| Item | Authoring library | LAS/LAZ/COPC FileFormat read |
| --- | --- | --- |
| `usdLod` hierarchy authoring | Supported | Supported via `lod` profiles |
| `UsdLodRootAPI` | Supported | Authored when `lod` is not `off` |
| `UsdLodScreenSizeHeuristic` | Supported | Authored when `lod` is not `off` |
| `UsdLodOverrideAPI` | Not authored | Not authored |
| Deterministic fixed-stride sampling | Supported | Supported |
| Spatial tiling into per-tile `usdLod` roots | Supported | Supported |
| Payload-backed tile assets (one USDC payload per tile/LOD) | Supported | Supported |
| Bounded-memory generation during file open | Implemented for LAS, LAZ, COPC, and PLY tiled reads; generated-corpus benchmark available | Source streams, spool buffers, and one-tile reconstruction are bounded; generated-corpus and checked-in real-data measurements are available |
| LOD file-format arguments | n/a | `lod=off\|preview\|balanced\|quality` |
| Spatial tile file-format arguments | n/a | `tile=true`, `tileSize`, `tileMemoryLimit`, `payloadDirectory` for LAS, LAZ, COPC, and PLY |
| Reopening a tiled read over its own payloads | Supported when the caller names a payload owner | Supported: a read replaces the payloads its layer generated before and refuses any file it did not generate; see [payload ownership](../architecture/FILE_FORMAT_ARGUMENTS.md#generated-payload-ownership) |

With `lod=off` every read authors one `UsdGeomPoints` prim at `/PointCloud`.
The other profiles author a single non-tiled `usdLod` root with fixed-stride
levels and a screen-size heuristic.

Direct LAS, LAZ, and COPC FileFormat reads now stream decoded chunks into tile
spools and author one payload tile at a time. A Windows baseline against a
14.6-million-point Shizuoka LAS input, including payload working-set
measurement, is documented in [streaming and
tiling](../roadmap/streaming-and-tiling.md); broader real-world dataset
coverage remains open.

LOD is authored with the OpenUSD 26.08 `usdLod` schemas and nothing else. The project will not publish a repository-specific LOD schema, a
`lodLevel` primvar, or a variant-set convention, and LOD selection will remain
the host application's responsibility. The target namespace and invariants are
in the [tile and LOD contract](../architecture/LOD.md).

## Known Limitations

- Tiled reads spool points and reconstruct one tile at a time. Generated-corpus
  and checked-in real-data RSS, spool, payload, and payload working-set
  measurements are available through the documented benchmark paths; broader
  real-world dataset coverage remains open.
- Extra Bytes support covers types 1-30. Non-finite values and integers not
  exactly representable as `double` are rejected. Descriptor names are
  normalized before USD authoring.
- File-format arguments expose normalized chunk and point-range read options,
  attribute selection, compact LOD profiles, spatial tiling, bounds, and
  classification filters. See the
  [file-format argument contract](../architecture/FILE_FORMAT_ARGUMENTS.md) and
  the [plugin adapter contract](../architecture/PLUGIN_ADAPTER.md).
- `metadataOnly` reads author source metadata without decoding point records;
  they do not produce point positions.
- Positions are stored as `float` relative to the local origin. Absolute source
  precision is preserved only through `geo:localOrigin`.
- Waveform sample data is not fetched or interpreted; packet offsets, sizes,
  parameters, external-data flag, and sibling `.wdp` reference are retained
  for deferred loading.
- Reader errors expose shared typed diagnostics and remain projected to stable
  `LASxxx`, `LAZxxx`, or `COPCxxx` plugin prefixes. See
  [diagnostics](../architecture/DIAGNOSTICS.md).
- Advanced tile planning such as adaptive depth and point-budget splitting is
  not exposed through LAS/LAZ/COPC/PLY file-format arguments. Those interfaces
  provide fixed-grid `tileSize` and `tileMemoryLimit`; the conversion tool
  additionally accepts point-budget arguments and uses two bounded stream
  passes before payload authoring. The conversion tool can generate and reuse
  deterministic USDC entries with `--cache-root`;
  direct LAS, LAZ, COPC, and PLY FileFormat lookup reuses committed entries
  through `USDGEO_CACHE_ROOT`. Reuse requires either a stable local filesystem
  identity or a `Stable` resolver identity; `Unstable` and `Unavailable`
  identity fails closed and reads from the source. The conversion tool
  publishes entries for local LAS, LAZ, and COPC paths plus resolver-addressable
  COPC identifiers when the resolver provides a `Stable` identity; unstable
  and unavailable resolver identities remain conversion-only.
- No HTTP client, cloud SDK, authentication flow, retry policy, or raw
  byte-range cache exists here. Resolver-backed reads consume whatever the
  active `ArResolver` provides, and no resolver implementation is a build-time
  dependency.
- Point decoding assumes a little-endian host.
- Writing LAS, LAZ, or COPC is out of scope; all three plugins export as
  `usda`.
