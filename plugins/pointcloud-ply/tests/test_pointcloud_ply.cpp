#include <pxr/base/plug/plugin.h>
#include <pxr/base/plug/registry.h>
#include <pxr/usd/sdf/fileFormat.h>
#include <pxr/usd/sdf/layer.h>
#include <pxr/usd/pcp/dynamicFileFormatInterface.h>
#include <pxr/usd/usd/stage.h>
#include <pxr/usd/usdGeom/points.h>

#include <cstdlib>
#include <filesystem>

namespace {

void Check(bool condition) {
    if (!condition) {
        std::abort();
    }
}

void TestCorpusAsset(const std::filesystem::path& relativePath,
                     std::size_t expectedPointCount) {
    const auto source = std::filesystem::path(USDGEOPLY_SOURCE_DIR) /
                        "tests" / "corpus" / relativePath;
    const pxr::SdfLayer::FileFormatArguments arguments = {{"epsg", "4978"}};
    const auto layer = pxr::SdfLayer::FindOrOpen(source.string(), arguments);
    Check(layer);
    const auto stage = pxr::UsdStage::Open(layer);
    Check(stage);
    const auto points = pxr::UsdGeomPoints::Get(
        stage, pxr::SdfPath("/PointCloud"));
    Check(points.GetPrim().IsValid());

    pxr::VtVec3fArray positions;
    Check(points.GetPointsAttr().Get(&positions));
    Check(positions.size() == expectedPointCount);
}

void TestFileFormatIntegration() {
    const auto plugInfo = std::filesystem::path(USDGEOPLY_SOURCE_DIR) /
                          "plugin" / "resources" / "pointcloud-ply" /
                          "plugInfo.json";
    const auto plugins = pxr::PlugRegistry::GetInstance().RegisterPlugins(
        plugInfo.string());
    Check(plugins.size() == 1);
    Check(plugins.front()->Load());
    const auto format = pxr::SdfFileFormat::FindByExtension("sample.ply");
    Check(format);
    Check(dynamic_cast<const pxr::PcpDynamicFileFormatInterface*>(
        format.operator->()));

    const auto source = std::filesystem::path(USDGEOPLY_SOURCE_DIR) /
                        "tests" / "fixtures" / "conformance.ply";
    const pxr::SdfLayer::FileFormatArguments arguments = {{"epsg", "4978"}};
    const auto layer = pxr::SdfLayer::FindOrOpen(source.string(), arguments);
    Check(layer);
    const auto stage = pxr::UsdStage::Open(layer);
    Check(stage);
    const auto points = pxr::UsdGeomPoints::Get(
        stage, pxr::SdfPath("/PointCloud"));
    Check(points.GetPrim().IsValid());

    pxr::VtVec3fArray positions;
    Check(points.GetPointsAttr().Get(&positions));
    Check(positions.size() == 2);
    Check(positions[0] == pxr::GfVec3f(1.0f, 2.0f, 3.0f));
    Check(points.GetPrim().GetAttribute(pxr::TfToken("geo:red")).IsValid());
    pxr::VtVec3fArray displayColors;
    Check(points.GetDisplayColorPrimvar().Get(&displayColors));
    Check(displayColors.size() == 2 &&
          displayColors[0] == pxr::GfVec3f(10.0f / 255.0f,
                                             20.0f / 255.0f,
                                             30.0f / 255.0f));

    const pxr::SdfLayer::FileFormatArguments selectedArguments = {
        {"attributes", "temperature"}, {"epsg", "4978"}};
    const auto selectedLayer = pxr::SdfLayer::FindOrOpen(
        source.string(), selectedArguments);
    Check(selectedLayer);
    Check(selectedLayer->GetAttributeAtPath(
              pxr::SdfPath("/PointCloud.geo:temperature")) != nullptr);
    Check(selectedLayer->GetAttributeAtPath(
              pxr::SdfPath("/PointCloud.geo:red")) == nullptr);

    TestCorpusAsset(
        std::filesystem::path("stanford-bunny") /
            "stanford-bunny-thinned-4096.ply",
        4096);

    const auto tiledPayloadDirectory =
        source.parent_path() / "usd_geo_ply_tiled_payloads";
    std::filesystem::remove_all(tiledPayloadDirectory);
    const pxr::SdfLayer::FileFormatArguments tiledArguments = {
        {"epsg", "4978"},
        {"payloadDirectory", tiledPayloadDirectory.string()},
        {"tile", "true"},
        {"tileMemoryLimit", "1"},
        {"tileSize", "10"}};
    const auto tiledLayer = pxr::SdfLayer::FindOrOpen(source.string(),
                                                       tiledArguments);
    Check(tiledLayer);
    const auto tiledStage = pxr::UsdStage::Open(tiledLayer);
    Check(tiledStage);
    Check(tiledStage->GetPrimAtPath(pxr::SdfPath(
              "/PointCloud/Tiles/Tile_L0_p0_p0_p0/LOD0"))
              .IsValid());
    // The layer publishes its payloads in a directory of its own below the
    // requested one.
    bool hasPayload = false;
    for (const auto& entry : std::filesystem::recursive_directory_iterator(
             tiledPayloadDirectory)) {
        hasPayload = hasPayload ||
                     entry.path().filename() == "Tile_L0_p0_p0_p0_LOD0.usdc";
    }
    Check(hasPayload);
    std::filesystem::remove_all(tiledPayloadDirectory);
}

} // namespace

int main() {
    TestFileFormatIntegration();
    return 0;
}