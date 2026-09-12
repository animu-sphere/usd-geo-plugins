#include "usdgeo/CacheKey.h"
#include "usdgeo/PointCloudLayer.h"
#include "usdcopc/Copc.h"

#include <pxr/base/gf/vec3d.h>
#include <pxr/base/gf/vec3f.h>
#include <pxr/base/tf/diagnosticMgr.h>
#include <pxr/base/vt/array.h>
#include <pxr/usd/sdf/layer.h>
#include <pxr/usd/usdGeom/points.h>
#include <pxr/usd/usdGeom/metrics.h>
#include <pxr/usd/usdLod/rootAPI.h>
#include <pxr/usd/usdLod/screenSizeHeuristic.h>
#include <pxr/usd/usd/payloads.h>

#include <algorithm>
#include <array>
#include <atomic>
#include <chrono>
#include <cstdlib>
#include <cstdio>
#include <filesystem>
#include <fstream>
#include <functional>
#include <limits>
#include <map>
#include <set>

namespace {

void Check(bool condition) {
    if (!condition) {
        std::abort();
    }
}

std::string ReadFile(const std::filesystem::path& path) {
    std::ifstream input(path, std::ios::binary);
    Check(input.good());
    return {std::istreambuf_iterator<char>(input),
            std::istreambuf_iterator<char>()};
}

void WriteFile(const std::filesystem::path& path, const std::string& text) {
    std::ofstream output(path, std::ios::binary | std::ios::trunc);
    output << text;
    Check(output.good());
}

// A directory no other test or earlier run shares, so payload ownership
// state never leaks between cases.
std::filesystem::path UniqueTestDirectory(const std::string& name) {
    const auto stamp =
        std::chrono::steady_clock::now().time_since_epoch().count();
    const auto directory = std::filesystem::temp_directory_path() /
                           (name + "_" + std::to_string(stamp));
    std::filesystem::remove_all(directory);
    return directory;
}

std::filesystem::path OwnerDirectory(const std::filesystem::path& directory,
                                     const std::string& owner) {
    return directory / usdgeo::StableCacheKey({{"payloadOwner", owner}});
}

std::vector<std::filesystem::path> Subdirectories(
    const std::filesystem::path& directory, const std::string& prefix) {
    std::vector<std::filesystem::path> matches;
    std::error_code error;
    for (const auto& entry :
         std::filesystem::directory_iterator(directory, error)) {
        if (entry.is_directory() &&
            entry.path().filename().string().rfind(prefix, 0) == 0) {
            matches.push_back(entry.path());
        }
    }
    std::sort(matches.begin(), matches.end());
    return matches;
}

// The one published generation an owner has, which a test expects to exist.
std::filesystem::path OnlyGeneration(const std::filesystem::path& ownerDirectory) {
    const auto generated = Subdirectories(ownerDirectory, "g-");
    const auto cached = Subdirectories(ownerDirectory, "c-");
    Check(generated.size() + cached.size() == 1);
    return generated.empty() ? cached.front() : generated.front();
}

class WarningCounter final : public pxr::TfDiagnosticMgr::Delegate {
public:
    WarningCounter() { pxr::TfDiagnosticMgr::GetInstance().AddDelegate(this); }
    ~WarningCounter() override {
        pxr::TfDiagnosticMgr::GetInstance().RemoveDelegate(this);
    }

    void IssueError(const pxr::TfError&) override {}
    void IssueFatalError(const pxr::TfCallContext&,
                         const std::string&) override {
        _UnhandledAbort();
    }
    void IssueStatus(const pxr::TfStatus&) override {}
    void IssueWarning(const pxr::TfWarning&) override { ++count_; }

    int Count() const noexcept { return count_; }

private:
    std::atomic_int count_{0};
};

std::map<std::string, std::string> ReadDirectoryFiles(
    const std::filesystem::path& directory) {
    std::map<std::string, std::string> files;
    std::error_code error;
    for (const auto& entry : std::filesystem::directory_iterator(
             directory, error)) {
        Check(!error);
        if (entry.is_regular_file(error)) {
            Check(!error);
            files.emplace(entry.path().filename().string(),
                          ReadFile(entry.path()));
        }
        error.clear();
    }
    Check(!error);
    return files;
}

bool HasPointSpoolFile(const std::filesystem::path& directory) {
    std::error_code error;
    if (!std::filesystem::is_directory(directory, error)) {
        Check(!error);
        return false;
    }
    for (const auto& entry : std::filesystem::directory_iterator(
             directory, error)) {
        Check(!error);
        const auto name = entry.path().filename().string();
        if (entry.is_regular_file(error) && name.rfind("tile_", 0) == 0 &&
            entry.path().extension() == ".bin") {
            return true;
        }
        Check(!error);
        error.clear();
    }
    Check(!error);
    return false;
}

class TestPointStream final : public usdpointcloud::PointStream {
public:
    explicit TestPointStream(
        std::vector<std::pair<usdpointcloud::PointChunk,
                              usdpointcloud::PointData>> chunks)
        : chunks_(std::move(chunks)) {}

    usdpointcloud::PointStreamStatus ReadNext(
        usdpointcloud::PointChunk& chunk,
        usdpointcloud::PointData& data,
        usdgeo::Diagnostic& diagnostic) override {
        diagnostic = {};
        if (index_ == chunks_.size()) {
            return usdpointcloud::PointStreamStatus::End;
        }
        chunk = chunks_[index_].first;
        data = chunks_[index_].second;
        ++index_;
        return usdpointcloud::PointStreamStatus::Chunk;
    }

private:
    std::vector<std::pair<usdpointcloud::PointChunk,
                          usdpointcloud::PointData>> chunks_;
    std::size_t index_ = 0;
};

class GeneratedPointStream final : public usdpointcloud::PointStream {
public:
    GeneratedPointStream(
        std::size_t pointCount,
                std::filesystem::path spoolDirectory)
        : pointCount_(pointCount),
                    spoolDirectory_(std::move(spoolDirectory)) {}

    usdpointcloud::PointStreamStatus ReadNext(
        usdpointcloud::PointChunk& chunk,
        usdpointcloud::PointData& data,
        usdgeo::Diagnostic& diagnostic) override {
        diagnostic = {};
        if (index_ == pointCount_) {
            return usdpointcloud::PointStreamStatus::End;
        }
        sawSpoolFile_ = sawSpoolFile_ || HasPointSpoolFile(spoolDirectory_);
        const auto tileIndex = index_ % 32;
        const auto pointInTile = index_ / 32;
        data.positions = {{static_cast<double>(tileIndex * 128 + 1),
                   0.0, static_cast<double>(pointInTile)}};
        data.intensity = {static_cast<std::uint16_t>(index_)};
        usdgeo::SpatialBounds bounds;
        bounds.Expand(data.positions.front());
        chunk = usdpointcloud::MakePointChunk(data, bounds);
        ++index_;
        return usdpointcloud::PointStreamStatus::Chunk;
    }

