# pointcloud-copc

`pointcloud-copc` is the thin OpenUSD FileFormat Plugin adapter for read-only
COPC assets. It delegates COPC metadata, hierarchy validation, and selected
LAZ chunk decoding to `usdCopc`, then uses the shared point-cloud authoring
path. The plugin opens the resolved asset through the active `ArResolver` and
adapts its `ArAsset` to the project-owned random-access source contract.

Transport is not this plugin's responsibility. Whatever resolver the host
configures owns the network protocol, authentication, retries, redirects, and
raw byte caching, and the plugin never detects which resolver opened the asset.
An external resolver such as `usd-http-resolver` is runtime composition through
`PXR_PLUGINPATH_NAME`, not a build-time dependency of this bundle.

The repository's resolver-backed integration test uses the independent
`tests/plugins/httpresolver` fixture. It is a Tier 1 test double which serves a
local fixture as an in-memory asset; a production resolver can replace it
without changing this plugin or `usdCopc`. See the
[resolver-backed source contract](../../docs/architecture/RESOLVER_SOURCE.md).

The adapter supports metadata-only reads, non-tiled reads, and native
hierarchy tiles authored as payload-backed `usdLod` roots. Each COPC
point-data node becomes one shared tile with its native spacing preserved.
Source point ranges are rejected because COPC hierarchy order is spatial
rather than LAS source order; bounds and classification filters are applied
while decoded points pass through a bounded buffer. Remote COPC is supported
only when the active resolver supplies an asset with efficient random-access
reads, such as HTTP byte-range support. The plugin does not implement an HTTP
client, transport retries, or a network cache. Remote tiled reads require an
absolute local `payloadDirectory`. Each tiled layer publishes its payloads in
a directory of its own below it, named by a hash, so no resolver identifier is
written. See
[payload ownership](../../docs/architecture/FILE_FORMAT_ARGUMENTS.md#generated-payload-ownership).

Generated-USDC cache lookup is enabled for a stable local filesystem identity
and for a `Stable` resolver identity. `Unstable` and `Unavailable` identity
fails closed: the read proceeds from the source and says why. This plugin does
not publish entries; `usd-pointcloud-convert` publishes entries for local
`.copc` and `.copc.laz` inputs and resolver-addressable COPC identifiers when
the resolver supplies a `Stable` identity.

Every cache decision is reported through four codes that project the stable
categories `usdgeo::cache` publishes, each message naming its exact category:

| Code | Severity | Categories |
| --- | --- | --- |
| `COPC009` | warning | `resolver-identity-unavailable`, `resolver-identity-unstable`, `generated-cache-reuse-disabled` |
| `COPC010` | status | `resolver-identity-stable`, `generated-cache-hit` |
| `COPC011` | status | `resolver-identity-changed` |
| `COPC012` | warning | `generated-cache-invalidated` |

Interoperability with an external resolver is recorded in the
[resolver read baseline](../../docs/reference/RESOLVER_BASELINE.md).
