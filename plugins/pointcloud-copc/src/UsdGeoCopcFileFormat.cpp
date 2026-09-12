#include "usdgeocopc/UsdGeoCopcFileFormat.h"
#include "usdgeocopc/UsdGeoCopcDiagnostics.h"
#include "usdgeo/ArAssetRandomAccessSource.h"

#include "usdgeo/Diagnostic.h"
#include "usdgeo/PointCloudCache.h"
#include "usdgeo/PointCloudLayer.h"
#include "usdpointcloud/FileFormatArguments.h"
#include "usdpointcloud/Sampling.h"
#include "usdcopc/Copc.h"
#include "usdlaz/Laz.h"

#include <pxr/base/tf/diagnostic.h>
#include <pxr/base/tf/registryManager.h>
#include <pxr/base/vt/value.h>
#include <pxr/usd/ar/resolver.h>
#include <pxr/usd/ar/assetInfo.h>
#include <pxr/usd/pcp/dynamicFileFormatContext.h>

#include <algorithm>
#include <cstdint>
#include <filesystem>
#include <limits>
#include <map>
#include <string>
#include <vector>

PXR_NAMESPACE_OPEN_SCOPE

namespace {

const TfToken DynamicLodField("pc_copc_lod");

std::string DynamicTokenValue(const VtValue& value) {
    if (value.IsHolding<TfToken>()) {
        return value.UncheckedGet<TfToken>().GetString();
    }
    if (value.IsHolding<std::string>()) {
        return value.UncheckedGet<std::string>();
    }
    return {};
}

std::string DiagnosticDetail(
    const std::vector<usdgeo::Diagnostic>& diagnostics,
    const std::string& fallback) {
    if (diagnostics.empty()) {
        return fallback;
    }
    const auto& diagnostic = diagnostics.front();
    std::string detail = diagnostic.message;
    if (diagnostic.byteOffset) {
        detail += " (byte offset " +
                  std::to_string(*diagnostic.byteOffset) + ")";
    }
    if (diagnostic.pointIndex) {
        detail += " (point " + std::to_string(*diagnostic.pointIndex) + ")";
    }
    return detail;
}

const char* ReaderDiagnosticCode(
    const std::vector<usdgeo::Diagnostic>& diagnostics) {
    if (!diagnostics.empty() &&
        (diagnostics.front().code == usdgeo::DiagnosticCode::InvalidOffset ||
         diagnostics.front().code == usdgeo::DiagnosticCode::SourceOpenFailed ||
         diagnostics.front().code ==
             usdgeo::DiagnosticCode::SourceSizeUnavailable)) {
        return usdgeocopc::diagnostics::FileOpenFailed;
    }
    return usdgeocopc::diagnostics::DecodeFailed;
}

// Every generated-cache decision reaches OpenUSD through one of four plugin
// codes and always carries the stable category name, so a consumer can match
// on the category rather than on prose. The projection lives in the
// diagnostics header so a test asserts the same code and text OpenUSD is told.
void ReportCacheDecision(usdgeo::cache::CacheDecision decision) {
    const auto message = usdgeocopc::diagnostics::DecisionMessage(decision);
    if (usdgeocopc::diagnostics::DecisionIsWarning(decision)) {
        TF_WARN("%s", message.c_str());
    } else {
        TF_STATUS("%s", message.c_str());
    }
}

bool IsLocalFileSource(const std::string& path) {
    std::error_code error;
    return std::filesystem::is_regular_file(std::filesystem::path(path), error) &&
           !error;
}

std::shared_ptr<usdgeo::RandomAccessSource> OpenResolvedAssetSource(
    const std::string& resolvedPath,
    std::string& error,
    usdgeo::cache::SourceIdentity& resolverIdentity,
    usdgeo::cache::ResolverIdentityStability& resolverStability) {
    resolverIdentity = {};
    resolverStability =
        usdgeo::cache::ResolverIdentityStability::Unavailable;
    const auto asset = pxr::ArGetResolver().OpenAsset(
        pxr::ArResolvedPath(resolvedPath));
    if (!asset) {
        error = "active resolver could not open COPC asset: " + resolvedPath;
        return nullptr;
    }
    std::string identityError;
    usdgeo::TryBuildResolverSourceIdentity(
        pxr::ArGetResolver(), resolvedPath, pxr::ArResolvedPath(resolvedPath),
        *asset, resolverIdentity, resolverStability, identityError);
    return std::make_shared<usdgeo::ArAssetRandomAccessSource>(
        asset, resolvedPath);
}

bool IsSelectedPoint(const usdlas::LasPoint& point,
                     std::uint64_t pointIndex,
                     const usdpointcloud::PointReadOptions& options) {
    if (pointIndex < options.range.firstPoint) {
        return false;
    }
    if (options.range.pointCount != 0 &&
        pointIndex - options.range.firstPoint >= options.range.pointCount) {
        return false;
    }
    return usdlas::MatchesReadOptions(point, options);
}

bool BuildTiledAssets(
    usdcopc::CopcReader& reader,
    const usdcopc::CopcHeader& header,
    const usdcopc::CopcHierarchy& hierarchy,
    const usdgeo::GeoReference& reference,
    const usdpointcloud::PointReadRequest& request,
    const std::string& sourcePath,
    std::vector<usdgeo::PointCloudTileAsset>& assets,
    std::vector<usdgeo::Diagnostic>& diagnostics) {
    assets.clear();
    const auto bytesPerPoint =
        sizeof(usdlas::LasPoint) + header.las.pointRecordLength;
    const auto budgetPointLimit =
        request.readOptions.memoryBudgetBytes / bytesPerPoint;
    if (budgetPointLimit == 0) {
        diagnostics.push_back(
            {usdgeo::DiagnosticCode::InvalidFormatArgument,
             usdgeo::Severity::Error,
             "COPC memory budget is too small for one point", std::nullopt,
             std::nullopt});
        return false;
    }

    usdpointcloud::TilePlan tilePlan;
    if (!usdcopc::BuildTilePlan(hierarchy, tilePlan, diagnostics)) {
        return false;
    }

    std::map<std::string, const usdcopc::CopcNode*> hierarchyNodes;
    for (const auto& node : hierarchy.nodes) {
        hierarchyNodes.emplace(node.tile.ToString(), &node);
    }
    std::map<std::string, const usdpointcloud::TilePlanNode*> planNodes;
    std::vector<const usdpointcloud::TilePlanNode*> pointDataNodes;
    for (const auto& node : tilePlan.nodes) {
        planNodes.emplace(node.id.ToString(), &node);
        if (!node.sourceRanges.empty()) {
            if (node.sourceRanges.size() != 1) {
                diagnostics.push_back(
                    {usdgeo::DiagnosticCode::InvalidPointTile,
                     usdgeo::Severity::Error,
                     "COPC tile plan contains multiple source ranges for a point-data node",
                     std::nullopt, std::nullopt});
                return false;
            }
            pointDataNodes.push_back(&node);
        }
    }

    std::map<std::string, std::vector<usdgeo::TileId>> authoredChildren;
    for (const auto* node : pointDataNodes) {
        auto parent = node->parent;
        while (parent.level >= 0) {
            const auto parentNode = planNodes.find(parent.ToString());
            if (parentNode == planNodes.end()) {
                diagnostics.push_back(
                    {usdgeo::DiagnosticCode::InvalidPointTile,
                     usdgeo::Severity::Error,
                     "COPC tile plan contains a missing point-data ancestor",
                     std::nullopt, std::nullopt});
                return false;
            }
            if (!parentNode->second->sourceRanges.empty()) {
                authoredChildren[parent.ToString()].push_back(node->id);
                break;
            }
            parent = parentNode->second->parent;
        }
    }
    for (auto& entry : authoredChildren) {
        std::sort(entry.second.begin(), entry.second.end(),
                  [](const auto& left, const auto& right) {
                      if (left.level != right.level) {
                          return left.level < right.level;
                      }
                      if (left.x != right.x) return left.x < right.x;
                      if (left.y != right.y) return left.y < right.y;
                      return left.z < right.z;
                  });
    }

    assets.reserve(pointDataNodes.size());
    for (const auto* planNode : pointDataNodes) {
        if (request.readOptions.isCancelled &&
            request.readOptions.isCancelled()) {
            diagnostics.push_back(
                {usdgeo::DiagnosticCode::DecodeFailure,
                 usdgeo::Severity::Error,
                 "COPC tiled read was cancelled", std::nullopt,
                 std::nullopt});
            return false;
        }
        const auto hierarchyNode = hierarchyNodes.find(planNode->id.ToString());
        if (hierarchyNode == hierarchyNodes.end() ||
            !hierarchyNode->second->hasPointData) {
            diagnostics.push_back(
                {usdgeo::DiagnosticCode::InvalidPointTile,
                 usdgeo::Severity::Error,
                 "COPC tile plan references a missing point-data node",
                 std::nullopt, std::nullopt});
            return false;
        }
        const auto nativePointCount = hierarchyNode->second->pointCount;
        const auto sourceRange = planNode->sourceRanges.front();
        if (nativePointCount > budgetPointLimit ||
            nativePointCount >
                static_cast<std::uint64_t>((std::numeric_limits<std::int32_t>::max)()) ||
            sourceRange.length >
                static_cast<std::uint64_t>((std::numeric_limits<std::int32_t>::max)())) {
            diagnostics.push_back(
                {usdgeo::DiagnosticCode::InvalidFormatArgument,
                 usdgeo::Severity::Error,
                 "COPC point tile exceeds the configured memory or point limit",
                 std::nullopt, std::nullopt});
            return false;
        }

        const auto tileId = planNode->id;
        const usdcopc::CopcHierarchyEntry entry{
            tileId.level,
            static_cast<std::int32_t>(tileId.x),
            static_cast<std::int32_t>(tileId.y),
            static_cast<std::int32_t>(tileId.z),
            static_cast<std::int32_t>(nativePointCount),
            sourceRange.offset,
            static_cast<std::int32_t>(sourceRange.length)};
        std::vector<usdlas::LasPoint> points;
        if (!reader.ReadPoints(header, entry, points, diagnostics)) {
            return false;
        }

        std::vector<usdlas::LasPoint> selected;
        selected.reserve(points.size());
        for (std::uint64_t index = 0; index < points.size(); ++index) {
            if (request.readOptions.isCancelled &&
                request.readOptions.isCancelled()) {
                diagnostics.push_back(
                    {usdgeo::DiagnosticCode::DecodeFailure,
                     usdgeo::Severity::Error,
                     "COPC tiled read was cancelled", std::nullopt,
                     std::nullopt});
                return false;
            }
            if (IsSelectedPoint(points[static_cast<std::size_t>(index)], index,
                                request.readOptions)) {
                selected.push_back(
                    std::move(points[static_cast<std::size_t>(index)]));
            }
        }
        if (selected.empty()) {
            continue;
        }

        usdpointcloud::PointData data;
        std::string appendError;
        if (!usdlas::AppendPointData(header.las, selected, sourcePath, data,
                                     appendError)) {
            diagnostics.push_back(
                {usdgeo::DiagnosticCode::DecodeFailure,
                 usdgeo::Severity::Error,
                 "Unable to construct COPC point tile: " + appendError,
                 std::nullopt, std::nullopt});
            return false;
        }
        std::string selectionError;
        if (!usdpointcloud::SelectPointDataAttributes(
                data, request.attributes, selectionError)) {
            diagnostics.push_back(
                {usdgeo::DiagnosticCode::InvalidFormatArgument,
                 usdgeo::Severity::Error,
                 "Unable to select COPC point attributes: " + selectionError,
                 std::nullopt, std::nullopt});
            return false;
        }

        usdgeo::SpatialBounds localBounds;
        if (!reference.TryToLocal(planNode->bounds, localBounds)) {
            diagnostics.push_back(
                {usdgeo::DiagnosticCode::DecodeFailure,
                 usdgeo::Severity::Error,
                 "Unable to transform COPC tile bounds", std::nullopt,
                 std::nullopt});
            return false;
        }
        usdgeo::PointCloudTileAsset asset;
        asset.tile.id = tileId;
        asset.tile.bounds = localBounds;
        asset.tile.children = authoredChildren[planNode->id.ToString()];
        asset.tile.lod.bounds = localBounds;
        asset.tile.lod.items.push_back({
            0, data.positions.size(), localBounds,
            {0, data.positions.size()}, hierarchyNode->second->spacing});
        asset.levels.push_back({
            reference, localBounds,
            usdpointcloud::MakePointChunk(data, localBounds), std::move(data)});
        if (!asset.levels.front().IsValid()) {
            diagnostics.push_back(
                {usdgeo::DiagnosticCode::InvalidPointTile,
                 usdgeo::Severity::Error,
                 "COPC point tile asset is invalid", std::nullopt,
                 std::nullopt});
            return false;
        }
        assets.push_back(std::move(asset));
    }
    return !assets.empty();
}

} // namespace

