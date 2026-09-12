#include "usdgeo/PointCloudLayer.h"

#include "TiledPayloadAuthoring.h"
#include "usdgeo/CacheKey.h"

#include <algorithm>
#include <array>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <map>
#include <memory>
#include <optional>
#include <string>

#include <pxr/usd/usdGeom/metrics.h>

namespace usdgeo {
namespace {

using usdpointcloud::PointAttribute;
using usdpointcloud::PointAttributeType;
using usdpointcloud::SpoolAttributeValue;

bool IsValidPrimPath(const std::string& primPath) {
    return !primPath.empty() && primPath.front() == '/';
}

void AddError(std::vector<Diagnostic>& diagnostics,
              DiagnosticCode code,
              const std::string& message) {
    diagnostics.push_back({code, Severity::Error, message, std::nullopt,
                           std::nullopt});
}

bool SameSchema(const usdpointcloud::SpoolSchema& left,
                const usdpointcloud::SpoolSchema& right) {
    if (left.attributes.size() != right.attributes.size()) {
        return false;
    }
    for (std::size_t index = 0; index < left.attributes.size(); ++index) {
        if (left.attributes[index].name != right.attributes[index].name ||
            left.attributes[index].type != right.attributes[index].type) {
            return false;
        }
    }
    return true;
}

template <typename T>
std::optional<SpoolAttributeValue> ReadScalar(const std::vector<T>& values,
                                               std::size_t index) {
    if (index >= values.size()) {
        return std::nullopt;
    }
    return SpoolAttributeValue(values[index]);
}

std::optional<SpoolAttributeValue> ReadAttributeValue(
    const usdpointcloud::PointData& data,
    const PointAttribute& attribute,
    std::size_t index) {
    const auto& name = attribute.name;
    if (name == "intensity") return ReadScalar(data.intensity, index);
    if (name == "returnNumber") return ReadScalar(data.returnNumber, index);
    if (name == "numberOfReturns") return ReadScalar(data.numberOfReturns, index);
    if (name == "classification") return ReadScalar(data.classification, index);
    if (name == "classificationFlags") return ReadScalar(data.classificationFlags, index);
    if (name == "scannerChannel") return ReadScalar(data.scannerChannel, index);
    if (name == "scanDirectionFlag") return ReadScalar(data.scanDirectionFlag, index);
    if (name == "edgeOfFlightLine") return ReadScalar(data.edgeOfFlightLine, index);
    if (name == "userData") return ReadScalar(data.userData, index);
    if (name == "scanAngle") return ReadScalar(data.scanAngle, index);
    if (name == "pointSourceId") return ReadScalar(data.pointSourceId, index);
    if (name == "red") return ReadScalar(data.red, index);
    if (name == "green") return ReadScalar(data.green, index);
    if (name == "blue") return ReadScalar(data.blue, index);
    if (name == "nir") return ReadScalar(data.nir, index);
    if (name == "gpsTime") return ReadScalar(data.gpsTime, index);
    if (name == "waveformDescriptorIndex") return ReadScalar(data.waveformDescriptorIndex, index);
    if (name == "waveformDataOffset") return ReadScalar(data.waveformDataOffset, index);
    if (name == "waveformPacketSize") return ReadScalar(data.waveformPacketSize, index);
    if (name == "returnPointWaveformLocation") return ReadScalar(data.returnPointWaveformLocation, index);
    if (name == "waveformXt") return ReadScalar(data.waveformXt, index);
    if (name == "waveformYt") return ReadScalar(data.waveformYt, index);
    if (name == "waveformZt") return ReadScalar(data.waveformZt, index);
    if (name == "waveformDataExternal") return ReadScalar(data.waveformDataExternal, index);

    const auto names = usdpointcloud::NormalizeExtraByteNames(data.extraByteNames);
    const auto extra = std::find(names.begin(), names.end(), name);
    if (extra == names.end()) return std::nullopt;
    const auto extraIndex = static_cast<std::size_t>(extra - names.begin());
    if (extraIndex >= data.extraBytes.size()) return std::nullopt;
    const auto componentCount = data.extraByteComponentCounts.empty()
                                    ? std::uint8_t{1}
                                    : data.extraByteComponentCounts[extraIndex];
    const auto& values = data.extraBytes[extraIndex];
    const auto valueIndex = index * componentCount;
    if (componentCount == 1) {
        if (valueIndex >= values.size()) return std::nullopt;
        return SpoolAttributeValue(values[valueIndex]);
    }
    if (componentCount == 2) {
        if (valueIndex + 1 >= values.size()) return std::nullopt;
        return SpoolAttributeValue(
            std::array<double, 2>{values[valueIndex], values[valueIndex + 1]});
    }
    if (componentCount == 3) {
        if (valueIndex + 2 >= values.size()) return std::nullopt;
        return SpoolAttributeValue(std::array<double, 3>{
            values[valueIndex], values[valueIndex + 1], values[valueIndex + 2]});
    }
    return std::nullopt;
}

template <typename T>
bool AppendScalar(std::vector<T>& values,
                  const SpoolAttributeValue& value,
                  std::size_t index) {
    if (!std::holds_alternative<T>(value) || values.size() != index) {
        return false;
    }
    values.push_back(std::get<T>(value));
    return true;
}

bool AppendAttribute(usdpointcloud::PointData& data,
                     const PointAttribute& attribute,
                     const SpoolAttributeValue& value,
                     std::size_t index) {
    const auto& name = attribute.name;
    if (name == "intensity") return AppendScalar(data.intensity, value, index);
    if (name == "returnNumber") return AppendScalar(data.returnNumber, value, index);
    if (name == "numberOfReturns") return AppendScalar(data.numberOfReturns, value, index);
    if (name == "classification") return AppendScalar(data.classification, value, index);
    if (name == "classificationFlags") return AppendScalar(data.classificationFlags, value, index);
    if (name == "scannerChannel") return AppendScalar(data.scannerChannel, value, index);
    if (name == "scanDirectionFlag") return AppendScalar(data.scanDirectionFlag, value, index);
    if (name == "edgeOfFlightLine") return AppendScalar(data.edgeOfFlightLine, value, index);
    if (name == "userData") return AppendScalar(data.userData, value, index);
    if (name == "scanAngle") return AppendScalar(data.scanAngle, value, index);
    if (name == "pointSourceId") return AppendScalar(data.pointSourceId, value, index);
    if (name == "red") return AppendScalar(data.red, value, index);
    if (name == "green") return AppendScalar(data.green, value, index);
    if (name == "blue") return AppendScalar(data.blue, value, index);
    if (name == "nir") return AppendScalar(data.nir, value, index);
    if (name == "gpsTime") return AppendScalar(data.gpsTime, value, index);
    if (name == "waveformDescriptorIndex") return AppendScalar(data.waveformDescriptorIndex, value, index);
    if (name == "waveformDataOffset") return AppendScalar(data.waveformDataOffset, value, index);
    if (name == "waveformPacketSize") return AppendScalar(data.waveformPacketSize, value, index);
    if (name == "returnPointWaveformLocation") return AppendScalar(data.returnPointWaveformLocation, value, index);
    if (name == "waveformXt") return AppendScalar(data.waveformXt, value, index);
    if (name == "waveformYt") return AppendScalar(data.waveformYt, value, index);
    if (name == "waveformZt") return AppendScalar(data.waveformZt, value, index);
    if (name == "waveformDataExternal") return AppendScalar(data.waveformDataExternal, value, index);

    const auto extra = std::find(data.extraByteNames.begin(),
                                 data.extraByteNames.end(), name);
    if (extra == data.extraByteNames.end()) return false;
    const auto extraIndex = static_cast<std::size_t>(extra - data.extraByteNames.begin());
    auto& values = data.extraBytes[extraIndex];
    if (std::holds_alternative<double>(value)) {
        if (values.size() != index) return false;
        values.push_back(std::get<double>(value));
        return true;
    }
    if (std::holds_alternative<std::array<double, 2>>(value)) {
        const auto item = std::get<std::array<double, 2>>(value);
        if (values.size() != index * 2) return false;
        values.insert(values.end(), item.begin(), item.end());
        return true;
    }
    if (std::holds_alternative<std::array<double, 3>>(value)) {
        const auto item = std::get<std::array<double, 3>>(value);
        if (values.size() != index * 3) return false;
        values.insert(values.end(), item.begin(), item.end());
        return true;
    }
    return false;
}

bool PrepareData(const usdpointcloud::SpoolSchema& schema,
                 usdpointcloud::PointData& data) {
    for (const auto& attribute : schema.attributes) {
        const auto isKnown =
            attribute.name == "intensity" || attribute.name == "returnNumber" ||
            attribute.name == "numberOfReturns" || attribute.name == "classification" ||
            attribute.name == "classificationFlags" || attribute.name == "scannerChannel" ||
            attribute.name == "scanDirectionFlag" || attribute.name == "edgeOfFlightLine" ||
            attribute.name == "userData" || attribute.name == "scanAngle" ||
            attribute.name == "pointSourceId" || attribute.name == "red" ||
            attribute.name == "green" || attribute.name == "blue" || attribute.name == "nir" ||
            attribute.name == "gpsTime" || attribute.name == "waveformDescriptorIndex" ||
            attribute.name == "waveformDataOffset" || attribute.name == "waveformPacketSize" ||
            attribute.name == "returnPointWaveformLocation" || attribute.name == "waveformXt" ||
            attribute.name == "waveformYt" || attribute.name == "waveformZt" ||
            attribute.name == "waveformDataExternal";
        if (isKnown) continue;
        const auto componentCount =
            attribute.type == PointAttributeType::Float64 ? std::uint8_t{1} :
            attribute.type == PointAttributeType::Float64Vec2 ? std::uint8_t{2} :
            attribute.type == PointAttributeType::Float64Vec3 ? std::uint8_t{3} :
                                                               std::uint8_t{0};
        if (componentCount == 0) return false;
        data.extraByteNames.push_back(attribute.name);
        data.extraByteComponentCounts.push_back(componentCount);
        data.extraBytes.emplace_back();
    }
    return true;
}

constexpr char kSpoolMarker[] = "USDGEO_SPOOL_WORKSPACE_V1";

std::filesystem::path MakeSpoolDirectory(
    const PointCloudPayloadOptions& options,
    std::vector<Diagnostic>& diagnostics) {
    auto directory = options.spoolDirectory.empty()
                         ? std::filesystem::path(options.directory) / ".spool"
                         : std::filesystem::path(options.spoolDirectory);
    if (options.spoolDirectory.empty() && !options.owner.empty()) {
        directory /= usdgeo::StableCacheKey({{"payloadOwner", options.owner}});
    }
    if (directory.empty()) {
        AddError(diagnostics, DiagnosticCode::DecodeFailure,
                 "unable to determine point spool directory");
        return {};
    }
    std::error_code error;
    if (std::filesystem::exists(directory, error)) {
        if (error || !std::filesystem::is_directory(directory, error)) {
            AddError(diagnostics, DiagnosticCode::DecodeFailure,
                     "point spool directory is not a directory");
            return {};
        }
        const auto marker = directory / ".usdgeo-spool";
        std::ifstream markerFile(marker, std::ios::binary);
        std::string markerValue;
        const auto markerIsFile =
            std::filesystem::is_regular_file(marker, error) && !error;
        const auto markerIsValid = markerIsFile &&
                                   static_cast<bool>(markerFile) &&
                                   static_cast<bool>(std::getline(
                                       markerFile, markerValue)) &&
                                   markerValue == kSpoolMarker;
        markerFile.close();
        if (!markerIsValid) {
            AddError(diagnostics, DiagnosticCode::DecodeFailure,
                     "point spool directory is not recoverable");
            return {};
        }
        std::filesystem::remove_all(directory, error);
        if (error) {
            AddError(diagnostics, DiagnosticCode::DecodeFailure,
                     "unable to recover point spool directory");
            return {};
        }
    }
    std::filesystem::create_directories(directory, error);
    if (error) {
        AddError(diagnostics, DiagnosticCode::DecodeFailure,
                 "unable to create point spool directory");
        return {};
    }
    std::ofstream marker(directory / ".usdgeo-spool", std::ios::binary);
    if (!marker || !(marker << kSpoolMarker << '\n')) {
        std::filesystem::remove_all(directory, error);
        AddError(diagnostics, DiagnosticCode::DecodeFailure,
                 "unable to initialize point spool directory");
        return {};
    }
    return directory;
}

struct TileSpool {
    usdpointcloud::PointTileId id;
    std::filesystem::path path;
    std::unique_ptr<usdpointcloud::TileSpoolWriter> writer;
};

} // namespace

bool AuthorPointCloudTiledAssetFromStream(
    pxr::SdfLayer* layer,
    const std::string& primPath,
    usdpointcloud::PointStream& stream,
    const GeoReference& reference,
    const usdpointcloud::TileRouter& router,
    const PointCloudPayloadOptions& options,
    std::vector<Diagnostic>& diagnostics) {
    diagnostics.clear();
    if (!layer || !IsValidPrimPath(primPath) || !reference.IsValid() ||
        !router.IsValid() || options.directory.empty() ||
        options.rootLayerPath.empty() || options.tileMemoryLimitBytes == 0) {
        AddError(diagnostics, DiagnosticCode::InvalidPointTile,
                 "invalid tiled point-cloud authoring request");
        return false;
    }

    std::error_code payloadDirectoryError;
    const auto payloadDirectoryExisted = std::filesystem::exists(
        std::filesystem::path(options.directory), payloadDirectoryError);
    if (payloadDirectoryError) {
        AddError(diagnostics, DiagnosticCode::DecodeFailure,
                 "unable to inspect point payload directory");
        return false;
    }
    const auto spoolDirectory = MakeSpoolDirectory(options, diagnostics);
    if (spoolDirectory.empty()) return false;
    std::map<std::string, TileSpool> spools;
    std::size_t bufferedBytes = 0;
    detail::PayloadGeneration payloads(options.directory, options.owner);
    std::vector<usdpointcloud::PointTileManifestEntry> manifestEntries;
    const auto cleanup = [&]() {
        for (auto& entry : spools) entry.second.writer.reset();
        std::vector<Diagnostic> cleanupDiagnostics;
        usdpointcloud::RemoveSpoolDirectory(spoolDirectory, cleanupDiagnostics);
        payloads.Rollback();
        if (!payloadDirectoryExisted) {
            std::error_code removeError;
            std::filesystem::remove(
                std::filesystem::path(options.directory) / ".spool",
                removeError);
            std::filesystem::remove(options.directory, removeError);
        }
    };
    const auto isCancelled = [&]() {
        return options.isCancelled && options.isCancelled();
    };
    const auto flushSpools = [&]() {
        for (auto& entry : spools) {
            if (!entry.second.writer->Flush(diagnostics)) return false;
        }
        bufferedBytes = 0;
        return true;
    };

    usdpointcloud::SpoolSchema schema;
    std::string waveformDataFile;
    for (;;) {
        if (isCancelled()) {
            AddError(diagnostics, DiagnosticCode::DecodeFailure,
                     "point-cloud authoring cancelled");
            cleanup();
            return false;
        }
        usdpointcloud::PointChunk chunk;
        usdpointcloud::PointData data;
        Diagnostic diagnostic;
        const auto status = stream.ReadNext(chunk, data, diagnostic);
        if (status == usdpointcloud::PointStreamStatus::End) break;
        if (status == usdpointcloud::PointStreamStatus::Error ||
            !chunk.IsValid() || !data.IsValid() ||
            chunk.pointCount != data.positions.size()) {
            if (diagnostic.message.empty()) {
                AddError(diagnostics, DiagnosticCode::DecodeFailure,
                         "point stream produced invalid data");
            } else {
                diagnostics.push_back(diagnostic);
            }
            cleanup();
            return false;
        }
        const usdpointcloud::SpoolSchema chunkSchema{
            usdpointcloud::kPointSpoolSchemaVersion,
            usdpointcloud::SpoolCoordinateSpace::SourceAndStage,
            chunk.attributes};
        if (schema.attributes.empty()) {
            schema = chunkSchema;
        } else if (!SameSchema(schema, chunkSchema)) {
            AddError(diagnostics, DiagnosticCode::InvalidPointTile,
                     "point stream attribute schema changed between chunks");
            cleanup();
            return false;
        }
        if (!schema.IsValid()) {
            AddError(diagnostics, DiagnosticCode::InvalidPointTile,
                     "point stream produced an invalid attribute schema");
            cleanup();
            return false;
        }
        if (waveformDataFile.empty()) waveformDataFile = data.waveformDataFile;

        for (std::size_t index = 0; index < data.positions.size(); ++index) {
            if (isCancelled()) {
                AddError(diagnostics, DiagnosticCode::DecodeFailure,
                         "point-cloud authoring cancelled");
                cleanup();
                return false;
            }
            const auto tileId = router.GetTileId(data.positions[index]);
            if (!tileId.IsValid()) {
                AddError(diagnostics, DiagnosticCode::InvalidPointTile,
                         "point stream produced a point outside the tile plan");
                cleanup();
                return false;
            }
            const auto key = tileId.ToString();
            auto found = spools.find(key);
            if (found == spools.end()) {
                TileSpool tile;
                tile.id = tileId;
                tile.path = spoolDirectory / ("tile_" + std::to_string(spools.size()) + ".bin");
                tile.writer = std::make_unique<usdpointcloud::TileSpoolWriter>();
                if (!tile.writer->Open(tile.path, tile.id, schema,
                                       options.tileMemoryLimitBytes, diagnostics,
                                       options.spoolIoStats)) {
                    cleanup();
                    return false;
                }
                found = spools.emplace(key, std::move(tile)).first;
            }
            Vec3d stagePosition;
            if (!reference.TryToLocal(data.positions[index], stagePosition)) {
                AddError(diagnostics, DiagnosticCode::NonFiniteCoordinate,
                         "unable to transform point into stage coordinates");
                cleanup();
                return false;
            }
            usdpointcloud::SpoolPoint point;
            point.sourcePosition = data.positions[index];
            point.stagePosition = stagePosition;
            point.attributes.reserve(schema.attributes.size());
            for (const auto& attribute : schema.attributes) {
                const auto value = ReadAttributeValue(data, attribute, index);
                if (!value) {
                    AddError(diagnostics, DiagnosticCode::DecodeFailure,
                             "point stream attribute data does not match its schema");
                    cleanup();
                    return false;
                }
                point.attributes.push_back(*value);
            }
            const auto bufferedBefore = found->second.writer->BufferedBytes();
            if (!found->second.writer->Append(point, diagnostics)) {
                cleanup();
                return false;
            }
            const auto bufferedAfter = found->second.writer->BufferedBytes();
            if (bufferedAfter >= bufferedBefore) {
                bufferedBytes += bufferedAfter - bufferedBefore;
            } else {
                bufferedBytes -= bufferedBefore - bufferedAfter;
            }
            if (options.onBufferedBytes) {
                options.onBufferedBytes(bufferedBytes);
            }
            if (bufferedBytes >= options.tileMemoryLimitBytes &&
                !flushSpools()) {
                cleanup();
                return false;
            }
        }
    }
    for (auto& entry : spools) {
        if (isCancelled()) {
            AddError(diagnostics, DiagnosticCode::DecodeFailure,
                     "point-cloud authoring cancelled");
            cleanup();
            return false;
        }
        if (!entry.second.writer->Close(diagnostics)) {
            cleanup();
            return false;
        }
    }
    if (spools.empty()) {
        AddError(diagnostics, DiagnosticCode::DecodeFailure,
                 "point stream did not produce any points");
        cleanup();
        return false;
    }

    // The tile set is known once the stream is spooled, so every payload is
    // declared before the first one is written.
    std::vector<std::string> payloadNames;
    payloadNames.reserve(spools.size());
    for (const auto& entry : spools) {
        payloadNames.push_back(detail::TilePayloadName(entry.second.id, 0));
    }
    std::string payloadError;
    if (!payloads.Begin(payloadNames, payloadError)) {
        AddError(diagnostics, DiagnosticCode::DecodeFailure, payloadError);
        cleanup();
        return false;
    }

    const auto stage = detail::CreateDetachedStage();
    if (!stage || !pxr::UsdGeomSetStageUpAxis(
                      stage, pxr::TfToken(reference.stageUpAxis)) ||
        !pxr::UsdGeomSetStageMetersPerUnit(stage, 1.0)) {
        AddError(diagnostics, DiagnosticCode::DecodeFailure,
                 "unable to create tiled point-cloud stage");
        cleanup();
        return false;
    }

    for (const auto& entry : spools) {
        if (isCancelled()) {
            AddError(diagnostics, DiagnosticCode::DecodeFailure,
                     "point-cloud authoring cancelled");
            cleanup();
            return false;
        }
        usdpointcloud::TileSpoolReader reader;
        const auto failTile = [&]() {
            reader.Close();
            cleanup();
            return false;
        };
        usdpointcloud::PointTileId tileId;
        usdpointcloud::SpoolSchema tileSchema;
        if (!reader.Open(entry.second.path, tileId, tileSchema, diagnostics,
                 options.spoolIoStats) ||
            tileId.ToString() != entry.first ||
            !SameSchema(schema, tileSchema)) {
            AddError(diagnostics, DiagnosticCode::DecodeFailure,
                     "unable to reopen point tile spool");
            return failTile();
        }
        usdpointcloud::PointData data;
        if (!PrepareData(tileSchema, data)) {
            AddError(diagnostics, DiagnosticCode::DecodeFailure,
                     "unsupported point tile spool schema");
            return failTile();
        }
        SpatialBounds bounds = SpatialBounds::Empty();
        usdpointcloud::SpoolPoint point;
        while (reader.ReadNext(point, diagnostics)) {
            if (isCancelled()) {
                AddError(diagnostics, DiagnosticCode::DecodeFailure,
                         "point-cloud authoring cancelled");
                return failTile();
            }
            bounds.Expand(point.sourcePosition);
            data.positions.push_back(point.sourcePosition);
            const auto pointIndex = data.positions.size() - 1;
            for (std::size_t index = 0; index < tileSchema.attributes.size(); ++index) {
                if (!AppendAttribute(data, tileSchema.attributes[index],
                                     point.attributes[index], pointIndex)) {
                    AddError(diagnostics, DiagnosticCode::DecodeFailure,
                             "unable to reconstruct point tile attributes");
                    return failTile();
                }
            }
        }
        if (!reader.IsComplete() || data.positions.empty()) {
            AddError(diagnostics, DiagnosticCode::DecodeFailure,
                     "point tile spool is incomplete or empty");
            return failTile();
        }
        data.waveformDataFile = waveformDataFile;
        PointCloudTileAsset tile;
        tile.tile.id = tileId;
        tile.tile.bounds = bounds;
        tile.tile.lod.bounds = bounds;
        tile.tile.lod.items = {{0, data.positions.size(), bounds,
                                {0, data.positions.size()}}};
        usdpointcloud::PointCloudAsset asset;
        asset.reference = reference;
        asset.bounds = bounds;
        asset.data = std::move(data);
        asset.chunk = usdpointcloud::MakePointChunk(asset.data, bounds);
        if (!asset.IsValid()) {
            AddError(diagnostics, DiagnosticCode::DecodeFailure,
                     "reconstructed point tile is invalid");
            return failTile();
        }
        tile.levels.push_back(std::move(asset));
        std::vector<PointCloudTileAsset> singleTile;
        singleTile.push_back(std::move(tile));
        if (!detail::AuthorClaimedTilePayloads(
                stage, primPath, singleTile, options, payloads,
                manifestEntries)) {
            AddError(diagnostics, DiagnosticCode::DecodeFailure,
                     "unable to author tiled point-cloud payloads");
            return failTile();
        }
        if (isCancelled()) {
            AddError(diagnostics, DiagnosticCode::DecodeFailure,
                     "point-cloud authoring cancelled");
            return failTile();
        }
    }
    std::vector<Diagnostic> cleanupDiagnostics;
    if (!usdpointcloud::RemoveSpoolDirectory(spoolDirectory,
                                             cleanupDiagnostics)) {
        diagnostics.insert(diagnostics.end(), cleanupDiagnostics.begin(),
                           cleanupDiagnostics.end());
        cleanup();
        return false;
    }
    if (!detail::CommitTilePayloads(stage->GetRootLayer(), payloads,
                                    manifestEntries, payloadError)) {
        AddError(diagnostics, DiagnosticCode::DecodeFailure, payloadError);
        cleanup();
        return false;
    }
    if (options.tileManifestEntries) {
        options.tileManifestEntries->insert(
            options.tileManifestEntries->end(), manifestEntries.begin(),
            manifestEntries.end());
    }
    layer->TransferContent(stage->GetRootLayer());
    return true;
}

bool AuthorPointCloudTiledAssetFromStream(
    pxr::SdfLayer* layer,
    const std::string& primPath,
    usdpointcloud::PointStream& stream,
    const GeoReference& reference,
    const usdpointcloud::TileGridConfig& tileConfig,
    const PointCloudPayloadOptions& options,
    std::vector<Diagnostic>& diagnostics) {
    usdpointcloud::FixedGridTileRouter router(tileConfig);
    return AuthorPointCloudTiledAssetFromStream(
        layer, primPath, stream, reference, router, options, diagnostics);
}

} // namespace usdgeo