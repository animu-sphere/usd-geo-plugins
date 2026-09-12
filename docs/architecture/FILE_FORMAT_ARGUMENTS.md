# File-Format Argument Contract

Section 9 of the LOD policy allows file-format arguments to control tiling and
LOD generation, and the [workspace contract](WORKSPACE.md)
requires arguments to be normalized before any reader or cache lookup. This
document defines that contract.

Status: **implemented for LAS, LAZ, and PLY**. LAS and LAZ normalize chunk,
point-range, attribute, bounds, classification, and spatial tiled-payload
options before the shared reader and authoring path. The compact `lod` profile
is parsed and canonicalized, and single-root LOD authoring is available.
PLY uses the same read controls and accepts explicit `epsg`, `linearUnit`,
`sourceUpAxis`, and `stageUpAxis` values; PLY requires `epsg` because it has no
embedded georeference.

## Why Arguments Exist

An argument changes what a layer contains. It therefore changes layer identity:
two layers for the same file with different arguments are different layers, and
they must not share a cache entry. Getting this wrong produces stale geometry
that survives an edit, which is worse than having no arguments at all.

Arguments are the only sanctioned way to supply information the source file
does not carry. A missing CRS on a PLY or CSV file is reported as a diagnostic
and can be supplied by an argument; it is never guessed. See the
[design policy](../design/DESIGN_POLICY.md) section 4.7.

## Syntax

Arguments use the standard OpenUSD encoding:

```usda
def "Survey" (
    references = @./sample.las:SDF_FORMAT_ARGS:lod=balanced&attributes=xyz,rgb@
)
{
}
```

```bash
usdcat "sample.laz:SDF_FORMAT_ARGS:lod=preview"
```

The plugins do not invent a second argument syntax.

## Candidate Arguments

| Argument | Controls | Alters topology |
| --- | --- | --- |
| `lod` | Compact LOD profile: `off`, `preview`, `balanced`, `quality` | Yes |
| `lodLevels` | Number of LOD items per root | Yes |
| `lodPointCounts` | Target point count per LOD item | Yes |
| `lodRatios` | Retention ratio per LOD item | Yes |
| `lodThresholds` | Screen-size thresholds | No |
| `tile` | Whether to partition spatially | Yes |
| `tileDepth` | Partition depth | Yes |
| `tilePointLimit` | Point budget that triggers a split | Yes |
| `sampling` | Sampling algorithm | Yes |
| `attributes` | Attribute subset to author | Yes |
| `classification` | Classification filter | Yes |
| `bounds` | Spatial filter | Yes |
| `originMode` | Local-origin policy | No |
| `upAxis` | Stage up-axis conversion | No |
| `epsg` | Explicit source CRS identifier for formats without embedded CRS | No |
| `linearUnit` | Source linear unit | No |
| `sourceUpAxis` | Source coordinate up axis (`X`, `Y`, or `Z`) | No |
| `stageUpAxis` | Authored stage up axis (`X`, `Y`, or `Z`) | No |

"Alters topology" means the authored namespace or point count changes, not just
attribute values. An argument in that column must never reuse a cached layer
produced with a different value.

The current implementation also accepts the bounded reader controls and the
two source-point filters. `bounds` is six comma-separated finite numbers in
`minX,minY,minZ,maxX,maxY,maxZ` order, inclusive. `classification` is a
comma-separated list of unsigned values from 0 through 255; values are
deduplicated and sorted during normalization. Filters are evaluated in source
coordinates before stage-local transforms. Metadata-only reads still describe
the complete source and do not apply point delivery filters.

The current pre-LOD surface also exposes the existing streaming reader controls
because they do not change authored topology:

| Argument | Normalized value |
| --- | --- |
| `chunkPointLimit` | Positive unsigned integer |
| `memoryBudgetBytes` | Positive unsigned integer |
| `rangeFirstPoint` | Unsigned source-point index |
| `rangePointCount` | Unsigned count; zero means all remaining points |
| `attributes` | Comma-separated supported names; `xyz` is implicit |

The tiled surface also exposes:

