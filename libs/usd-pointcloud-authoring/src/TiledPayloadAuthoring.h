#pragma once

#include "PayloadGeneration.h"

#include "usdgeo/PointCloudLayer.h"

#include <cstddef>
#include <string>
#include <vector>

namespace usdgeo::detail {

// An in-memory stage whose content is built for transfer into a caller's
// layer. It loads nothing, so a payload arc authored into it is never
// resolved or opened while the content is built, and it never needs a file
// identity to anchor one.
pxr::UsdStageRefPtr CreateDetachedStage();

std::string TilePayloadName(const usdpointcloud::PointTileId& id,
                            std::size_t lodIndex);

std::vector<std::string> TilePayloadNames(
    const std::vector<PointCloudTileAsset>& tiles);

// Authors payload-backed tiles whose payload names `payloads` has declared,
// writing each payload where the generation stages it. Manifest entries are
// appended to `manifestEntries` and name the staged payloads until
// CommitTilePayloads repoints them.
bool AuthorClaimedTilePayloads(
    const pxr::UsdStageRefPtr& stage,
    const std::string& primPath,
    const std::vector<PointCloudTileAsset>& tiles,
    const PointCloudPayloadOptions& options,
    PayloadGeneration& payloads,
    std::vector<usdpointcloud::PointTileManifestEntry>& manifestEntries);

// Publishes `payloads`, then points the payload arcs in `layer` and the
// manifest entries at the published generation instead of the staging
// directory.
bool CommitTilePayloads(
    const pxr::SdfLayerHandle& layer,
    PayloadGeneration& payloads,
    std::vector<usdpointcloud::PointTileManifestEntry>& manifestEntries,
    std::string& error);

// Replaces one exact path component in every payload asset path in `layer`.
void RepointPayloads(const pxr::SdfLayerHandle& layer,
                     const std::string& fromComponent,
                     const std::string& toComponent);

} // namespace usdgeo::detail