TF_DEFINE_PUBLIC_TOKENS(UsdGeoCopcFileFormatTokens,
                        USDGEOCOPC_FILE_FORMAT_TOKENS);

UsdGeoCopcFileFormat::UsdGeoCopcFileFormat()
    : SdfFileFormat(UsdGeoCopcFileFormatTokens->Id,
                    UsdGeoCopcFileFormatTokens->Version,
                    UsdGeoCopcFileFormatTokens->Target,
                    UsdGeoCopcFileFormatTokens->Extension) {}

UsdGeoCopcFileFormat::~UsdGeoCopcFileFormat() = default;

void UsdGeoCopcFileFormat::ComposeFieldsForFileFormatArguments(
    const std::string&,
    const PcpDynamicFileFormatContext& context,
    FileFormatArguments* args,
    VtValue*) const {
    VtValue value;
    if (context.ComposeValue(DynamicLodField, &value)) {
        const auto lod = DynamicTokenValue(value);
        if (!lod.empty()) {
            (*args)["lod"] = lod;
        }
    }
}

bool UsdGeoCopcFileFormat::CanFieldChangeAffectFileFormatArguments(
    const TfToken& field,
    const VtValue&,
    const VtValue&,
    const VtValue&) const {
    return field == DynamicLodField;
}