    bool SawSpoolFile() const noexcept { return sawSpoolFile_; }

private:
    std::size_t pointCount_ = 0;
    std::size_t index_ = 0;
    std::filesystem::path spoolDirectory_;
    bool sawSpoolFile_ = false;
};

class FailingPointStream final : public usdpointcloud::PointStream {
public:
    usdpointcloud::PointStreamStatus ReadNext(
        usdpointcloud::PointChunk& chunk,
        usdpointcloud::PointData& data,
        usdgeo::Diagnostic& diagnostic) override {
        if (failed_) {
            diagnostic = {usdgeo::DiagnosticCode::DecodeFailure,
                          usdgeo::Severity::Error, "injected stream failure"};
            return usdpointcloud::PointStreamStatus::Error;
        }
        failed_ = true;
        data.positions = {{0.0, 0.0, 0.0}};
        usdgeo::SpatialBounds bounds;
        bounds.Expand(data.positions.front());
        chunk = usdpointcloud::MakePointChunk(data, bounds);
        diagnostic = {};
        return usdpointcloud::PointStreamStatus::Chunk;
    }

private:
    bool failed_ = false;
};

void TestPointCloudRoundTrip() {
    const auto stage = usdgeo::PointCloudLayer::CreateStage();
    usdgeo::GeoReference reference;
    reference.epsgCode = 26910;
    reference.wkt = "WKT";
    reference.localOrigin = {1000.0, 2000.0, 3000.0};

    usdpointcloud::PointChunk chunk;
    chunk.pointCount = 2;
    chunk.bounds.Expand({1000.0, 2000.0, 3000.0});
    chunk.bounds.Expand({1002.0, 2003.0, 3004.0});
    chunk.attributes = {
        {"intensity", usdpointcloud::PointAttributeType::UInt16},
        {"red", usdpointcloud::PointAttributeType::UInt16},
        {"green", usdpointcloud::PointAttributeType::UInt16},
        {"blue", usdpointcloud::PointAttributeType::UInt16},
        {"classification", usdpointcloud::PointAttributeType::UInt8},
        {"classificationFlags", usdpointcloud::PointAttributeType::UInt8},
        {"scannerChannel", usdpointcloud::PointAttributeType::UInt8},
        {"scanDirectionFlag", usdpointcloud::PointAttributeType::UInt8},
        {"edgeOfFlightLine", usdpointcloud::PointAttributeType::UInt8},
        {"userData", usdpointcloud::PointAttributeType::UInt8},
        {"scanAngle", usdpointcloud::PointAttributeType::Int16},
        {"pointSourceId", usdpointcloud::PointAttributeType::UInt16},
        {"nir", usdpointcloud::PointAttributeType::UInt16},
        {"waveformDescriptorIndex", usdpointcloud::PointAttributeType::UInt8},
        {"waveformDataOffset", usdpointcloud::PointAttributeType::UInt64},
        {"waveformPacketSize", usdpointcloud::PointAttributeType::UInt32},
        {"returnPointWaveformLocation", usdpointcloud::PointAttributeType::Float32},
        {"waveformXt", usdpointcloud::PointAttributeType::Float32},
        {"waveformYt", usdpointcloud::PointAttributeType::Float32},
        {"waveformZt", usdpointcloud::PointAttributeType::Float32},
        {"waveformDataExternal", usdpointcloud::PointAttributeType::UInt8},
        {"normal", usdpointcloud::PointAttributeType::Float64Vec3}};
    const std::vector<usdgeo::Vec3d> positions = {
        {1000.0, 2000.0, 3000.0}, {1002.0, 2003.0, 3004.0}};
    usdgeo::PointCloudLayer::Data data;
    data.positions = positions;
    data.intensity = {42, 84};
    data.colorBitDepth = 16;
    data.red = {65535, 32768};
    data.green = {16384, 49152};
    data.blue = {0, 65535};
    data.classification = {2, 5};
    data.classificationFlags = {1, 3};
    data.scannerChannel = {0, 2};
    data.scanDirectionFlag = {0, 1};
    data.edgeOfFlightLine = {1, 0};
    data.userData = {7, 9};
    data.scanAngle = {-12, 34};
    data.pointSourceId = {100, 200};
    data.nir = {300, 400};
    data.waveformDescriptorIndex = {7, 8};
    data.waveformDataOffset = {1234, 5678};
    data.waveformPacketSize = {48, 64};
    data.returnPointWaveformLocation = {0.25f, 0.5f};
    data.waveformXt = {1.0f, 4.0f};
    data.waveformYt = {2.0f, 5.0f};
    data.waveformZt = {3.0f, 6.0f};
    data.waveformDataExternal = {1, 0};
    data.waveformDataFile = "sample.wdp";
    data.extraByteNames = {"normal"};
    data.extraByteComponentCounts = {3};
    data.extraBytes = {{1.0, 2.0, 3.0, 4.0, 5.0, 6.0}};

    Check(usdgeo::AuthorPointCloudAsset(
        stage, "/PointCloud", reference, chunk.bounds, chunk, data));

    const auto layerPath =
        std::filesystem::temp_directory_path() / "usd_geo_points.usda";
    Check(stage->GetRootLayer()->Export(layerPath.string()));
    const auto reopenedStage = pxr::UsdStage::Open(layerPath.string());
    Check(reopenedStage);
    std::filesystem::remove(layerPath);

    const auto points =
        pxr::UsdGeomPoints::Get(reopenedStage, pxr::SdfPath("/PointCloud"));
    Check(points.GetPrim().IsValid());
    pxr::VtVec3fArray authoredPositions;
    Check(points.GetPointsAttr().Get(&authoredPositions));
    Check(authoredPositions.size() == 2);
    Check(authoredPositions[1] == pxr::GfVec3f(2.0f, 3.0f, 4.0f));
    pxr::VtFloatArray authoredWidths;
    Check(points.GetWidthsAttr().Get(&authoredWidths));
    Check(authoredWidths.size() == 1 && authoredWidths[0] > 0.0f);
    Check(points.GetWidthsInterpolation() == pxr::UsdGeomTokens->constant);
    pxr::VtVec3fArray authoredColors;
    Check(points.GetDisplayColorPrimvar().Get(&authoredColors));
    Check(authoredColors.size() == 2 &&
          authoredColors[0] == pxr::GfVec3f(1.0f, 16384.0f / 65535.0f,
                             0.0f) &&
          authoredColors[1] == pxr::GfVec3f(32768.0f / 65535.0f,
                             49152.0f / 65535.0f, 1.0f));
    pxr::VtIntArray authoredIntensity;
    Check(points.GetPrim()
              .GetAttribute(pxr::TfToken("geo:intensity"))
              .Get(&authoredIntensity));
    Check(authoredIntensity.size() == 2 && authoredIntensity[1] == 84);

        pxr::VtArray<unsigned char> authoredClassificationFlags;
        Check(points.GetPrim()
              .GetAttribute(pxr::TfToken("geo:classificationFlags"))
              .Get(&authoredClassificationFlags));
        Check(authoredClassificationFlags.size() == 2 &&
            authoredClassificationFlags[1] == 3);
        pxr::VtIntArray authoredScanAngle;
        Check(points.GetPrim().GetAttribute(pxr::TfToken("geo:scanAngle"))
              .Get(&authoredScanAngle));
        Check(authoredScanAngle.size() == 2 && authoredScanAngle[0] == -12);
        pxr::VtIntArray authoredNir;
        Check(points.GetPrim().GetAttribute(pxr::TfToken("geo:nir"))
              .Get(&authoredNir));
        Check(authoredNir.size() == 2 && authoredNir[1] == 400);
          pxr::VtArray<std::uint64_t> authoredWaveformOffsets;
          Check(points.GetPrim()
                .GetAttribute(pxr::TfToken("geo:waveformDataOffset"))
                .Get(&authoredWaveformOffsets));
          Check(authoredWaveformOffsets.size() == 2 &&
              authoredWaveformOffsets[1] == 5678);
          pxr::VtArray<float> authoredWaveformParameters;
          Check(points.GetPrim()
                .GetAttribute(pxr::TfToken("geo:waveformXt"))
                .Get(&authoredWaveformParameters));
          Check(authoredWaveformParameters.size() == 2 &&
              authoredWaveformParameters[0] == 1.0f);
        std::string authoredWaveformDataFile;
        Check(points.GetPrim()
              .GetAttribute(pxr::TfToken("geo:waveformDataFile"))
              .Get(&authoredWaveformDataFile));
        Check(authoredWaveformDataFile == "sample.wdp");
        pxr::VtArray<pxr::GfVec3d> authoredNormals;
        Check(points.GetPrim().GetAttribute(pxr::TfToken("geo:normal"))
                            .Get(&authoredNormals));
        Check(authoredNormals.size() == 2 &&
                    authoredNormals[1] == pxr::GfVec3d(4.0, 5.0, 6.0));

    int epsgCode = 0;
    Check(points.GetPrim().GetAttribute(pxr::TfToken("geo:epsgCode")).Get(&epsgCode));
    Check(epsgCode == 26910);
}

void TestOptionalAttributesAreIndependent() {
    const auto stage = usdgeo::PointCloudLayer::CreateStage();
    usdgeo::GeoReference reference;
    reference.epsgCode = 26910;

    usdpointcloud::PointChunk chunk;
    chunk.pointCount = 1;
    chunk.bounds.Expand({0.0, 0.0, 0.0});

    usdgeo::PointCloudLayer::Data data;
    data.positions = {{0.0, 0.0, 0.0}};
    data.scanAngle = {-12};

    Check(usdgeo::AuthorPointCloudAsset(
        stage, "/PointCloud", reference, chunk.bounds, chunk, data));

    const auto prim = stage->GetPrimAtPath(pxr::SdfPath("/PointCloud"));
    Check(prim.IsValid());
    pxr::VtIntArray scanAngle;
    Check(prim.GetAttribute(pxr::TfToken("geo:scanAngle")).Get(&scanAngle));
    Check(scanAngle.size() == 1 && scanAngle[0] == -12);
    Check(!prim.GetAttribute(pxr::TfToken("geo:classificationFlags")));
}

void TestInvalidPositionDoesNotMutateStage() {
    const auto stage = usdgeo::PointCloudLayer::CreateStage();
    usdgeo::GeoReference reference;
    reference.epsgCode = 26910;

    usdpointcloud::PointChunk chunk;
    chunk.pointCount = 1;
    chunk.bounds.Expand({0.0, 0.0, 0.0});

    const std::vector<usdgeo::Vec3d> positions = {
        {std::numeric_limits<double>::quiet_NaN(), 0.0, 0.0}};
    Check(!usdgeo::AuthorPointCloudAsset(
        stage, "/InvalidPointCloud", reference, chunk.bounds, chunk,
        positions));
    Check(!stage->GetPrimAtPath(pxr::SdfPath("/InvalidPointCloud")).IsValid());
}

void TestLayerAuthoringEntryPoint() {
    const auto layer = pxr::SdfLayer::CreateAnonymous("usd_geo_asset.usda");
    usdgeo::GeoReference reference;
    reference.epsgCode = 26910;
    reference.stageUpAxis = "Y";

    usdpointcloud::PointCloudAsset asset;
    asset.reference = reference;
    asset.data.positions = {{1000.0, 2000.0, 3000.0}};
    asset.bounds.Expand({0.0, 0.0, 0.0});
    asset.chunk = usdpointcloud::MakePointChunk(asset.data, asset.bounds);

    Check(usdgeo::AuthorPointCloudAsset(
        layer.operator->(), "/PointCloud", asset));
    const auto stage = pxr::UsdStage::Open(layer);
    Check(stage);
    Check(pxr::UsdGeomGetStageUpAxis(stage) == pxr::TfToken("Y"));
    Check(stage->GetPrimAtPath(pxr::SdfPath("/PointCloud")).IsValid());

    usdgeo::PointCloudAuthorFailure failure;
    Check(!usdgeo::AuthorPointCloudAsset(
        nullptr, "/PointCloud", asset, failure));
    Check(failure == usdgeo::PointCloudAuthorFailure::InvalidLayer);
}

void TestMetadataAuthoringAllowsEmptyPointCloud() {
    const auto layer = pxr::SdfLayer::CreateAnonymous("metadata.usda");
    usdgeo::GeoReference reference;
    reference.epsgCode = 26910;
    usdgeo::SpatialBounds bounds;
    bounds.Expand({0.0, 0.0, 0.0});

    usdpointcloud::PointChunk chunk;
    chunk.bounds = bounds;
    chunk.attributes = {{"xyz", usdpointcloud::PointAttributeType::Float64}};

    Check(usdgeo::AuthorPointCloudMetadata(
        layer.operator->(), "/PointCloud", reference, bounds, chunk,
        {3, {0.01, 0.01, 0.01}, {1000.0, 2000.0, 3000.0}}));
    const auto stage = pxr::UsdStage::Open(layer);
    Check(stage);
    const auto points = pxr::UsdGeomPoints::Get(
        stage, pxr::SdfPath("/PointCloud"));
    pxr::VtVec3fArray positions;
    Check(points.GetPointsAttr().Get(&positions));
    Check(positions.empty());
    std::uint64_t pointCount = 1;
    Check(points.GetPrim().GetAttribute(pxr::TfToken("geo:pointCount"))
              .Get(&pointCount));
    Check(pointCount == 0);
}

void TestLodAuthoring() {
    const auto stage = usdgeo::PointCloudLayer::CreateStage();
    usdgeo::GeoReference reference;
    reference.epsgCode = 26910;

    std::vector<usdpointcloud::PointCloudAsset> levels(2);
    for (auto& level : levels) {
        level.reference = reference;
        level.data.positions = {{1.0, 2.0, 3.0}};
        level.bounds.Expand({0.0, 1.0, 2.0});
        level.bounds.Expand({4.0, 5.0, 6.0});
        level.chunk = usdpointcloud::MakePointChunk(level.data, level.bounds);
    }
    usdpointcloud::PointLodHierarchy hierarchy;
    hierarchy.bounds = levels[0].bounds;
    hierarchy.items = {{0, 1, hierarchy.bounds, {0, 1}},
                        {1, 1, hierarchy.bounds, {0, 1}}};
    hierarchy.screenSizeThresholds = {0.1f};

    Check(usdgeo::AuthorPointCloudLodAsset(
        stage, "/PointCloud", levels, hierarchy));
    const auto prim = stage->GetPrimAtPath(pxr::SdfPath("/PointCloud"));
    Check(prim.HasAPI<pxr::UsdLodRootAPI>());
    Check(prim.GetAttribute(pxr::TfToken("lod:default:index")).IsValid());
    Check(stage->GetPrimAtPath(pxr::SdfPath("/PointCloud/LOD0")).IsValid());
    Check(stage->GetPrimAtPath(pxr::SdfPath("/PointCloud/LOD1")).IsValid());
    const auto children = prim.GetChildren();
    auto child = children.begin();
    Check(child != children.end() && child->GetName() == "LOD0");
    ++child;
    Check(child != children.end() && child->GetName() == "LOD1");
    ++child;
    Check(child == children.end());
    Check(!stage->GetPrimAtPath(pxr::SdfPath("/PointCloud/LodHeuristics"))
              .IsValid());
    pxr::SdfPathVector heuristicTargets;
    Check(pxr::UsdLodRootAPI(prim)
              .GetLodHeuristicsRel()
              .GetTargets(&heuristicTargets));
    Check(heuristicTargets.size() == 1 &&
          heuristicTargets[0] ==
              pxr::SdfPath("/LodHeuristics/PointCloudScreenSize"));
    const auto heuristic = pxr::UsdLodScreenSizeHeuristic::Get(
        stage, heuristicTargets[0]);
    Check(heuristic.GetPrim().IsValid());
    pxr::VtArray<pxr::GfVec3f> extent;
    Check(heuristic.GetExtentAttr().Get(&extent));
    Check(extent.size() == 2 &&
          extent[0] == pxr::GfVec3f(0.0f, 1.0f, 2.0f) &&
          extent[1] == pxr::GfVec3f(4.0f, 5.0f, 6.0f));
}

void TestLodAuthoringRejectsMismatchedMetadata() {
    const auto stage = usdgeo::PointCloudLayer::CreateStage();
    usdgeo::GeoReference reference;
    reference.epsgCode = 26910;

    usdpointcloud::PointCloudAsset level;
    level.reference = reference;
    level.data.positions = {{0.0, 0.0, 0.0}};
    level.bounds.Expand({0.0, 0.0, 0.0});
    level.chunk = usdpointcloud::MakePointChunk(level.data, level.bounds);

    usdpointcloud::PointLodHierarchy hierarchy;
    hierarchy.bounds = level.bounds;
    hierarchy.items = {{0, 2, hierarchy.bounds, {0, 2}}};

    Check(!usdgeo::AuthorPointCloudLodAsset(
        stage, "/PointCloud", {level}, hierarchy));
    Check(!stage->GetPrimAtPath(pxr::SdfPath("/PointCloud")).IsValid());
}

void TestLodAuthoringRejectsNonRepresentableExtent() {
    const auto stage = usdgeo::PointCloudLayer::CreateStage();
    usdgeo::GeoReference reference;
    reference.epsgCode = 26910;

    std::vector<usdpointcloud::PointCloudAsset> levels(2);
    for (auto& level : levels) {
        level.reference = reference;
        level.data.positions = {{1.0e300, 2.0e300, 3.0e300}};
        level.bounds.Expand(level.data.positions.front());
        level.chunk = usdpointcloud::MakePointChunk(level.data, level.bounds);
    }
    usdpointcloud::PointLodHierarchy hierarchy;
    hierarchy.bounds = levels[0].bounds;
    hierarchy.items = {{0, 1, hierarchy.bounds, {0, 1}},
                       {1, 1, hierarchy.bounds, {0, 1}}};
    hierarchy.screenSizeThresholds = {0.1f};

    Check(!usdgeo::AuthorPointCloudLodAsset(
        stage, "/PointCloud", levels, hierarchy));
    Check(!stage->GetPrimAtPath(pxr::SdfPath("/PointCloud")).IsValid());
}

void TestLodAuthoringRejectsInvalidLevelWithoutMutation() {
    const auto stage = usdgeo::PointCloudLayer::CreateStage();
    usdgeo::GeoReference reference;
    reference.epsgCode = 26910;

    usdpointcloud::PointCloudAsset validLevel;
    validLevel.reference = reference;
    validLevel.data.positions = {{0.0, 0.0, 0.0}};
    validLevel.bounds.Expand({0.0, 0.0, 0.0});
    validLevel.chunk = usdpointcloud::MakePointChunk(
        validLevel.data, validLevel.bounds);
    auto invalidLevel = validLevel;
    invalidLevel.reference.epsgCode.reset();

    usdpointcloud::PointLodHierarchy hierarchy;
    hierarchy.bounds = validLevel.bounds;
    hierarchy.items = {{0, 1, hierarchy.bounds, {0, 1}},
                        {1, 1, hierarchy.bounds, {0, 1}}};

    Check(!usdgeo::AuthorPointCloudLodAsset(
        stage, "/PointCloud", {validLevel, invalidLevel}, hierarchy));
    Check(!stage->GetPrimAtPath(pxr::SdfPath("/PointCloud")).IsValid());
}

void TestTiledLodAuthoring() {
    const auto stage = usdgeo::PointCloudLayer::CreateStage();
    usdgeo::GeoReference reference;
    reference.epsgCode = 26910;

    const auto makeTile = [&](const usdpointcloud::PointTileId& id,
                              double x) {
        usdgeo::PointCloudTileAsset result;
        result.tile.id = id;
        result.tile.bounds.Expand({x, 0.0, 0.0});
        result.tile.lod.bounds = result.tile.bounds;
        result.tile.lod.screenSizeThresholds = {0.1F};
        result.tile.lod.items = {
            {0, 1, result.tile.bounds, {0, 1}},
            {1, 1, result.tile.bounds, {0, 1}}};
        for (int index = 0; index < 2; ++index) {
            usdpointcloud::PointCloudAsset level;
            level.reference = reference;
            level.bounds = result.tile.bounds;
            level.data.positions = {{x, 0.0, 0.0}};
            level.chunk = usdpointcloud::MakePointChunk(
                level.data, level.bounds);
            result.levels.push_back(std::move(level));
        }
        return result;
    };

    Check(usdgeo::AuthorPointCloudTiledAsset(
        stage, "/PointCloud",
        {makeTile({0, 0, 0, 0}, 0.0), makeTile({0, 1, 0, 0}, 10.0)}));
    Check(stage->GetPrimAtPath(pxr::SdfPath(
              "/PointCloud/Tiles/Tile_L0_p0_p0_p0"))
              .HasAPI<pxr::UsdLodRootAPI>());
    Check(stage->GetPrimAtPath(pxr::SdfPath(
              "/PointCloud/Tiles/Tile_L0_p1_p0_p0/LOD1"))
              .IsValid());
    const auto firstTile = stage->GetPrimAtPath(pxr::SdfPath(
        "/PointCloud/Tiles/Tile_L0_p0_p0_p0"));
    const auto secondTile = stage->GetPrimAtPath(pxr::SdfPath(
        "/PointCloud/Tiles/Tile_L0_p1_p0_p0"));
    pxr::SdfPathVector firstTargets;
    pxr::SdfPathVector secondTargets;
    Check(pxr::UsdLodRootAPI(firstTile)
              .GetLodHeuristicsRel()
              .GetTargets(&firstTargets));
    Check(pxr::UsdLodRootAPI(secondTile)
              .GetLodHeuristicsRel()
              .GetTargets(&secondTargets));
    Check(firstTargets.size() == 1 && secondTargets.size() == 1 &&
          firstTargets[0] != secondTargets[0]);
    const auto firstHeuristic = pxr::UsdLodScreenSizeHeuristic::Get(
        stage, firstTargets[0]);
    const auto secondHeuristic = pxr::UsdLodScreenSizeHeuristic::Get(
        stage, secondTargets[0]);
    Check(firstHeuristic.GetPrim().IsValid() &&
          secondHeuristic.GetPrim().IsValid());
    pxr::VtArray<pxr::GfVec3f> firstExtent;
    pxr::VtArray<pxr::GfVec3f> secondExtent;
    Check(firstHeuristic.GetExtentAttr().Get(&firstExtent));
    Check(secondHeuristic.GetExtentAttr().Get(&secondExtent));
    Check(firstExtent.size() == 2 && secondExtent.size() == 2 &&
          firstExtent != secondExtent);
}

void TestTiledLodAuthoringSupportsNegativeTileCoordinates() {
    const auto stage = usdgeo::PointCloudLayer::CreateStage();
    usdgeo::GeoReference reference;
    reference.epsgCode = 26910;
    usdgeo::PointCloudTileAsset tile;
    tile.tile.id = {0, std::numeric_limits<std::int64_t>::min(), -2, 3};
    tile.tile.bounds.Expand({0.0, 0.0, 0.0});
    tile.tile.lod.bounds = tile.tile.bounds;
    tile.tile.lod.items = {{0, 1, tile.tile.bounds, {0, 1}}};
    usdpointcloud::PointCloudAsset level;
    level.reference = reference;
    level.bounds = tile.tile.bounds;
    level.data.positions = {{0.0, 0.0, 0.0}};
    level.chunk = usdpointcloud::MakePointChunk(level.data, level.bounds);
    tile.levels.push_back(std::move(level));

    Check(usdgeo::AuthorPointCloudTiledAsset(stage, "/PointCloud", {tile}));
    Check(stage->GetPrimAtPath(pxr::SdfPath(
              "/PointCloud/Tiles/Tile_L0_n9223372036854775808_n2_p3"))
              .IsValid());
}

void TestTiledLodAuthoringSupportsMismatchedThresholds() {
    const auto stage = usdgeo::PointCloudLayer::CreateStage();
    usdgeo::GeoReference reference;
    reference.epsgCode = 26910;
    usdgeo::PointCloudTileAsset tile;
    tile.tile.id = {0, 0, 0, 0};
    tile.tile.bounds.Expand({0.0, 0.0, 0.0});
    tile.tile.lod.bounds = tile.tile.bounds;
    tile.tile.lod.screenSizeThresholds = {0.2F};
    tile.tile.lod.items = {{0, 1, tile.tile.bounds, {0, 1}},
                           {1, 1, tile.tile.bounds, {0, 1}}};
    usdpointcloud::PointCloudAsset level;
    level.reference = reference;
    level.bounds = tile.tile.bounds;
    level.data.positions = {{0.0, 0.0, 0.0}};
    level.chunk = usdpointcloud::MakePointChunk(level.data, level.bounds);
    tile.levels.push_back(level);
    tile.levels.push_back(level);
    auto secondTile = tile;
    secondTile.tile.id.x = 1;
    secondTile.tile.lod.screenSizeThresholds = {0.1F};

    const auto authored = usdgeo::AuthorPointCloudTiledAsset(
        stage, "/PointCloud", {tile, secondTile});
    Check(authored);
    Check(stage->GetPrimAtPath(pxr::SdfPath(
              "/PointCloud/LodHeuristics/Tile_L0_p0_p0_p0/ScreenSize"))
              .IsValid());
    Check(stage->GetPrimAtPath(pxr::SdfPath(
              "/PointCloud/LodHeuristics/Tile_L0_p1_p0_p0/ScreenSize"))
              .IsValid());
}

void TestTiledLodPayloadAuthoring() {
    const auto stage = usdgeo::PointCloudLayer::CreateStage();
    usdgeo::GeoReference reference;
    reference.epsgCode = 26910;

    usdgeo::PointCloudTileAsset tile;
    tile.tile.id = {0, 0, 0, 0};
    tile.tile.bounds.Expand({0.0, 0.0, 0.0});
    tile.tile.lod.bounds = tile.tile.bounds;
    tile.tile.lod.items = {{0, 1, tile.tile.bounds, {0, 1}}};

    usdpointcloud::PointCloudAsset level;
    level.reference = reference;
    level.bounds = tile.tile.bounds;
    level.data.positions = {{0.0, 0.0, 0.0}};
    level.chunk = usdpointcloud::MakePointChunk(level.data, level.bounds);
    tile.levels.push_back(std::move(level));

    const auto payloadDirectory =
        std::filesystem::temp_directory_path() / "usd_geo_payloads";
    const auto rootLayerPath = payloadDirectory / "PointCloud.usda";
    std::filesystem::remove_all(payloadDirectory);
    const auto originalIdentifier = stage->GetRootLayer()->GetIdentifier();
    Check(usdgeo::AuthorPointCloudTiledAssetWithPayloads(
        stage, "/PointCloud", {tile},
        {payloadDirectory.string(), rootLayerPath.string()}));
    Check(stage->GetRootLayer()->GetIdentifier() == originalIdentifier);

    const auto lodPrim = stage->GetPrimAtPath(pxr::SdfPath(
        "/PointCloud/Tiles/Tile_L0_p0_p0_p0/LOD0"));
    Check(lodPrim.IsValid());
    Check(std::filesystem::exists(
        payloadDirectory / "Tile_L0_p0_p0_p0_LOD0.usdc"));
    stage->GetRootLayer()->SetIdentifier(rootLayerPath.generic_string());
    Check(stage->GetRootLayer()->Export(rootLayerPath.string()));
    const auto reopenedStage = pxr::UsdStage::Open(rootLayerPath.string());
    Check(reopenedStage);
    const auto points = pxr::UsdGeomPoints::Get(
        reopenedStage,
        pxr::SdfPath("/PointCloud/Tiles/Tile_L0_p0_p0_p0/LOD0/Points"));
    Check(points.GetPrim().IsValid());
    pxr::VtVec3fArray positions;
    Check(points.GetPointsAttr().Get(&positions));
    Check(positions.size() == 1 && positions[0] == pxr::GfVec3f(0.0f));
    std::filesystem::remove_all(payloadDirectory);

    const auto failedStage = usdgeo::PointCloudLayer::CreateStage();
    const auto failedIdentifier = failedStage->GetRootLayer()->GetIdentifier();
    std::filesystem::create_directories(payloadDirectory);
    {
        std::ofstream conflict(
            payloadDirectory / "Tile_L0_p0_p0_p0_LOD0.usdc",
            std::ios::binary);
        conflict << "pre-existing payload";
    }
    Check(!usdgeo::AuthorPointCloudTiledAssetWithPayloads(
        failedStage, "/PointCloud", {tile},
        {payloadDirectory.string(), rootLayerPath.string()}));
    Check(failedStage->GetRootLayer()->GetIdentifier() == failedIdentifier);
    std::filesystem::remove_all(payloadDirectory);

    const auto invalidPayloadDirectory =
        std::filesystem::temp_directory_path() / "usd_geo_invalid_payloads";
    const auto invalidRootLayerPath = invalidPayloadDirectory / "PointCloud.usda";
    std::filesystem::remove_all(invalidPayloadDirectory);
    auto invalidTile = tile;
    invalidTile.tile.lod.items.clear();
    const auto invalidStage = usdgeo::PointCloudLayer::CreateStage();
    Check(!usdgeo::AuthorPointCloudTiledAssetWithPayloads(
        invalidStage, "/PointCloud", {invalidTile},
        {invalidPayloadDirectory.string(), invalidRootLayerPath.string()}));
    Check(!std::filesystem::exists(invalidPayloadDirectory));
}

void TestStreamTiledPayloadAuthoring() {
    const auto layer = pxr::SdfLayer::CreateAnonymous("streamed.usda");
    usdgeo::GeoReference reference;
    reference.epsgCode = 26910;
    reference.localOrigin = {0.0, 0.0, 0.0};

    const auto makeChunk = [](const std::vector<usdgeo::Vec3d>& positions,
                              const std::vector<std::uint16_t>& intensity,
                              const std::vector<double>& temperatures) {
        usdpointcloud::PointData data;
        data.positions = positions;
        data.intensity = intensity;
        data.extraByteNames = {"temperature (C)"};
        data.extraByteComponentCounts = {1};
        data.extraBytes = {temperatures};
        usdgeo::SpatialBounds bounds = usdgeo::SpatialBounds::Empty();
        for (const auto& position : positions) bounds.Expand(position);
        return std::make_pair(usdpointcloud::MakePointChunk(data, bounds),
                              std::move(data));
    };
    TestPointStream stream({
        makeChunk({{0.25, 0.0, 0.0}, {1.25, 0.0, 0.0}}, {10, 20},
                  {12.5, 18.0}),
        makeChunk({{-0.25, 0.0, 0.0}}, {30}, {7.0}),
    });

    const auto payloadDirectory =
        std::filesystem::temp_directory_path() / "usd_geo_stream_payloads";
    const auto rootLayerPath = payloadDirectory / "PointCloud.usda";
    std::filesystem::remove_all(payloadDirectory);
    std::vector<usdgeo::Diagnostic> diagnostics;
    Check(usdgeo::AuthorPointCloudTiledAssetFromStream(
        layer.operator->(), "/PointCloud", stream, reference, {1.0, 0},
        {payloadDirectory.string(), rootLayerPath.string(), 1}, diagnostics));
    Check(diagnostics.empty());
    Check(layer->Export(rootLayerPath.string()));

    const auto stage = pxr::UsdStage::Open(rootLayerPath.string());
    Check(stage);
    const auto points = pxr::UsdGeomPoints::Get(
        stage, pxr::SdfPath(
            "/PointCloud/Tiles/Tile_L0_p1_p0_p0/LOD0/Points"));
    Check(points.GetPrim().IsValid());
    pxr::VtIntArray authoredIntensity;
    Check(points.GetPrim()
              .GetAttribute(pxr::TfToken("geo:intensity"))
              .Get(&authoredIntensity));
    Check(authoredIntensity.size() == 1 && authoredIntensity[0] == 20);
    pxr::VtArray<double> authoredTemperature;
    Check(points.GetPrim()
              .GetAttribute(pxr::TfToken("geo:temperature__C_"))
              .Get(&authoredTemperature));
    Check(authoredTemperature.size() == 1 && authoredTemperature[0] == 18.0);
    Check(std::filesystem::exists(
        payloadDirectory / "Tile_L0_n1_p0_p0_LOD0.usdc"));
    Check(std::filesystem::exists(
        payloadDirectory / "Tile_L0_p0_p0_p0_LOD0.usdc"));
    std::filesystem::remove_all(payloadDirectory);
}

void TestFixedGridTilingPreservesBoundariesAndMemoryLimit() {
    const auto layer = pxr::SdfLayer::CreateAnonymous("fixed_grid_stream.usda");
    usdgeo::GeoReference reference;
    reference.epsgCode = 26910;

    usdpointcloud::PointData data;
    data.positions = {{0.25, 0.25, 0.0},
                      {1.0, 0.25, 0.0},
                      {0.25, 1.0, 0.0},
                      {-0.25, -0.25, 0.0}};
    data.intensity = {10, 20, 30, 40};
    usdgeo::SpatialBounds bounds = usdgeo::SpatialBounds::Empty();
    for (const auto& position : data.positions) bounds.Expand(position);
    TestPointStream stream({{
        usdpointcloud::MakePointChunk(data, bounds), data}});

    const auto payloadDirectory =
        std::filesystem::temp_directory_path() / "usd_geo_fixed_grid_payloads";
    const auto rootLayerPath = payloadDirectory / "PointCloud.usda";
    std::filesystem::remove_all(payloadDirectory);
    std::size_t peakBufferedBytes = 0;
    std::vector<usdgeo::Diagnostic> diagnostics;
    Check(usdgeo::AuthorPointCloudTiledAssetFromStream(
        layer.operator->(), "/PointCloud", stream, reference, {1.0, 0},
        {payloadDirectory.string(), rootLayerPath.string(), 1, {},
         [&peakBufferedBytes](std::size_t bufferedBytes) {
             peakBufferedBytes = std::max(peakBufferedBytes, bufferedBytes);
         }},
        diagnostics));
    Check(diagnostics.empty());
    Check(peakBufferedBytes <= 1);

    const std::array<std::string, 4> tileNames = {
        "Tile_L0_p0_p0_p0", "Tile_L0_p1_p0_p0", "Tile_L0_p0_p1_p0",
        "Tile_L0_n1_n1_p0"};
    for (const auto& tileName : tileNames) {
        Check(layer->GetPrimAtPath(pxr::SdfPath(
                  "/PointCloud/Tiles/" + tileName)) != nullptr);
        Check(std::filesystem::exists(
            payloadDirectory / (tileName + "_LOD0.usdc")));
    }
    std::filesystem::remove_all(payloadDirectory);
}

void TestAdaptiveTilingRouterAuthorsPlannedLeaves() {
    const auto layer = pxr::SdfLayer::CreateAnonymous("adaptive_stream.usda");
    usdgeo::GeoReference reference;
    reference.epsgCode = 26910;

    usdpointcloud::PointData data;
    data.positions = {{0.0, 0.0, 0.0}, {1.0, 0.0, 0.0},
                      {0.0, 1.0, 0.0}, {1.0, 1.0, 0.0}};
    data.intensity = {10, 20, 30, 40};
    usdgeo::SpatialBounds bounds = usdgeo::SpatialBounds::Empty();
    for (const auto& position : data.positions) bounds.Expand(position);

    usdpointcloud::PointBudgetPlan plan;
    std::vector<usdgeo::Diagnostic> diagnostics;
    Check(usdpointcloud::BuildPointBudgetPlan(
        data.positions, {2, 1, 1}, plan, diagnostics));
    Check(plan.tiles.size() == 4);
    usdpointcloud::PointBudgetTileRouter router(plan);
    TestPointStream stream({{
        usdpointcloud::MakePointChunk(data, bounds), data}});

    const auto payloadDirectory =
        std::filesystem::temp_directory_path() / "usd_geo_adaptive_payloads";
    std::filesystem::remove_all(payloadDirectory);
    Check(usdgeo::AuthorPointCloudTiledAssetFromStream(
        layer.operator->(), "/PointCloud", stream, reference, router,
        {payloadDirectory.string(),
         (payloadDirectory / "PointCloud.usda").string(), 1}, diagnostics));
    Check(diagnostics.empty());
    for (const auto& tileName : {"Tile_L1_p0_p0_p0", "Tile_L1_p1_p0_p0",
                                 "Tile_L1_p0_p1_p0", "Tile_L1_p1_p1_p0"}) {
        Check(layer->GetPrimAtPath(pxr::SdfPath(
                  "/PointCloud/Tiles/" + std::string(tileName))) != nullptr);
    }
    std::filesystem::remove_all(payloadDirectory);
}

void TestSequentialAndCopcPlansAuthorEquivalentOutput() {
    const std::vector<usdgeo::Vec3d> positions = {
        {0.25, 0.25, 0.0}, {0.75, 0.25, 0.0},
        {0.25, 0.75, 0.0}, {0.75, 0.75, 0.0}};
    usdpointcloud::PointData data;
    data.positions = positions;
    data.intensity = {10, 20, 30, 40};
    usdgeo::SpatialBounds bounds = usdgeo::SpatialBounds::Empty();
    for (const auto& position : positions) bounds.Expand(position);

    const usdpointcloud::PointBudgetConfig config{1, 1, 1};
    usdpointcloud::PointBudgetPlan budgetPlan;
    std::vector<usdgeo::Diagnostic> diagnostics;
    Check(usdpointcloud::BuildPointBudgetPlan(
        positions, config, budgetPlan, diagnostics));
    usdpointcloud::TilePlan sequentialPlan;
    Check(usdpointcloud::BuildTilePlan(
        budgetPlan, sequentialPlan, diagnostics));

    usdcopc::CopcHierarchy hierarchy;
    hierarchy.bounds = sequentialPlan.nodes.front().bounds;
    hierarchy.spacing = 1.0;
    hierarchy.nodes.reserve(sequentialPlan.nodes.size());
    for (std::size_t index = 0; index < sequentialPlan.nodes.size(); ++index) {
        const auto& planNode = sequentialPlan.nodes[index];
        usdcopc::CopcNode node;
        node.tile = planNode.id;
        node.bounds = planNode.bounds;
        node.spacing = 1.0;
        if (planNode.isLeaf) {
            node.pointCount = planNode.pointCount;
            node.pointDataOffset = index + 1;
            node.pointDataSize = 1;
            node.hasPointData = true;
        } else {
            node.hasEmptyNode = true;
        }
        hierarchy.nodes.push_back(std::move(node));
    }
    usdpointcloud::TilePlan copcPlan;
    Check(usdcopc::BuildTilePlan(hierarchy, copcPlan, diagnostics));
    Check(diagnostics.empty() && copcPlan.IsValid());

    const auto sequentialDirectory =
        std::filesystem::temp_directory_path() /
        "usd_geo_sequential_plan_equivalence";
    const auto copcDirectory =
        std::filesystem::temp_directory_path() /
        "usd_geo_copc_plan_equivalence";
    std::filesystem::remove_all(sequentialDirectory);
    std::filesystem::remove_all(copcDirectory);
    const auto sequentialLayer = pxr::SdfLayer::CreateAnonymous(
        "sequential_plan_equivalence.usda");
    const auto copcLayer = pxr::SdfLayer::CreateAnonymous(
        "copc_plan_equivalence.usda");
    usdgeo::GeoReference reference;
    reference.epsgCode = 26910;
    std::vector<usdpointcloud::PointTileManifestEntry> sequentialManifest;
    std::vector<usdpointcloud::PointTileManifestEntry> copcManifest;
    const usdpointcloud::PointBudgetTileRouter sequentialRouter(sequentialPlan);
    const usdpointcloud::PointBudgetTileRouter copcRouter(copcPlan);
    TestPointStream sequentialStream({{
        usdpointcloud::MakePointChunk(data, bounds), data}});
    TestPointStream copcStream({{
        usdpointcloud::MakePointChunk(data, bounds), data}});
    Check(usdgeo::AuthorPointCloudTiledAssetFromStream(
        sequentialLayer.operator->(), "/PointCloud",
        sequentialStream,
        reference, sequentialRouter,
        {sequentialDirectory.string(),
         (sequentialDirectory / "PointCloud.usda").string(), 1, {}, {},
         &sequentialManifest},
        diagnostics));
    Check(usdgeo::AuthorPointCloudTiledAssetFromStream(
        copcLayer.operator->(), "/PointCloud",
        copcStream,
        reference, copcRouter,
        {copcDirectory.string(),
         (copcDirectory / "PointCloud.usda").string(), 1, {}, {},
         &copcManifest},
        diagnostics));
    Check(sequentialLayer->Export(
        (sequentialDirectory / "PointCloud.usda").string()));
    Check(copcLayer->Export((copcDirectory / "PointCloud.usda").string()));

    std::string sequentialManifestText;
    std::string copcManifestText;
    Check(usdpointcloud::SerializePointTileManifest(
        {sequentialManifest}, sequentialManifestText, diagnostics));
    Check(usdpointcloud::SerializePointTileManifest(
        {copcManifest}, copcManifestText, diagnostics));
    Check(sequentialManifestText == copcManifestText);
    Check(ReadDirectoryFiles(sequentialDirectory) ==
          ReadDirectoryFiles(copcDirectory));
    std::filesystem::remove_all(sequentialDirectory);
    std::filesystem::remove_all(copcDirectory);
}

void TestAdaptiveTilingRejectsPointsOutsidePlan() {
    const auto layer = pxr::SdfLayer::CreateAnonymous("adaptive_outside.usda");
    usdgeo::GeoReference reference;
    reference.epsgCode = 26910;

    std::vector<usdgeo::Vec3d> plannedPositions = {{0.0, 0.0, 0.0},
                                                    {1.0, 1.0, 0.0}};
    usdpointcloud::PointBudgetPlan plan;
    std::vector<usdgeo::Diagnostic> diagnostics;
    Check(usdpointcloud::BuildPointBudgetPlan(
        plannedPositions, {1, 1, 1}, plan, diagnostics));
    usdpointcloud::PointBudgetTileRouter router(plan);

    usdpointcloud::PointData data;
    data.positions = {{2.0, 2.0, 0.0}};
    usdgeo::SpatialBounds bounds = usdgeo::SpatialBounds::Empty();
    bounds.Expand(data.positions.front());
    TestPointStream stream({{
        usdpointcloud::MakePointChunk(data, bounds), data}});
    const auto payloadDirectory =
        std::filesystem::temp_directory_path() / "usd_geo_adaptive_outside";
    std::filesystem::remove_all(payloadDirectory);
    Check(!usdgeo::AuthorPointCloudTiledAssetFromStream(
        layer.operator->(), "/PointCloud", stream, reference, router,
        {payloadDirectory.string(),
         (payloadDirectory / "PointCloud.usda").string(), 1}, diagnostics));
    Check(!diagnostics.empty());
    Check(layer->GetPrimAtPath(pxr::SdfPath("/PointCloud")) == nullptr);
    Check(!std::filesystem::exists(payloadDirectory));
}

void TestGeneratedStreamTiledPayloadAuthoring() {
    const auto layer = pxr::SdfLayer::CreateAnonymous("generated_stream.usda");
    usdgeo::GeoReference reference;
    reference.epsgCode = 26910;

    const auto payloadDirectory =
        std::filesystem::temp_directory_path() / "usd_geo_generated_payloads";
    const auto rootLayerPath = payloadDirectory / "PointCloud.usda";
    const auto spoolDirectory = payloadDirectory / "spool-work";
    std::filesystem::remove_all(payloadDirectory);
    std::filesystem::create_directories(spoolDirectory);
    {
        std::ofstream marker(spoolDirectory / ".usdgeo-spool", std::ios::binary);
        marker << "USDGEO_SPOOL_WORKSPACE_V1\n";
        std::ofstream stale(spoolDirectory / "tile_stale.bin");
        stale << "stale";
    }
    GeneratedPointStream stream(131072, spoolDirectory);
    std::vector<usdgeo::Diagnostic> diagnostics;
    std::vector<usdpointcloud::PointTileManifestEntry> manifestEntries;
    usdgeo::PointCloudPayloadOptions options{
        payloadDirectory.string(), rootLayerPath.string(), 1024, {}, {},
        &manifestEntries};
    options.spoolDirectory = spoolDirectory.string();
    Check(usdgeo::AuthorPointCloudTiledAssetFromStream(
        layer.operator->(), "/PointCloud", stream, reference, {128.0, 0},
        options, diagnostics));
    Check(diagnostics.empty());
    Check(manifestEntries.size() == 32);
    Check(manifestEntries.front().payloadPath ==
          "Tile_L0_p0_p0_p0_LOD0.usdc");
    const auto lastTile = std::find_if(
        manifestEntries.begin(), manifestEntries.end(),
        [](const auto& entry) { return entry.id.x == 31; });
    Check(lastTile != manifestEntries.end());
    Check(lastTile->payloadPath == "Tile_L0_p31_p0_p0_LOD0.usdc");
    Check(manifestEntries.front().pointCount > 0);
    Check(manifestEntries.front().bounds.IsValid());
    Check(std::filesystem::exists(
        payloadDirectory / "Tile_L0_p0_p0_p0_LOD0.usdc"));
    Check(std::filesystem::exists(
        payloadDirectory / "Tile_L0_p31_p0_p0_LOD0.usdc"));
    Check(layer->GetPrimAtPath(pxr::SdfPath(
              "/PointCloud/Tiles/Tile_L0_p31_p0_p0")) != nullptr);
    Check(stream.SawSpoolFile());
    Check(!std::filesystem::exists(spoolDirectory));
    std::filesystem::remove_all(payloadDirectory);
}

void TestStreamCancellationCleansSpools() {
    const auto layer = pxr::SdfLayer::CreateAnonymous("cancelled_stream.usda");
    usdgeo::GeoReference reference;
    reference.epsgCode = 26910;

    usdpointcloud::PointData data;
    data.positions = {{0.25, 0.0, 0.0}, {0.75, 0.0, 0.0}};
    usdgeo::SpatialBounds bounds = usdgeo::SpatialBounds::Empty();
    for (const auto& position : data.positions) bounds.Expand(position);
    TestPointStream stream({{
        usdpointcloud::MakePointChunk(data, bounds), data}});

    const auto payloadDirectory =
        std::filesystem::temp_directory_path() / "usd_geo_cancelled_payloads";
    std::filesystem::remove_all(payloadDirectory);
    const auto spoolDirectory = payloadDirectory / "spool-work";
    int cancellationChecks = 0;
    std::vector<usdgeo::Diagnostic> diagnostics;
    usdgeo::PointCloudPayloadOptions options{
        payloadDirectory.string(),
        (payloadDirectory / "PointCloud.usda").string(),
        1,
        [&cancellationChecks]() { return ++cancellationChecks >= 3; }};
    options.spoolDirectory = spoolDirectory.string();

    Check(!usdgeo::AuthorPointCloudTiledAssetFromStream(
        layer.operator->(), "/PointCloud", stream, reference, {1.0, 0},
        options, diagnostics));
    Check(!diagnostics.empty());
    Check(!std::filesystem::exists(spoolDirectory));
    Check(!std::filesystem::exists(payloadDirectory));
}

void TestStreamCancellationDuringSpoolReadCleansSpools() {
    const auto layer = pxr::SdfLayer::CreateAnonymous(
        "cancelled_spool_read.usda");
    usdgeo::GeoReference reference;
    reference.epsgCode = 26910;

    usdpointcloud::PointData data;
    data.positions = {{0.25, 0.0, 0.0}};
    usdgeo::SpatialBounds bounds = usdgeo::SpatialBounds::Empty();
    bounds.Expand(data.positions.front());
    TestPointStream stream({{
        usdpointcloud::MakePointChunk(data, bounds), data}});

    const auto payloadDirectory =
        std::filesystem::temp_directory_path() /
        "usd_geo_cancelled_spool_read_payloads";
    std::filesystem::remove_all(payloadDirectory);
    const auto spoolDirectory = payloadDirectory / "spool-work";
    int cancellationChecks = 0;
    std::vector<usdgeo::Diagnostic> diagnostics;
    usdgeo::PointCloudPayloadOptions options{
        payloadDirectory.string(),
        (payloadDirectory / "PointCloud.usda").string(),
        1,
        [&cancellationChecks]() { return ++cancellationChecks >= 6; }};
    options.spoolDirectory = spoolDirectory.string();

    Check(!usdgeo::AuthorPointCloudTiledAssetFromStream(
        layer.operator->(), "/PointCloud", stream, reference, {1.0, 0},
        options, diagnostics));
    Check(!diagnostics.empty());
    Check(!std::filesystem::exists(spoolDirectory));
    Check(!std::filesystem::exists(payloadDirectory));
}

void TestStreamFailureDoesNotMutateLayer() {
    const auto layer = pxr::SdfLayer::CreateAnonymous("failed_stream.usda");
    usdgeo::GeoReference reference;
    reference.epsgCode = 26910;
    FailingPointStream stream;
    const auto payloadDirectory =
        std::filesystem::temp_directory_path() / "usd_geo_failed_payloads";
    std::filesystem::remove_all(payloadDirectory);
    std::vector<usdgeo::Diagnostic> diagnostics;

    Check(!usdgeo::AuthorPointCloudTiledAssetFromStream(
        layer.operator->(), "/PointCloud", stream, reference, {1.0, 0},
        {payloadDirectory.string(),
         (payloadDirectory / "PointCloud.usda").string(), 1}, diagnostics));
    Check(!diagnostics.empty());
    Check(layer->GetPrimAtPath(pxr::SdfPath("/PointCloud")) == nullptr);
    Check(!std::filesystem::exists(payloadDirectory));
}

void TestStreamPayloadFailureRollsBackGeneratedPayloads() {
    const auto layer = pxr::SdfLayer::CreateAnonymous("payload_failure.usda");
    usdgeo::GeoReference reference;
    reference.epsgCode = 26910;

    const auto makeChunk = [](double x) {
        usdpointcloud::PointData data;
        data.positions = {{x, 0.0, 0.0}};
        usdgeo::SpatialBounds bounds;
        bounds.Expand(data.positions.front());
        return std::make_pair(usdpointcloud::MakePointChunk(data, bounds),
                              std::move(data));
    };
    TestPointStream stream({makeChunk(0.0), makeChunk(1.0)});
    const auto payloadDirectory =
        std::filesystem::temp_directory_path() / "usd_geo_payload_failure";
    const auto firstPayload =
        payloadDirectory / "Tile_L0_p0_p0_p0_LOD0.usdc";
    const auto conflictingPayload =
        payloadDirectory / "Tile_L0_p1_p0_p0_LOD0.usdc";
    std::filesystem::remove_all(payloadDirectory);
    std::filesystem::create_directories(payloadDirectory);
    {
        std::ofstream conflict(conflictingPayload, std::ios::binary);
        conflict << "pre-existing payload";
    }

    std::vector<usdgeo::Diagnostic> diagnostics;
    Check(!usdgeo::AuthorPointCloudTiledAssetFromStream(
        layer.operator->(), "/PointCloud", stream, reference, {1.0, 0},
        {payloadDirectory.string(),
         (payloadDirectory / "PointCloud.usda").string(), 1}, diagnostics));
    Check(!diagnostics.empty());
    Check(layer->GetPrimAtPath(pxr::SdfPath("/PointCloud")) == nullptr);
    Check(!std::filesystem::exists(firstPayload));
    Check(std::filesystem::exists(conflictingPayload));
    std::filesystem::remove_all(payloadDirectory);
}

std::pair<usdpointcloud::PointChunk, usdpointcloud::PointData> MakeTestChunk(
    const std::vector<usdgeo::Vec3d>& positions) {
    usdpointcloud::PointData data;
    data.positions = positions;
    usdgeo::SpatialBounds bounds = usdgeo::SpatialBounds::Empty();
    for (const auto& position : positions) bounds.Expand(position);
    return {usdpointcloud::MakePointChunk(data, bounds), std::move(data)};
}

// Streams `positions` into unit fixed-grid tiles as `owner`'s payloads.
bool AuthorOwnedStream(pxr::SdfLayer* layer,
                       const std::filesystem::path& directory,
                       const std::string& owner,
                       const std::vector<usdgeo::Vec3d>& positions,
                       std::vector<usdgeo::Diagnostic>& diagnostics,
                       std::function<bool()> isCancelled = {}) {
    usdgeo::GeoReference reference;
    reference.epsgCode = 26910;
    TestPointStream stream({MakeTestChunk(positions)});
    usdgeo::PointCloudPayloadOptions options{
        directory.string(), (directory / "PointCloud.usda").string(), 1,
        std::move(isCancelled)};
    options.owner = owner;
    return usdgeo::AuthorPointCloudTiledAssetFromStream(
        layer, "/PointCloud", stream, reference, {1.0, 0}, options,
        diagnostics);
}

void TestOwnedPayloadsLiveInTheirOwnersGeneration() {
    const auto directory = UniqueTestDirectory("usd_geo_owned_payloads");
    const auto ownerDirectory = OwnerDirectory(directory, "layer-a");
    std::vector<usdgeo::Diagnostic> diagnostics;

    const auto layer = pxr::SdfLayer::CreateAnonymous("owned.usda");
    Check(AuthorOwnedStream(layer.operator->(), directory, "layer-a",
                            {{0.25, 0.0, 0.0}, {1.25, 0.0, 0.0}},
                            diagnostics));
    Check(diagnostics.empty());
    const auto generation = OnlyGeneration(ownerDirectory);
    const auto first = generation / "Tile_L0_p0_p0_p0_LOD0.usdc";
    Check(std::filesystem::exists(first) &&
          std::filesystem::exists(generation / "Tile_L0_p1_p0_p0_LOD0.usdc"));
    Check(Subdirectories(ownerDirectory, ".tmp-").empty());
    const auto lod = layer->GetPrimAtPath(
        pxr::SdfPath("/PointCloud/Tiles/Tile_L0_p0_p0_p0/LOD0"));
    Check(lod != nullptr);
    const auto arcs = lod->GetPayloadList().GetPrependedItems();
    Check(arcs.size() == 1 &&
          arcs.front().GetAssetPath() ==
              ownerDirectory.filename().generic_string() + "/" +
                  generation.filename().generic_string() +
                  "/Tile_L0_p0_p0_p0_LOD0.usdc");

    // Reading the layer again with identical content keeps the published
    // generation as it is instead of replacing files that may be open.
    const auto firstWrite = std::filesystem::last_write_time(first);
    const auto again = pxr::SdfLayer::CreateAnonymous("again.usda");
    Check(AuthorOwnedStream(again.operator->(), directory, "layer-a",
                            {{0.25, 0.0, 0.0}, {1.25, 0.0, 0.0}},
                            diagnostics));
    Check(OnlyGeneration(ownerDirectory) == generation);
    Check(std::filesystem::last_write_time(first) == firstWrite);

    // Changed content is published as a new generation, and the superseded
    // one is removed.
    const auto changed = pxr::SdfLayer::CreateAnonymous("changed.usda");
    Check(AuthorOwnedStream(changed.operator->(), directory, "layer-a",
                            {{0.25, 0.0, 0.0}, {0.75, 0.0, 0.0}},
                            diagnostics));
    const auto changedGeneration = OnlyGeneration(ownerDirectory);
    Check(changedGeneration != generation);
    Check(!std::filesystem::exists(generation));
    Check(!std::filesystem::exists(
        changedGeneration / "Tile_L0_p1_p0_p0_LOD0.usdc"));
    {
        const auto rootLayerPath = directory / "PointCloud.usda";
        Check(changed->Export(rootLayerPath.string()));
        const auto stage = pxr::UsdStage::Open(rootLayerPath.string());
        Check(stage);
        const auto points = pxr::UsdGeomPoints::Get(
            stage,
            pxr::SdfPath("/PointCloud/Tiles/Tile_L0_p0_p0_p0/LOD0/Points"));
        pxr::VtVec3fArray positions;
        Check(points.GetPointsAttr().Get(&positions));
        Check(positions.size() == 2);
        std::filesystem::remove(rootLayerPath);
    }

    // Other layers and user files in the same payload directory never meet
    // this owner's payloads: each owner has its own directory.
    const auto published =
        ReadFile(changedGeneration / "Tile_L0_p0_p0_p0_LOD0.usdc");
    WriteFile(directory / "Tile_L0_p0_p0_p0_LOD0.usdc", "user file");
    const auto other = pxr::SdfLayer::CreateAnonymous("other.usda");
    Check(AuthorOwnedStream(other.operator->(), directory, "layer-b",
                            {{0.25, 0.0, 0.0}}, diagnostics));
    Check(OnlyGeneration(OwnerDirectory(directory, "layer-b")) !=
          changedGeneration);
    Check(ReadFile(changedGeneration / "Tile_L0_p0_p0_p0_LOD0.usdc") ==
          published);
    Check(ReadFile(directory / "Tile_L0_p0_p0_p0_LOD0.usdc") == "user file");

    // Without an owner the directory stays exclusive.
    Check(!AuthorOwnedStream(other.operator->(), directory, "",
                             {{0.25, 0.0, 0.0}}, diagnostics));
    Check(ReadFile(directory / "Tile_L0_p0_p0_p0_LOD0.usdc") == "user file");
    std::filesystem::remove_all(directory);
}

void TestInterruptedOwnedGenerationIsRecovered() {
    const auto directory =
        UniqueTestDirectory("usd_geo_interrupted_owned_payloads");
    const auto ownerDirectory = OwnerDirectory(directory, "layer-a");
    // What interrupted generations leave behind: staging directories that
    // were never published. A live generation keeps its own fresh.
    const auto stale = ownerDirectory / ".tmp-stale";
    const auto live = ownerDirectory / ".tmp-live";
    std::filesystem::create_directories(stale);
    std::filesystem::create_directories(live);
    WriteFile(stale / "Tile_L0_p0_p0_p0_LOD0.usdc", "partial payload");
    std::filesystem::last_write_time(
        stale, std::filesystem::file_time_type::clock::now() -
                   std::chrono::hours(2));

    std::vector<usdgeo::Diagnostic> diagnostics;
    const auto layer = pxr::SdfLayer::CreateAnonymous("recovered.usda");
    Check(AuthorOwnedStream(layer.operator->(), directory, "layer-a",
                            {{0.25, 0.0, 0.0}}, diagnostics));
    Check(diagnostics.empty());
    Check(!std::filesystem::exists(stale));
    Check(std::filesystem::exists(live));

    // A published generation that lost a file is replaced when the same
    // content is published again, rather than trusted.
    const auto generation = OnlyGeneration(ownerDirectory);
    const auto payload = generation / "Tile_L0_p0_p0_p0_LOD0.usdc";
    std::filesystem::remove(payload);
    Check(AuthorOwnedStream(layer.operator->(), directory, "layer-a",
                            {{0.25, 0.0, 0.0}}, diagnostics));
    Check(OnlyGeneration(ownerDirectory) == generation);
    Check(std::filesystem::exists(payload));
    std::filesystem::remove_all(directory);
}

void TestFailedOwnedGenerationKeepsPublishedPayloads() {
    const auto directory =
        UniqueTestDirectory("usd_geo_failed_owned_payloads");
    const auto ownerDirectory = OwnerDirectory(directory, "layer-a");
    std::vector<usdgeo::Diagnostic> diagnostics;

    const auto layer = pxr::SdfLayer::CreateAnonymous("failed_owned.usda");
    Check(AuthorOwnedStream(layer.operator->(), directory, "layer-a",
                            {{0.25, 0.0, 0.0}, {1.25, 0.0, 0.0}},
                            diagnostics));
    const auto generation = OnlyGeneration(ownerDirectory);
    const auto published = ReadDirectoryFiles(generation);

    // Cancel as soon as the regeneration has staged its first payload.
    const auto stagedFirst = [&ownerDirectory]() {
        for (const auto& staging : Subdirectories(ownerDirectory, ".tmp-")) {
            if (std::filesystem::exists(staging /
                                        "Tile_L0_p0_p0_p0_LOD0.usdc")) {
                return true;
            }
        }
        return false;
    };
    const auto regenerated = pxr::SdfLayer::CreateAnonymous("cancelled.usda");
    Check(!AuthorOwnedStream(regenerated.operator->(), directory, "layer-a",
                             {{0.5, 0.0, 0.0}, {1.25, 0.0, 0.0}},
                             diagnostics, stagedFirst));
    Check(!diagnostics.empty());
    Check(regenerated->GetPrimAtPath(pxr::SdfPath("/PointCloud")) == nullptr);
    Check(OnlyGeneration(ownerDirectory) == generation);
    Check(ReadDirectoryFiles(generation) == published);
    Check(Subdirectories(ownerDirectory, ".tmp-").empty());
    std::filesystem::remove_all(directory);

    // A cancelled first generation removes every directory it created.
    const auto created = UniqueTestDirectory("usd_geo_cancelled_owned_payloads");
    const auto createdOwner = OwnerDirectory(created, "layer-a");
    const auto fresh = pxr::SdfLayer::CreateAnonymous("fresh.usda");
    Check(!AuthorOwnedStream(
        fresh.operator->(), created, "layer-a",
        {{0.25, 0.0, 0.0}, {1.25, 0.0, 0.0}}, diagnostics,
        [&createdOwner]() {
            std::error_code error;
            for (const auto& staging :
                 std::filesystem::directory_iterator(createdOwner, error)) {
                if (std::filesystem::exists(staging.path() /
                                            "Tile_L0_p0_p0_p0_LOD0.usdc")) {
                    return true;
                }
            }
            return false;
        }));
    Check(!std::filesystem::exists(created));
}

void TestDetachedAuthoringDoesNotResolvePayloads() {
    const auto directory = UniqueTestDirectory("usd_geo_detached_payloads");
    usdgeo::GeoReference reference;
    reference.epsgCode = 26910;

    usdgeo::PointCloudTileAsset tile;
    tile.tile.id = {0, 0, 0, 0};
    tile.tile.bounds.Expand({0.0, 0.0, 0.0});
    tile.tile.lod.bounds = tile.tile.bounds;
    tile.tile.lod.items = {{0, 1, tile.tile.bounds, {0, 1}}};
    usdpointcloud::PointCloudAsset level;
    level.reference = reference;
    level.bounds = tile.tile.bounds;
    level.data.positions = {{0.0, 0.0, 0.0}};
    level.chunk = usdpointcloud::MakePointChunk(level.data, level.bounds);
    tile.levels.push_back(std::move(level));

    // The payload arcs are relative to a root that is not the working
    // directory. Building the layer must not try to resolve them: resolving
    // from a detached stage is what reported "Could not open asset".
    const auto layer = pxr::SdfLayer::CreateAnonymous("detached.usda");
    std::vector<usdgeo::Diagnostic> diagnostics;
    usdgeo::PointCloudPayloadOptions options{
        directory.string(), (directory / "PointCloud.usda").string()};
    options.owner = "detached-layer";
    {
        const WarningCounter warnings;
        Check(usdgeo::AuthorPointCloudTiledAssetWithPayloads(
            layer.operator->(), "/PointCloud", {tile}, options, diagnostics));
        Check(warnings.Count() == 0);
    }
    Check(diagnostics.empty());
    Check(layer->GetPrimAtPath(pxr::SdfPath(
              "/PointCloud/Tiles/Tile_L0_p0_p0_p0/LOD0")) != nullptr);
    const auto payload =
        OnlyGeneration(OwnerDirectory(directory, "detached-layer")) /
        "Tile_L0_p0_p0_p0_LOD0.usdc";
    Check(std::filesystem::exists(payload));
    Check(!pxr::SdfLayer::Find(payload.string()));

    // The same layer is authored again over its own payloads.
    Check(usdgeo::AuthorPointCloudTiledAssetWithPayloads(
        layer.operator->(), "/PointCloud", {tile}, options, diagnostics));
    std::filesystem::remove_all(directory);

    // A failure reports why and leaves the layer as it was.
    WriteFile(directory, "not a directory");
    const auto untouched = pxr::SdfLayer::CreateAnonymous("untouched.usda");
    Check(!usdgeo::AuthorPointCloudTiledAssetWithPayloads(
        untouched.operator->(), "/PointCloud", {tile}, options, diagnostics));
    Check(!diagnostics.empty() &&
          diagnostics.front().message.find("not a usable directory") !=
              std::string::npos);
    Check(untouched->GetPrimAtPath(pxr::SdfPath("/PointCloud")) == nullptr);
    std::filesystem::remove(directory);

    const auto streamDirectory =
        UniqueTestDirectory("usd_geo_detached_stream_payloads");
    const auto streamLayer = pxr::SdfLayer::CreateAnonymous("stream.usda");
    {
        const WarningCounter warnings;
        Check(AuthorOwnedStream(streamLayer.operator->(), streamDirectory,
                                "detached-stream",
                                {{0.25, 0.0, 0.0}, {1.25, 0.0, 0.0}},
                                diagnostics));
        Check(warnings.Count() == 0);
    }
    Check(!pxr::SdfLayer::Find(
        (OnlyGeneration(OwnerDirectory(streamDirectory, "detached-stream")) /
         "Tile_L0_p0_p0_p0_LOD0.usdc")
            .string()));
    std::filesystem::remove_all(streamDirectory);
}

void TestInvalidExtraByteNameDoesNotAuthor() {
    const auto stage = usdgeo::PointCloudLayer::CreateStage();
    usdgeo::GeoReference reference;
    reference.localOrigin = {0.0, 0.0, 0.0};

    usdpointcloud::PointChunk chunk;
    chunk.pointCount = 1;
    chunk.bounds.Expand({0.0, 0.0, 0.0});
    chunk.attributes = {
        {"temperature (C)", usdpointcloud::PointAttributeType::Float64}};
    usdpointcloud::PointData data;
    data.positions = {{0.0, 0.0, 0.0}};
    data.extraByteNames = {"temperature (C)"};
    data.extraBytes = {{21.5}};

    Check(!usdgeo::AuthorPointCloudAsset(
        stage, "/PointCloud", reference, chunk.bounds, chunk, data));
}

} // namespace

