#include "usdgeo/PointCloudCache.h"

#include "usdgeo/CacheKey.h"
#include "usdgeo/PointCloudLayer.h"
#include "usdgeo/cache/Cache.h"
#include "usdpointcloud/FileFormatArguments.h"

#include <pxr/base/gf/vec3f.h>
#include <pxr/base/vt/array.h>
#include <pxr/usd/sdf/layer.h>
#include <pxr/usd/sdf/payload.h>
#include <pxr/usd/usd/payloads.h>
#include <pxr/usd/usdGeom/points.h>
#include <pxr/usd/usdGeom/xform.h>
#include <pxr/usd/usd/stage.h>

#include <chrono>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <iomanip>
#include <map>
#include <sstream>
#include <string>
#include <vector>

namespace {

void Check(bool condition) {
    if (!condition) {
        std::abort();
    }
}

std::string ReadBytes(const std::filesystem::path& path) {
    std::ifstream input(path, std::ios::binary);
    Check(input.good());
    return {std::istreambuf_iterator<char>(input),
            std::istreambuf_iterator<char>()};
}

std::string FormatDouble(double value) {
    std::ostringstream result;
    result << std::setprecision(17) << value;
    return result.str();
}

usdgeo::cache::Descriptor MakeDescriptor(
    const usdgeo::cache::SourceIdentity& sourceIdentity,
    const usdgeo::GeoReference& reference,
    const usdpointcloud::PointReadRequest& request) {
    usdgeo::cache::Descriptor descriptor;
    descriptor.source = sourceIdentity;
    descriptor.pluginVersion = "usd-pointcloud-plugins-0.3.0-display-v2";
    descriptor.parserVersion = "las-reader-1";
    descriptor.openUsdVersion = "26.08";
    descriptor.coordinateTransform = {
        {"epsg", reference.epsgCode ? std::to_string(*reference.epsgCode) : "0"},
        {"linearUnit", reference.linearUnit},
        {"sourceUpAxis", reference.sourceUpAxis},
        {"stageUpAxis", reference.stageUpAxis},
        {"origin.x", FormatDouble(reference.localOrigin.x)},
        {"origin.y", FormatDouble(reference.localOrigin.y)},
        {"origin.z", FormatDouble(reference.localOrigin.z)}};
    for (const auto& [key, value] : request.canonicalArguments) {
        if (key == "attributes") {
            descriptor.attributes.emplace_back(key, value);
        } else if (key != "payloadDirectory" && key != "chunkPointLimit" &&
                   key != "memoryBudgetBytes") {
            descriptor.tileAndLod.emplace_back(key, value);
        }
    }
    descriptor.downsampling = {{"algorithm", "fixed-stride"},
                               {"version", "1"}};
    Check(descriptor.IsValid());
    return descriptor;
}

usdgeo::cache::Descriptor MakeDescriptor(
    const std::filesystem::path& sourcePath,
    const usdgeo::GeoReference& reference,
    const usdpointcloud::PointReadRequest& request) {
    usdgeo::cache::SourceIdentity sourceIdentity;
    std::string errorMessage;
    Check(usdgeo::cache::TryBuildLocalSourceIdentity(
        sourcePath, sourceIdentity, errorMessage));
    Check(errorMessage.empty());
    return MakeDescriptor(sourceIdentity, reference, request);
}

void SetCacheRoot(const std::filesystem::path& root) {
#if defined(_WIN32)
    Check(_putenv_s("USDGEO_CACHE_ROOT", root.string().c_str()) == 0);
#else
    Check(setenv("USDGEO_CACHE_ROOT", root.string().c_str(), 1) == 0);
#endif
}

void ClearCacheRoot() {
#if defined(_WIN32)
    Check(_putenv_s("USDGEO_CACHE_ROOT", "") == 0);
#else
    Check(unsetenv("USDGEO_CACHE_ROOT") == 0);
#endif
}

void TestPointCloudCacheMissAndMaterialization() {
    const auto suffix = std::to_string(
        std::chrono::steady_clock::now().time_since_epoch().count());
    const auto testRoot = std::filesystem::temp_directory_path() /
                          ("usdgeo-pointcloud-cache-" + suffix);
    std::filesystem::remove_all(testRoot);
    std::filesystem::create_directories(testRoot);

    const auto sourcePath = testRoot / "source.las";
    {
        std::ofstream source(sourcePath, std::ios::binary);
        source << "cache identity source";
    }

    usdgeo::GeoReference reference;
    reference.epsgCode = 26910;
    reference.localOrigin = {1000.0, 2000.0, 3000.0};
    const std::map<std::string, std::string> arguments = {
        {"attributes", "intensity"},
        {"payloadDirectory", "requested_payloads"},
        {"tile", "true"},
        {"tileSize", "1"}};
    usdpointcloud::PointReadRequest request;
    std::vector<usdgeo::Diagnostic> diagnostics;
    Check(usdpointcloud::MakeReadRequest(arguments, request, diagnostics));
    Check(diagnostics.empty());

    const auto cacheRoot = testRoot / "cache";
    usdgeo::cache::Layout layout;
    Check(usdgeo::cache::TryBuildLayout(
        cacheRoot, MakeDescriptor(sourcePath, reference, request), layout));
    Check(!usdgeo::cache::IsCacheHit(layout));

    const auto previousCacheRoot = std::getenv("USDGEO_CACHE_ROOT");
    const bool hadPreviousCacheRoot = previousCacheRoot != nullptr;
    const std::string previousCacheRootValue =
        hadPreviousCacheRoot ? previousCacheRoot : "";
    SetCacheRoot(cacheRoot);

    const auto missLayer = pxr::SdfLayer::CreateNew(
        (testRoot / "miss.usda").string());
    Check(missLayer);
    bool hit = true;
    std::string errorMessage;
    Check(usdgeo::TryLoadPointCloudCache(
        missLayer.operator->(), sourcePath, reference, request,
        "las-reader-1", hit, errorMessage));
    Check(!hit);
    Check(errorMessage.empty());

    usdgeo::cache::SourceIdentity resolverIdentity;
    Check(usdgeo::cache::TryBuildLocalSourceIdentity(
        sourcePath, resolverIdentity, errorMessage));
    const auto resolverLayer = pxr::SdfLayer::CreateAnonymous(
        "resolver-identity-cache.usda");
    Check(resolverLayer);
    hit = false;
    Check(usdgeo::TryLoadPointCloudCache(
        resolverLayer.operator->(), resolverIdentity, sourcePath.parent_path(),
        reference, request, "las-reader-1", hit, errorMessage));
    Check(!hit);
    Check(errorMessage.empty());

    std::filesystem::create_directories(layout.payloadDirectory);
    const auto payloadPath = layout.payloadDirectory / "tile.usdc";
    const auto payloadStage = pxr::UsdStage::CreateNew(payloadPath.string());
    Check(payloadStage);
    const auto payloadPoints = pxr::UsdGeomPoints::Define(
        payloadStage, pxr::SdfPath("/CachedTile/Points"));
    Check(payloadPoints.GetPrim().IsValid());
    pxr::VtVec3fArray positions(1);
    positions[0] = pxr::GfVec3f(1.0f, 2.0f, 3.0f);
    Check(payloadPoints.GetPointsAttr().Set(positions));
    Check(payloadStage->GetRootLayer()->Save());

    const auto rootStage = pxr::UsdStage::CreateNew(layout.rootLayer.string());
    Check(rootStage);
    const auto payloadPrim = pxr::UsdGeomXform::Define(
        rootStage, pxr::SdfPath("/PointCloud/Tile"));
    Check(payloadPrim.GetPrim().IsValid());
    Check(payloadPrim.GetPrim().GetPayloads().AddPayload(
        pxr::SdfPayload("payloads/tile.usdc",
                        pxr::SdfPath("/CachedTile"))));
    Check(rootStage->GetRootLayer()->Save());
    {
        std::ofstream manifest(layout.manifest);
        manifest << "committed\n";
    }
    Check(usdgeo::cache::IsCacheHit(layout));

    const auto requestedDirectory = testRoot / "output";
    Check(std::filesystem::create_directories(requestedDirectory));
    const auto requestedRoot = requestedDirectory / "requested.usda";
    const auto requestedLayer = pxr::SdfLayer::CreateNew(requestedRoot.string());
    Check(requestedLayer);
    hit = false;
    errorMessage.clear();
    Check(usdgeo::TryLoadPointCloudCache(
        requestedLayer.operator->(), resolverIdentity, sourcePath.parent_path(),
        reference, request, "las-reader-1", hit, errorMessage));
    Check(hit);
    Check(errorMessage.empty());
    const auto requestedPayload = testRoot / "requested_payloads" / "tile.usdc";
    Check(std::filesystem::exists(requestedPayload));

    const auto cachedPrim = requestedLayer->GetPrimAtPath(
        pxr::SdfPath("/PointCloud/Tile"));
    Check(cachedPrim != nullptr);
    const auto payloadItems = cachedPrim->GetPayloadList().GetPrependedItems();
    Check(payloadItems.size() == 1);
    Check(payloadItems.front().GetAssetPath() ==
            "../requested_payloads/tile.usdc");

    {
        const auto requestedStage = pxr::UsdStage::Open(requestedLayer);
        Check(requestedStage);
        const auto requestedPoints = pxr::UsdGeomPoints::Get(
            requestedStage, pxr::SdfPath("/PointCloud/Tile/Points"));
        Check(requestedPoints.GetPrim().IsValid());
        pxr::VtVec3fArray requestedPositions;
        Check(requestedPoints.GetPointsAttr().Get(&requestedPositions));
        Check(requestedPositions.size() == 1 &&
              requestedPositions[0] == pxr::GfVec3f(1.0f, 2.0f, 3.0f));
    }

    // Materialized payloads belong to the layer they were materialized for:
    // a later hit replaces them when they are stale, and never overwrites a
    // file someone else placed.
    const auto requestedOwnerRecord =
        requestedPayload.parent_path() /
        ("payload-owner-" +
         usdgeo::StableCacheKey(
             {{"payloadOwner", usdgeo::PointCloudPayloadOwner(
                                   resolverIdentity.identifier, request)}}) +
         ".manifest");
    Check(std::filesystem::exists(requestedOwnerRecord));
    const auto cachedPayloadBytes = ReadBytes(payloadPath);
    const auto loadRequested = [&]() {
        const auto layer =
            pxr::SdfLayer::CreateAnonymous("requested-again.usda");
        bool loadedHit = false;
        std::string loadError;
        Check(usdgeo::TryLoadPointCloudCache(
            layer.operator->(), resolverIdentity, sourcePath.parent_path(),
            reference, request, "las-reader-1", loadedHit, loadError));
        Check(loadError.empty());
        return loadedHit;
    };
    std::ofstream(requestedPayload, std::ios::binary | std::ios::trunc)
        << "stale payload";
    Check(loadRequested());
    Check(ReadBytes(requestedPayload) == cachedPayloadBytes);

    std::filesystem::remove(requestedOwnerRecord);
    std::ofstream(requestedPayload, std::ios::binary | std::ios::trunc)
        << "user payload";
    Check(!loadRequested());
    Check(ReadBytes(requestedPayload) == "user payload");

    // A file that already holds the cached bytes is taken over, so payloads
    // materialized before ownership was recorded keep resolving.
    Check(std::filesystem::copy_file(
        payloadPath, requestedPayload,
        std::filesystem::copy_options::overwrite_existing));
    Check(loadRequested());
    Check(std::filesystem::exists(requestedOwnerRecord));

    std::error_code error;
    std::filesystem::remove(layout.manifest, error);
    Check(!error);
    const auto invalidLayer = pxr::SdfLayer::CreateNew(
        (testRoot / "invalid.usda").string());
    Check(invalidLayer);
    hit = true;
    errorMessage.clear();
    Check(usdgeo::TryLoadPointCloudCache(
        invalidLayer.operator->(), sourcePath, reference, request,
        "las-reader-1", hit, errorMessage));
    Check(!hit);
    Check(errorMessage.empty());
    Check(!std::filesystem::exists(layout.entryDirectory));

    const usdgeo::cache::SourceIdentity incompleteResolverIdentity{
        "https://memory.example/pointcloud.copc", 42, 0, "revision-a"};
    const auto resolverDescriptor =
        MakeDescriptor(incompleteResolverIdentity, reference, request);
    usdgeo::cache::Layout resolverLayout;
    Check(usdgeo::cache::TryBuildLayout(
        cacheRoot, resolverDescriptor, resolverLayout));
    std::filesystem::create_directories(resolverLayout.payloadDirectory);
    std::ofstream(resolverLayout.rootLayer, std::ios::binary)
        << "incomplete resolver root";
    const auto incompleteResolverLayer = pxr::SdfLayer::CreateNew(
        (testRoot / "resolver-incomplete.usda").string());
    Check(incompleteResolverLayer);
    hit = true;
    errorMessage.clear();
    Check(usdgeo::TryLoadPointCloudCache(
        incompleteResolverLayer.operator->(), incompleteResolverIdentity, {},
        reference, request, "las-reader-1", hit, errorMessage));
    Check(!hit);
    Check(errorMessage.empty());
    Check(!std::filesystem::exists(resolverLayout.entryDirectory));

    const auto corruptSource = testRoot / "corrupt-source.las";
    {
        std::ofstream source(corruptSource, std::ios::binary);
        source << "corrupt cache identity source";
    }
    usdgeo::cache::Layout corruptLayout;
    Check(usdgeo::cache::TryBuildLayout(
        cacheRoot, MakeDescriptor(corruptSource, reference, request),
        corruptLayout));
    std::filesystem::create_directories(corruptLayout.payloadDirectory);
    std::ofstream(corruptLayout.rootLayer, std::ios::binary | std::ios::trunc)
        << "corrupt root layer";
    {
        std::ofstream manifest(corruptLayout.manifest);
        manifest << "committed\n";
    }
    Check(usdgeo::cache::IsCacheHit(corruptLayout));
    const auto corruptLayer = pxr::SdfLayer::CreateNew(
        (testRoot / "corrupt.usda").string());
    Check(corruptLayer);
    hit = true;
    errorMessage.clear();
    Check(usdgeo::TryLoadPointCloudCache(
        corruptLayer.operator->(), corruptSource, reference, request,
        "las-reader-1", hit, errorMessage));
    Check(!hit);
    Check(errorMessage.empty());
    Check(!std::filesystem::exists(corruptLayout.entryDirectory));

    const auto missingPayloadSource = testRoot / "missing-payload-source.las";
    {
        std::ofstream source(missingPayloadSource, std::ios::binary);
        source << "missing payload cache identity source";
    }
    usdgeo::cache::Layout missingPayloadLayout;
    Check(usdgeo::cache::TryBuildLayout(
        cacheRoot,
        MakeDescriptor(missingPayloadSource, reference, request),
        missingPayloadLayout));
    std::filesystem::create_directories(
        missingPayloadLayout.payloadDirectory);
    const auto missingPayloadStage = pxr::UsdStage::CreateNew(
        missingPayloadLayout.rootLayer.string());
    Check(missingPayloadStage);
    const auto missingPayloadPrim = pxr::UsdGeomXform::Define(
        missingPayloadStage, pxr::SdfPath("/PointCloud/Tile"));
    Check(missingPayloadPrim.GetPrim().IsValid());
    Check(missingPayloadPrim.GetPrim().GetPayloads().AddPayload(
        pxr::SdfPayload("payloads/missing.usdc")));
    Check(missingPayloadStage->GetRootLayer()->Save());
    {
        std::ofstream manifest(missingPayloadLayout.manifest);
        manifest << "committed\n";
    }
    const auto missingPayloadLayer = pxr::SdfLayer::CreateNew(
        (testRoot / "missing-payload.usda").string());
    Check(missingPayloadLayer);
    hit = true;
    errorMessage.clear();
    Check(usdgeo::TryLoadPointCloudCache(
        missingPayloadLayer.operator->(), missingPayloadSource, reference,
        request, "las-reader-1", hit, errorMessage));
    Check(!hit);
    Check(errorMessage.empty());
    Check(!std::filesystem::exists(missingPayloadLayout.entryDirectory));

    const auto corruptPayloadSource = testRoot / "corrupt-payload-source.las";
    {
        std::ofstream source(corruptPayloadSource, std::ios::binary);
        source << "corrupt payload cache identity source";
    }
    usdgeo::cache::Layout corruptPayloadLayout;
    Check(usdgeo::cache::TryBuildLayout(
        cacheRoot,
        MakeDescriptor(corruptPayloadSource, reference, request),
        corruptPayloadLayout));
    std::filesystem::create_directories(
        corruptPayloadLayout.payloadDirectory);
    const auto corruptPayloadStage = pxr::UsdStage::CreateNew(
        corruptPayloadLayout.rootLayer.string());
    Check(corruptPayloadStage);
    const auto corruptPayloadPrim = pxr::UsdGeomXform::Define(
        corruptPayloadStage, pxr::SdfPath("/PointCloud/Tile"));
    Check(corruptPayloadPrim.GetPrim().IsValid());
    Check(corruptPayloadPrim.GetPrim().GetPayloads().AddPayload(
        pxr::SdfPayload("payloads/corrupt.usdc")));
    Check(corruptPayloadStage->GetRootLayer()->Save());
    std::ofstream(corruptPayloadLayout.payloadDirectory / "corrupt.usdc",
                  std::ios::binary)
        << "corrupt payload";
    {
        std::ofstream manifest(corruptPayloadLayout.manifest);
        manifest << "committed\n";
    }
    const auto corruptPayloadLayer = pxr::SdfLayer::CreateNew(
        (testRoot / "corrupt-payload.usda").string());
    Check(corruptPayloadLayer);
    hit = true;
    errorMessage.clear();
    Check(usdgeo::TryLoadPointCloudCache(
        corruptPayloadLayer.operator->(), corruptPayloadSource, reference,
        request, "las-reader-1", hit, errorMessage));
    Check(!hit);
    Check(errorMessage.empty());
    Check(!std::filesystem::exists(corruptPayloadLayout.entryDirectory));

    if (hadPreviousCacheRoot) {
        SetCacheRoot(previousCacheRootValue);
    } else {
        ClearCacheRoot();
    }
    std::filesystem::remove_all(testRoot);
}

} // namespace

int main() {
    TestPointCloudCacheMissAndMaterialization();
    return 0;
}
