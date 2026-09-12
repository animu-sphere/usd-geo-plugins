# usdPointCloudTiling

`usdPointCloudTiling` defines format-independent spatial partitioning
contracts for point clouds. It assigns source-coordinate points to stable
fixed-grid tile IDs without depending on LAS, LAZ, OpenUSD, or filesystem
spooling.

CMake target `usdpointcloud::tiling` and namespace `usdpointcloud`.

The initial contract contains `TileGridConfig`, `TileRouter`, and
`FixedGridTileRouter`. Tile indices use `floor(sourceCoordinate / tileSize)`,
so points on a boundary enter the tile to their positive side and negative
coordinates remain deterministic. The router uses source X/Y and ignores Z.

`PointBudgetConfig` and `BuildPointBudgetPlan` provide deterministic adaptive
planning over source X/Y positions. The planner preserves the fixed-grid
configuration separately, honors minimum and maximum points per tile and a
maximum depth, and reports point count, tile count, minimum, maximum, and
average leaf density, split count, and reached depth in `PointBudgetPlan`.
When the source distribution cannot satisfy the requested budget, the planner
returns a failure and emits a typed diagnostic instead of silently relaxing the
limit.

`TilePlan` is the common planning representation for sequential and native
hierarchy planners. It carries the planner identity and version, tile bounds,
point counts, parent and child relationships, and optional source byte ranges.
`BuildTilePlan` currently adapts the existing point-budget output without
changing its partitioning behavior; COPC native hierarchy mapping will use the
same contract. `TilePlanCacheArguments` and `StableTilePlanKey` provide a
deterministic cache identity over that representation, including planner
identity and version, node relationships, bounds, counts, and source ranges.

`SpoolSchema`, `TileSpoolWriter`, and `TileSpoolReader` define a versioned,
source-and-stage-coordinate binary spool. Records are written in append order,
the footer commits the point count, and readers use the fixed record layout to
reject missing or truncated footers. Attribute values are serialized using the
scalar types declared by the schema. The writer buffers records up to its
configured byte threshold before flushing; streaming callers may combine
multiple writers under a total working-set budget. Callers own the working
directory and can remove it with `RemoveSpoolDirectory`. The OpenUSD
authoring bridge places stream spools in a caller-selected directory or an
owner-scoped `.spool` directory, marks it before writing, and removes a
marked leftover before the next run so an interrupted process can be
recovered without scanning the system temporary directory.

`PointTileManifest` records the tile ID, LOD level, source bounds, point count,
and portable payload path for generated tile assets. Its v1 serializer sorts
entries by tile ID and LOD, rejects duplicate entries and unsafe paths, and
produces deterministic key-value output. The conversion tool writes this
manifest as `tiles.manifest` beside the payloads; the existing conversion
manifest remains the source-argument and payload-list sidecar.
