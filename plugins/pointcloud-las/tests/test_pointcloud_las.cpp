#include <pxr/base/vt/array.h>
#include <pxr/base/plug/plugin.h>
#include <pxr/base/plug/registry.h>
#include <pxr/base/tf/errorMark.h>
#include <pxr/usd/sdf/fileFormat.h>
#include <pxr/usd/sdf/layer.h>
#include <pxr/usd/pcp/dynamicFileFormatInterface.h>
#include <pxr/usd/usdGeom/points.h>
#include <pxr/usd/usdLod/rootAPI.h>

#include <cstdint>
#include <chrono>
#include <cstdlib>
#include <cstdio>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <string>
#include <vector>

namespace {

void Check(bool condition) {
    if (!condition) {
        std::abort();
    }
}

template <typename T>
void Write(std::vector<std::uint8_t>& bytes, std::size_t offset, T value) {
    std::memcpy(bytes.data() + offset, &value, sizeof(T));
}

std::vector<std::uint8_t> MakeFixture() {
    constexpr std::size_t headerSize = 227;
    constexpr std::size_t recordLength = 34;
    constexpr std::size_t pointCount = 2;
    const std::string wkt = "WKT[\"EPSG:4978\"]";
    const auto pointDataOffset = headerSize + 54 + wkt.size() + 1;
    std::vector<std::uint8_t> bytes(pointDataOffset + recordLength * pointCount,
                                    0);
    std::memcpy(bytes.data(), "LASF", 4);
    Write(bytes, 24, std::uint8_t{1});
    Write(bytes, 25, std::uint8_t{2});
    Write(bytes, 94, static_cast<std::uint16_t>(headerSize));
    Write(bytes, 96, static_cast<std::uint32_t>(pointDataOffset));
    Write(bytes, 100, std::uint32_t{1});
    Write(bytes, 104, std::uint8_t{3});
    Write(bytes, 105, static_cast<std::uint16_t>(recordLength));
    Write(bytes, 107, static_cast<std::uint32_t>(pointCount));
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
    Write(bytes, 211, 3003.0);
    Write(bytes, 219, 3000.0);

        const auto vlrOffset = headerSize;
        std::memcpy(bytes.data() + vlrOffset + 2, "LASF_Projection", 15);
        Write(bytes, vlrOffset + 18, std::uint16_t{2112});
        Write(bytes, vlrOffset + 20,
            static_cast<std::uint16_t>(wkt.size() + 1));
        std::memcpy(bytes.data() + vlrOffset + 54, wkt.c_str(), wkt.size() + 1);

    const auto record = [&](std::size_t offset, std::int32_t x,
                            std::int32_t y, std::int32_t z,
                            std::uint16_t intensity,
                            std::uint8_t classification, double gpsTime) {
        Write(bytes, offset, x);
        Write(bytes, offset + 4, y);
        Write(bytes, offset + 8, z);
        Write(bytes, offset + 12, intensity);
        Write(bytes, offset + 14, std::uint8_t{0x21});
        Write(bytes, offset + 15, classification);
        Write(bytes, offset + 16, static_cast<std::int8_t>(-12));
        Write(bytes, offset + 17, std::uint8_t{9});
        Write(bytes, offset + 18, std::uint16_t{44});
        Write(bytes, offset + 20, gpsTime);
        Write(bytes, offset + 28, static_cast<std::uint16_t>(100));
        Write(bytes, offset + 30, static_cast<std::uint16_t>(200));
        Write(bytes, offset + 32, static_cast<std::uint16_t>(300));
    };
    record(pointDataOffset, 0, 0, 0, 42, 2, 12.5);
    record(pointDataOffset + recordLength, 100, 100, 300, 84, 5, 25.0);
    return bytes;
}

// Tiled reads publish their payloads in a directory of their own below the
// requested one, so tests find a payload by name.
bool HasPayload(const std::filesystem::path& directory, const std::string& name) {
    std::error_code error;
    for (std::filesystem::recursive_directory_iterator entry(directory, error),
         end;
         !error && entry != end; entry.increment(error)) {
        if (entry->path().filename() == name) {
            return true;
        }
    }
    return false;
}

void TestFileFormatIntegration() {
    const auto plugInfo = std::filesystem::path(USDGEOLAS_SOURCE_DIR) /
                          "plugin" / "resources" / "pointcloud-las" /
                          "plugInfo.json";
    const auto plugins = pxr::PlugRegistry::GetInstance().RegisterPlugins(
        plugInfo.string());
    Check(plugins.size() == 1);
    Check(plugins.front()->Load());
    const auto format = pxr::SdfFileFormat::FindByExtension("sample.las");
    Check(format);
    Check(dynamic_cast<const pxr::PcpDynamicFileFormatInterface*>(
              format.operator->()));

    const auto path = std::filesystem::temp_directory_path() /
                      "usd_pointcloud_plugins_conformance.las";
    {
        const auto bytes = MakeFixture();
        std::ofstream output(path, std::ios::binary);
        Check(output.good());
        output.write(reinterpret_cast<const char*>(bytes.data()),
                     static_cast<std::streamsize>(bytes.size()));
        Check(output.good());
    }

    const auto layer = pxr::SdfLayer::FindOrOpen(path.string());
    Check(layer);
    Check(layer->GetPrimAtPath(pxr::SdfPath("/PointCloud")));
    const auto stage = pxr::UsdStage::Open(layer);
    Check(stage);
    const auto points = pxr::UsdGeomPoints::Get(
        stage, pxr::SdfPath("/PointCloud"));
    Check(points.GetPrim().IsValid());

    pxr::VtVec3fArray positions;
    Check(points.GetPointsAttr().Get(&positions));
    Check(positions.size() == 2);
    Check(positions[1] == pxr::GfVec3f(1.0f, 3.0f, -1.0f));

    pxr::VtIntArray intensity;
    const auto intensityAttribute = layer->GetAttributeAtPath(
        pxr::SdfPath("/PointCloud.geo:intensity"));
    Check(intensityAttribute != nullptr);
    const auto intensityValue = intensityAttribute->GetDefaultValue();
    Check(intensityValue.IsHolding<pxr::VtIntArray>());
    intensity = intensityValue.UncheckedGet<pxr::VtIntArray>();
    Check(intensity.size() == 2 && intensity[0] == 42 && intensity[1] == 84);

    pxr::VtArray<unsigned char> classification;
    Check(points.GetPrim()
              .GetAttribute(pxr::TfToken("geo:classification"))
              .Get(&classification));
    Check(classification.size() == 2 && classification[0] == 2 &&
          classification[1] == 5);
        Check(layer->GetAttributeAtPath(
              pxr::SdfPath("/PointCloud.geo:classificationFlags")) == nullptr);
        Check(layer->GetAttributeAtPath(
              pxr::SdfPath("/PointCloud.geo:scannerChannel")) == nullptr);

    pxr::VtIntArray scanAngle;
    Check(points.GetPrim().GetAttribute(pxr::TfToken("geo:scanAngle"))
              .Get(&scanAngle));
    Check(scanAngle.size() == 2 && scanAngle[0] == -12);
    pxr::VtArray<unsigned char> userData;
    Check(points.GetPrim().GetAttribute(pxr::TfToken("geo:userData"))
              .Get(&userData));
    Check(userData.size() == 2 && userData[1] == 9);
    pxr::VtIntArray pointSourceId;
    Check(points.GetPrim().GetAttribute(pxr::TfToken("geo:pointSourceId"))
              .Get(&pointSourceId));
    Check(pointSourceId.size() == 2 && pointSourceId[0] == 44);

    const auto wktAttribute = layer->GetAttributeAtPath(
        pxr::SdfPath("/PointCloud.geo:wkt"));
    Check(wktAttribute != nullptr);
    Check(wktAttribute->GetDefaultValue().IsHolding<std::string>());
    Check(wktAttribute->GetDefaultValue().UncheckedGet<std::string>() ==
          "WKT[\"EPSG:4978\"]");

    const pxr::SdfLayer::FileFormatArguments arguments = {
        {"attributes", "intensity"},
        {"rangeFirstPoint", "1"},
        {"rangePointCount", "1"}};
    const auto rangedLayer = pxr::SdfLayer::FindOrOpen(
        path.string(), arguments);
    Check(rangedLayer);
    const auto rangedStage = pxr::UsdStage::Open(rangedLayer);
    Check(rangedStage);
    const auto rangedPoints = pxr::UsdGeomPoints::Get(
        rangedStage, pxr::SdfPath("/PointCloud"));
    pxr::VtVec3fArray rangedPositions;
    Check(rangedPoints.GetPointsAttr().Get(&rangedPositions));
    Check(rangedPositions.size() == 1 &&
          rangedPositions[0] == pxr::GfVec3f(1.0f, 3.0f, -1.0f));
    const auto rangedIntensity = rangedLayer->GetAttributeAtPath(
        pxr::SdfPath("/PointCloud.geo:intensity"));
    Check(rangedIntensity != nullptr);
    Check(rangedIntensity->GetDefaultValue().IsHolding<pxr::VtIntArray>());
    Check(rangedIntensity->GetDefaultValue()
              .UncheckedGet<pxr::VtIntArray>()
              .size() == 1);
    Check(rangedLayer->GetAttributeAtPath(
              pxr::SdfPath("/PointCloud.geo:classification")) == nullptr);

    const auto tiledPayloadDirectory =
        std::filesystem::temp_directory_path() / "usd_geo_las_tiled_payloads" /
        "tiles";
    std::filesystem::remove_all(tiledPayloadDirectory);
    const pxr::SdfLayer::FileFormatArguments tiledArguments = {
        {"payloadDirectory", tiledPayloadDirectory.string()},
        {"tile", "true"},
        {"tileMemoryLimit", "1"},
        {"tileSize", "1"}};
    // The second open reads the layer again once the first is released, over
    // the payloads the first open generated.
    for (int open = 0; open < 2; ++open) {
        const auto tiledLayer = pxr::SdfLayer::FindOrOpen(
            path.string(), tiledArguments);
        Check(tiledLayer);
        const auto tiledStage = pxr::UsdStage::Open(tiledLayer);
        Check(tiledStage);
        Check(tiledStage->GetPrimAtPath(pxr::SdfPath(
                  "/PointCloud/Tiles/Tile_L0_p1000_p2000_p0/LOD0"))
                  .IsValid());
        Check(tiledStage->GetPrimAtPath(pxr::SdfPath(
                  "/PointCloud/Tiles/Tile_L0_p1001_p2001_p0/LOD0"))
                  .IsValid());
        Check(HasPayload(tiledPayloadDirectory,
                         "Tile_L0_p1000_p2000_p0_LOD0.usdc"));
        Check(HasPayload(tiledPayloadDirectory,
                         "Tile_L0_p1001_p2001_p0_LOD0.usdc"));
    }
    Check(!pxr::SdfLayer::Find(path.string(), tiledArguments));
    std::filesystem::remove_all(tiledPayloadDirectory);

    const auto metadataLayer =
        pxr::SdfLayer::CreateAnonymous("metadata.las");
    Check(metadataLayer);
    Check(format->Read(metadataLayer.operator->(), path.string(), true));
    const auto metadataStage = pxr::UsdStage::Open(metadataLayer);
    Check(metadataStage);
    const auto metadataPoints = pxr::UsdGeomPoints::Get(
        metadataStage, pxr::SdfPath("/PointCloud"));
    Check(metadataPoints.GetPrim().IsValid());
    pxr::VtVec3fArray metadataPositions;
    Check(metadataPoints.GetPointsAttr().Get(&metadataPositions));
    Check(metadataPositions.empty());
    std::uint64_t metadataPointCount = 0;
    Check(metadataPoints.GetPrim()
              .GetAttribute(pxr::TfToken("geo:pointCount"))
              .Get(&metadataPointCount));
    Check(metadataPointCount == 2);
    bool metadataOnlyValue = false;
    Check(metadataPoints.GetPrim()
              .GetAttribute(pxr::TfToken("geo:metadataOnly"))
              .Get(&metadataOnlyValue));
    Check(metadataOnlyValue);

    const auto dynamicPath = std::filesystem::temp_directory_path() /
                             "usd_pointcloud_plugins_dynamic_lod.usda";
    {
        std::ofstream output(dynamicPath);
        Check(output.good());
        output << "#usda 1.0\n"
               << "def \"Survey\" (\n"
               << "    prepend payload = @" << path.generic_string()
               << "@</PointCloud>\n"
               << "    pc_las_lod = \"balanced\"\n"
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
    Check(survey.SetMetadata(
        pxr::TfToken("pc_las_lod"), pxr::VtValue(pxr::TfToken("off"))));
    Check(!survey.HasAPI<pxr::UsdLodRootAPI>());
    std::filesystem::remove(dynamicPath);

    pxr::VtArray<std::string> availableAttributes;
    Check(metadataPoints.GetPrim()
              .GetAttribute(pxr::TfToken("geo:availableAttributes"))
              .Get(&availableAttributes));
    const auto hasAttribute = [&](const std::string& name) {
        for (const auto& attribute : availableAttributes) {
            if (attribute == name) {
                return true;
            }
        }
        return false;
    };
    Check(hasAttribute("xyz"));
    Check(hasAttribute("intensity"));
    Check(hasAttribute("classification"));
    Check(hasAttribute("red"));
    Check(hasAttribute("green"));
    Check(hasAttribute("blue"));
    Check(hasAttribute("gpsTime"));

    std::error_code error;
    std::filesystem::remove(path, error);
}

void TestCheckedInAsset(const std::filesystem::path& path,
                        std::size_t expectedPointCount) {
    const auto layer = pxr::SdfLayer::FindOrOpen(path.string());
    Check(layer);
    const auto stage = pxr::UsdStage::Open(layer);
    Check(stage);
    const auto points = pxr::UsdGeomPoints::Get(
        stage, pxr::SdfPath("/PointCloud"));
    Check(points.GetPrim().IsValid());

    pxr::VtVec3fArray positions;
    Check(points.GetPointsAttr().Get(&positions));
    Check(positions.size() == expectedPointCount);

    const auto intensityAttribute = layer->GetAttributeAtPath(
        pxr::SdfPath("/PointCloud.geo:intensity"));
    Check(intensityAttribute != nullptr);
    const auto intensity = intensityAttribute->GetDefaultValue();
    Check(intensity.IsHolding<pxr::VtIntArray>());
    Check(intensity.UncheckedGet<pxr::VtIntArray>().size() ==
          expectedPointCount);
}

void TestCheckedInAssets() {
    const auto root = std::filesystem::path(USDGEOLAS_SOURCE_DIR) / "tests";
    TestCheckedInAsset(root / "fixtures" / "conformance.las", 2);
    TestCheckedInAsset(
        root / "corpus" / "virtual-shizuoka-2019" /
            "virtual-shizuoka-08NF2330-thinned-4096.las",
        4096);
    TestCheckedInAsset(
        root / "corpus" / "usgs-3dep-2020" /
            "usgs-3dep-2020-thinned-4096.las",
        4096);
}

void TestMissingFileDiagnostic() {
    pxr::TfErrorMark mark;
    const auto uniqueSuffix =
        std::chrono::high_resolution_clock::now().time_since_epoch().count();
    const auto path = std::filesystem::temp_directory_path() /
                      ("usd_pointcloud_plugins_missing_" +
                       std::to_string(uniqueSuffix) + ".las");
    const auto format = pxr::SdfFileFormat::FindByExtension("sample.las");
    Check(format);
    const auto layer = pxr::SdfLayer::CreateAnonymous("missing.las");
    Check(layer);
    Check(!format->Read(layer.operator->(), path.string(), false));
    Check(!mark.IsClean());
    Check(mark.GetBegin()->GetCommentary().find("[LAS002]") !=
          std::string::npos);
    mark.Clear();
}

} // namespace

int main() {
    TestFileFormatIntegration();
    TestMissingFileDiagnostic();
    TestCheckedInAssets();
    return 0;
}