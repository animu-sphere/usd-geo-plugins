#include "lazperf/io.hpp"

#include "usdgeocopc/ArAssetRandomAccessSource.h"
#include "usdgeocopc/UsdGeoCopcDiagnostics.h"
#include "usdgeo/PointCloudCache.h"
#include "usdcopc/Copc.h"
#include "usdlas/Las.h"

#include <pxr/base/plug/plugin.h>
#include <pxr/base/plug/registry.h>
#include <pxr/base/gf/vec3d.h>
#include <pxr/base/tf/diagnosticMgr.h>
#include <pxr/base/vt/array.h>
#include <pxr/usd/sdf/attributeSpec.h>
#include <pxr/usd/sdf/fileFormat.h>
#include <pxr/usd/sdf/layer.h>
#include <pxr/usd/sdf/primSpec.h>
#include <pxr/usd/ar/resolver.h>
#include <pxr/usd/pcp/dynamicFileFormatInterface.h>
#include <pxr/usd/usd/stage.h>
#include <pxr/usd/usdGeom/points.h>
#include <pxr/usd/usdLod/rootAPI.h>

#include <algorithm>
#include <array>
#include <atomic>
#include <chrono>
#include <cstdint>
#include <cstdlib>
#include <cstring>
#include <utility>
#include <cstdio>
#include <filesystem>
#include <fstream>
#include <string>
#include <vector>

namespace {

class MemoryAsset final : public pxr::ArAsset {
public:
    explicit MemoryAsset(std::vector<std::uint8_t> bytes)
        : bytes_(std::make_shared<std::vector<std::uint8_t>>(
              std::move(bytes))) {}

    std::size_t GetSize() const override { return bytes_->size(); }

    std::shared_ptr<const char> GetBuffer() const override {
        std::shared_ptr<const std::vector<std::uint8_t>> owner = bytes_;
        return std::shared_ptr<const char>(
            owner, reinterpret_cast<const char*>(owner->data()));
    }

    std::size_t Read(void* buffer,
                     std::size_t count,
                     std::size_t offset) const override {
        if (offset > bytes_->size() || count > bytes_->size() - offset) {
            return 0;
        }
        std::memcpy(buffer, bytes_->data() + offset, count);
        return count;
    }