bool UsdGeoCopcFileFormat::CanRead(const std::string& file) const {
    return SdfFileFormat::GetFileExtension(file) == "copc";
}

bool UsdGeoCopcFileFormat::Read(SdfLayer* layer,
                                const std::string& resolvedPath,
                                bool metadataOnly) const {
    if (!layer) {
        TF_RUNTIME_ERROR("%s", usdgeocopc::diagnostics::Message(
                                  usdgeocopc::diagnostics::InvalidReadRequest,
                                  "pointcloud-copc requires a writable layer")
                                  .c_str());
        return false;
    }

    usdpointcloud::PointReadRequest request;
    std::vector<usdgeo::Diagnostic> diagnostics;
        if (!usdpointcloud::MakeReadRequest(
            layer->GetFileFormatArguments(), request, diagnostics,
            usdpointcloud::PointReadFormat::Copc)) {
        TF_RUNTIME_ERROR("%s", usdgeocopc::diagnostics::Message(
                                  usdgeocopc::diagnostics::FormatArgumentInvalid,
                                  "Invalid COPC file-format arguments: " +
                                      DiagnosticDetail(diagnostics,
                                                       "invalid arguments"))
                                  .c_str());
        return false;
    }
    if (request.readOptions.range.firstPoint != 0 ||
        request.readOptions.range.pointCount != 0) {
        TF_RUNTIME_ERROR("%s", usdgeocopc::diagnostics::Message(
                                  usdgeocopc::diagnostics::FormatArgumentInvalid,
                                  "COPC source point ranges are not supported because hierarchy order is spatial")
                                  .c_str());
        return false;
    }

    std::string sourceError;
    usdgeo::cache::SourceIdentity resolverIdentity;
    usdgeo::cache::ResolverIdentityStability resolverStability =
        usdgeo::cache::ResolverIdentityStability::Unavailable;
    const auto source = OpenResolvedAssetSource(
        resolvedPath, sourceError, resolverIdentity, resolverStability);
    if (!source) {
        TF_RUNTIME_ERROR("%s", usdgeocopc::diagnostics::Message(
                                  usdgeocopc::diagnostics::FileOpenFailed,
                                  sourceError)
                                  .c_str());
        return false;
    }
    usdcopc::CopcReader reader(source);
    usdcopc::CopcHeader header;
    if (!reader.ReadMetadata(header, diagnostics)) {
        TF_RUNTIME_ERROR("%s", usdgeocopc::diagnostics::Message(
                                  ReaderDiagnosticCode(diagnostics),
                                  "Unable to inspect COPC file " + resolvedPath +
                                      ": " +
                                      DiagnosticDetail(diagnostics,
                                                       "inspection failed"))
                                  .c_str());
        return false;
    }

    if (metadataOnly) {
        usdpointcloud::PointChunk chunk;
        usdgeo::GeoReference reference;
        usdgeo::SpatialBounds bounds;
        usdgeo::PointCloudSourceMetadata sourceMetadata{
            header.las.pointFormat,
            {header.las.xScale, header.las.yScale, header.las.zScale},
            {header.las.xOffset, header.las.yOffset, header.las.zOffset}};
        std::string metadataError;
        if (!usdlas::BuildPointCloudMetadata(
                header.las, chunk, reference, bounds, metadataError)) {
            TF_RUNTIME_ERROR("%s", usdgeocopc::diagnostics::Message(
                                      usdgeocopc::diagnostics::BoundsTransformFailed,
                                      "Unable to build COPC metadata: " +
                                          metadataError)
                                      .c_str());
            return false;
        }
        if (!usdgeo::AuthorPointCloudMetadata(
                layer, "/PointCloud", reference, bounds, chunk,
                sourceMetadata)) {
            TF_RUNTIME_ERROR("%s",
                             usdgeocopc::diagnostics::PointCloudAuthorFailed);
            return false;
        }
        return true;
    }

    if (!usdgeo::PointCloudCacheRootFromEnvironment().empty()) {
        usdpointcloud::PointChunk chunk;
        usdgeo::GeoReference reference;
        usdgeo::SpatialBounds bounds;
        std::string metadataError;
        if (!usdlas::BuildPointCloudMetadata(
                header.las, chunk, reference, bounds, metadataError)) {
            TF_RUNTIME_ERROR("%s", usdgeocopc::diagnostics::BoundsTransformFailed);
            return false;
        }
        bool cacheHit = false;
        std::string cacheError;
        const auto loadCache = [&]() {
            auto decision = usdgeo::cache::CacheDecision::ReuseDisabled;
            if (IsLocalFileSource(resolvedPath)) {
                const auto loaded = usdgeo::TryLoadPointCloudCache(
                    layer, resolvedPath, reference, request, "copc-reader-1",
                    cacheHit, cacheError, &decision);
                if (loaded && decision !=
                                  usdgeo::cache::CacheDecision::ReuseDisabled) {
                    ReportCacheDecision(decision);
                }
                return loaded;
            }
            if (resolverStability !=
                usdgeo::cache::ResolverIdentityStability::Stable) {
                // The identity category is the reason reuse is disabled, so it
                // is what the diagnostic reports.
                ReportCacheDecision(
                    usdgeo::cache::IdentityDecision(resolverStability));
                return true;
            }
            const std::filesystem::path payloadDirectory(request.payloadDirectory);
            if (!payloadDirectory.empty() &&
                payloadDirectory.is_relative()) {
                // A remote source has no directory to resolve a relative
                // payload path against, so reuse is refused even though the
                // identity would permit it.
                ReportCacheDecision(
                    usdgeo::cache::CacheDecision::ReuseDisabled);
                return true;
            }
            decision = usdgeo::cache::CacheDecision::IdentityStable;
            const auto loaded = usdgeo::TryLoadPointCloudCache(
                layer, resolverIdentity, {}, reference, request,
                "copc-reader-1", cacheHit, cacheError, &decision);
            if (loaded) {
                ReportCacheDecision(decision);
            }
            return loaded;
        };
        if (!loadCache()) {
            TF_RUNTIME_ERROR("%s", usdgeocopc::diagnostics::Message(
                                      usdgeocopc::diagnostics::PointCloudAuthorFailed,
                                      cacheError)
                                      .c_str());
            return false;
        }
        if (cacheHit) {
            return true;
        }
    }

    std::vector<usdcopc::CopcHierarchyEntry> hierarchy;
    if (!reader.ReadHierarchy(header, hierarchy, diagnostics)) {
        TF_RUNTIME_ERROR("%s", usdgeocopc::diagnostics::Message(
                                  usdgeocopc::diagnostics::DecodeFailed,
                                  "Unable to inspect COPC hierarchy: " +
                                      DiagnosticDetail(diagnostics,
                                                       "hierarchy read failed"))
                                  .c_str());
        return false;
    }

    if (request.tiled) {
        usdcopc::CopcHierarchy copcHierarchy;
        if (!reader.BuildHierarchy(header, hierarchy, copcHierarchy,
                                   diagnostics)) {
            TF_RUNTIME_ERROR("%s", usdgeocopc::diagnostics::Message(
                                      usdgeocopc::diagnostics::DecodeFailed,
                                      "Unable to build COPC hierarchy: " +
                                          DiagnosticDetail(
                                              diagnostics, "hierarchy build failed"))
                                      .c_str());
            return false;
        }
        usdpointcloud::PointChunk metadataChunk;
        usdgeo::GeoReference reference;
        usdgeo::SpatialBounds bounds;
        std::string metadataError;
        if (!usdlas::BuildPointCloudMetadata(
                header.las, metadataChunk, reference, bounds, metadataError)) {
            TF_RUNTIME_ERROR("%s", usdgeocopc::diagnostics::Message(
                                      usdgeocopc::diagnostics::DecodeFailed,
                                      "Unable to build COPC metadata: " +
                                          metadataError)
                                      .c_str());
            return false;
        }

        std::vector<usdgeo::PointCloudTileAsset> tiles;
    if (!BuildTiledAssets(reader, header, copcHierarchy, reference, request,
                              resolvedPath, tiles, diagnostics)) {
            TF_RUNTIME_ERROR("%s", usdgeocopc::diagnostics::Message(
                                      usdgeocopc::diagnostics::DecodeFailed,
                                      "Unable to build COPC tiled assets: " +
                                          DiagnosticDetail(
                                              diagnostics, "tile read failed"))
                                      .c_str());
            return false;
        }

        std::filesystem::path payloadDirectory(request.payloadDirectory);
        if (payloadDirectory.is_relative()) {
            if (!IsLocalFileSource(resolvedPath)) {
                TF_RUNTIME_ERROR(
                    "%s",
                    usdgeocopc::diagnostics::Message(
                        usdgeocopc::diagnostics::FormatArgumentInvalid,
                        "remote COPC tiled reads require an absolute local payloadDirectory")
                        .c_str());
                return false;
            }
            payloadDirectory =
                std::filesystem::path(resolvedPath).parent_path() /
                payloadDirectory;
        }
        usdgeo::PointCloudPayloadOptions payloadOptions{
            payloadDirectory.string(), resolvedPath,
            request.tileMemoryLimitBytes};
        payloadOptions.owner = usdgeo::PointCloudPayloadOwner(
            resolvedPath, payloadDirectory, layer->GetFileFormatArguments());
        if (!usdgeo::AuthorPointCloudTiledAssetWithPayloads(
                layer, "/PointCloud", tiles, payloadOptions, diagnostics)) {
            TF_RUNTIME_ERROR("%s", usdgeocopc::diagnostics::Message(
                                      usdgeocopc::diagnostics::PointCloudAuthorFailed,
                                      "Unable to author tiled COPC point cloud: " +
                                          DiagnosticDetail(
                                              diagnostics,
                                              "tiled authoring failed"))
                                      .c_str());
            return false;
        }
        return true;
    }

    const auto bytesPerPoint =
        sizeof(usdlas::LasPoint) + header.las.pointRecordLength;
    const auto budgetPointLimit =
        request.readOptions.memoryBudgetBytes / bytesPerPoint;
    const auto maximumPoints =
        (std::min)(request.readOptions.chunkPointLimit, budgetPointLimit);
    if (maximumPoints == 0) {
        TF_RUNTIME_ERROR("%s", usdgeocopc::diagnostics::Message(
                                  usdgeocopc::diagnostics::FormatArgumentInvalid,
                                  "COPC memory budget is too small for one point")
                                  .c_str());
        return false;
    }

    usdpointcloud::PointData pointData;
    std::vector<usdlas::LasPoint> selected;
    selected.reserve(maximumPoints);
    std::string appendError;
    const auto flush = [&]() {
        if (selected.empty()) {
            return true;
        }
        appendError.clear();
        if (!usdlas::AppendPointData(header.las, selected, resolvedPath,
                                     pointData, appendError)) {
            selected.clear();
            return false;
        }
        selected.clear();
        return true;
    };
    std::uint64_t pointIndex = 0;
    for (const auto& entry : hierarchy) {
        if (!entry.IsPointData()) {
            continue;
        }
        std::vector<std::uint8_t> bytes;
        if (!reader.ReadPointData(header, entry, bytes, diagnostics) ||
            !usdlaz::DecodeLazChunk(
                header.las, bytes,
                static_cast<std::uint64_t>(entry.pointCount),
                [&](const usdlas::LasPoint& point, std::uint64_t) {
                    const auto selectedPoint =
                        IsSelectedPoint(point, pointIndex, request.readOptions);
                    ++pointIndex;
                    if (selectedPoint) {
                        selected.push_back(point);
                        if (selected.size() == maximumPoints) {
                            return flush();
                        }
                    }
                    return true;
                },
                diagnostics)) {
            TF_RUNTIME_ERROR("%s", usdgeocopc::diagnostics::Message(
                                      usdgeocopc::diagnostics::DecodeFailed,
                                      "Unable to decode COPC point data: " +
                                          DiagnosticDetail(
                                              diagnostics, "decode failed"))
                                      .c_str());
            return false;
        }
    }
    if (!flush()) {
        TF_RUNTIME_ERROR("%s", usdgeocopc::diagnostics::Message(
                                  usdgeocopc::diagnostics::DecodeFailed,
                                  "Unable to construct COPC point data: " +
                                      appendError)
                                  .c_str());
        return false;
    }

    std::string selectionError;
    if (!usdpointcloud::SelectPointDataAttributes(
            pointData, request.attributes, selectionError)) {
        TF_RUNTIME_ERROR("%s", usdgeocopc::diagnostics::Message(
                                  usdgeocopc::diagnostics::FormatArgumentInvalid,
                                  "Unable to select COPC point attributes: " +
                                      selectionError)
                                  .c_str());
        return false;
    }

    usdpointcloud::PointCloudAsset asset;
    std::string assetError;
    if (!usdlas::BuildPointCloudAsset(
            header.las, pointData, "COPC CRS unavailable; inspect VLR metadata",
            asset, assetError)) {
        TF_RUNTIME_ERROR("%s", usdgeocopc::diagnostics::Message(
                                  usdgeocopc::diagnostics::BoundsTransformFailed,
                                  "Unable to build COPC point cloud asset: " +
                                      assetError)
                                  .c_str());
        return false;
    }

    bool authored = false;
    usdgeo::PointCloudAuthorFailure authorFailure =
        usdgeo::PointCloudAuthorFailure::PointCloud;
    if (request.lodProfile != usdpointcloud::LodProfile::Off) {
        std::vector<usdpointcloud::PointCloudAsset> levels;
        usdpointcloud::PointLodHierarchy lodHierarchy;
        if (usdpointcloud::BuildPointLodAssets(
                asset, request.lodProfile, levels, lodHierarchy, diagnostics)) {
            authored = usdgeo::AuthorPointCloudLodAsset(
                layer, "/PointCloud", levels, lodHierarchy);
        }
    } else {
        authored = usdgeo::AuthorPointCloudAsset(layer, "/PointCloud", asset,
                                                 authorFailure);
    }
    if (!authored) {
        const char* code = usdgeocopc::diagnostics::PointCloudAuthorFailed;
        if (authorFailure == usdgeo::PointCloudAuthorFailure::InvalidLayer ||
            authorFailure == usdgeo::PointCloudAuthorFailure::StageCreation) {
            code = usdgeocopc::diagnostics::UsdLayerCreateFailed;
        } else if (authorFailure == usdgeo::PointCloudAuthorFailure::StageMetrics) {
            code = usdgeocopc::diagnostics::StageMetricsFailed;
        }
        TF_RUNTIME_ERROR("%s", usdgeocopc::diagnostics::Message(
                                  code,
                                  "Unable to author COPC point cloud to USD layer: " +
                                      resolvedPath)
                                  .c_str());
        return false;
    }
    return true;
}

bool UsdGeoCopcFileFormat::WriteToString(const SdfLayer& layer,
                                         std::string* str,
                                         const std::string& comment) const {
    const auto usda = SdfFileFormat::FindByExtension("usda");
    return usda ? usda->WriteToString(layer, str, comment)
                : layer.ExportToString(str);
}

TF_REGISTRY_FUNCTION(TfType) {
    SDF_DEFINE_FILE_FORMAT(UsdGeoCopcFileFormat, SdfFileFormat);
}

PXR_NAMESPACE_CLOSE_SCOPE