int main() {
    TestPointCloudRoundTrip();
    TestOptionalAttributesAreIndependent();
    TestInvalidPositionDoesNotMutateStage();
    TestLayerAuthoringEntryPoint();
    TestMetadataAuthoringAllowsEmptyPointCloud();
    TestLodAuthoring();
    TestLodAuthoringRejectsMismatchedMetadata();
    TestLodAuthoringRejectsNonRepresentableExtent();
    TestLodAuthoringRejectsInvalidLevelWithoutMutation();
    TestTiledLodAuthoring();
    TestTiledLodAuthoringSupportsNegativeTileCoordinates();
    TestInvalidExtraByteNameDoesNotAuthor();
    TestTiledLodAuthoringSupportsMismatchedThresholds();
    TestTiledLodPayloadAuthoring();
    TestStreamTiledPayloadAuthoring();
    TestFixedGridTilingPreservesBoundariesAndMemoryLimit();
    TestAdaptiveTilingRouterAuthorsPlannedLeaves();
    TestSequentialAndCopcPlansAuthorEquivalentOutput();
    TestAdaptiveTilingRejectsPointsOutsidePlan();
    TestGeneratedStreamTiledPayloadAuthoring();
    TestStreamCancellationCleansSpools();
    TestStreamCancellationDuringSpoolReadCleansSpools();
    TestStreamFailureDoesNotMutateLayer();
    TestStreamPayloadFailureRollsBackGeneratedPayloads();
    TestOwnedPayloadsLiveInTheirOwnersGeneration();
    TestInterruptedOwnedGenerationIsRecovered();
    TestFailedOwnedGenerationKeepsPublishedPayloads();
    TestDetachedAuthoringDoesNotResolvePayloads();
    return 0;
}