    std::pair<FILE*, std::size_t> GetFileUnsafe() const override {
        return {nullptr, 0};
    }

private:
    std::shared_ptr<std::vector<std::uint8_t>> bytes_;
};

void Check(bool condition, const char* message = nullptr) {
    if (!condition) {
        if (message) {
            std::fprintf(stderr, "check failed: %s\n", message);
        }
        std::abort();
    }
}

void SelectHttpResolver() {
#if defined(_WIN32)
    Check(_putenv_s("PXR_AR_DEFAULT_RESOLVER", "HttpResolver") == 0);
#else
    Check(setenv("PXR_AR_DEFAULT_RESOLVER", "HttpResolver", 1) == 0);
#endif
}

void SetHttpResolverAsset(const std::filesystem::path& assetPath) {
#if defined(_WIN32)
    Check(_putenv_s("USDGEOCOPC_TEST_ASSET", assetPath.string().c_str()) == 0);
#else
    Check(setenv("USDGEOCOPC_TEST_ASSET", assetPath.string().c_str(), 1) == 0);
#endif
}

void SetHttpResolverIdentityMode(const char* mode) {
#if defined(_WIN32)
    Check(_putenv_s("USDGEOCOPC_TEST_IDENTITY", mode) == 0);
#else
    Check(setenv("USDGEOCOPC_TEST_IDENTITY", mode, 1) == 0);
#endif
}

void SetCacheRoot(const std::filesystem::path& cacheRoot) {
#if defined(_WIN32)
    Check(_putenv_s("USDGEO_CACHE_ROOT", cacheRoot.string().c_str()) == 0);
#else
    Check(setenv("USDGEO_CACHE_ROOT", cacheRoot.string().c_str(), 1) == 0);
#endif
}

std::filesystem::path FindGeneratedCacheRoot(
    const std::filesystem::path& cacheRoot) {
    std::error_code error;
    for (const auto& entry :
         std::filesystem::recursive_directory_iterator(cacheRoot, error)) {
        if (error) {
            break;
        }
        if (entry.is_regular_file(error) &&
            entry.path().filename() == "root.usdc") {
            return entry.path().parent_path();
        }
    }
    return {};
}

constexpr const char* CacheSentinelPath = "/CacheSentinel";

void WriteResolverCacheEntry(const usdgeo::cache::Layout& layout,
                             const pxr::SdfLayerHandle& sourceLayer,
                             bool corruptedRoot) {
    std::error_code error;
    std::filesystem::remove_all(layout.entryDirectory, error);
    error.clear();
    std::filesystem::create_directories(layout.entryDirectory, error);
    Check(!error, "create resolver cache entry");
    if (corruptedRoot) {
        std::ofstream root(layout.rootLayer,
                           std::ios::binary | std::ios::trunc);
        Check(root.good(), "create corrupted resolver cache root");
        root << "corrupted generated cache";
        Check(root.good(), "write corrupted resolver cache root");
    } else {
        const auto cacheLayer =
            pxr::SdfLayer::CreateAnonymous("resolver-cache-entry.usda");
        Check(sourceLayer && cacheLayer, "create resolver cache layer");
        cacheLayer->TransferContent(sourceLayer);
        const auto sentinel = pxr::SdfPrimSpec::New(
            cacheLayer->GetPseudoRoot(), "CacheSentinel", pxr::SdfSpecifierDef);
        Check(static_cast<bool>(sentinel), "author resolver cache sentinel");
        Check(cacheLayer->Export(layout.rootLayer.string()),
              "export resolver cache root");
    }
    std::ofstream manifest(layout.manifest,
                           std::ios::binary | std::ios::trunc);
    Check(manifest.good(), "create resolver cache manifest");
    manifest << "committed\n";
    Check(manifest.good(), "write resolver cache manifest");
}

void TestArAssetRandomAccessSource() {
    auto asset = std::make_shared<MemoryAsset>(
        std::vector<std::uint8_t>{1, 2, 3, 4, 5});
    usdgeocopc::ArAssetRandomAccessSource source(asset, "memory.copc");

    std::uint64_t size = 0;
    std::vector<usdgeo::Diagnostic> diagnostics;
    Check(source.GetSize(size, diagnostics));
    Check(size == 5);

    std::vector<std::uint8_t> bytes;
    Check(source.Read(1, 3, bytes, diagnostics));
    Check((bytes == std::vector<std::uint8_t>{2, 3, 4}));
    Check(!source.Read(4, 2, bytes, diagnostics));
    Check(!diagnostics.empty());
    Check(diagnostics.back().code == usdgeo::DiagnosticCode::InvalidOffset);
}

void TestResolverCacheDiagnostic() {
    const auto message = usdgeocopc::diagnostics::Message(
        usdgeocopc::diagnostics::ResolverCacheReuseDisabled,
        "Generated cache reuse disabled: the active resolver did not provide "
        "a stable source validation identity.");
    Check(message ==
              "[COPC009] Generated cache reuse disabled: the active resolver "
              "did not provide a stable source validation identity.",
          "resolver cache diagnostic format");

    // The four decision codes are distinct and stable, and each emitted
    // message ends with the shared category name so a consumer can match the
    // category instead of the prose.
    const std::vector<const char*> codes{
        usdgeocopc::diagnostics::ResolverCacheReuseDisabled,
        usdgeocopc::diagnostics::ResolverCacheReusePermitted,
        usdgeocopc::diagnostics::ResolverIdentityChanged,
        usdgeocopc::diagnostics::ResolverCacheInvalidated};
    for (std::size_t index = 0; index != codes.size(); ++index) {
        for (std::size_t other = index + 1; other != codes.size(); ++other) {
            Check(std::string(codes[index]) != codes[other],
                  "resolver decision codes must be distinct");
        }
    }
    Check(std::string(usdgeocopc::diagnostics::ResolverCacheReusePermitted) ==
              "COPC010" &&
          std::string(usdgeocopc::diagnostics::ResolverIdentityChanged) ==
              "COPC011" &&
          std::string(usdgeocopc::diagnostics::ResolverCacheInvalidated) ==
              "COPC012",
          "resolver decision codes changed meaning");

    // Assert the projection the plugin actually emits, category by category.
    const std::vector<std::pair<usdgeo::cache::CacheDecision, const char*>>
        projection{
            {usdgeo::cache::CacheDecision::IdentityUnavailable,
             usdgeocopc::diagnostics::ResolverCacheReuseDisabled},
            {usdgeo::cache::CacheDecision::IdentityUnstable,
             usdgeocopc::diagnostics::ResolverCacheReuseDisabled},
            {usdgeo::cache::CacheDecision::ReuseDisabled,
             usdgeocopc::diagnostics::ResolverCacheReuseDisabled},
            {usdgeo::cache::CacheDecision::IdentityStable,
             usdgeocopc::diagnostics::ResolverCacheReusePermitted},
            {usdgeo::cache::CacheDecision::Hit,
             usdgeocopc::diagnostics::ResolverCacheReusePermitted},
            {usdgeo::cache::CacheDecision::IdentityChanged,
             usdgeocopc::diagnostics::ResolverIdentityChanged},
            {usdgeo::cache::CacheDecision::Invalidated,
             usdgeocopc::diagnostics::ResolverCacheInvalidated}};
    for (const auto& [decision, expectedCode] : projection) {
        Check(std::string(usdgeocopc::diagnostics::DecisionCode(decision)) ==
                  expectedCode,
              "resolver decision projected onto the wrong code");
        const auto rendered =
            usdgeocopc::diagnostics::DecisionMessage(decision);
        Check(rendered.rfind(std::string("[") + expectedCode + "] ", 0) == 0,
              "resolver decision message must lead with its code");
        Check(rendered.find(usdgeo::cache::CacheDecisionMessage(decision)) !=
                  std::string::npos,
              "resolver decision message must carry the shared text");
        // A consumer matches the category, so the category has to survive the
        // projection. Building the expectation from the code path under test
        // would assert nothing, so it is spelled out here.
        Check(rendered.size() > 3 &&
                  rendered.compare(
                      rendered.size() -
                          std::strlen(
                              usdgeo::cache::CacheDecisionName(decision)) - 2,
                      std::string::npos,
                      std::string("(") +
                          usdgeo::cache::CacheDecisionName(decision) + ")") == 0,
              "resolver decision message must end with its category");
        for (const char* forbidden : {"http", "ETag", "etag", "Authorization"}) {
            Check(rendered.find(forbidden) == std::string::npos,
                  "resolver decision message leaked a transport detail");
        }
    }
    Check(usdgeocopc::diagnostics::DecisionIsWarning(
              usdgeo::cache::CacheDecision::ReuseDisabled) &&
          usdgeocopc::diagnostics::DecisionIsWarning(
              usdgeo::cache::CacheDecision::Invalidated) &&
          !usdgeocopc::diagnostics::DecisionIsWarning(
              usdgeo::cache::CacheDecision::Hit) &&
          !usdgeocopc::diagnostics::DecisionIsWarning(
              usdgeo::cache::CacheDecision::IdentityChanged),
          "resolver decision severity changed");
}

void RegisterPlugin(const std::filesystem::path& plugInfo) {
    const auto plugins = pxr::PlugRegistry::GetInstance().RegisterPlugins(
        plugInfo.string());
    Check(plugins.size() == 1);
    Check(plugins.front()->Load());
}

template <typename T>
void Write(std::vector<std::uint8_t>& bytes, std::size_t offset, T value) {
    std::array<std::uint8_t, sizeof(T)> encoded{};
    std::memcpy(encoded.data(), &value, sizeof(T));
    const std::uint16_t marker = 1;
    const bool nativeLittleEndian =
        *reinterpret_cast<const std::uint8_t*>(&marker) == 1;
    if (!nativeLittleEndian) {
        std::reverse(encoded.begin(), encoded.end());
    }
    std::copy(encoded.begin(), encoded.end(), bytes.begin() + offset);
}

std::vector<std::uint8_t> MakeMetadataFixture() {
    constexpr std::size_t headerSize = 375;
    constexpr std::size_t infoVlrOffset = headerSize;
    constexpr std::size_t hierarchyVlrOffset = infoVlrOffset + 54 + 160;
    constexpr std::size_t rootOffset = hierarchyVlrOffset + 54;
    constexpr std::size_t pointDataOffset = rootOffset + 32;
    constexpr std::size_t fileSize = pointDataOffset + 30;

    std::vector<std::uint8_t> bytes(fileSize, 0);
    std::memcpy(bytes.data(), "LASF", 4);
    Write(bytes, 24, std::uint8_t{1});
    Write(bytes, 25, std::uint8_t{4});
    Write(bytes, 94, static_cast<std::uint16_t>(headerSize));
    Write(bytes, 96, static_cast<std::uint32_t>(rootOffset));
    Write(bytes, 100, std::uint32_t{2});
    Write(bytes, 104, std::uint8_t{6});
    Write(bytes, 105, std::uint16_t{30});
    Write(bytes, 107, std::uint32_t{1});
    Write(bytes, 247, std::uint64_t{1});
    Write(bytes, 131, 0.01);
    Write(bytes, 139, 0.01);
    Write(bytes, 147, 0.01);
    Write(bytes, 155, 1000.0);
    Write(bytes, 163, 2000.0);
    Write(bytes, 171, 3000.0);
    Write(bytes, 179, 1001.0);
    Write(bytes, 187, 1000.0);
    Write(bytes, 195, 2001.0);
    Write(bytes, 203, 2000.0);
    Write(bytes, 211, 3001.0);
    Write(bytes, 219, 3000.0);

    Write(bytes, infoVlrOffset, std::uint16_t{0});
    std::memcpy(bytes.data() + infoVlrOffset + 2, "copc", 4);
    Write(bytes, infoVlrOffset + 18, std::uint16_t{1});
    Write(bytes, infoVlrOffset + 20, std::uint16_t{160});
    std::memcpy(bytes.data() + infoVlrOffset + 22, "COPC info", 9);

    Write(bytes, hierarchyVlrOffset, std::uint16_t{0});
    std::memcpy(bytes.data() + hierarchyVlrOffset + 2, "copc", 4);
    Write(bytes, hierarchyVlrOffset + 18, std::uint16_t{1000});
    Write(bytes, hierarchyVlrOffset + 20, std::uint16_t{0});
    std::memcpy(bytes.data() + hierarchyVlrOffset + 22, "hierarchy", 9);

    const auto infoOffset = infoVlrOffset + 54;
    Write(bytes, infoOffset, 1000.5);
    Write(bytes, infoOffset + 8, 2000.5);
    Write(bytes, infoOffset + 16, 3000.5);
    Write(bytes, infoOffset + 24, 2.0);
    Write(bytes, infoOffset + 32, 0.5);
    Write(bytes, infoOffset + 40, static_cast<std::uint64_t>(rootOffset));
    Write(bytes, infoOffset + 48, std::uint64_t{32});
    Write(bytes, infoOffset + 56, -10.0);
    Write(bytes, infoOffset + 64, 10.0);
    Write(bytes, rootOffset + 16, static_cast<std::uint64_t>(pointDataOffset));
    Write(bytes, rootOffset + 24, std::int32_t{30});
    Write(bytes, rootOffset + 28, std::int32_t{1});
    return bytes;
}

std::vector<std::vector<std::uint8_t>> MakeEquivalentRecords() {
    std::vector<std::vector<std::uint8_t>> records(
        3, std::vector<std::uint8_t>(30, 0));
    for (std::size_t index = 0; index < records.size(); ++index) {
        auto& record = records[index];
        Write(record, 0, static_cast<std::int32_t>(index * 100));
        Write(record, 4, static_cast<std::int32_t>(index * 100));
        Write(record, 8, static_cast<std::int32_t>(index * 100));
        Write(record, 12, static_cast<std::uint16_t>(10 + index));
        Write(record, 14, std::uint8_t{0x21});
        Write(record, 15, static_cast<std::uint8_t>(2 + index));
        Write(record, 16, static_cast<std::uint8_t>(7 + index));
        Write(record, 17, static_cast<std::uint8_t>(20 + index));
        Write(record, 18, static_cast<std::int16_t>(-4 + index));
        Write(record, 20, static_cast<std::uint16_t>(42 + index));
        Write(record, 22, 12.5 + index);
    }
    return records;
}

void SetEquivalentHeader(std::vector<std::uint8_t>& bytes,
                         std::uint32_t pointDataOffset,
                         std::uint32_t vlrCount) {
    std::memcpy(bytes.data(), "LASF", 4);
    Write(bytes, 24, std::uint8_t{1});
    Write(bytes, 25, std::uint8_t{4});
    Write(bytes, 94, std::uint16_t{375});
    Write(bytes, 96, pointDataOffset);
    Write(bytes, 100, vlrCount);
    Write(bytes, 104, std::uint8_t{6});
    Write(bytes, 105, std::uint16_t{30});
    Write(bytes, 107, std::uint32_t{3});
    Write(bytes, 131, 0.01);
    Write(bytes, 139, 0.01);
    Write(bytes, 147, 0.01);
    Write(bytes, 155, 1000.0);
    Write(bytes, 163, 2000.0);
    Write(bytes, 171, 3000.0);
    Write(bytes, 179, 1002.0);
    Write(bytes, 187, 1000.0);
    Write(bytes, 195, 2002.0);
    Write(bytes, 203, 2000.0);
    Write(bytes, 211, 3002.0);
    Write(bytes, 219, 3000.0);
    Write(bytes, 247, std::uint64_t{3});
}

std::filesystem::path WriteFixture(
    const std::vector<std::uint8_t>& bytes,
    const std::string& name) {
    const auto path = std::filesystem::temp_directory_path() / name;
    std::ofstream output(path, std::ios::binary | std::ios::trunc);
    Check(output.good());
    output.write(reinterpret_cast<const char*>(bytes.data()),
                 static_cast<std::streamsize>(bytes.size()));
    Check(output.good());
    return path;
}

std::filesystem::path WriteEquivalentLas(
    const std::vector<std::vector<std::uint8_t>>& records) {
    std::vector<std::uint8_t> bytes(375 + records.size() * 30, 0);
    SetEquivalentHeader(bytes, 375, 0);
    for (std::size_t index = 0; index < records.size(); ++index) {
        std::copy(records[index].begin(), records[index].end(),
                  bytes.begin() + 375 + index * 30);
    }
    return WriteFixture(bytes, "usd_copc_lod_equivalent.las");
}

template <typename T>
T ReadValue(const std::vector<std::uint8_t>& bytes, std::size_t offset) {
    T value{};
    std::memcpy(&value, bytes.data() + offset, sizeof(T));
    return value;
}

std::vector<std::uint8_t> WriteEquivalentLaz(
    const std::vector<std::vector<std::uint8_t>>& records,
    const std::filesystem::path& path) {
    lazperf::writer::named_file::config config;
    config.scale = {0.01, 0.01, 0.01};
    config.offset = {1000.0, 2000.0, 3000.0};
    config.chunk_size = static_cast<unsigned int>(records.size());
    config.pdrf = 6;
    config.minor_version = 4;
    lazperf::writer::named_file writer(path.string(), config);
    for (const auto& record : records) {
        writer.writePoint(reinterpret_cast<const char*>(record.data()));
    }
    writer.close();

    std::ifstream input(path, std::ios::binary);
    input.seekg(0, std::ios::end);
    const auto size = input.tellg();
    Check(size > 0);
    input.seekg(0);
    std::vector<std::uint8_t> bytes(static_cast<std::size_t>(size));
    input.read(reinterpret_cast<char*>(bytes.data()), size);
    Check(static_cast<bool>(input));

    const auto pointDataOffset = ReadValue<std::uint32_t>(bytes, 96);
    const auto chunkTableOffset = ReadValue<std::int64_t>(
        bytes, pointDataOffset);
    Check(chunkTableOffset > static_cast<std::int64_t>(pointDataOffset + 8));
    const auto headerSize = ReadValue<std::uint16_t>(bytes, 94);
    Write(bytes, static_cast<std::size_t>(headerSize) + 54,
          std::uint16_t{2});
    std::ofstream output(path, std::ios::binary | std::ios::trunc);
    Check(output.good());
    output.write(reinterpret_cast<const char*>(bytes.data()),
                 static_cast<std::streamsize>(bytes.size()));
    Check(output.good());
    return std::vector<std::uint8_t>(
        bytes.begin() + pointDataOffset + 8,
        bytes.begin() + static_cast<std::size_t>(chunkTableOffset));
}

void WriteHierarchyEntry(std::vector<std::uint8_t>& bytes,
                         std::size_t offset,
                         std::int32_t level,
                         std::int32_t x,
                         std::int32_t y,
                         std::int32_t z,
                         std::int32_t pointCount,
                         std::uint64_t dataOffset,
                         std::int32_t byteSize) {
    Write(bytes, offset, level);
    Write(bytes, offset + 4, x);
    Write(bytes, offset + 8, y);
    Write(bytes, offset + 12, z);
    Write(bytes, offset + 16, dataOffset);
    Write(bytes, offset + 24, byteSize);
    Write(bytes, offset + 28, pointCount);
}

std::filesystem::path WriteEquivalentCopc(
    const std::vector<std::uint8_t>& compressedPoints) {
    constexpr std::size_t headerSize = 375;
    constexpr std::size_t infoVlrOffset = headerSize;
    constexpr std::size_t hierarchyVlrOffset = infoVlrOffset + 54 + 160;
    constexpr std::size_t rootOffset = hierarchyVlrOffset + 54;
    constexpr std::size_t pointDataOffset = rootOffset + 32;
    std::vector<std::uint8_t> bytes(pointDataOffset + compressedPoints.size(),
                                    0);
    SetEquivalentHeader(bytes, static_cast<std::uint32_t>(pointDataOffset), 2);

    Write(bytes, infoVlrOffset, std::uint16_t{0});
    std::memcpy(bytes.data() + infoVlrOffset + 2, "copc", 4);
    Write(bytes, infoVlrOffset + 18, std::uint16_t{1});
    Write(bytes, infoVlrOffset + 20, std::uint16_t{160});
    std::memcpy(bytes.data() + infoVlrOffset + 22, "COPC info", 9);
    Write(bytes, infoVlrOffset + 54, 1001.0);
    Write(bytes, infoVlrOffset + 62, 2001.0);
    Write(bytes, infoVlrOffset + 70, 3001.0);
    Write(bytes, infoVlrOffset + 78, 10.0);
    Write(bytes, infoVlrOffset + 86, 0.5);
    Write(bytes, infoVlrOffset + 94, static_cast<std::uint64_t>(rootOffset));
    Write(bytes, infoVlrOffset + 102, std::uint64_t{32});
    Write(bytes, infoVlrOffset + 110, 12.5);
    Write(bytes, infoVlrOffset + 118, 14.5);

    Write(bytes, hierarchyVlrOffset, std::uint16_t{0});
    std::memcpy(bytes.data() + hierarchyVlrOffset + 2, "copc", 4);
    Write(bytes, hierarchyVlrOffset + 18, std::uint16_t{1000});
    Write(bytes, hierarchyVlrOffset + 20, std::uint16_t{32});
    std::memcpy(bytes.data() + hierarchyVlrOffset + 22, "hierarchy", 9);
    WriteHierarchyEntry(bytes, rootOffset, 0, 0, 0, 0, 3,
                        static_cast<std::uint64_t>(pointDataOffset),
                        static_cast<std::int32_t>(compressedPoints.size()));
    std::copy(compressedPoints.begin(), compressedPoints.end(),
              bytes.begin() + pointDataOffset);
    return WriteFixture(bytes, "usd_copc_lod_equivalent.copc");
}

struct LodSnapshot {
    std::vector<pxr::VtVec3fArray> positions;
    std::vector<pxr::VtIntArray> intensity;
    std::vector<pxr::VtArray<unsigned char>> classification;
    std::vector<pxr::VtArray<double>> gpsTime;
    std::vector<pxr::GfVec3d> boundsMin;
    std::vector<pxr::GfVec3d> boundsMax;
    std::vector<std::uint64_t> pointCounts;
    pxr::VtArray<float> thresholds;
    int defaultIndex = -1;
};

LodSnapshot ReadLodSnapshot(const pxr::UsdStageRefPtr& stage) {
    const auto root = stage->GetPrimAtPath(pxr::SdfPath("/PointCloud"));
    Check(root.HasAPI<pxr::UsdLodRootAPI>());
    LodSnapshot snapshot;
    Check(root.GetAttribute(pxr::TfToken("lod:default:index"))
              .Get(&snapshot.defaultIndex));
    const auto heuristic = stage->GetPrimAtPath(pxr::SdfPath(
        "/LodHeuristics/PointCloudScreenSize"));
    Check(heuristic.IsValid());
    Check(heuristic.GetAttribute(pxr::TfToken("thresholds"))
              .Get(&snapshot.thresholds));
    for (int index = 0; index < 4; ++index) {
        const auto path = pxr::SdfPath(
            "/PointCloud/LOD" + std::to_string(index));
        const auto points = pxr::UsdGeomPoints::Get(stage, path);
        if (!points.GetPrim().IsValid()) break;
        pxr::VtVec3fArray positions;
        pxr::VtIntArray intensity;
        pxr::VtArray<unsigned char> classification;
        pxr::VtArray<double> gpsTime;
        pxr::GfVec3d boundsMin;
        pxr::GfVec3d boundsMax;
        std::uint64_t pointCount = 0;
        Check(points.GetPointsAttr().Get(&positions));
        Check(points.GetPrim()
                  .GetAttribute(pxr::TfToken("geo:intensity"))
                  .Get(&intensity));
        Check(points.GetPrim()
                  .GetAttribute(pxr::TfToken("geo:classification"))
                  .Get(&classification));
        Check(points.GetPrim()
                  .GetAttribute(pxr::TfToken("geo:gpsTime"))
                  .Get(&gpsTime));
        Check(points.GetPrim()
              .GetAttribute(pxr::TfToken("geo:boundsMin"))
              .Get(&boundsMin));
        Check(points.GetPrim()
              .GetAttribute(pxr::TfToken("geo:boundsMax"))
              .Get(&boundsMax));
        Check(points.GetPrim()
              .GetAttribute(pxr::TfToken("geo:pointCount"))
              .Get(&pointCount));
        snapshot.positions.push_back(std::move(positions));
        snapshot.intensity.push_back(std::move(intensity));
        snapshot.classification.push_back(std::move(classification));
        snapshot.gpsTime.push_back(std::move(gpsTime));
        snapshot.boundsMin.push_back(boundsMin);
        snapshot.boundsMax.push_back(boundsMax);
        snapshot.pointCounts.push_back(pointCount);
    }
    return snapshot;
}

void CheckEquivalentLodSnapshots(const LodSnapshot& expected,
                                 const LodSnapshot& actual) {
    Check(expected.defaultIndex == actual.defaultIndex);
    Check(expected.thresholds == actual.thresholds);
    Check(expected.positions == actual.positions);
    Check(expected.intensity == actual.intensity);
    Check(expected.classification == actual.classification);
    Check(expected.gpsTime == actual.gpsTime);
    Check(expected.boundsMin == actual.boundsMin);
    Check(expected.boundsMax == actual.boundsMax);
    Check(expected.pointCounts == actual.pointCounts);
}

void TestMetadataIntegration() {
    const auto format = pxr::SdfFileFormat::FindByExtension("sample.copc");
    Check(format);
    Check(dynamic_cast<const pxr::PcpDynamicFileFormatInterface*>(
              format.operator->()));

    const auto path = std::filesystem::temp_directory_path() /
                      "usd_pointcloud_plugins_metadata.copc";
    {
        const auto bytes = MakeMetadataFixture();
        std::ofstream output(path, std::ios::binary | std::ios::trunc);
        Check(output.good());
        output.write(reinterpret_cast<const char*>(bytes.data()),
                     static_cast<std::streamsize>(bytes.size()));
        Check(output.good());
    }

    const auto layer = pxr::SdfLayer::CreateAnonymous("metadata.usda");
    Check(layer);
    Check(format->Read(layer.operator->(), path.string(), true));
    Check(layer->GetPrimAtPath(pxr::SdfPath("/PointCloud")));
    Check(layer->GetAttributeAtPath(
        pxr::SdfPath("/PointCloud.geo:pointFormat")));
    std::filesystem::remove(path);
}

void TestAuthoredLodEquivalence() {
    const auto records = MakeEquivalentRecords();
    const auto lasPath = WriteEquivalentLas(records);
    const auto lazPath = std::filesystem::temp_directory_path() /
                         "usd_copc_lod_equivalent.laz";
    const auto compressedPoints = WriteEquivalentLaz(records, lazPath);
    const auto copcPath = WriteEquivalentCopc(compressedPoints);
    const pxr::SdfLayer::FileFormatArguments arguments = {
        {"lod", "balanced"}};

    const auto lasLayer = pxr::SdfLayer::FindOrOpen(
        lasPath.string(), arguments);
    const auto lazLayer = pxr::SdfLayer::FindOrOpen(
        lazPath.string(), arguments);
    const auto copcLayer = pxr::SdfLayer::FindOrOpen(
        copcPath.string(), arguments);
    Check(lasLayer && lazLayer && copcLayer);

    const auto lasStage = pxr::UsdStage::Open(lasLayer);
    const auto lazStage = pxr::UsdStage::Open(lazLayer);
    const auto copcStage = pxr::UsdStage::Open(copcLayer);
    Check(lasStage && lazStage && copcStage);

    const auto lasSnapshot = ReadLodSnapshot(lasStage);
    const auto lazSnapshot = ReadLodSnapshot(lazStage);
    const auto copcSnapshot = ReadLodSnapshot(copcStage);
    Check(lasSnapshot.positions.size() == 3);
    CheckEquivalentLodSnapshots(lasSnapshot, lazSnapshot);
    CheckEquivalentLodSnapshots(lasSnapshot, copcSnapshot);

    const auto dynamicPath = std::filesystem::temp_directory_path() /
                             "usd_pointcloud_plugins_dynamic_copc_lod.usda";
    {
        std::ofstream output(dynamicPath);
        Check(output.good());
        output << "#usda 1.0\n"
               << "def \"Survey\" (\n"
               << "    prepend payload = @" << copcPath.generic_string()
               << "@</PointCloud>\n"
               << "    pc_copc_lod = \"balanced\"\n"
               << ")\n"
               << "{}\n";
        Check(output.good());
    }
    const auto dynamicStage = pxr::UsdStage::Open(dynamicPath.string());
    Check(dynamicStage);
    const auto survey = dynamicStage->GetPrimAtPath(
        pxr::SdfPath("/Survey"));
    Check(survey.IsValid());
    Check(survey.HasAPI<pxr::UsdLodRootAPI>());
    std::filesystem::remove(dynamicPath);

    std::error_code error;
    std::filesystem::remove(lasPath, error);
    std::filesystem::remove(lazPath, error);
    std::filesystem::remove(copcPath, error);
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

void TestTiledReadReopens() {
    const auto directory =
        std::filesystem::temp_directory_path() /
        ("usd_copc_tiled_reopen_" +
         std::to_string(
             std::chrono::steady_clock::now().time_since_epoch().count()));
    std::filesystem::remove_all(directory);
    std::filesystem::create_directories(directory);
    const auto source = directory / "conformance.copc";
    Check(std::filesystem::copy_file(
              std::filesystem::path(USDGEOCOPC_SOURCE_DIR) / "tests" /
                  "fixtures" / "conformance.copc",
              source),
          "copy COPC fixture");

    // The second open reads the layer again once the first is released, over
    // the payloads the first open generated. Neither open may resolve those
    // payloads while the layer is being built.
    const pxr::SdfLayer::FileFormatArguments arguments = {
        {"payloadDirectory", "payloads"}, {"tile", "true"}, {"tileSize", "1"}};
    for (int open = 0; open < 2; ++open) {
        const WarningCounter warnings;
        const auto layer = pxr::SdfLayer::FindOrOpen(source.string(), arguments);
        Check(static_cast<bool>(layer), "tiled COPC layer opens");
        Check(layer->GetPrimAtPath(pxr::SdfPath("/PointCloud/Tiles")) != nullptr,
              "tiled COPC layer authors tiles");
        Check(warnings.Count() == 0,
              "tiled COPC read resolved payloads while authoring");
    }
    Check(!pxr::SdfLayer::Find(source.string(), arguments),
          "tiled COPC layer was released");
    bool hasPayload = false;
    for (const auto& entry : std::filesystem::recursive_directory_iterator(
             directory / "payloads")) {
        hasPayload = hasPayload ||
                     entry.path().filename() == "Tile_L0_p0_p0_p0_LOD0.usdc";
    }
    Check(hasPayload, "tiled COPC payload exists");
    std::error_code error;
    std::filesystem::remove_all(directory, error);
}

void TestResolverBackedRead() {
    const auto records = MakeEquivalentRecords();
    const auto lazPath = std::filesystem::temp_directory_path() /
                         "usd_copc_resolver_test.laz";
    const auto compressedPoints = WriteEquivalentLaz(records, lazPath);
    const auto copcPath = WriteEquivalentCopc(compressedPoints);
    std::ifstream input(copcPath, std::ios::binary | std::ios::ate);
    Check(input.good());
    const auto end = input.tellg();
    Check(end > 0);
    input.seekg(0, std::ios::beg);
    std::vector<std::uint8_t> bytes(static_cast<std::size_t>(end));
    input.read(reinterpret_cast<char*>(bytes.data()),
               static_cast<std::streamsize>(bytes.size()));
    Check(static_cast<bool>(input));
    const auto resolverAsset = std::filesystem::temp_directory_path() /
                               "usd_copc_resolver_asset.copc";
    std::ofstream resolverOutput(resolverAsset,
                                 std::ios::binary | std::ios::trunc);
    Check(resolverOutput.good());
    resolverOutput.write(reinterpret_cast<const char*>(bytes.data()),
                         static_cast<std::streamsize>(bytes.size()));
    Check(resolverOutput.good());
    resolverOutput.close();
    SetHttpResolverAsset(resolverAsset);
    const auto resolverInfo = pxr::ArGetResolver().GetAssetInfo(
        "http://memory.copc", pxr::ArResolvedPath("http://memory.copc"));
    Check(!resolverInfo.version.empty(),
          "resolver did not provide an asset validation version");
    const auto cacheRoot = std::filesystem::temp_directory_path() /
                           "usd_copc_resolver_cache_baseline";
    std::error_code cacheError;
    std::filesystem::remove_all(cacheRoot, cacheError);
    SetCacheRoot(cacheRoot);

    const auto resolvedAsset = pxr::ArGetResolver().OpenAsset(
        pxr::ArResolvedPath("http://memory.copc"));
    Check(static_cast<bool>(resolvedAsset), "resolver returned an asset");
    Check(resolvedAsset->GetSize() == bytes.size(),
          "resolver asset size matches");
    usdgeo::cache::SourceIdentity resolverIdentity;
    usdgeo::cache::ResolverIdentityStability resolverStability;
    std::string identityError;
    Check(usdgeo::TryBuildResolverSourceIdentity(
              pxr::ArGetResolver(), "http://memory.copc",
              pxr::ArResolvedPath("http://memory.copc"), *resolvedAsset,
              resolverIdentity, resolverStability, identityError));
    Check(resolverStability ==
          usdgeo::cache::ResolverIdentityStability::Stable);
    Check(resolverIdentity.identifier == "http://memory.copc");
    Check(!resolverIdentity.validationToken.empty());
        const auto initialValidationToken = resolverIdentity.validationToken;

    SetHttpResolverIdentityMode("unstable");
    const auto unstableInfo = pxr::ArGetResolver().GetAssetInfo(
        "http://memory.copc", pxr::ArResolvedPath("http://memory.copc"));
    usdgeo::cache::ResolverAssetIdentity unstableAsset{
        "http://memory.copc", static_cast<std::uintmax_t>(bytes.size()),
        unstableInfo.version};
    Check(usdgeo::cache::ClassifyResolverIdentity(unstableAsset) ==
          usdgeo::cache::ResolverIdentityStability::Unstable);
    Check(!usdgeo::TryBuildResolverSourceIdentity(
              pxr::ArGetResolver(), "http://memory.copc",
              pxr::ArResolvedPath("http://memory.copc"), *resolvedAsset,
              resolverIdentity, resolverStability, identityError));
    Check(resolverStability ==
          usdgeo::cache::ResolverIdentityStability::Unstable);

    SetHttpResolverIdentityMode("unavailable");
    const auto unavailableInfo = pxr::ArGetResolver().GetAssetInfo(
        "http://memory.copc", pxr::ArResolvedPath("http://memory.copc"));
    const auto unavailablePath =
        pxr::ArGetResolver().Resolve("http://memory.copc");
    Check(unavailablePath.GetPathString().empty());
    usdgeo::cache::ResolverAssetIdentity unavailableAsset{
        unavailablePath.GetPathString(),
        static_cast<std::uintmax_t>(bytes.size()),
        unavailableInfo.version};
    Check(usdgeo::cache::ClassifyResolverIdentity(unavailableAsset) ==
          usdgeo::cache::ResolverIdentityStability::Unavailable);
    Check(!usdgeo::TryBuildResolverSourceIdentity(
              pxr::ArGetResolver(), "http://memory.copc",
              unavailablePath, *resolvedAsset,
              resolverIdentity, resolverStability, identityError));
    Check(resolverStability ==
          usdgeo::cache::ResolverIdentityStability::Unavailable);
    SetHttpResolverIdentityMode("");
    Check(usdgeo::TryBuildResolverSourceIdentity(
              pxr::ArGetResolver(), "http://memory.copc",
              pxr::ArResolvedPath("http://memory.copc"), *resolvedAsset,
              resolverIdentity, resolverStability, identityError),
          "restore stable resolver identity");

    char signature[4]{};
    Check(resolvedAsset->Read(signature, sizeof(signature), 0) ==
              sizeof(signature),
            "resolver asset signature read");
        Check(std::memcmp(signature, "LASF", sizeof(signature)) == 0,
                    "resolver asset has LAS signature");

    const auto format = pxr::SdfFileFormat::FindByExtension("sample.copc");
    Check(format);
    const auto layer = pxr::SdfLayer::CreateAnonymous("resolver.usda");
    Check(layer);
    Check(format->Read(layer.operator->(), "http://memory.copc", false));
    Check(layer->GetPrimAtPath(pxr::SdfPath("/PointCloud")));
    Check(layer->GetAttributeAtPath(
        pxr::SdfPath("/PointCloud.geo:pointCount")));

    const auto firstStage = pxr::UsdStage::Open(layer);
    Check(firstStage);
    const auto firstPoints = pxr::UsdGeomPoints::Get(
        firstStage, pxr::SdfPath("/PointCloud"));
    pxr::VtVec3fArray firstPositions;
    Check(firstPoints.GetPointsAttr().Get(&firstPositions));
    Check(!firstPositions.empty());

    auto cacheSource =
        std::make_shared<usdgeocopc::ArAssetRandomAccessSource>(
            resolvedAsset, "http://memory.copc");
    usdcopc::CopcReader cacheReader(cacheSource);
    usdcopc::CopcHeader cacheHeader;
    std::vector<usdgeo::Diagnostic> cacheDiagnostics;
    Check(cacheReader.ReadMetadata(cacheHeader, cacheDiagnostics),
          "read cache fixture metadata");
    usdpointcloud::PointChunk metadataChunk;
    usdgeo::GeoReference cacheReference;
    usdgeo::SpatialBounds cacheBounds;
    std::string cacheErrorMessage;
    Check(usdlas::BuildPointCloudMetadata(
        cacheHeader.las, metadataChunk, cacheReference, cacheBounds,
        cacheErrorMessage), "build cache fixture metadata");
    usdpointcloud::PointReadRequest cacheRequest;
    Check(usdpointcloud::MakeReadRequest(
        {}, cacheRequest, cacheDiagnostics,
        usdpointcloud::PointReadFormat::Copc), "build cache read request");
    usdgeo::cache::Layout cacheLayout;
    Check(usdgeo::TryBuildPointCloudCacheLayout(
        cacheRoot, resolverIdentity, cacheReference, cacheRequest,
        "copc-reader-1", cacheLayout, cacheErrorMessage),
        "build resolver cache layout");
    WriteResolverCacheEntry(cacheLayout, layer, false);

    const auto cachedLayer =
        pxr::SdfLayer::CreateAnonymous("resolver-cached.usda");
    Check(cachedLayer);
    Check(format->Read(cachedLayer.operator->(), "http://memory.copc", false));
    Check(cachedLayer->GetPrimAtPath(pxr::SdfPath(CacheSentinelPath)),
          "stable resolver identity did not reuse generated cache");

    const auto generatedCacheRoot = FindGeneratedCacheRoot(cacheRoot);
    Check(generatedCacheRoot == cacheLayout.entryDirectory,
          "resolver cache entry layout mismatch");
    std::filesystem::remove(cacheLayout.rootLayer);
    const auto recoveredLayer =
        pxr::SdfLayer::CreateAnonymous("resolver-recovered.usda");
    Check(recoveredLayer);
    Check(format->Read(recoveredLayer.operator->(), "http://memory.copc",
                       false));
    Check(!recoveredLayer->GetPrimAtPath(pxr::SdfPath(CacheSentinelPath)),
          "incomplete resolver cache entry was reused");
    Check(!std::filesystem::exists(cacheLayout.entryDirectory),
          "incomplete resolver cache entry was not invalidated");

    WriteResolverCacheEntry(cacheLayout, layer, true);
    const auto corruptedLayer =
        pxr::SdfLayer::CreateAnonymous("resolver-corrupted.usda");
    Check(corruptedLayer);
    Check(format->Read(corruptedLayer.operator->(), "http://memory.copc",
                       false));
    Check(corruptedLayer->GetPrimAtPath(pxr::SdfPath("/PointCloud")));
    Check(!std::filesystem::exists(cacheLayout.entryDirectory),
          "corrupted resolver cache entry was not invalidated");

    WriteResolverCacheEntry(cacheLayout, layer, false);

    auto changedRecords = records;
    Write(changedRecords.front(), 0, std::int32_t{9000});
    const auto changedCompressedPoints =
        WriteEquivalentLaz(changedRecords, lazPath);
    const auto changedCopcPath = WriteEquivalentCopc(changedCompressedPoints);
    std::ifstream changedInput(changedCopcPath,
                               std::ios::binary | std::ios::ate);
    Check(changedInput.good());
    const auto changedEnd = changedInput.tellg();
    Check(changedEnd > 0);
    changedInput.seekg(0, std::ios::beg);
    std::vector<std::uint8_t> changedBytes(
        static_cast<std::size_t>(changedEnd));
    changedInput.read(reinterpret_cast<char*>(changedBytes.data()),
                      static_cast<std::streamsize>(changedBytes.size()));
    Check(static_cast<bool>(changedInput));
    std::ofstream resolverReplacement(
        resolverAsset, std::ios::binary | std::ios::trunc);
    Check(resolverReplacement.good());
    resolverReplacement.write(
        reinterpret_cast<const char*>(changedBytes.data()),
        static_cast<std::streamsize>(changedBytes.size()));
    Check(resolverReplacement.good());
    resolverReplacement.close();

        const auto changedInfo = pxr::ArGetResolver().GetAssetInfo(
          "http://memory.copc", pxr::ArResolvedPath("http://memory.copc"));
        Check(!changedInfo.version.empty());
        Check(changedInfo.version != initialValidationToken,
            "resolver test asset validation token did not change");

    const auto changedLayer =
        pxr::SdfLayer::CreateAnonymous("resolver-changed.usda");
    Check(changedLayer);
    Check(format->Read(changedLayer.operator->(), "http://memory.copc", false));
    Check(!changedLayer->GetPrimAtPath(pxr::SdfPath(CacheSentinelPath)),
          "changed resolver validation token reused the old cache entry");
    const auto changedStage = pxr::UsdStage::Open(changedLayer);
    Check(changedStage);
    const auto changedPoints = pxr::UsdGeomPoints::Get(
        changedStage, pxr::SdfPath("/PointCloud"));
    pxr::VtVec3fArray changedPositions;
    Check(changedPoints.GetPointsAttr().Get(&changedPositions));
    Check(!changedPositions.empty());
    Check(changedPositions.front() != firstPositions.front(),
          "resolver read reused cached output");

    // The regenerated entry lands beside the one it supersedes, under the same
    // generation directory. That adjacency is what `resolver-identity-changed`
    // reports; without it a changed revision is indistinguishable from a source
    // that was never generated.
    const auto changedAsset = pxr::ArGetResolver().OpenAsset(
        pxr::ArResolvedPath("http://memory.copc"));
    Check(static_cast<bool>(changedAsset), "reopen changed resolver asset");
    usdgeo::cache::SourceIdentity changedIdentity;
    usdgeo::cache::ResolverIdentityStability changedStability;
    Check(usdgeo::TryBuildResolverSourceIdentity(
              pxr::ArGetResolver(), "http://memory.copc",
              pxr::ArResolvedPath("http://memory.copc"), *changedAsset,
              changedIdentity, changedStability, identityError),
          "changed resolver identity");
    Check(changedStability ==
          usdgeo::cache::ResolverIdentityStability::Stable);
    Check(changedIdentity.validationToken != initialValidationToken);
    // Rebuild the georeference from the changed source rather than reusing the
    // original: asserting adjacency against a reference this test already holds
    // would hold by construction. This fixture writes a fixed header bounding
    // box, so the rebuilt reference happens to match, and the case where a
    // revision *does* move the georeference is covered directly in
    // usdGeoCache's TestSupersededIdentityEntry.
    auto changedSource =
        std::make_shared<usdgeocopc::ArAssetRandomAccessSource>(
            changedAsset, "http://memory.copc");
    usdcopc::CopcReader changedReader(changedSource);
    usdcopc::CopcHeader changedHeader;
    std::vector<usdgeo::Diagnostic> changedDiagnostics;
    Check(changedReader.ReadMetadata(changedHeader, changedDiagnostics),
          "read changed fixture metadata");
    usdpointcloud::PointChunk changedMetadataChunk;
    usdgeo::GeoReference changedReference;
    usdgeo::SpatialBounds changedBounds;
    Check(usdlas::BuildPointCloudMetadata(
              changedHeader.las, changedMetadataChunk, changedReference,
              changedBounds, cacheErrorMessage),
          "build changed fixture metadata");
    usdgeo::cache::Layout changedLayout;
    Check(usdgeo::TryBuildPointCloudCacheLayout(
        cacheRoot, changedIdentity, changedReference, cacheRequest,
        "copc-reader-1", changedLayout, cacheErrorMessage),
        "build changed resolver cache layout");
    Check(changedLayout.entryDirectory != cacheLayout.entryDirectory,
          "changed validation identity reused the superseded entry");
    Check(changedLayout.entryDirectory.parent_path() ==
              cacheLayout.entryDirectory.parent_path(),
          "changed validation identity left the generation directory");
    Check(usdgeo::cache::HasSupersededIdentityEntry(changedLayout),
          "changed validation identity did not observe the superseded entry");

    std::error_code error;
    std::filesystem::remove_all(cacheRoot, error);
    std::filesystem::remove(lazPath, error);
    std::filesystem::remove(copcPath, error);
    std::filesystem::remove(resolverAsset, error);
}

} // namespace

int main() {
    SelectHttpResolver();
    TestArAssetRandomAccessSource();
    TestResolverCacheDiagnostic();
    RegisterPlugin(std::filesystem::path(USDGEOCOPC_HTTP_RESOLVER_SOURCE_DIR) /
                   "plugin" / "resources" / "httpresolver" /
                   "plugInfo.json");
    RegisterPlugin(std::filesystem::path(USDGEOLAS_SOURCE_DIR) /
                   "plugin" / "resources" / "pointcloud-las" /
                   "plugInfo.json");
    RegisterPlugin(std::filesystem::path(USDGEOLAZ_SOURCE_DIR) /
                   "plugin" / "resources" / "pointcloud-laz" /
                   "plugInfo.json");
    RegisterPlugin(std::filesystem::path(USDGEOCOPC_SOURCE_DIR) /
                   "plugin" / "resources" / "pointcloud-copc" /
                   "plugInfo.json");
    TestMetadataIntegration();
    TestAuthoredLodEquivalence();
    TestTiledReadReopens();
    TestResolverBackedRead();
    return 0;
}
