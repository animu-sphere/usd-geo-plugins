#pragma once

#include "GeneratedPayloadSet.h"

#include "usdgeo/PointCloudLayer.h"

#include <cstddef>
#include <filesystem>
#include <string>
#include <vector>

namespace usdgeo::detail {

// An in-memory stage whose content is built for transfer into a caller's
// layer. It loads nothing, so a payload arc authored into it is never
// resolved or opened while the content is built, and it never needs a file
// identity to anchor one.
pxr::UsdStageRefPtr CreateDetachedStage();

std::filesystem::path TilePayloadPath(const std::filesystem::path& directory,
                                      const usdpointcloud::PointTileId& id,
                                      std::size_t lodIndex);

std::vector<std::filesystem::path> TilePayloadPaths(
    const std::filesystem::path& directory,
    const std::vector<PointCloudTileAsset>& tiles);

// Authors payload-backed tiles whose payload paths `payloads` has already
// claimed, writing each payload file and marking it written.
bool AuthorClaimedTilePayloads(const pxr::UsdStageRefPtr& stage,
                               const std::string& primPath,
                               const std::vector<PointCloudTileAsset>& tiles,
                               const PointCloudPayloadOptions& options,
                               GeneratedPayloadSet& payloads);

} // namespace usdgeo::detail