| Argument | Normalized value |
| --- | --- |
| `tile` | `true` when tiling is enabled |
| `tileSize` | Positive source-coordinate tile size |
| `tileMemoryLimit` | Positive total spool working-set limit in bytes |
| `payloadDirectory` | Payload output directory; see [generated payload ownership](#generated-payload-ownership) |

The explicit `usd-pointcloud-convert` tool additionally accepts the generic
point-budget arguments `maxPointsPerTile`, `minPointsPerTile`, and `maxDepth`.
`maxDepth` is an unsigned value from 0 through 31.
They are normalized into the conversion manifest and cache identity, then
planned with two bounded stream passes before adaptive payload authoring.
These arguments are intentionally not accepted by LAS, LAZ, COPC, or PLY
FileFormat reads; those interactive paths retain fixed-grid `tileSize`
behavior.

The `lod` profile is normalized as `off`, `preview`, `balanced`, or `quality`.
`off` is the default and has the same canonical identity as an omitted
argument. The other profiles author a single non-tiled `usdLod` root using
versioned fixed-stride samples. Explicit level counts and the remaining
topology-generation candidates are recognized as planned arguments and
rejected until their shared contracts are implemented. The spatial set is
implemented for LAS and LAZ and produces payload-backed tile roots; `tile` and
`lod` are mutually exclusive.

`NormalizeFileFormatArguments` also returns a canonical argument map. Static
hosts must pass that map to `SdfLayer::FindOrOpen` or
`SdfLayer::CreateIdentifier` before layer lookup. A static
`SdfFileFormat::Read` implementation runs after that lookup and cannot repair a
non-canonical layer identifier. The implemented format-specific dynamic LOD
fields map
back into this same normalized argument path; other dynamic composition is
deliberately not part of the current plugin contract.

Dynamic LAS, LAZ, and COPC payloads may author their registered `pc_las_lod`,
`pc_laz_lod`, or `pc_copc_lod` prim metadata field. Values are the same compact
profiles as `lod`, and changing the field recomposes the payload. The fields
are format-specific because OpenUSD registers plugin metadata globally. They
do not replace `SDF_FORMAT_ARGS` for attributes, filters, ranges, tiling, or
payload paths.

Derived cache storage is configured independently through the optional
`USDGEO_CACHE_ROOT` host environment variable. It is intentionally not a
file-format argument and therefore does not affect layer identity or cache
descriptor identity. When a committed entry exists for the source, normalized
arguments, and reader metadata, the LAS, LAZ, or COPC adapter loads it before
point delivery; a miss follows the normal reader and authoring path.

## Generated Payload Ownership

A tiled read writes its payloads into `payloadDirectory` and authors a root
that references them by relative path. The root lives only in the opened
layer, so every read of the layer writes the payloads again, by generating
them or by materializing a cache hit. Ownership of the files in the directory
is therefore explicit:

- The owner of a payload set is the layer's identity: the resolved source and
  its normalized arguments. The directory records it in
  `payload-owner-<key>.manifest`, where `<key>` is a 64-bit hash of that
  identity. The record lists relative payload names only; no source path,
  resolver identifier, or validation token is written.
- Reading the same layer again replaces the payloads that layer generated
  before and removes those it no longer generates. This is what lets a host
  reopen or reload a tiled layer.
- A file the layer did not generate is never replaced. When another source,
  other arguments, or a user file already holds a payload name, the read fails
  with the plugin's author-failure code, names the file, and leaves the
  directory unchanged.
- Ownership is recorded before the first payload is written, so an
  interrupted read leaves payloads the next read of the same layer replaces. A
  read that fails or is cancelled removes the payloads it wrote, restores the
  previous record, and removes a directory it created.
- Materializing a committed `USDGEO_CACHE_ROOT` entry into `payloadDirectory`
  follows the same rule. An existing file that already holds the cached bytes
  is taken over, so payloads materialized before ownership was recorded keep
  resolving.

`usd-pointcloud-convert` does not share this rule. Its output root and payload
directory must not exist yet, and its transaction marker owns recovery.

## Rules

1. **Validate everything.** An unknown key, an unparsable value, or an
   out-of-range value produces a typed diagnostic. Unknown keys are not
   silently ignored, because a typo would otherwise be indistinguishable from a
   default.
2. **Defaults are deterministic** and documented. An absent argument and an
   argument set to its default value produce identical output.
3. **Normalize before use.** Key order, whitespace, numeric formatting, and
   list order are normalized into one canonical form before the reader or the
   cache sees them. Static callers use the canonical argument map when creating
   or opening the layer; the plugin consumes the map already stored on the
   resulting layer.
4. **Normalized arguments participate in layer identity and cache keys.**
   Equivalent arguments that normalize to the same form share a cache entry;
   arguments that do not, do not.
5. **Unsupported combinations produce stable diagnostics**, not a silently
   narrowed result. `lod=off` combined with `lodLevels=3` is a conflict, not a
   precedence puzzle.
6. **Arguments never replace the standard USD representation.** They control
   generation. LOD is still authored as `usdLod`; see the
   [tile and LOD contract](LOD.md).
7. **Normalization lives at the plugin boundary, validation in the shared
   layer.** OpenUSD parses `SDF_FORMAT_ARGS` into the layer argument map; the
   plugin converts that map into a project-owned options struct. The readers
   and `usdPointCloudAuthoring` validate that struct and know nothing about Sdf argument
   encoding.

## Relationship to Read Options

`PointReadOptions` already carries `chunkPointLimit`, `memoryBudgetBytes`,
`range`, and `isCancelled`; see
[point reader architecture](POINT_READER.md). Arguments are how a host reaches
those fields, and both plugins now normalize arguments and pass the supported
options through, as recorded in the
[plugin adapter contract](PLUGIN_ADAPTER.md).

Not every read option becomes an argument. `isCancelled` is a host-supplied
callback and never appears in a layer identifier. A memory budget is a
candidate but is a host policy rather than a property of the asset, so it
should not silently change authored output.

## Testing

- Equivalent arguments in different orders normalize to one cache key.
- Different topology-altering arguments never collide in the cache.
- Unknown keys and out-of-range values produce stable diagnostic codes.
- Conflicting combinations are rejected rather than resolved by precedence.
- A default-valued argument and an absent argument produce identical layers.
- LAS and LAZ accept the same arguments with the same meaning.
- A tiled layer read a second time regenerates over its own payloads, never
  replaces a file another layer or a user placed, and restores its ownership
  record when the read fails.
