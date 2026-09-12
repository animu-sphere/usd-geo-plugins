#pragma once

#include "usdgeo/GeoReference.h"
#include "usdpointcloud/PointCloud.h"
#include "usdpointcloud/Lod.h"
#include "usdpointcloud/Spool.h"
#include "usdpointcloud/Tiling.h"

#include <pxr/usd/usd/stage.h>
#include <pxr/usd/sdf/layer.h>

#include <cstdint>
#include <cstddef>
#include <filesystem>
#include <functional>
#include <string>
#include <vector>

namespace usdgeo {

class PointCloudLayer {
public:
    using Data = usdpointcloud::PointData;

    static pxr::UsdStageRefPtr CreateStage();

    static bool AuthorPointCloud(const pxr::UsdStageRefPtr& stage,
                                 const std::string& primPath,
                                 const GeoReference& reference,
                                 const SpatialBounds& bounds,
                                 const usdpointcloud::PointChunk& chunk,
                                 const std::vector<Vec3d>& positions);

    static bool AuthorPointCloud(const pxr::UsdStageRefPtr& stage,
                                 const std::string& primPath,
                                 const GeoReference& reference,
                                 const SpatialBounds& bounds,
                                 const usdpointcloud::PointChunk& chunk,
                                 const Data& data);
};

enum class PointCloudAuthorFailure {
    None,
    InvalidLayer,
    StageCreation,
    StageMetrics,
    PointCloud,
};

bool AuthorPointCloudAsset(
    pxr::SdfLayer* layer,
    const std::string& primPath,
    const usdpointcloud::PointCloudAsset& asset);

bool AuthorPointCloudAsset(
    pxr::SdfLayer* layer,
    const std::string& primPath,
    const usdpointcloud::PointCloudAsset& asset,
    PointCloudAuthorFailure& failure);

bool AuthorPointCloudAsset(
    pxr::SdfLayer* layer,
    const std::string& primPath,
    const GeoReference& reference,
    const SpatialBounds& bounds,
    const usdpointcloud::PointChunk& chunk,
    const PointCloudLayer::Data& data);

bool AuthorPointCloudAsset(
    pxr::SdfLayer* layer,
    const std::string& primPath,
    const GeoReference& reference,
    const SpatialBounds& bounds,
    const usdpointcloud::PointChunk& chunk,
    const PointCloudLayer::Data& data,
    PointCloudAuthorFailure& failure);

bool AuthorPointCloudAsset(
    const pxr::UsdStageRefPtr& stage,
    const std::string& primPath,
    const GeoReference& reference,
    const SpatialBounds& bounds,
    const usdpointcloud::PointChunk& chunk,
    const std::vector<Vec3d>& positions);

bool AuthorPointCloudAsset(
    const pxr::UsdStageRefPtr& stage,
    const std::string& primPath,
    const GeoReference& reference,
    const SpatialBounds& bounds,
    const usdpointcloud::PointChunk& chunk,
    const PointCloudLayer::Data& data);

bool AuthorPointCloudMetadata(
    pxr::SdfLayer* layer,
    const std::string& primPath,
    const GeoReference& reference,
    const SpatialBounds& bounds,
    const usdpointcloud::PointChunk& chunk,
    const struct PointCloudSourceMetadata& sourceMetadata);

bool AuthorPointCloudLodAsset(
    const pxr::UsdStageRefPtr& stage,
    const std::string& primPath,
    const std::vector<usdpointcloud::PointCloudAsset>& levels,
    const usdpointcloud::PointLodHierarchy& hierarchy);

bool AuthorPointCloudLodAsset(
    pxr::SdfLayer* layer,
    const std::string& primPath,
    const std::vector<usdpointcloud::PointCloudAsset>& levels,
    const usdpointcloud::PointLodHierarchy& hierarchy);

struct PointCloudTileAsset {
    usdpointcloud::PointTile tile;
    std::vector<usdpointcloud::PointCloudAsset> levels;
};

struct PointCloudSourceMetadata {
    std::uint8_t pointFormat = 0;
    Vec3d scale;
    Vec3d offset;
};

struct PointCloudPayloadOptions {
    std::string directory;
    std::string rootLayerPath;
    std::size_t tileMemoryLimitBytes = 64 * 1024 * 1024;
    std::function<bool()> isCancelled;
    std::function<void(std::size_t)> onBufferedBytes;
    std::vector<usdpointcloud::PointTileManifestEntry>* tileManifestEntries =
        nullptr;
    usdpointcloud::SpoolIoStats* spoolIoStats = nullptr;
    // Identity of the layer the generated payloads belong to. When empty the
    // payload directory is exclusive: payloads are written straight into it
    // and any existing payload path refuses the write. When set, payloads
    // live in `directory/<owner key>/<generation>/`, where the owner key is a
    // hash of this value: each generation is staged privately and published
    // whole, identical content reuses the published generation, and
    // superseded generations of the same owner are removed. Nothing outside
    // the owner's directory is touched, and the value is never persisted.
    std::string owner;
    std::string spoolDirectory;
};

// The payload owner for a FileFormat read: which source the layer reads and
// the layer's file-format arguments exactly as it holds them. A local source
// is named relative to the payload directory, so a project that moves as a
// whole keeps its owner; any other source is named by its resolved
// identifier.
std::string PointCloudPayloadOwner(
    const std::string& source,
    const std::filesystem::path& payloadDirectory,
    const pxr::SdfLayer::FileFormatArguments& arguments);

bool AuthorPointCloudTiledAssetFromStream(
    pxr::SdfLayer* layer,
    const std::string& primPath,
    usdpointcloud::PointStream& stream,
    const GeoReference& reference,
    const usdpointcloud::TileRouter& router,
    const PointCloudPayloadOptions& options,
    std::vector<usdgeo::Diagnostic>& diagnostics);

bool AuthorPointCloudTiledAssetFromStream(
    pxr::SdfLayer* layer,
    const std::string& primPath,
    usdpointcloud::PointStream& stream,
    const GeoReference& reference,
    const usdpointcloud::TileGridConfig& tileConfig,
    const PointCloudPayloadOptions& options,
    std::vector<usdgeo::Diagnostic>& diagnostics);

bool AuthorPointCloudTiledAsset(
    const pxr::UsdStageRefPtr& stage,
    const std::string& primPath,
    const std::vector<PointCloudTileAsset>& tiles);

bool AuthorPointCloudTiledAsset(
    pxr::SdfLayer* layer,
    const std::string& primPath,
    const std::vector<PointCloudTileAsset>& tiles);

bool AuthorPointCloudTiledAssetWithPayloads(
    const pxr::UsdStageRefPtr& stage,
    const std::string& primPath,
    const std::vector<PointCloudTileAsset>& tiles,
    const PointCloudPayloadOptions& options);

bool AuthorPointCloudTiledAssetWithPayloads(
    const pxr::UsdStageRefPtr& stage,
    const std::string& primPath,
    const std::vector<PointCloudTileAsset>& tiles,
    const PointCloudPayloadOptions& options,
    std::vector<std::filesystem::path>& generatedPayloads);

// Authors the payload-backed tiles into a detached stage and transfers the
// result into `layer`. On failure the layer is unchanged, nothing this call
// wrote remains, and `diagnostics` explains why.
bool AuthorPointCloudTiledAssetWithPayloads(
    pxr::SdfLayer* layer,
    const std::string& primPath,
    const std::vector<PointCloudTileAsset>& tiles,
    const PointCloudPayloadOptions& options,
    std::vector<usdgeo::Diagnostic>& diagnostics);

} // namespace usdgeo