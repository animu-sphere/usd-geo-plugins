#include "usdgeo/PointCloudLayer.h"

#include "TiledPayloadAuthoring.h"

#include <algorithm>
#include <cstdint>
#include <cmath>
#include <filesystem>
#include <set>

#include <pxr/base/gf/vec2d.h>
#include <pxr/base/gf/vec3f.h>
#include <pxr/base/gf/vec3d.h>
#include <pxr/base/tf/stringUtils.h>
#include <pxr/base/vt/array.h>
#include <pxr/usd/usdGeom/metrics.h>
#include <pxr/usd/sdf/layer.h>
#include <pxr/usd/usdGeom/points.h>
#include <pxr/usd/usdGeom/scope.h>
#include <pxr/usd/usdGeom/xform.h>
#include <pxr/usd/sdf/payload.h>
#include <pxr/usd/usd/payloads.h>
#include <pxr/usd/usdLod/rootAPI.h>
#include <pxr/usd/usdLod/screenSizeHeuristic.h>
#include <pxr/usd/usdLod/tokens.h>

namespace usdgeo {

namespace {

bool IsValidPrimPath(const std::string& primPath) {
    return !primPath.empty() && primPath.front() == '/';
}

bool SameBounds(const usdgeo::SpatialBounds& left,
                const usdgeo::SpatialBounds& right) {
    return left.minimum.x == right.minimum.x &&
           left.minimum.y == right.minimum.y &&
           left.minimum.z == right.minimum.z &&
           left.maximum.x == right.maximum.x &&
           left.maximum.y == right.maximum.y &&
           left.maximum.z == right.maximum.z;
}

bool IsValidMetadataChunk(const usdpointcloud::PointChunk& chunk) {
    if (chunk.pointCount != 0) {
        return chunk.IsValid();
    }
    if (!chunk.bounds.IsValid()) {
        return false;
    }
    std::set<std::string> names;
    for (const auto& attribute : chunk.attributes) {
        if (!attribute.IsValid() || !names.insert(attribute.name).second) {
            return false;
        }
    }
    return true;
}

float EstimatePointWidth(const usdgeo::SpatialBounds& bounds,
                         std::size_t pointCount) {
    if (!bounds.IsValid() || pointCount == 0) {
        return 0.001f;
    }
    const auto size = bounds.Size();
    const auto diagonal = std::sqrt(
        size.x * size.x + size.y * size.y + size.z * size.z);
    if (!std::isfinite(diagonal) || diagonal <= 0.0) {
        return 0.001f;
    }
    const auto width = diagonal / 5000.0;
    return static_cast<float>((std::max)(width, 1.0e-6));
}

bool HasPointColors(const PointCloudLayer::Data& data,
                    std::size_t pointCount) {
    return data.red.size() == pointCount &&
           data.green.size() == pointCount &&
           data.blue.size() == pointCount;
}

float PointColorScale(const PointCloudLayer::Data& data) {
    return data.colorBitDepth <= 8 ? 255.0f : 65535.0f;
}

pxr::VtIntArray ToIntArray(const std::vector<std::uint16_t>& values) {
    pxr::VtIntArray result;
    result.reserve(values.size());
    for (const auto value : values) {
        result.push_back(static_cast<int>(value));
    }
    return result;
}

pxr::VtIntArray ToIntArray(const std::vector<std::int16_t>& values) {
    pxr::VtIntArray result;
    result.reserve(values.size());
    for (const auto value : values) {
        result.push_back(static_cast<int>(value));
    }
    return result;
}

std::string TilePrimName(const usdpointcloud::PointTileId& id) {
    const auto axisName = [](std::int64_t value) {
        if (value >= 0) {
            return "p" + std::to_string(static_cast<std::uint64_t>(value));
        }
        const auto magnitude = static_cast<std::uint64_t>(-(value + 1)) + 1;
        return "n" + std::to_string(magnitude);
    };
    return "Tile_L" + std::to_string(id.level) + "_" + axisName(id.x) +
           "_" + axisName(id.y) + "_" + axisName(id.z);
}

// Where a payload-backed LOD root writes its payload files. Every path it
// writes has already been claimed by `payloads`.
struct PayloadTarget {
    const std::filesystem::path& directory;
    const std::filesystem::path& rootLayerPath;
    detail::GeneratedPayloadSet& payloads;
};

bool AuthorLodRoot(
    const pxr::UsdStageRefPtr& stage,
    const std::string& primPath,
    const std::vector<usdpointcloud::PointCloudAsset>& levels,
    const usdpointcloud::PointLodHierarchy& hierarchy,
    const pxr::SdfPath& heuristicPath,
    const PayloadTarget* payloadTarget = nullptr) {
    if (!stage || !IsValidPrimPath(primPath) ||
        levels.size() != hierarchy.items.size()) {
        return false;
    }
    const auto root = pxr::UsdGeomXform::Define(stage, pxr::SdfPath(primPath));
    if (!root || !pxr::UsdLodRootAPI::Apply(root.GetPrim())) {
        return false;
    }
    const auto lodRoot = pxr::UsdLodRootAPI(root.GetPrim());
    if (!lodRoot.CreateLodDefaultIndexAttr().Set(
            static_cast<int>(hierarchy.defaultIndex))) {
        return false;
    }
    if (!hierarchy.screenSizeThresholds.empty()) {
        if (!lodRoot.CreateLodHeuristicsRel().AddTarget(heuristicPath)) {
            return false;
        }
    }
    for (std::size_t index = 0; index < levels.size(); ++index) {
        const auto& level = levels[index];
        const auto lodName = "LOD" + std::to_string(index);
        const auto lodPath = primPath + "/" + lodName;
        if (payloadTarget == nullptr) {
            if (!AuthorPointCloudAsset(stage, lodPath, level.reference,
                                       level.bounds, level.chunk, level.data)) {
                return false;
            }
            continue;
        }

        const auto payloadPath = payloadTarget->directory /
            (primPath.substr(primPath.find_last_of('/') + 1) + "_" +
             lodName + ".usdc");
        std::error_code relativeError;
        const auto payloadIdentifier = std::filesystem::relative(
            payloadPath, payloadTarget->rootLayerPath.parent_path(),
            relativeError);
        if (relativeError || payloadIdentifier.empty() ||
            payloadIdentifier.is_absolute()) {
            return false;
        }
        // The payload is built in memory and exported, so writing it never
        // registers a layer under the payload path. A regeneration can then
        // replace a payload this process still has open.
        const auto payloadStage =
            pxr::UsdStage::CreateInMemory("payload.usdc");
        if (!payloadStage || !pxr::UsdGeomXform::Define(
                                  payloadStage, pxr::SdfPath("/" + lodName)) ||
            !AuthorPointCloudAsset(payloadStage, "/" + lodName + "/Points",
                                   level.reference, level.bounds, level.chunk,
                                   level.data)) {
            return false;
        }
        payloadTarget->payloads.MarkWritten(payloadPath);
        if (!payloadStage->GetRootLayer()->Export(payloadPath.string())) {
            return false;
        }
        const auto lodPrim = pxr::UsdGeomXform::Define(
            stage, pxr::SdfPath(lodPath));
        if (!lodPrim || !lodPrim.GetPrim().GetPayloads().AddPayload(
                pxr::SdfPayload(payloadIdentifier.generic_string(),
                                pxr::SdfPath("/" + lodName)))) {
            return false;
        }
    }
    return true;
}

bool AuthorScreenSizeHeuristic(
    const pxr::UsdStageRefPtr& stage,
    const pxr::SdfPath& heuristicPath,
    const GeoReference& reference,
    const SpatialBounds& bounds,
    const std::vector<float>& thresholds) {
    SpatialBounds localBounds;
    if (!stage || !reference.TryToLocal(bounds, localBounds)) {
        return false;
    }
    if (!pxr::UsdGeomScope::Define(stage, heuristicPath.GetParentPath())) {
        return false;
    }
    const auto heuristic =
        pxr::UsdLodScreenSizeHeuristic::Define(stage, heuristicPath);
    if (!heuristic) {
        return false;
    }
    const pxr::GfVec3f minimum(
        static_cast<float>(localBounds.minimum.x),
        static_cast<float>(localBounds.minimum.y),
        static_cast<float>(localBounds.minimum.z));
    const pxr::GfVec3f maximum(
        static_cast<float>(localBounds.maximum.x),
        static_cast<float>(localBounds.maximum.y),
        static_cast<float>(localBounds.maximum.z));
    for (int index = 0; index < 3; ++index) {
        if (!std::isfinite(minimum[index]) ||
            !std::isfinite(maximum[index]) || minimum[index] > maximum[index]) {
            return false;
        }
    }
    const pxr::VtArray<pxr::GfVec3f> extent = {
        minimum, maximum};
    return heuristic.CreateLodDomainAttr().Set(pxr::UsdLodTokens->imaging) &&
           heuristic.CreateThresholdsAttr().Set(
               pxr::VtArray<float>(thresholds.begin(), thresholds.end())) &&
           heuristic.CreateExtentAttr().Set(extent);
}

} // namespace

namespace detail {

pxr::UsdStageRefPtr CreateDetachedStage() {
    return pxr::UsdStage::CreateInMemory(pxr::UsdStage::LoadNone);
}

std::filesystem::path TilePayloadPath(const std::filesystem::path& directory,
                                      const usdpointcloud::PointTileId& id,
                                      std::size_t lodIndex) {
    return directory /
           (TilePrimName(id) + "_LOD" + std::to_string(lodIndex) + ".usdc");
}

std::vector<std::filesystem::path> TilePayloadPaths(
    const std::filesystem::path& directory,
    const std::vector<PointCloudTileAsset>& tiles) {
    std::vector<std::filesystem::path> paths;
    for (const auto& tile : tiles) {
        for (std::size_t index = 0; index < tile.levels.size(); ++index) {
            paths.push_back(TilePayloadPath(directory, tile.tile.id, index));
        }
    }
    return paths;
}

} // namespace detail

std::string PointCloudPayloadOwner(
    const std::string& resolvedPath,
    const usdpointcloud::PointReadRequest& request) {
    return resolvedPath + "?" + request.normalizedArguments;
}

pxr::UsdStageRefPtr PointCloudLayer::CreateStage() {
    return pxr::UsdStage::CreateInMemory();
}

bool PointCloudLayer::AuthorPointCloud(
    const pxr::UsdStageRefPtr& stage,
    const std::string& primPath,
    const GeoReference& reference,
    const SpatialBounds& bounds,
    const usdpointcloud::PointChunk& chunk,
    const std::vector<Vec3d>& positions) {
    return AuthorPointCloudAsset(
        stage, primPath, reference, bounds, chunk, positions);
}

bool PointCloudLayer::AuthorPointCloud(
    const pxr::UsdStageRefPtr& stage,
    const std::string& primPath,
    const GeoReference& reference,
    const SpatialBounds& bounds,
    const usdpointcloud::PointChunk& chunk,
    const Data& data) {
    return AuthorPointCloudAsset(
        stage, primPath, reference, bounds, chunk, data);
}

bool AuthorPointCloudAsset(
    pxr::SdfLayer* layer,
    const std::string& primPath,
    const usdpointcloud::PointCloudAsset& asset) {
    PointCloudAuthorFailure failure;
    return AuthorPointCloudAsset(layer, primPath, asset, failure);
}

bool AuthorPointCloudAsset(
    pxr::SdfLayer* layer,
    const std::string& primPath,
    const usdpointcloud::PointCloudAsset& asset,
    PointCloudAuthorFailure& failure) {
    return AuthorPointCloudAsset(layer, primPath, asset.reference,
                                 asset.bounds, asset.chunk, asset.data, failure);
}

bool AuthorPointCloudAsset(
    pxr::SdfLayer* layer,
    const std::string& primPath,
    const GeoReference& reference,
    const SpatialBounds& bounds,
    const usdpointcloud::PointChunk& chunk,
    const PointCloudLayer::Data& data) {
    PointCloudAuthorFailure failure;
    return AuthorPointCloudAsset(layer, primPath, reference, bounds, chunk,
                                 data, failure);
}

bool AuthorPointCloudAsset(
    pxr::SdfLayer* layer,
    const std::string& primPath,
    const GeoReference& reference,
    const SpatialBounds& bounds,
    const usdpointcloud::PointChunk& chunk,
    const PointCloudLayer::Data& data,
    PointCloudAuthorFailure& failure) {
    failure = PointCloudAuthorFailure::None;
    if (!layer) {
        failure = PointCloudAuthorFailure::InvalidLayer;
        return false;
    }

    const auto stage = detail::CreateDetachedStage();
    if (!stage) {
        failure = PointCloudAuthorFailure::StageCreation;
        return false;
    }
    if (!pxr::UsdGeomSetStageUpAxis(
            stage, pxr::TfToken(reference.stageUpAxis)) ||
        !pxr::UsdGeomSetStageMetersPerUnit(stage, 1.0)) {
        failure = PointCloudAuthorFailure::StageMetrics;
        return false;
    }
    if (!AuthorPointCloudAsset(stage, primPath, reference, bounds, chunk,
                               data)) {
        failure = PointCloudAuthorFailure::PointCloud;
        return false;
    }

    layer->TransferContent(stage->GetRootLayer());
    return true;
}

bool AuthorPointCloudAsset(
    const pxr::UsdStageRefPtr& stage,
    const std::string& primPath,
    const GeoReference& reference,
    const SpatialBounds& bounds,
    const usdpointcloud::PointChunk& chunk,
    const std::vector<Vec3d>& positions) {
    PointCloudLayer::Data data;
    data.positions = positions;
    return AuthorPointCloudAsset(stage, primPath, reference, bounds, chunk,
                                 data);
}

bool AuthorPointCloudAsset(
    const pxr::UsdStageRefPtr& stage,
    const std::string& primPath,
    const GeoReference& reference,
    const SpatialBounds& bounds,
    const usdpointcloud::PointChunk& chunk,
    const PointCloudLayer::Data& data) {
    if (!stage || !IsValidPrimPath(primPath) || !reference.IsValid() ||
        !bounds.IsValid() || !chunk.IsValid() || !data.IsValid() ||
        data.positions.size() != chunk.pointCount) {
        return false;
    }

    pxr::VtVec3fArray localPositions;
    localPositions.reserve(data.positions.size());
    usdgeo::SpatialBounds localBounds = usdgeo::SpatialBounds::Empty();
    for (const Vec3d& position : data.positions) {
        Vec3d local;
        if (!reference.TryToLocal(position, local)) {
            return false;
        }
        localPositions.push_back(pxr::GfVec3f(
            static_cast<float>(local.x), static_cast<float>(local.y),
            static_cast<float>(local.z)));
        localBounds.Expand(local);
    }

    auto points = pxr::UsdGeomPoints::Define(stage, pxr::SdfPath(primPath));
    if (!points) {
        return false;
    }

    points.GetPointsAttr().Set(localPositions);
    pxr::VtFloatArray widths(1);
    widths[0] = EstimatePointWidth(localBounds, localPositions.size());
    if (!points.SetWidthsInterpolation(pxr::UsdGeomTokens->constant) ||
        !points.CreateWidthsAttr().Set(widths)) {
        return false;
    }
    if (HasPointColors(data, localPositions.size())) {
        const auto scale = PointColorScale(data);
        pxr::VtVec3fArray displayColors;
        displayColors.reserve(localPositions.size());
        for (std::size_t index = 0; index < localPositions.size(); ++index) {
            displayColors.push_back(pxr::GfVec3f(
                static_cast<float>(data.red[index]) / scale,
                static_cast<float>(data.green[index]) / scale,
                static_cast<float>(data.blue[index]) / scale));
        }
        const auto displayColor = points.CreateDisplayColorPrimvar(
            pxr::UsdGeomTokens->vertex);
        if (!displayColor || !displayColor.Set(displayColors)) {
            return false;
        }
    }
    if (!data.intensity.empty()) {
        points.GetPrim().CreateAttribute(
            pxr::TfToken("geo:intensity"), pxr::SdfValueTypeNames->IntArray)
            .Set(ToIntArray(data.intensity));
    }
    if (!data.returnNumber.empty()) {
        points.GetPrim().CreateAttribute(
            pxr::TfToken("geo:returnNumber"), pxr::SdfValueTypeNames->UCharArray)
            .Set(pxr::VtArray<unsigned char>(data.returnNumber.begin(),
                                              data.returnNumber.end()));
        points.GetPrim().CreateAttribute(
            pxr::TfToken("geo:numberOfReturns"),
            pxr::SdfValueTypeNames->UCharArray)
            .Set(pxr::VtArray<unsigned char>(data.numberOfReturns.begin(),
                                              data.numberOfReturns.end()));
    }
    if (!data.classification.empty()) {
        points.GetPrim().CreateAttribute(
            pxr::TfToken("geo:classification"),
            pxr::SdfValueTypeNames->UCharArray)
            .Set(pxr::VtArray<unsigned char>(data.classification.begin(),
                                              data.classification.end()));
    }
    if (!data.classificationFlags.empty()) {
        points.GetPrim().CreateAttribute(
            pxr::TfToken("geo:classificationFlags"),
            pxr::SdfValueTypeNames->UCharArray)
            .Set(pxr::VtArray<unsigned char>(data.classificationFlags.begin(),
                                              data.classificationFlags.end()));
    }
    if (!data.scannerChannel.empty()) {
        points.GetPrim().CreateAttribute(
            pxr::TfToken("geo:scannerChannel"),
            pxr::SdfValueTypeNames->UCharArray)
            .Set(pxr::VtArray<unsigned char>(data.scannerChannel.begin(),
                                              data.scannerChannel.end()));
    }
    if (!data.scanDirectionFlag.empty()) {
        points.GetPrim().CreateAttribute(
            pxr::TfToken("geo:scanDirectionFlag"),
            pxr::SdfValueTypeNames->UCharArray)
            .Set(pxr::VtArray<unsigned char>(data.scanDirectionFlag.begin(),
                                              data.scanDirectionFlag.end()));
    }
    if (!data.edgeOfFlightLine.empty()) {
        points.GetPrim().CreateAttribute(
            pxr::TfToken("geo:edgeOfFlightLine"),
            pxr::SdfValueTypeNames->UCharArray)
            .Set(pxr::VtArray<unsigned char>(data.edgeOfFlightLine.begin(),
                                              data.edgeOfFlightLine.end()));
    }
    if (!data.userData.empty()) {
        points.GetPrim().CreateAttribute(
            pxr::TfToken("geo:userData"), pxr::SdfValueTypeNames->UCharArray)
            .Set(pxr::VtArray<unsigned char>(data.userData.begin(),
                                              data.userData.end()));
    }
    if (!data.scanAngle.empty()) {
        points.GetPrim().CreateAttribute(
            pxr::TfToken("geo:scanAngle"), pxr::SdfValueTypeNames->IntArray)
            .Set(ToIntArray(data.scanAngle));
    }
    if (!data.pointSourceId.empty()) {
        points.GetPrim().CreateAttribute(
            pxr::TfToken("geo:pointSourceId"), pxr::SdfValueTypeNames->IntArray)
            .Set(ToIntArray(data.pointSourceId));
    }
    if (!data.red.empty()) {
        points.GetPrim().CreateAttribute(
            pxr::TfToken("geo:red"), pxr::SdfValueTypeNames->IntArray)
            .Set(ToIntArray(data.red));
        points.GetPrim().CreateAttribute(
            pxr::TfToken("geo:green"), pxr::SdfValueTypeNames->IntArray)
            .Set(ToIntArray(data.green));
        points.GetPrim().CreateAttribute(
            pxr::TfToken("geo:blue"), pxr::SdfValueTypeNames->IntArray)
            .Set(ToIntArray(data.blue));
    }
    if (!data.nir.empty()) {
        points.GetPrim().CreateAttribute(
            pxr::TfToken("geo:nir"), pxr::SdfValueTypeNames->IntArray)
            .Set(ToIntArray(data.nir));
    }
    if (!data.gpsTime.empty()) {
        points.GetPrim().CreateAttribute(
            pxr::TfToken("geo:gpsTime"), pxr::SdfValueTypeNames->DoubleArray)
            .Set(pxr::VtArray<double>(data.gpsTime.begin(), data.gpsTime.end()));
    }
    if (!data.waveformDescriptorIndex.empty()) {
        points.GetPrim().CreateAttribute(
            pxr::TfToken("geo:waveformDescriptorIndex"),
            pxr::SdfValueTypeNames->UCharArray)
            .Set(pxr::VtArray<unsigned char>(data.waveformDescriptorIndex.begin(),
                                              data.waveformDescriptorIndex.end()));
        points.GetPrim().CreateAttribute(
            pxr::TfToken("geo:waveformDataOffset"),
            pxr::SdfValueTypeNames->UInt64Array)
            .Set(pxr::VtArray<std::uint64_t>(data.waveformDataOffset.begin(),
                                              data.waveformDataOffset.end()));
        points.GetPrim().CreateAttribute(
            pxr::TfToken("geo:waveformPacketSize"),
            pxr::SdfValueTypeNames->UIntArray)
            .Set(pxr::VtArray<unsigned int>(data.waveformPacketSize.begin(),
                                             data.waveformPacketSize.end()));
        points.GetPrim().CreateAttribute(
            pxr::TfToken("geo:returnPointWaveformLocation"),
            pxr::SdfValueTypeNames->FloatArray)
            .Set(pxr::VtArray<float>(data.returnPointWaveformLocation.begin(),
                                     data.returnPointWaveformLocation.end()));
        points.GetPrim().CreateAttribute(
            pxr::TfToken("geo:waveformXt"), pxr::SdfValueTypeNames->FloatArray)
            .Set(pxr::VtArray<float>(data.waveformXt.begin(), data.waveformXt.end()));
        points.GetPrim().CreateAttribute(
            pxr::TfToken("geo:waveformYt"), pxr::SdfValueTypeNames->FloatArray)
            .Set(pxr::VtArray<float>(data.waveformYt.begin(), data.waveformYt.end()));
        points.GetPrim().CreateAttribute(
            pxr::TfToken("geo:waveformZt"), pxr::SdfValueTypeNames->FloatArray)
            .Set(pxr::VtArray<float>(data.waveformZt.begin(), data.waveformZt.end()));
        points.GetPrim().CreateAttribute(
            pxr::TfToken("geo:waveformDataExternal"),
            pxr::SdfValueTypeNames->UCharArray)
            .Set(pxr::VtArray<unsigned char>(data.waveformDataExternal.begin(),
                                              data.waveformDataExternal.end()));
    }
    if (!data.waveformDataFile.empty()) {
        points.GetPrim().CreateAttribute(
            pxr::TfToken("geo:waveformDataFile"),
            pxr::SdfValueTypeNames->String)
            .Set(data.waveformDataFile);
    }
    for (std::size_t index = 0; index < data.extraBytes.size(); ++index) {
        if (index >= data.extraByteNames.size() ||
            data.extraByteNames[index].empty() || data.extraBytes[index].empty()) {
            continue;
        }
        if (!pxr::TfIsValidIdentifier(data.extraByteNames[index])) {
            return false;
        }
        const auto componentCount = data.extraByteComponentCounts.empty()
                                        ? std::uint8_t{1}
                                        : data.extraByteComponentCounts[index];
        const auto token = pxr::TfToken("geo:" + data.extraByteNames[index]);
        if (componentCount == 1) {
            const auto attribute = points.GetPrim().CreateAttribute(
                token, pxr::SdfValueTypeNames->DoubleArray);
            if (!attribute ||
                !attribute.Set(pxr::VtArray<double>(data.extraBytes[index].begin(),
                                                    data.extraBytes[index].end()))) {
                return false;
            }
        } else if (componentCount == 2) {
            pxr::VtArray<pxr::GfVec2d> values(data.extraBytes[index].size() / 2);
            for (std::size_t valueIndex = 0; valueIndex < values.size(); ++valueIndex) {
                values[valueIndex] = pxr::GfVec2d(
                    data.extraBytes[index][valueIndex * 2],
                    data.extraBytes[index][valueIndex * 2 + 1]);
            }
            const auto attribute = points.GetPrim().CreateAttribute(
                token, pxr::SdfValueTypeNames->Double2Array);
            if (!attribute || !attribute.Set(values)) {
                return false;
            }
        } else {
            pxr::VtArray<pxr::GfVec3d> values(data.extraBytes[index].size() / 3);
            for (std::size_t valueIndex = 0; valueIndex < values.size(); ++valueIndex) {
                values[valueIndex] = pxr::GfVec3d(
                    data.extraBytes[index][valueIndex * 3],
                    data.extraBytes[index][valueIndex * 3 + 1],
                    data.extraBytes[index][valueIndex * 3 + 2]);
            }
            const auto attribute = points.GetPrim().CreateAttribute(
                token, pxr::SdfValueTypeNames->Double3Array);
            if (!attribute || !attribute.Set(values)) {
                return false;
            }
        }
    }

    points.GetPrim().CreateAttribute(
        pxr::TfToken("geo:epsgCode"), pxr::SdfValueTypeNames->Int)
        .Set(reference.epsgCode.value_or(0));
    points.GetPrim().CreateAttribute(
        pxr::TfToken("geo:wkt"), pxr::SdfValueTypeNames->String)
        .Set(reference.wkt);
    points.GetPrim().CreateAttribute(
        pxr::TfToken("geo:projJson"), pxr::SdfValueTypeNames->String)
        .Set(reference.projJson);
    points.GetPrim().CreateAttribute(
        pxr::TfToken("geo:linearUnit"), pxr::SdfValueTypeNames->String)
        .Set(reference.linearUnit);
    points.GetPrim().CreateAttribute(
        pxr::TfToken("geo:upAxis"), pxr::SdfValueTypeNames->String)
        .Set(reference.stageUpAxis);
    points.GetPrim().CreateAttribute(
        pxr::TfToken("geo:localOrigin"), pxr::SdfValueTypeNames->Double3)
        .Set(pxr::GfVec3d(reference.localOrigin.x, reference.localOrigin.y,
                          reference.localOrigin.z));
    points.GetPrim().CreateAttribute(
        pxr::TfToken("geo:boundsMin"), pxr::SdfValueTypeNames->Double3)
        .Set(pxr::GfVec3d(bounds.minimum.x, bounds.minimum.y,
                          bounds.minimum.z));
    points.GetPrim().CreateAttribute(
        pxr::TfToken("geo:boundsMax"), pxr::SdfValueTypeNames->Double3)
        .Set(pxr::GfVec3d(bounds.maximum.x, bounds.maximum.y,
                          bounds.maximum.z));
    points.GetPrim().CreateAttribute(
        pxr::TfToken("geo:pointCount"), pxr::SdfValueTypeNames->UInt64)
        .Set(chunk.pointCount);

    return true;
}

bool AuthorPointCloudMetadata(
    pxr::SdfLayer* layer,
    const std::string& primPath,
    const GeoReference& reference,
    const SpatialBounds& bounds,
    const usdpointcloud::PointChunk& chunk,
    const PointCloudSourceMetadata& sourceMetadata) {
    if (!layer || !IsValidPrimPath(primPath) || !reference.IsValid() ||
        !bounds.IsValid() || !IsValidMetadataChunk(chunk)) {
        return false;
    }

    const auto stage = detail::CreateDetachedStage();
    if (!stage || !pxr::UsdGeomSetStageUpAxis(
                      stage, pxr::TfToken(reference.stageUpAxis)) ||
        !pxr::UsdGeomSetStageMetersPerUnit(stage, 1.0)) {
        return false;
    }

    const auto points = pxr::UsdGeomPoints::Define(
        stage, pxr::SdfPath(primPath));
    if (!points) {
        return false;
    }
    points.GetPointsAttr().Set(pxr::VtVec3fArray());
    points.GetPrim().CreateAttribute(
        pxr::TfToken("geo:epsgCode"), pxr::SdfValueTypeNames->Int)
        .Set(reference.epsgCode.value_or(0));
    points.GetPrim().CreateAttribute(
        pxr::TfToken("geo:wkt"), pxr::SdfValueTypeNames->String)
        .Set(reference.wkt);
    points.GetPrim().CreateAttribute(
        pxr::TfToken("geo:projJson"), pxr::SdfValueTypeNames->String)
        .Set(reference.projJson);
    points.GetPrim().CreateAttribute(
        pxr::TfToken("geo:linearUnit"), pxr::SdfValueTypeNames->String)
        .Set(reference.linearUnit);
    points.GetPrim().CreateAttribute(
        pxr::TfToken("geo:upAxis"), pxr::SdfValueTypeNames->String)
        .Set(reference.stageUpAxis);
    points.GetPrim().CreateAttribute(
        pxr::TfToken("geo:localOrigin"), pxr::SdfValueTypeNames->Double3)
        .Set(pxr::GfVec3d(reference.localOrigin.x, reference.localOrigin.y,
                          reference.localOrigin.z));
    points.GetPrim().CreateAttribute(
        pxr::TfToken("geo:boundsMin"), pxr::SdfValueTypeNames->Double3)
        .Set(pxr::GfVec3d(bounds.minimum.x, bounds.minimum.y,
                          bounds.minimum.z));
    points.GetPrim().CreateAttribute(
        pxr::TfToken("geo:boundsMax"), pxr::SdfValueTypeNames->Double3)
        .Set(pxr::GfVec3d(bounds.maximum.x, bounds.maximum.y,
                          bounds.maximum.z));
    points.GetPrim().CreateAttribute(
        pxr::TfToken("geo:pointCount"), pxr::SdfValueTypeNames->UInt64)
        .Set(chunk.pointCount);
    points.GetPrim().CreateAttribute(
        pxr::TfToken("geo:pointFormat"), pxr::SdfValueTypeNames->UChar)
        .Set(sourceMetadata.pointFormat);
    points.GetPrim().CreateAttribute(
        pxr::TfToken("geo:scale"), pxr::SdfValueTypeNames->Double3)
        .Set(pxr::GfVec3d(sourceMetadata.scale.x, sourceMetadata.scale.y,
                          sourceMetadata.scale.z));
    points.GetPrim().CreateAttribute(
        pxr::TfToken("geo:offset"), pxr::SdfValueTypeNames->Double3)
        .Set(pxr::GfVec3d(sourceMetadata.offset.x, sourceMetadata.offset.y,
                          sourceMetadata.offset.z));
    points.GetPrim().CreateAttribute(
        pxr::TfToken("geo:metadataOnly"), pxr::SdfValueTypeNames->Bool)
        .Set(true);
    pxr::VtStringArray availableAttributes;
    availableAttributes.reserve(chunk.attributes.size());
    for (const auto& attribute : chunk.attributes) {
        availableAttributes.push_back(attribute.name);
    }
    points.GetPrim().CreateAttribute(
        pxr::TfToken("geo:availableAttributes"),
        pxr::SdfValueTypeNames->StringArray)
        .Set(availableAttributes);

    layer->TransferContent(stage->GetRootLayer());
    return true;
}

bool AuthorPointCloudLodAsset(
    const pxr::UsdStageRefPtr& stage,
    const std::string& primPath,
    const std::vector<usdpointcloud::PointCloudAsset>& levels,
    const usdpointcloud::PointLodHierarchy& hierarchy) {
    std::vector<usdgeo::Diagnostic> diagnostics;
    if (!stage || !IsValidPrimPath(primPath) ||
        levels.size() != hierarchy.items.size() ||
        !usdpointcloud::ValidatePointLodHierarchy(hierarchy, diagnostics)) {
        return false;
    }
    for (std::size_t index = 0; index < levels.size(); ++index) {
        const auto& level = levels[index];
        const auto& item = hierarchy.items[index];
        if (!level.IsValid() || level.chunk.pointCount != item.pointCount ||
            !SameBounds(level.bounds, item.bounds)) {
            return false;
        }
    }

    const auto heuristicPath =
        pxr::SdfPath(primPath).GetParentPath().AppendChild(
            pxr::TfToken("LodHeuristics"))
            .AppendChild(pxr::TfToken(
                pxr::SdfPath(primPath).GetName() + "ScreenSize"));
    if (!hierarchy.screenSizeThresholds.empty() &&
        !AuthorScreenSizeHeuristic(stage, heuristicPath,
                                   levels.front().reference, hierarchy.bounds,
                                   hierarchy.screenSizeThresholds)) {
        return false;
    }
    return AuthorLodRoot(
        stage, primPath, levels, hierarchy, heuristicPath);
}

bool AuthorPointCloudLodAsset(
    pxr::SdfLayer* layer,
    const std::string& primPath,
    const std::vector<usdpointcloud::PointCloudAsset>& levels,
    const usdpointcloud::PointLodHierarchy& hierarchy) {
    if (!layer || levels.empty() || !levels.front().reference.IsValid()) {
        return false;
    }
    const auto stage = detail::CreateDetachedStage();
    if (!stage || !pxr::UsdGeomSetStageUpAxis(
                      stage, pxr::TfToken(levels.front().reference.stageUpAxis)) ||
        !pxr::UsdGeomSetStageMetersPerUnit(stage, 1.0) ||
        !AuthorPointCloudLodAsset(stage, primPath, levels, hierarchy)) {
        return false;
    }
    layer->TransferContent(stage->GetRootLayer());
    return true;
}

bool AuthorPointCloudTiledAsset(
    const pxr::UsdStageRefPtr& stage,
    const std::string& primPath,
    const std::vector<PointCloudTileAsset>& tiles) {
    if (!stage || !IsValidPrimPath(primPath) || tiles.empty()) {
        return false;
    }
    std::set<std::string> tileNames;
    for (const auto& tile : tiles) {
        std::vector<usdgeo::Diagnostic> diagnostics;
        if (!usdpointcloud::ValidatePointTile(tile.tile, diagnostics) ||
            tile.levels.size() != tile.tile.lod.items.size() ||
            !tileNames.insert(tile.tile.id.ToString()).second) {
            return false;
        }
        for (std::size_t index = 0; index < tile.levels.size(); ++index) {
            const auto& level = tile.levels[index];
            const auto& item = tile.tile.lod.items[index];
            if (!level.IsValid() || level.chunk.pointCount != item.pointCount ||
                !SameBounds(level.bounds, item.bounds)) {
                return false;
            }
        }
    }

    if (!pxr::UsdGeomXform::Define(stage, pxr::SdfPath(primPath)) ||
        !pxr::UsdGeomScope::Define(
            stage, pxr::SdfPath(primPath + "/LodHeuristics")) ||
        !pxr::UsdGeomScope::Define(stage, pxr::SdfPath(primPath + "/Tiles"))) {
        return false;
    }
    const auto heuristicPath =
        pxr::SdfPath(primPath + "/LodHeuristics");
    for (const auto& tile : tiles) {
        const auto tilePath = pxr::SdfPath(
            primPath + "/Tiles/" + TilePrimName(tile.tile.id));
        const auto tileHeuristicPath =
            heuristicPath.AppendChild(pxr::TfToken(TilePrimName(tile.tile.id)))
                .AppendChild(pxr::TfToken("ScreenSize"));
        if (!tile.tile.lod.screenSizeThresholds.empty() &&
            !AuthorScreenSizeHeuristic(
                stage, tileHeuristicPath, tile.levels.front().reference,
                tile.tile.bounds, tile.tile.lod.screenSizeThresholds)) {
            return false;
        }
        if (!AuthorLodRoot(stage, tilePath.GetString(), tile.levels,
                           tile.tile.lod, tileHeuristicPath)) {
            return false;
        }
    }
    return true;
}

bool AuthorPointCloudTiledAssetWithPayloads(
    const pxr::UsdStageRefPtr& stage,
    const std::string& primPath,
    const std::vector<PointCloudTileAsset>& tiles,
    const PointCloudPayloadOptions& options) {
    std::vector<std::filesystem::path> generatedPayloads;
    return AuthorPointCloudTiledAssetWithPayloads(
        stage, primPath, tiles, options, generatedPayloads);
}

namespace detail {

bool AuthorClaimedTilePayloads(const pxr::UsdStageRefPtr& stage,
                               const std::string& primPath,
                               const std::vector<PointCloudTileAsset>& tiles,
                               const PointCloudPayloadOptions& options,
                               GeneratedPayloadSet& payloads) {
    if (!stage || !IsValidPrimPath(primPath) || tiles.empty() ||
        options.directory.empty() || options.rootLayerPath.empty()) {
        return false;
    }

    const std::filesystem::path payloadDirectory(options.directory);
    const std::filesystem::path rootLayerPath(options.rootLayerPath);
    std::vector<usdpointcloud::PointTileManifestEntry> manifestEntries;
    std::set<std::string> tileNames;
    for (const auto& tile : tiles) {
        std::vector<usdgeo::Diagnostic> diagnostics;
        if (!usdpointcloud::ValidatePointTile(tile.tile, diagnostics) ||
            tile.levels.size() != tile.tile.lod.items.size() ||
            !tileNames.insert(tile.tile.id.ToString()).second) {
            return false;
        }
        for (std::size_t index = 0; index < tile.levels.size(); ++index) {
            const auto& level = tile.levels[index];
            const auto& item = tile.tile.lod.items[index];
            if (!level.IsValid() || level.chunk.pointCount != item.pointCount ||
                !SameBounds(level.bounds, item.bounds)) {
                return false;
            }
            std::error_code error;
            const auto relativePayloadPath = std::filesystem::relative(
                TilePayloadPath(payloadDirectory, tile.tile.id, index),
                rootLayerPath.parent_path(), error);
            if (error || relativePayloadPath.empty()) {
                return false;
            }
            manifestEntries.push_back({
                tile.tile.id,
                static_cast<std::uint32_t>(index),
                item.bounds,
                level.chunk.pointCount,
                relativePayloadPath.generic_string()});
        }
    }
    if (options.tileManifestEntries) {
        std::vector<usdgeo::Diagnostic> manifestDiagnostics;
        const usdpointcloud::PointTileManifest tileManifest{manifestEntries};
        if (!usdpointcloud::ValidatePointTileManifest(
                tileManifest, manifestDiagnostics)) {
            return false;
        }
    }

    if (!pxr::UsdGeomXform::Define(stage, pxr::SdfPath(primPath)) ||
        !pxr::UsdGeomScope::Define(
            stage, pxr::SdfPath(primPath + "/LodHeuristics")) ||
        !pxr::UsdGeomScope::Define(
            stage, pxr::SdfPath(primPath + "/Tiles"))) {
        return false;
    }

    const PayloadTarget payloadTarget{payloadDirectory, rootLayerPath,
                                      payloads};
    const auto heuristicPath =
        pxr::SdfPath(primPath + "/LodHeuristics");
    for (const auto& tile : tiles) {
        const auto tilePath = pxr::SdfPath(
            primPath + "/Tiles/" + TilePrimName(tile.tile.id));
        const auto tileHeuristicPath =
            heuristicPath.AppendChild(pxr::TfToken(TilePrimName(tile.tile.id)))
                .AppendChild(pxr::TfToken("ScreenSize"));
        if (!tile.tile.lod.screenSizeThresholds.empty() &&
            !AuthorScreenSizeHeuristic(
                stage, tileHeuristicPath, tile.levels.front().reference,
                tile.tile.bounds, tile.tile.lod.screenSizeThresholds)) {
            return false;
        }
        if (!AuthorLodRoot(stage, tilePath.GetString(), tile.levels,
                           tile.tile.lod, tileHeuristicPath, &payloadTarget)) {
            return false;
        }
    }
    if (options.tileManifestEntries) {
        options.tileManifestEntries->insert(
            options.tileManifestEntries->end(), manifestEntries.begin(),
            manifestEntries.end());
    }
    return true;
}

} // namespace detail

namespace {

bool AuthorGeneratedTilePayloads(
    const pxr::UsdStageRefPtr& stage,
    const std::string& primPath,
    const std::vector<PointCloudTileAsset>& tiles,
    const PointCloudPayloadOptions& options,
    std::vector<std::filesystem::path>& generatedPayloads,
    std::string& error) {
    if (!stage || !IsValidPrimPath(primPath) || tiles.empty() ||
        options.directory.empty() || options.rootLayerPath.empty()) {
        error = "invalid tiled point-cloud payload request";
        return false;
    }
    const std::filesystem::path payloadDirectory(options.directory);
    const auto payloadPaths = detail::TilePayloadPaths(payloadDirectory, tiles);
    detail::GeneratedPayloadSet payloads(payloadDirectory, options.owner);
    if (!payloads.Claim(payloadPaths, error)) {
        return false;
    }
    if (!detail::AuthorClaimedTilePayloads(stage, primPath, tiles, options,
                                           payloads)) {
        error = "unable to author tiled point-cloud payloads";
        payloads.Rollback();
        return false;
    }
    if (!payloads.Commit(error)) {
        payloads.Rollback();
        return false;
    }
    generatedPayloads.insert(generatedPayloads.end(), payloadPaths.begin(),
                             payloadPaths.end());
    return true;
}

} // namespace

bool AuthorPointCloudTiledAssetWithPayloads(
    const pxr::UsdStageRefPtr& stage,
    const std::string& primPath,
    const std::vector<PointCloudTileAsset>& tiles,
    const PointCloudPayloadOptions& options,
    std::vector<std::filesystem::path>& generatedPayloads) {
    std::string error;
    return AuthorGeneratedTilePayloads(stage, primPath, tiles, options,
                                       generatedPayloads, error);
}

bool AuthorPointCloudTiledAssetWithPayloads(
    pxr::SdfLayer* layer,
    const std::string& primPath,
    const std::vector<PointCloudTileAsset>& tiles,
    const PointCloudPayloadOptions& options,
    std::vector<Diagnostic>& diagnostics) {
    diagnostics.clear();
    const auto fail = [&diagnostics](DiagnosticCode code,
                                     const std::string& message) {
        diagnostics.push_back(
            {code, Severity::Error, message, std::nullopt, std::nullopt});
        return false;
    };
    if (!layer || tiles.empty() || tiles.front().levels.empty() ||
        !tiles.front().levels.front().reference.IsValid()) {
        return fail(DiagnosticCode::InvalidPointTile,
                    "invalid tiled point-cloud authoring request");
    }
    const auto stage = detail::CreateDetachedStage();
    if (!stage || !pxr::UsdGeomSetStageUpAxis(
                      stage, pxr::TfToken(
                          tiles.front().levels.front().reference.stageUpAxis)) ||
        !pxr::UsdGeomSetStageMetersPerUnit(stage, 1.0)) {
        return fail(DiagnosticCode::DecodeFailure,
                    "unable to create tiled point-cloud stage");
    }
    std::vector<std::filesystem::path> generatedPayloads;
    std::string error;
    if (!AuthorGeneratedTilePayloads(stage, primPath, tiles, options,
                                     generatedPayloads, error)) {
        return fail(DiagnosticCode::DecodeFailure, error);
    }
    layer->TransferContent(stage->GetRootLayer());
    return true;
}

bool AuthorPointCloudTiledAsset(
    pxr::SdfLayer* layer,
    const std::string& primPath,
    const std::vector<PointCloudTileAsset>& tiles) {
    if (!layer || tiles.empty() || tiles.front().levels.empty() ||
        !tiles.front().levels.front().reference.IsValid()) {
        return false;
    }
    const auto stage = detail::CreateDetachedStage();
    if (!stage || !pxr::UsdGeomSetStageUpAxis(
                      stage, pxr::TfToken(
                          tiles.front().levels.front().reference.stageUpAxis)) ||
        !pxr::UsdGeomSetStageMetersPerUnit(stage, 1.0) ||
        !AuthorPointCloudTiledAsset(stage, primPath, tiles)) {
        return false;
    }
    layer->TransferContent(stage->GetRootLayer());
    return true;
}

} // namespace usdgeo