#include "usdpointcloud/PointCloud.h"
#include "usdpointcloud/FileFormatArguments.h"
#include "usdpointcloud/Lod.h"
#include "usdpointcloud/Sampling.h"

#include <cstdlib>
#include <cmath>
#include <limits>
#include <map>
#include <string>
#include <vector>

namespace {

void Check(bool condition) {
    if (!condition) {
        std::abort();
    }
}

void TestAttributesAndChunks() {
    usdpointcloud::PointAttribute position{"position",
                                           usdpointcloud::PointAttributeType::Float64};
    Check(position.IsValid());
    const usdpointcloud::PointAttribute invalidAttribute{"", position.type};
    Check(!invalidAttribute.IsValid());

    usdpointcloud::PointChunk chunk;
    Check(chunk.IsValid());

    chunk.pointCount = 2;
    chunk.bounds.Expand({1.0, 2.0, 3.0});
    chunk.bounds.Expand({4.0, 5.0, 6.0});
    chunk.attributes = {position, {"classification",
                                   usdpointcloud::PointAttributeType::UInt8}};
    Check(chunk.IsValid());

    chunk.attributes.push_back(position);
    Check(!chunk.IsValid());
}

void TestInvalidChunk() {
    usdpointcloud::PointChunk chunk;
    chunk.pointCount = 1;
    Check(!chunk.IsValid());

    chunk.bounds.Expand({0.0, 0.0, 0.0});
    chunk.attributes = {{"", usdpointcloud::PointAttributeType::Float32}};
    Check(!chunk.IsValid());
}

void TestPointDataAndAssetChunk() {
    usdpointcloud::PointData data;
    data.positions = {{1.0, 2.0, 3.0}, {4.0, 5.0, 6.0}};
    data.intensity = {10, 20};
    data.classification = {2, 5};
    Check(data.IsValid());

    usdgeo::SpatialBounds bounds;
    bounds.Expand(data.positions[0]);
    bounds.Expand(data.positions[1]);
    const auto chunk = usdpointcloud::MakePointChunk(data, bounds);
    Check(chunk.IsValid() && chunk.pointCount == 2);
    Check(chunk.attributes.size() == 2);
    Check(chunk.attributes[0].name == "intensity");
    Check(chunk.attributes[1].name == "classification");

    data.red = {1};
    Check(!data.IsValid());
}

void TestMultiScanContract() {
    usdgeo::GeoReference reference;
    reference.epsgCode = 4978;
    reference.localOrigin = {10.0, 20.0, 30.0};

    usdpointcloud::PointData data;
    data.positions = {{1.0, 2.0, 3.0}};
    usdgeo::SpatialBounds bounds;
    bounds.Expand(data.positions.front());

    usdpointcloud::PointCloudScan first;
    first.id = "scan-1";
    first.asset = {reference, bounds,
                   usdpointcloud::MakePointChunk(data, bounds), data};
    Check(first.IsValid());

    usdpointcloud::PointCloudScan second = first;
    second.id = "scan-2";
    second.pose[3] = 4.0;
    Check(second.IsValid());

    usdpointcloud::PointCloudCollection collection;
    collection.reference = reference;
    collection.bounds = bounds;
    collection.scans = {first, second};
    Check(!collection.IsValid());

    collection.bounds.maximum.x = 5.0;
    Check(collection.IsValid());

    second.id = first.id;
    collection.scans[1] = second;
    Check(!collection.IsValid());

    collection.scans[1] = first;
    collection.scans[1].pose[15] = 2.0;
    Check(!collection.scans[1].IsValid());

    collection.scans[1] = first;
    collection.scans[1].pose[0] = 0.5;
    Check(!collection.scans[1].IsValid());
}

void TestExtraByteNameNormalization() {
    const auto names = usdpointcloud::NormalizeExtraByteNames(
        {"air temperature", "2nd-return", "intensity", "air-temperature",
         "", "air_temperature"});
    Check(names == std::vector<std::string>{
                       "air_temperature", "_2nd_return", "intensity_2",
                       "air_temperature_2", "extra", "air_temperature_3"});
    Check(usdpointcloud::NormalizeExtraByteNames(
              {std::string("\xC3\xA9") + "levation"}) ==
          std::vector<std::string>{"__levation"});

    usdpointcloud::PointData data;
    data.positions = {{0.0, 0.0, 0.0}};
    data.extraByteNames = {"air temperature", "intensity"};
    data.extraBytes = {{12.0}, {24.0}};
    usdgeo::SpatialBounds bounds;
    bounds.Expand(data.positions.front());
    const auto chunk = usdpointcloud::MakePointChunk(data, bounds);
    Check(chunk.IsValid());
    Check(chunk.attributes.size() == 2 &&
          chunk.attributes[0].name == "air_temperature" &&
          chunk.attributes[0].type ==
              usdpointcloud::PointAttributeType::Float64 &&
          chunk.attributes[1].name == "intensity_2" &&
          chunk.attributes[1].type ==
              usdpointcloud::PointAttributeType::Float64);

    data.extraByteNames = {"normal"};
    data.extraByteComponentCounts = {3};
    data.extraBytes = {{1.0, 2.0, 3.0}};
    const auto vectorChunk = usdpointcloud::MakePointChunk(data, bounds);
    Check(data.IsValid() && vectorChunk.attributes.size() == 1 &&
          vectorChunk.attributes.front().type ==
              usdpointcloud::PointAttributeType::Float64Vec3);

    data.extraByteNames = {"normal", "incomplete"};
    data.extraBytes = {{1.0, 2.0}, {3.0}};
    data.extraByteComponentCounts = {2};
    Check(!data.IsValid());
    const auto invalidChunk = usdpointcloud::MakePointChunk(data, bounds);
    Check(invalidChunk.attributes.size() == 1 &&
          invalidChunk.attributes.front().type ==
              usdpointcloud::PointAttributeType::Float64Vec2);
}

void TestReadOptions() {
    usdpointcloud::PointReadOptions options;
    Check(options.IsValid());

    options.range = {4, 3};
    Check(options.IsValid());
    options.range = {(std::numeric_limits<std::uint64_t>::max)(), 2};
    Check(!options.IsValid());
    options.range = {};
    options.memoryBudgetBytes = 0;
    Check(!options.IsValid());

    options.memoryBudgetBytes = 64 * 1024 * 1024;
    options.bounds = usdgeo::SpatialBounds::Empty();
    Check(!options.IsValid());
    options.bounds = usdgeo::SpatialBounds{
        {std::numeric_limits<double>::quiet_NaN(), 0.0, 0.0},
        {1.0, 1.0, 1.0}};
    Check(!options.IsValid());
}

void TestPointStreamContract() {
    class TestStream final : public usdpointcloud::PointStream {
    public:
        usdpointcloud::PointStreamStatus ReadNext(
            usdpointcloud::PointChunk& chunk,
            usdpointcloud::PointData& data,
            usdgeo::Diagnostic& diagnostic) override {
            diagnostic = {};
            if (readCount++ == 0) {
                chunk.pointCount = 1;
                data.positions = {{1.0, 2.0, 3.0}};
                return usdpointcloud::PointStreamStatus::Chunk;
            }
            return usdpointcloud::PointStreamStatus::End;
        }

    private:
        std::size_t readCount = 0;
    } stream;

    usdpointcloud::PointChunk chunk;
    usdpointcloud::PointData data;
    usdgeo::Diagnostic diagnostic;
    Check(stream.ReadNext(chunk, data, diagnostic) ==
          usdpointcloud::PointStreamStatus::Chunk);
    Check(chunk.pointCount == 1 && data.positions.size() == 1);
    Check(stream.ReadNext(chunk, data, diagnostic) ==
          usdpointcloud::PointStreamStatus::End);

    class ErrorStream final : public usdpointcloud::PointStream {
    public:
        usdpointcloud::PointStreamStatus ReadNext(
            usdpointcloud::PointChunk&,
            usdpointcloud::PointData&,
            usdgeo::Diagnostic& diagnostic) override {
            diagnostic.message = "test failure";
            return usdpointcloud::PointStreamStatus::Error;
        }
    } errorStream;
    Check(errorStream.ReadNext(chunk, data, diagnostic) ==
          usdpointcloud::PointStreamStatus::Error);
    Check(diagnostic.message == "test failure");
}

void TestFileFormatArgumentNormalization() {
    std::map<std::string, std::string> arguments = {
        {"attributes", "rgb, xyz"},
        {"chunkPointLimit", "2"},
        {"rangeFirstPoint", "4"},
        {"rangePointCount", "3"}};
    usdpointcloud::PointReadRequest request;
    std::vector<usdgeo::Diagnostic> diagnostics;
    Check(usdpointcloud::NormalizeFileFormatArguments(
        arguments, request, diagnostics));
    Check(diagnostics.empty());
    Check(request.readOptions.chunkPointLimit == 2);
    Check(request.readOptions.range.firstPoint == 4);
    Check(request.readOptions.range.pointCount == 3);
    Check(request.normalizedArguments ==
          "chunkPointLimit=2&rangeFirstPoint=4&rangePointCount=3&attributes=blue,green,red,xyz");
    Check(request.canonicalArguments ==
          std::map<std::string, std::string>{
              {"attributes", "blue,green,red,xyz"},
              {"chunkPointLimit", "2"},
              {"rangeFirstPoint", "4"},
              {"rangePointCount", "3"}});

    arguments = {{"bounds", "0, 1, -2, 10, 11, 12"},
                 {"classification", "5, 2, 5"}};
    Check(usdpointcloud::NormalizeFileFormatArguments(
        arguments, request, diagnostics));
    Check(request.readOptions.bounds &&
          request.readOptions.bounds->minimum.y == 1.0 &&
          request.readOptions.bounds->maximum.z == 12.0 &&
          request.readOptions.classifications ==
              std::vector<std::uint8_t>{2, 5});
    Check(request.normalizedArguments ==
          "bounds=0.000000,1.000000,-2.000000,10.000000,11.000000,12.000000&classification=2,5");

    arguments = {{"bounds", "0,1,2,1,0,3"}};
    Check(!usdpointcloud::NormalizeFileFormatArguments(
        arguments, request, diagnostics));
    Check(diagnostics.front().code ==
          usdgeo::DiagnosticCode::InvalidFormatArgument);
    arguments = {{"bounds", "0,1,2,3,4,5,6"}};
    Check(!usdpointcloud::NormalizeFileFormatArguments(
        arguments, request, diagnostics));
    Check(diagnostics.front().code ==
          usdgeo::DiagnosticCode::InvalidFormatArgument);
    arguments = {{"classification", "256"}};
    Check(!usdpointcloud::NormalizeFileFormatArguments(
        arguments, request, diagnostics));
    Check(diagnostics.front().code ==
          usdgeo::DiagnosticCode::InvalidFormatArgument);

    arguments = {{"lod", " balanced "}};
    Check(usdpointcloud::NormalizeFileFormatArguments(
        arguments, request, diagnostics));
    Check(request.lodProfile == usdpointcloud::LodProfile::Balanced);
    Check(request.normalizedArguments == "lod=balanced");
    arguments = {{"lod", "quality"}};
    Check(usdpointcloud::NormalizeFileFormatArguments(
        arguments, request, diagnostics));
    Check(request.lodProfile == usdpointcloud::LodProfile::Quality);
    Check(request.normalizedArguments == "lod=quality");
    arguments = {{"lod", "off"}};
    Check(usdpointcloud::NormalizeFileFormatArguments(
        arguments, request, diagnostics));
    Check(request.lodProfile == usdpointcloud::LodProfile::Off &&
          request.normalizedArguments.empty());

        arguments = {{"payloadDirectory", "tiles"},
                 {"tile", "true"},
                 {"tileMemoryLimit", "1024"},
                 {"tileSize", "2"}};
        Check(usdpointcloud::NormalizeFileFormatArguments(
          arguments, request, diagnostics));
        Check(request.tiled && request.tileSize == 2.0 &&
            request.tileMemoryLimitBytes == 1024 &&
            request.payloadDirectory == "tiles");
        Check(request.normalizedArguments ==
                "tile=true&tileSize=2.000000&tileMemoryLimit=1024&payloadDirectory=tiles");

          arguments = {{"maxPointsPerTile", "128"},
                   {"minPointsPerTile", "16"},
                   {"maxDepth", "6"},
                   {"payloadDirectory", "adaptive"}};
          Check(usdpointcloud::NormalizeFileFormatArguments(
            arguments, request, diagnostics));
          Check(request.tiled && request.maxPointsPerTile == 128 &&
              request.minPointsPerTile == 16 && request.maxTileDepth == 6 &&
              request.tileSize == 0.0);
          Check(request.normalizedArguments ==
              "tile=true&maxPointsPerTile=128&minPointsPerTile=16&maxDepth=6&payloadDirectory=adaptive");

          arguments = {{"maxPointsPerTile", "128"},
                   {"tileSize", "2"},
                   {"payloadDirectory", "adaptive"}};
          Check(!usdpointcloud::NormalizeFileFormatArguments(
            arguments, request, diagnostics));
          Check(diagnostics.front().code ==
              usdgeo::DiagnosticCode::ConflictingFormatArguments);

          arguments = {{"maxPointsPerTile", "128"},
                   {"maxDepth", "32"},
                   {"payloadDirectory", "adaptive"}};
          Check(!usdpointcloud::NormalizeFileFormatArguments(
            arguments, request, diagnostics));
          Check(diagnostics.front().code ==
              usdgeo::DiagnosticCode::InvalidFormatArgument);

        arguments = {{"lod", "preview"}, {"payloadDirectory", "tiles"},
                 {"tileSize", "2"}};
        Check(!usdpointcloud::NormalizeFileFormatArguments(
          arguments, request, diagnostics));
        Check(diagnostics.front().code ==
            usdgeo::DiagnosticCode::ConflictingFormatArguments);

    arguments = {{"chunkPointLimit", "065536"},
                 {"memoryBudgetBytes", "67108864"},
                 {"rangePointCount", "0"}};
    Check(usdpointcloud::NormalizeFileFormatArguments(
        arguments, request, diagnostics));
    Check(request.canonicalArguments.empty());

    std::map<std::string, std::string> encoded;
    std::string error;
    Check(usdpointcloud::ParseFileFormatArgumentString(
        "rangePointCount=3&rangeFirstPoint=4", encoded, error));
    Check(encoded["rangeFirstPoint"] == "4");

    arguments = {{"unknown", "value"}};
    Check(!usdpointcloud::NormalizeFileFormatArguments(
        arguments, request, diagnostics));
    Check(diagnostics.size() == 1 &&
          diagnostics.front().code == usdgeo::DiagnosticCode::UnknownFormatArgument);

    arguments = {{"epsg", "4978"}};
    Check(!usdpointcloud::NormalizeFileFormatArguments(
        arguments, request, diagnostics, usdpointcloud::PointReadFormat::Las));
    Check(diagnostics.size() == 1 &&
          diagnostics.front().code == usdgeo::DiagnosticCode::UnknownFormatArgument);
    Check(usdpointcloud::NormalizeFileFormatArguments(
        arguments, request, diagnostics, usdpointcloud::PointReadFormat::Ply));
    Check(request.epsgCode && *request.epsgCode == 4978 &&
          request.normalizedArguments == "epsg=4978");

    arguments = {{"attributes", "temperature"}, {"epsg", "4978"}};
    Check(usdpointcloud::NormalizeFileFormatArguments(
        arguments, request, diagnostics, usdpointcloud::PointReadFormat::Ply));
    Check(request.attributes ==
              std::vector<std::string>{"temperature", "xyz"} &&
          request.normalizedArguments ==
              "epsg=4978&attributes=temperature,xyz");
}

void TestAttributeSelection() {
    usdpointcloud::PointData data;
    data.positions = {{1.0, 2.0, 3.0}, {4.0, 5.0, 6.0}};
    data.intensity = {10, 20};
    data.red = {1, 2};
    data.green = {3, 4};
    data.blue = {5, 6};
    std::string error;
    Check(usdpointcloud::SelectPointDataAttributes(
        data, {"blue", "green", "red", "xyz"}, error));
    Check(data.positions.size() == 2 && data.intensity.empty() &&
          data.red.size() == 2 && data.green.size() == 2 &&
          data.blue.size() == 2);
}

void TestLodContracts() {
    usdgeo::SpatialBounds bounds;
    bounds.Expand({0.0, 0.0, 0.0});
    bounds.Expand({10.0, 10.0, 10.0});
    usdgeo::SpatialBounds previewBounds;
    previewBounds.Expand({1.0, 1.0, 1.0});
    previewBounds.Expand({9.0, 9.0, 9.0});

    const usdpointcloud::PointLodItem detailed{0, 100, bounds, {0, 100}};
    const usdpointcloud::PointLodItem preview{
        1, 25, previewBounds, {0, 100}};
    usdpointcloud::PointLodHierarchy hierarchy{
        bounds, {detailed, preview}, 0, {0.25F}};
    std::vector<usdgeo::Diagnostic> diagnostics;
    Check(usdpointcloud::ValidatePointLodHierarchy(hierarchy, diagnostics));
    Check(diagnostics.empty() && hierarchy.IsValid());

    hierarchy.screenSizeThresholds = {2.0F};
    Check(usdpointcloud::ValidatePointLodHierarchy(hierarchy, diagnostics));
    hierarchy.screenSizeThresholds = {0.0F};
    Check(!usdpointcloud::ValidatePointLodHierarchy(hierarchy, diagnostics));
    Check(diagnostics.front().code == usdgeo::DiagnosticCode::InvalidLodHierarchy);
    hierarchy.screenSizeThresholds = {
        std::numeric_limits<float>::quiet_NaN()};
    Check(!usdpointcloud::ValidatePointLodHierarchy(hierarchy, diagnostics));
    hierarchy.screenSizeThresholds = {0.25F};

    hierarchy.defaultIndex = 2;
    Check(!usdpointcloud::ValidatePointLodHierarchy(hierarchy, diagnostics));
    Check(diagnostics.front().code == usdgeo::DiagnosticCode::InvalidLodHierarchy);
    hierarchy.defaultIndex = 0;

    hierarchy.items[1].sourceRange = {
        (std::numeric_limits<std::uint64_t>::max)(), 2};
    Check(!usdpointcloud::ValidatePointLodHierarchy(hierarchy, diagnostics));
    Check(diagnostics.front().code == usdgeo::DiagnosticCode::InvalidPointSourceRange);
    hierarchy.items[1].sourceRange = {0, 100};

    hierarchy.screenSizeThresholds = {0.25F, 0.10F};
    Check(!usdpointcloud::ValidatePointLodHierarchy(hierarchy, diagnostics));
    Check(diagnostics.front().code == usdgeo::DiagnosticCode::InvalidLodHierarchy);

    hierarchy.screenSizeThresholds = {0.25F};
    hierarchy.items[1].pointCount = 101;
    Check(!usdpointcloud::ValidatePointLodHierarchy(hierarchy, diagnostics));
    Check(diagnostics.back().code == usdgeo::DiagnosticCode::InvalidLodHierarchy);

    hierarchy.items[1].pointCount = 25;
    hierarchy.items[1].bounds.maximum.x = 11.0;
    Check(!usdpointcloud::ValidatePointLodHierarchy(hierarchy, diagnostics));
    Check(diagnostics.front().code == usdgeo::DiagnosticCode::InvalidLodItem);

    hierarchy.items[1].bounds = previewBounds;
    const usdpointcloud::PointTile tile{
        {2, 0, 1, 3}, bounds, {{3, 0, 2}, {3, 0, 3}}, hierarchy};
    Check(usdpointcloud::ValidatePointTile(tile, diagnostics));
    Check(tile.id.ToString() == "L2/0/1/3");

    auto duplicateChildren = tile;
    duplicateChildren.children.push_back(duplicateChildren.children.front());
    Check(!usdpointcloud::ValidatePointTile(duplicateChildren, diagnostics));
    Check(diagnostics.back().code == usdgeo::DiagnosticCode::InvalidPointTile);

    auto invalidTileId = tile;
    invalidTileId.id.level = -1;
    Check(!usdpointcloud::ValidatePointTile(invalidTileId, diagnostics));
    Check(diagnostics.front().code == usdgeo::DiagnosticCode::InvalidPointTileId);
}

void TestDeterministicSampling() {
    usdpointcloud::PointData source;
    source.positions = {{0.0, 0.0, 0.0}, {1.0, 1.0, 1.0},
                        {2.0, 2.0, 2.0}, {3.0, 3.0, 3.0},
                        {4.0, 4.0, 4.0}};
    source.classification = {10, 11, 12, 13, 14};
    source.gpsTime = {20.0, 21.0, 22.0, 23.0, 24.0};
    const usdpointcloud::PointSamplingOptions options{3};
    std::vector<usdgeo::Diagnostic> diagnostics;
    usdpointcloud::PointData first;
    usdpointcloud::PointData second;
    Check(usdpointcloud::SamplePointData(source, options, first, diagnostics));
    Check(usdpointcloud::SamplePointData(source, options, second, diagnostics));
    Check(diagnostics.empty() && first.IsValid() && second.IsValid());
    Check(first.positions.size() == second.positions.size());
    for (std::size_t index = 0; index < first.positions.size(); ++index) {
        Check(first.positions[index].x == second.positions[index].x &&
              first.positions[index].y == second.positions[index].y &&
              first.positions[index].z == second.positions[index].z);
    }
    Check(first.classification == std::vector<std::uint8_t>{10, 11, 13});
    Check(first.gpsTime == std::vector<double>{20.0, 21.0, 23.0});

    const auto firstKey = usdgeo::StableCacheKey(
        usdpointcloud::MakeSamplingCacheArguments(options));
    const auto secondKey = usdgeo::StableCacheKey(
        usdpointcloud::MakeSamplingCacheArguments({2}));
    Check(firstKey != secondKey);

    Check(!usdpointcloud::SamplePointData(
        source, {0}, first, diagnostics));
    Check(diagnostics.back().code == usdgeo::DiagnosticCode::InvalidLodItem);

    diagnostics.clear();
    const auto sourceBeforeAliasAttempt = source.positions;
    Check(!usdpointcloud::SamplePointData(
        source, options, source, diagnostics));
    Check(diagnostics.size() == 1 &&
          diagnostics.front().code == usdgeo::DiagnosticCode::InvalidLodItem);
    Check(source.positions.size() == sourceBeforeAliasAttempt.size());
}

void TestLodProfileBuild() {
    usdpointcloud::PointCloudAsset source;
    source.reference.epsgCode = 4326;
    for (int index = 0; index < 100; ++index) {
        source.data.positions.push_back(
            {static_cast<double>(index), 0.0, 0.0});
    }
    source.bounds.Expand({0.0, 0.0, 0.0});
    source.bounds.Expand({99.0, 0.0, 0.0});
    source.chunk = usdpointcloud::MakePointChunk(source.data, source.bounds);

    std::vector<usdpointcloud::PointCloudAsset> levels;
    usdpointcloud::PointLodHierarchy hierarchy;
    std::vector<usdgeo::Diagnostic> diagnostics;
    Check(usdpointcloud::BuildPointLodAssets(
        source, usdpointcloud::LodProfile::Balanced, levels, hierarchy,
        diagnostics));
    Check(diagnostics.empty());
        Check(levels.size() == 3 && levels[0].data.positions.size() == 100 &&
            levels[1].data.positions.size() == 25 &&
            levels[2].data.positions.size() == 7);
    Check(hierarchy.IsValid() && hierarchy.defaultIndex == 0);
}

} // namespace

int main() {
    TestAttributesAndChunks();
    TestInvalidChunk();
    TestPointDataAndAssetChunk();
    TestMultiScanContract();
    TestExtraByteNameNormalization();
    TestReadOptions();
    TestPointStreamContract();
    TestFileFormatArgumentNormalization();
    TestAttributeSelection();
    TestLodContracts();
    TestDeterministicSampling();
    TestLodProfileBuild();
    return 0;
}