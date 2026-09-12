#include "usdgeo/PointCloudLayer.h"
#include "usdgeo/ArAssetRandomAccessSource.h"
#include "usdgeo/PointCloudCache.h"
#include "usdgeo/cache/Cache.h"
#include "usdcopc/Copc.h"
#include "usdpointcloud/FileFormatArguments.h"
#include "usdlas/Las.h"
#include "usdlaz/Laz.h"

#include <pxr/usd/ar/asset.h>
#include <pxr/usd/ar/resolver.h>
#include <pxr/usd/sdf/layer.h>

#include <chrono>
#include <csignal>
#include <filesystem>
#include <fstream>
#include <algorithm>
#include <iostream>
#include <map>
#include <memory>
#include <sstream>
#include <string>
#include <vector>

namespace {

volatile std::sig_atomic_t interrupted = 0;

void HandleInterrupt(int) {
    interrupted = 1;
}

void PrintUsage() {
    std::cerr
          << "Usage: usd-pointcloud-convert <input.las|input.laz|input.copc|input.copc.laz> "
              "<output.usda> "
              "(--tile-size <source-units> | --max-points-per-tile <count>) [options]\n"
        << "Options:\n"
          << "  --tile-size <source-units>\n"
          << "  --max-points-per-tile <count>\n"
          << "  --min-points-per-tile <count>\n"
          << "  --max-depth <count>\n"
        << "  --memory-limit <bytes>\n"
        << "  --chunk-points <count>\n"
        << "  --attributes <comma-separated names>\n"
        << "  --payload-directory <directory>\n"
        << "  --cache-root <directory>\n"
        << "    (cache output uses a portable sibling payloads directory)\n";
}

bool ReadOptionValue(int& index, int argc, char** argv, std::string& value) {
    if (index + 1 >= argc) {
        std::cerr << "Missing value for " << argv[index] << "\n";
        return false;
    }
    value = argv[++index];
    return true;
}

void PrintDiagnostics(const std::vector<usdgeo::Diagnostic>& diagnostics) {
    for (const auto& diagnostic : diagnostics) {
        std::cerr << "diagnostic[" << static_cast<int>(diagnostic.code) << "]: "
                  << diagnostic.message << "\n";
    }
}

void PrintCacheStatistics() {
    const auto statistics = usdgeo::cache::GetLookupStatistics();
    std::cout << "Cache lookups: " << statistics.lookups
              << ", hits: " << statistics.hits
              << ", misses: " << statistics.misses
              << ", incomplete: " << statistics.incomplete
              << ", invalid-layout: " << statistics.invalidLayouts << "\n";
}

bool IsExtension(const std::filesystem::path& path, const char* extension) {
    auto value = path.extension().string();
    for (auto& character : value) {
        if (character >= 'A' && character <= 'Z') {
            character = static_cast<char>(character - 'A' + 'a');
        }
    }
    return value == extension;
}

enum class InputFormat {
    Unsupported,
    Las,
    Laz,
    Copc,
};

InputFormat DetectInputFormat(const std::filesystem::path& path) {
    if (IsExtension(path, ".copc")) {
        return InputFormat::Copc;
    }
    if (IsExtension(path, ".las")) {
        return InputFormat::Las;
    }
    if (IsExtension(path, ".laz")) {
        auto stem = path.stem().string();
        for (auto& character : stem) {
            if (character >= 'A' && character <= 'Z') {
                character = static_cast<char>(character - 'A' + 'a');
            }
        }
        return std::filesystem::path(stem).extension() == ".copc"
                   ? InputFormat::Copc
                   : InputFormat::Laz;
    }
    return InputFormat::Unsupported;
}

std::unique_ptr<usdpointcloud::PointStream> OpenPointStream(
    InputFormat format,
    const std::filesystem::path& inputPath,
    const std::shared_ptr<usdgeo::RandomAccessSource>& randomAccessSource,
    const std::string& sourceName,
    const usdpointcloud::PointReadOptions& options,
    usdlas::LasHeader& header,
    std::vector<usdgeo::Diagnostic>& diagnostics) {
    if (format == InputFormat::Las) {
        return usdlas::OpenLasPointStream(
            inputPath.string(), options, header, diagnostics);
    }
    if (format == InputFormat::Laz) {
        return usdlaz::OpenLazPointStream(
            inputPath.string(), options, header, diagnostics);
    }
    if (format == InputFormat::Copc) {
        if (randomAccessSource) {
            usdcopc::CopcHeader copcHeader;
            auto stream = usdcopc::OpenCopcPointStream(
                randomAccessSource, options, copcHeader, diagnostics,
                sourceName);
            header = copcHeader.las;
            return stream;
        }
        usdcopc::CopcHeader copcHeader;
        auto stream = usdcopc::OpenCopcPointStream(
            inputPath.string(), options, copcHeader, diagnostics);
        header = copcHeader.las;
        return stream;
    }
    return nullptr;
}

struct ConverterSource {
    std::string identifier;
    std::string resolvedPath;
    std::string sourceName;
    std::filesystem::path localPath;
    std::shared_ptr<pxr::ArAsset> asset;
    std::shared_ptr<usdgeo::RandomAccessSource> randomAccessSource;
    usdgeo::cache::SourceIdentity identity;
    usdgeo::cache::ResolverIdentityStability resolverStability =
        usdgeo::cache::ResolverIdentityStability::Unavailable;
    bool resolverBacked = false;
};

bool IsResolverIdentifier(const std::string& identifier) {
    const auto colon = identifier.find(':');
    if (colon == std::string::npos) {
        return false;
    }
    if (colon == 1 &&
        ((identifier[0] >= 'A' && identifier[0] <= 'Z') ||
         (identifier[0] >= 'a' && identifier[0] <= 'z')) &&
        identifier.size() > 2 &&
        (identifier[2] == '/' || identifier[2] == '\\')) {
        return false;
    }
    const auto pathSeparator = identifier.find_first_of("/\\");
    return pathSeparator == std::string::npos || colon < pathSeparator;
}

std::filesystem::path FormatPath(const std::string& identifier) {
    const auto queryStart = identifier.find_first_of("?#");
    const auto withoutQuery = identifier.substr(0, queryStart);
    const auto slash = withoutQuery.find_last_of("/\\");
    return std::filesystem::path(
        slash == std::string::npos ? withoutQuery
                                   : withoutQuery.substr(slash + 1));
}

bool PrepareSource(const std::string& identifier,
                   InputFormat format,
                   ConverterSource& source,
                   std::string& errorMessage) {
    source = {};
    source.identifier = identifier;
    if (!IsResolverIdentifier(identifier)) {
        source.localPath = std::filesystem::absolute(
            std::filesystem::path(identifier));
        source.sourceName = source.localPath.string();
        if (!usdgeo::cache::TryBuildLocalSourceIdentity(
                source.localPath, source.identity, errorMessage)) {
            return false;
        }
        return true;
    }

    if (format != InputFormat::Copc) {
        errorMessage =
            "resolver-addressable conversion currently supports COPC only";
        return false;
    }
    const auto resolvedPath = pxr::ArGetResolver().Resolve(identifier);
    if (resolvedPath.GetPathString().empty()) {
        errorMessage = "active resolver could not resolve COPC asset";
        return false;
    }
    source.asset = pxr::ArGetResolver().OpenAsset(resolvedPath);
    if (!source.asset) {
        errorMessage = "active resolver could not open COPC asset";
        return false;
    }
    source.resolverBacked = true;
    source.resolvedPath = resolvedPath.GetPathString();
    source.sourceName = "resolver-addressed-source";
    source.randomAccessSource =
        std::make_shared<usdgeo::ArAssetRandomAccessSource>(
            source.asset, source.sourceName);
    std::string identityError;
    usdgeo::TryBuildResolverSourceIdentity(
        pxr::ArGetResolver(), identifier, resolvedPath, *source.asset,
        source.identity, source.resolverStability, identityError);
    return true;
}

bool RefreshSourceIdentity(const ConverterSource& source,
                           usdgeo::cache::SourceIdentity& identity,
                           std::string& errorMessage) {
    if (!source.resolverBacked) {
        return usdgeo::cache::TryBuildLocalSourceIdentity(
            source.localPath, identity, errorMessage);
    }
    usdgeo::cache::ResolverIdentityStability stability;
    return usdgeo::TryBuildResolverSourceIdentity(
        pxr::ArGetResolver(), source.identifier,
        pxr::ArResolvedPath(source.resolvedPath), *source.asset, identity,
        stability, errorMessage);
}

bool SameSourceIdentity(const usdgeo::cache::SourceIdentity& left,
                        const usdgeo::cache::SourceIdentity& right) {
    return left.identifier == right.identifier &&
           left.sizeBytes == right.sizeBytes &&
           left.modifiedTime == right.modifiedTime &&
           left.validationToken == right.validationToken;
}

std::filesystem::path ManifestPath(
    const std::filesystem::path& outputPath) {
    return std::filesystem::path(outputPath.string() + ".manifest");
}

std::filesystem::path TileManifestPath(
    const std::filesystem::path& payloadDirectory) {
    return payloadDirectory / "tiles.manifest";
}

std::filesystem::path TransactionPath(
    const std::filesystem::path& outputPath) {
    return std::filesystem::path(outputPath.string() + ".transaction");
}

std::filesystem::path TransactionStatePath(
    const std::filesystem::path& transactionPath) {
    return transactionPath / "state";
}

bool WriteTransactionState(const std::filesystem::path& transactionPath,
                           const std::filesystem::path& payloadDirectory,
                           std::string& errorMessage) {
    std::ofstream state(TransactionStatePath(transactionPath),
                        std::ios::binary | std::ios::trunc);
    if (!state) {
        errorMessage = "unable to create conversion transaction state";
        return false;
    }
    state << "payloadDirectory=" << payloadDirectory.generic_string() << "\n";
    if (!state) {
        errorMessage = "unable to write conversion transaction state";
        return false;
    }
    return true;
}

bool ReadTransactionPayloadDirectory(
    const std::filesystem::path& transactionPath,
    const std::filesystem::path& defaultPayloadDirectory,
    std::filesystem::path& payloadDirectory,
    std::string& errorMessage) {
    const auto statePath = TransactionStatePath(transactionPath);
    std::error_code error;
    const auto stateExists = std::filesystem::exists(statePath, error);
    if (error) {
        errorMessage = "unable to inspect conversion transaction state: " +
                       error.message();
        return false;
    }
    if (!stateExists) {
        payloadDirectory = defaultPayloadDirectory;
        return true;
    }

    std::ifstream state(statePath, std::ios::binary);
    std::string line;
    if (!state || !std::getline(state, line) ||
        line.rfind("payloadDirectory=", 0) != 0) {
        errorMessage = "invalid conversion transaction state";
        return false;
    }
    if (!line.empty() && line.back() == '\r') {
        line.pop_back();
    }
    payloadDirectory = std::filesystem::path(line.substr(17));
    if (payloadDirectory.empty() || payloadDirectory.is_relative()) {
        errorMessage = "conversion transaction state has an invalid payload directory";
        return false;
    }
    return true;
}

bool IsPathWithinDirectory(const std::filesystem::path& path,
                           const std::filesystem::path& directory) {
    const auto relative = path.lexically_normal().lexically_relative(
        directory.lexically_normal());
    if (relative.empty() || relative == ".") return false;
    for (const auto& component : relative) {
        if (component == "..") return false;
    }
    return true;
}

bool IsSamePath(const std::filesystem::path& left,
                const std::filesystem::path& right) {
    return left.lexically_normal() == right.lexically_normal();
}

bool RecoverIncompleteTransaction(
    const std::filesystem::path& outputPath,
    const std::filesystem::path& manifestPath,
    const std::filesystem::path& temporaryRootPath,
    const std::filesystem::path& temporaryManifestPath,
    const std::filesystem::path& payloadDirectory,
    const std::filesystem::path& transactionPath,
    std::string& errorMessage) {
    std::error_code error;
    const auto transactionExists =
        std::filesystem::exists(transactionPath, error);
    if (error) {
        errorMessage = "unable to inspect conversion transaction: " +
                       error.message();
        return false;
    }
    if (!transactionExists) return true;

    const auto outputExists = std::filesystem::exists(outputPath, error);
    if (error) {
        errorMessage = "unable to inspect incomplete conversion root: " +
                       error.message();
        return false;
    }
    const auto manifestExists = std::filesystem::exists(manifestPath, error);
    if (error) {
        errorMessage = "unable to inspect incomplete conversion: " +
                       error.message();
        return false;
    }
    if (outputExists && manifestExists) {
        std::filesystem::remove_all(transactionPath, error);
        if (error) {
            errorMessage = "unable to remove completed conversion marker: " +
                           error.message();
            return false;
        }
        return true;
    }
    if (outputExists) {
        errorMessage =
            "conversion transaction has a root layer without a manifest";
        return false;
    }

    std::filesystem::path recordedPayloadDirectory;
    if (!ReadTransactionPayloadDirectory(
            transactionPath, payloadDirectory, recordedPayloadDirectory,
            errorMessage)) {
        return false;
    }
    if (!IsSamePath(recordedPayloadDirectory, payloadDirectory) &&
        !IsPathWithinDirectory(
            recordedPayloadDirectory, outputPath.parent_path())) {
        errorMessage =
            "conversion transaction payload directory is outside the output workspace";
        return false;
    }

    const std::filesystem::path artifacts[] = {
        manifestPath, temporaryRootPath, temporaryManifestPath};
    for (const auto& artifact : artifacts) {
        std::filesystem::remove(artifact, error);
        if (error) {
            errorMessage = "unable to remove incomplete conversion artifact: " +
                           error.message();
            return false;
        }
    }
    std::filesystem::remove_all(recordedPayloadDirectory, error);
    if (error) {
        errorMessage = "unable to remove incomplete payload directory: " +
                       error.message();
        return false;
    }
    std::filesystem::remove_all(transactionPath, error);
    if (error) {
        errorMessage = "unable to remove conversion transaction marker: " +
                       error.message();
        return false;
    }
    return true;
}

std::string RelativeManifestPath(const std::filesystem::path& base,
                                 const std::filesystem::path& path) {
    return std::filesystem::relative(path, base).generic_string();
}

std::string FormatDouble(double value) {
    std::ostringstream result;
    result << std::setprecision(17) << value;
    return result.str();
}

bool IsCachePayloadDirectory(const std::filesystem::path& outputPath,
                             const std::filesystem::path& payloadDirectory) {
    return IsSamePath(payloadDirectory,
                      outputPath.parent_path() / "payloads");
}

bool CopyDirectory(const std::filesystem::path& source,
                   const std::filesystem::path& destination,
                   std::string& errorMessage) {
    std::error_code error;
    std::filesystem::create_directories(destination, error);
    if (error) {
        errorMessage = "unable to create cached payload directory: " +
                       error.message();
        return false;
    }
    std::filesystem::copy(
        source, destination,
        std::filesystem::copy_options::recursive |
            std::filesystem::copy_options::overwrite_existing,
        error);
    if (error) {
        errorMessage = "unable to materialize cached payloads: " +
                       error.message();
        return false;
    }
    return true;
}

bool BuildCacheDescriptor(
    const usdgeo::cache::SourceIdentity& sourceIdentity,
    InputFormat inputFormat,
    const usdgeo::GeoReference& reference,
    const usdpointcloud::PointReadRequest& request,
    const std::string& tilePlanKey,
    usdgeo::cache::Descriptor& descriptor,
    std::string& errorMessage) {
    descriptor.source = sourceIdentity;
    descriptor.pluginVersion = "usd-pointcloud-plugins-0.3.0-display-v3";
    descriptor.parserVersion = inputFormat == InputFormat::Las
                                   ? "las-reader-1"
                               : inputFormat == InputFormat::Copc
                                   ? "copc-reader-1"
                                   : "laz-reader-1";
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
    if (request.maxPointsPerTile) {
        descriptor.tileAndLod.emplace_back(
            "planner.id", usdpointcloud::kAdaptivePointBudgetPlannerId);
        descriptor.tileAndLod.emplace_back(
            "planner.version",
            std::to_string(usdpointcloud::kAdaptivePointBudgetPlannerVersion));
        if (tilePlanKey.empty()) {
            errorMessage = "adaptive cache descriptor requires a tile plan key";
            return false;
        }
        // The planner's identity and version are caller intent; the plan it
        // produced is read out of the source, so it belongs in the
        // source-derived half of the descriptor.
        descriptor.sourceDerived.emplace_back("tile.plan", tilePlanKey);
    }
    descriptor.downsampling = {{"algorithm", "fixed-stride"},
                               {"version", "1"}};
    if (!descriptor.IsValid()) {
        errorMessage = "cache descriptor is invalid";
        return false;
    }
    return true;
}

bool MaterializeCache(const usdgeo::cache::Layout& layout,
                      const std::filesystem::path& rootPath,
                      const std::filesystem::path& payloadDirectory,
                      std::string& errorMessage) {
    if (!IsCachePayloadDirectory(rootPath, payloadDirectory)) {
        errorMessage =
            "--cache-root requires --payload-directory to be output-dir/payloads";
        return false;
    }
    const auto cachedRoot = pxr::SdfLayer::FindOrOpen(layout.rootLayer.string());
    if (!cachedRoot || !cachedRoot->Export(rootPath.string())) {
        errorMessage = "unable to export cached root layer";
        return false;
    }
    return CopyDirectory(layout.payloadDirectory, payloadDirectory,
                         errorMessage);
}

usdgeo::cache::Layout LayoutAt(
    const usdgeo::cache::Layout& layout,
    const std::filesystem::path& entryDirectory) {
    auto result = layout;
    result.entryDirectory = entryDirectory;
    result.rootLayer = entryDirectory / "root.usdc";
    result.manifest = entryDirectory / "cache.manifest";
    result.payloadDirectory = entryDirectory / "payloads";
    return result;
}

bool CreateTemporaryCacheLayout(
    const usdgeo::cache::Layout& layout,
    usdgeo::cache::Layout& temporaryLayout,
    std::string& errorMessage) {
    std::error_code error;
    std::filesystem::create_directories(layout.entryDirectory.parent_path(),
                                        error);
    if (error) {
        errorMessage = "unable to create cache root: " + error.message();
        return false;
    }

    const auto timestamp = std::chrono::steady_clock::now()
                               .time_since_epoch()
                               .count();
    for (int attempt = 0; attempt != 16; ++attempt) {
        const auto directory = std::filesystem::path(
            layout.entryDirectory.string() + ".tmp-" +
            std::to_string(timestamp) + "-" + std::to_string(attempt));
        if (std::filesystem::create_directory(directory, error)) {
            temporaryLayout = LayoutAt(layout, directory);
            return true;
        }
        if (error && error != std::errc::file_exists) {
            errorMessage = "unable to create temporary cache entry: " +
                           error.message();
            return false;
        }
        error.clear();
    }
    errorMessage = "unable to create a unique temporary cache entry";
    return false;
}

bool PublishCacheEntry(const usdgeo::cache::Layout& temporaryLayout,
                       const usdgeo::cache::Layout& layout,
                       std::string& errorMessage) {
    std::error_code error;
    std::filesystem::rename(temporaryLayout.entryDirectory,
                             layout.entryDirectory, error);
    if (!error) return true;

    if (usdgeo::cache::IsCacheHit(layout)) {
        std::error_code cleanupError;
        std::filesystem::remove_all(temporaryLayout.entryDirectory,
                                     cleanupError);
        if (cleanupError) {
            errorMessage = "unable to remove duplicate temporary cache entry: " +
                           cleanupError.message();
            return false;
        }
        return true;
    }

    errorMessage = "unable to publish cache entry: " + error.message();
    return false;
}

bool WriteManifest(const std::filesystem::path& manifestPath,
                   const std::string& inputName,
                   std::uintmax_t inputSize,
                   const std::filesystem::path& outputPath,
                   const std::filesystem::path& payloadDirectory,
                   const usdpointcloud::PointReadRequest& request,
                   std::string& errorMessage) {
    std::error_code error;
    std::vector<std::string> payloads;
    for (std::filesystem::recursive_directory_iterator iterator(
             payloadDirectory, error), end;
         iterator != end && !error; iterator.increment(error)) {
        if (iterator->is_regular_file(error) && !error &&
            iterator->path().filename() != "tiles.manifest") {
            payloads.push_back(RelativeManifestPath(
                outputPath.parent_path(), iterator->path()));
        }
    }
    if (error) {
        errorMessage = "unable to enumerate payloads for manifest: " +
                       error.message();
        return false;
    }
    std::sort(payloads.begin(), payloads.end());

    std::map<std::string, std::string> manifestArguments =
        request.canonicalArguments;
    manifestArguments["payloadDirectory"] = RelativeManifestPath(
        outputPath.parent_path(), payloadDirectory);

    std::ofstream output(manifestPath, std::ios::binary | std::ios::trunc);
    if (!output) {
        errorMessage = "unable to create manifest";
        return false;
    }
    output << "format=usd-pointcloud-manifest-v1\n"
            << "input.file=" << inputName << "\n"
           << "input.sizeBytes=" << inputSize << "\n"
           << "output.root=" << outputPath.filename().generic_string() << "\n"
           << "output.payloadDirectory="
           << RelativeManifestPath(outputPath.parent_path(), payloadDirectory)
           << "\n";
    for (const auto& [key, value] : manifestArguments) {
        output << "argument." << key << "=" << value << "\n";
    }
    output << "payload.count=" << payloads.size() << "\n";
    for (const auto& payload : payloads) {
        output << "payload.file=" << payload << "\n";
    }
    if (!output) {
        errorMessage = "unable to write manifest";
        return false;
    }
    return true;
}

bool WriteTileManifest(const std::filesystem::path& manifestPath,
                       const usdpointcloud::PointTileManifest& manifest,
                       std::string& errorMessage) {
    std::string serialized;
    std::vector<usdgeo::Diagnostic> diagnostics;
    if (!usdpointcloud::SerializePointTileManifest(
            manifest, serialized, diagnostics)) {
        errorMessage = diagnostics.empty()
                           ? "invalid tile manifest"
                           : diagnostics.front().message;
        return false;
    }
    std::ofstream output(manifestPath, std::ios::binary | std::ios::trunc);
    if (!output) {
        errorMessage = "unable to create tile manifest";
        return false;
    }
    output << serialized;
    if (!output) {
        errorMessage = "unable to write tile manifest";
        return false;
    }
    return true;
}

class SelectedPointStream final : public usdpointcloud::PointStream {
public:
    SelectedPointStream(std::unique_ptr<usdpointcloud::PointStream> stream,
                        std::vector<std::string> attributes)
        : stream_(std::move(stream)), attributes_(std::move(attributes)) {}

    usdpointcloud::PointStreamStatus ReadNext(
        usdpointcloud::PointChunk& chunk,
        usdpointcloud::PointData& data,
        usdgeo::Diagnostic& diagnostic) override {
        const auto status = stream_->ReadNext(chunk, data, diagnostic);
        if (status != usdpointcloud::PointStreamStatus::Chunk ||
            attributes_.empty()) {
            return status;
        }
        std::string error;
        if (!usdpointcloud::SelectPointDataAttributes(data, attributes_, error)) {
            diagnostic = {usdgeo::DiagnosticCode::InvalidFormatArgument,
                          usdgeo::Severity::Error, error, std::nullopt,
                          std::nullopt};
            return usdpointcloud::PointStreamStatus::Error;
        }
        chunk = usdpointcloud::MakePointChunk(data, chunk.bounds);
        return usdpointcloud::PointStreamStatus::Chunk;
    }

private:
    std::unique_ptr<usdpointcloud::PointStream> stream_;
    std::vector<std::string> attributes_;
};

} // namespace

int main(int argc, char** argv) {
    usdgeo::cache::ResetLookupStatistics();
    if (argc >= 2 && std::string(argv[1]) == "--help") {
        PrintUsage();
        return 0;
    }
    if (argc < 4) {
        PrintUsage();
        return 2;
    }

    const std::string inputIdentifier = argv[1];
    const std::filesystem::path outputPath = std::filesystem::absolute(argv[2]);
    const auto manifestPath = ManifestPath(outputPath);
    const auto inputFormat = DetectInputFormat(FormatPath(inputIdentifier));
    if (inputFormat == InputFormat::Unsupported) {
        std::cerr << "Input must have a .las, .laz, .copc, or .copc.laz extension\n";
        return 2;
    }
    if (!IsExtension(outputPath, ".usda")) {
        std::cerr << "Output must have a .usda extension\n";
        return 2;
    }

    ConverterSource source;
    std::string sourceError;
    if (!PrepareSource(inputIdentifier, inputFormat, source, sourceError)) {
        std::cerr << "Unable to prepare input source: " << sourceError << "\n";
        return 1;
    }
    const auto manifestInputName = source.resolverBacked
                                       ? "resolver-addressed-source"
                                       : source.localPath.filename().generic_string();
    const auto inputSize = source.resolverBacked
                               ? static_cast<std::uintmax_t>(source.asset->GetSize())
                               : source.identity.sizeBytes;

    std::map<std::string, std::string> arguments;
    arguments.emplace("tile", "true");
    bool hasTileSize = false;
    bool hasAdaptiveBudget = false;
    std::filesystem::path cacheRoot;
    std::filesystem::path payloadDirectory;
    for (int index = 3; index < argc; ++index) {
        const std::string option = argv[index];
        std::string value;
        if (option == "--tile-size") {
            if (!ReadOptionValue(index, argc, argv, value)) return 2;
            arguments["tileSize"] = value;
            hasTileSize = true;
        } else if (option == "--max-points-per-tile") {
            if (!ReadOptionValue(index, argc, argv, value)) return 2;
            arguments["maxPointsPerTile"] = value;
            hasAdaptiveBudget = true;
        } else if (option == "--min-points-per-tile") {
            if (!ReadOptionValue(index, argc, argv, value)) return 2;
            arguments["minPointsPerTile"] = value;
        } else if (option == "--max-depth") {
            if (!ReadOptionValue(index, argc, argv, value)) return 2;
            arguments["maxDepth"] = value;
        } else if (option == "--memory-limit") {
            if (!ReadOptionValue(index, argc, argv, value)) return 2;
            arguments["tileMemoryLimit"] = value;
        } else if (option == "--chunk-points") {
            if (!ReadOptionValue(index, argc, argv, value)) return 2;
            arguments["chunkPointLimit"] = value;
        } else if (option == "--attributes") {
            if (!ReadOptionValue(index, argc, argv, value)) return 2;
            arguments["attributes"] = value;
        } else if (option == "--payload-directory") {
            if (!ReadOptionValue(index, argc, argv, value)) return 2;
            payloadDirectory = std::filesystem::path(value);
        } else if (option == "--cache-root") {
            if (!ReadOptionValue(index, argc, argv, value)) return 2;
            cacheRoot = std::filesystem::path(value);
        } else {
            std::cerr << "Unknown option: " << option << "\n";
            PrintUsage();
            return 2;
        }
    }

    if (!hasTileSize && !hasAdaptiveBudget) {
        std::cerr << "--tile-size or --max-points-per-tile is required\n";
        return 2;
    }
    if (hasTileSize && hasAdaptiveBudget) {
        std::cerr << "--tile-size cannot be combined with --max-points-per-tile\n";
        return 2;
    }
    if (payloadDirectory.empty()) {
        payloadDirectory = cacheRoot.empty()
                               ? outputPath.parent_path() /
                                     (outputPath.stem().string() + "_payloads")
                               : outputPath.parent_path() / "payloads";
    } else if (payloadDirectory.is_relative()) {
        payloadDirectory = outputPath.parent_path() / payloadDirectory;
    }
    payloadDirectory = std::filesystem::absolute(payloadDirectory);
    if (!cacheRoot.empty()) {
        cacheRoot = std::filesystem::absolute(cacheRoot);
        if (!IsCachePayloadDirectory(outputPath, payloadDirectory)) {
            std::cerr << "--cache-root requires --payload-directory to be "
                         "output-dir/payloads\n";
            return 2;
        }
    }
    arguments["payloadDirectory"] = payloadDirectory.string();

    const auto temporaryRootPath = outputPath.parent_path() /
        (outputPath.stem().string() + ".tmp.usda");
    const auto temporaryManifestPath =
        std::filesystem::path(manifestPath.string() + ".tmp");
    const auto transactionPath = TransactionPath(outputPath);
    std::error_code error;
    std::string recoveryError;
    if (!RecoverIncompleteTransaction(
            outputPath, manifestPath, temporaryRootPath,
            temporaryManifestPath, payloadDirectory, transactionPath,
            recoveryError)) {
        std::cerr << "Unable to recover incomplete conversion: "
                  << recoveryError << "\n";
        return 2;
    }
    if (std::filesystem::exists(outputPath, error) || error ||
        std::filesystem::exists(manifestPath, error) || error ||
        std::filesystem::exists(payloadDirectory, error) || error ||
        std::filesystem::exists(transactionPath, error) || error) {
        std::cerr << "Output or payload directory already exists\n";
        return 2;
    }
    std::filesystem::create_directories(outputPath.parent_path(), error);
    if (error) {
        std::cerr << "Unable to create output directory: " << error.message()
                  << "\n";
        return 1;
    }

    usdpointcloud::PointReadRequest request;
    std::vector<usdgeo::Diagnostic> diagnostics;
    if (!usdpointcloud::NormalizeFileFormatArguments(
            arguments, request, diagnostics)) {
        if (diagnostics.empty()) {
            std::cerr << "Unable to normalize conversion arguments\n";
        }
        PrintDiagnostics(diagnostics);
        return 2;
    }
    request.readOptions.isCancelled = [] { return interrupted != 0; };

    usdlas::LasHeader header;
    auto stream = OpenPointStream(
        inputFormat, source.localPath, source.randomAccessSource,
        source.sourceName, request.readOptions, header, diagnostics);
    if (!stream) {
        if (diagnostics.empty()) {
            std::cerr << "Unable to open point stream\n";
        }
        PrintDiagnostics(diagnostics);
        return 1;
    }
    if (!request.attributes.empty()) {
        stream = std::make_unique<SelectedPointStream>(
            std::move(stream), request.attributes);
    }

    usdpointcloud::PointChunk metadataChunk;
    usdgeo::GeoReference reference;
    usdgeo::SpatialBounds bounds;
    std::string metadataError;
    if (!usdlas::BuildPointCloudMetadata(
            header, metadataChunk, reference, bounds, metadataError)) {
        std::cerr << "Unable to build source metadata: " << metadataError << "\n";
        return 1;
    }

    usdpointcloud::PointBudgetPlan adaptivePlan;
    usdpointcloud::TilePlan adaptiveTilePlan;
    usdpointcloud::PointStreamFactory adaptiveStreamFactory;
    usdgeo::cache::SourceIdentity planningIdentity;
    std::string tilePlanKey;
    if (request.maxPointsPerTile) {
        planningIdentity = source.identity;
        adaptiveStreamFactory = [&]() {
            usdlas::LasHeader passHeader;
            auto passStream = OpenPointStream(
                inputFormat, source.localPath, source.randomAccessSource,
                source.sourceName, request.readOptions, passHeader, diagnostics);
            if (passStream && !request.attributes.empty()) {
                passStream = std::make_unique<SelectedPointStream>(
                    std::move(passStream), request.attributes);
            }
            return passStream;
        };
        const usdpointcloud::PointBudgetConfig budget{
            *request.maxPointsPerTile, request.minPointsPerTile,
            request.maxTileDepth};
        if (!usdpointcloud::BuildPointBudgetPlan(
                adaptiveStreamFactory, budget, adaptivePlan, diagnostics) ||
            !usdpointcloud::BuildTilePlan(
                adaptivePlan, adaptiveTilePlan, diagnostics)) {
            PrintDiagnostics(diagnostics);
            return 1;
        }
        tilePlanKey = usdpointcloud::StableTilePlanKey(adaptiveTilePlan);
        if (tilePlanKey.empty()) {
            std::cerr << "Unable to build adaptive tile plan cache identity\n";
            return 1;
        }
    }

    const bool cacheEnabled = !cacheRoot.empty() && source.identity.IsValid();
    usdgeo::cache::Layout cacheLayout;
    usdgeo::cache::Layout cacheGenerationLayout;
    bool cacheHit = false;
    bool cacheCommitted = false;
    if (cacheEnabled) {
        usdgeo::cache::Descriptor descriptor;
        std::string cacheError;
        if (!BuildCacheDescriptor(source.identity, inputFormat, reference, request,
                      tilePlanKey, descriptor, cacheError)) {
            std::cerr << "Unable to build cache layout: " << cacheError << "\n";
            return 1;
        }
        if (request.maxPointsPerTile &&
            !SameSourceIdentity(planningIdentity, descriptor.source)) {
            std::cerr << "Input changed during adaptive tile planning\n";
            return 1;
        }
        if (!usdgeo::cache::TryBuildLayout(cacheRoot, descriptor, cacheLayout)) {
            std::cerr << "Unable to build cache layout: invalid cache root\n";
            return 1;
        }
        cacheHit = usdgeo::cache::IsCacheHit(cacheLayout);
        if (cacheHit) {
            std::error_code tileManifestError;
            cacheHit = std::filesystem::is_regular_file(
                           TileManifestPath(cacheLayout.payloadDirectory),
                           tileManifestError) &&
                       !tileManifestError;
        }
        if (!cacheHit) {
            std::error_code cacheCleanupError;
            std::filesystem::remove_all(cacheLayout.entryDirectory,
                                         cacheCleanupError);
            if (cacheCleanupError) {
                std::cerr << "Unable to clear incomplete cache entry: "
                          << cacheCleanupError.message() << "\n";
                return 1;
            }
            if (!CreateTemporaryCacheLayout(cacheLayout,
                                             cacheGenerationLayout,
                                             cacheError)) {
                std::cerr << cacheError << "\n";
                return 1;
            }
        } else {
            cacheCommitted = true;
        }
    }

    std::signal(SIGINT, HandleInterrupt);
    if (!std::filesystem::create_directory(transactionPath, error) || error) {
        std::cerr << "Unable to create conversion transaction marker: "
                  << error.message() << "\n";
        return 2;
    }
    std::string transactionStateError;
    if (!WriteTransactionState(
            transactionPath, payloadDirectory, transactionStateError)) {
        std::cerr << "Unable to create conversion transaction state: "
                  << transactionStateError << "\n";
        std::filesystem::remove_all(transactionPath, error);
        return 2;
    }
    auto layer = pxr::SdfLayer::CreateAnonymous("PointCloud.tmp.usda");
    if (!layer) {
        std::cerr << "Unable to create temporary root layer\n";
        std::filesystem::remove_all(transactionPath, error);
        return 1;
    }

    const auto generationRootPath = cacheEnabled
                                        ? cacheGenerationLayout.rootLayer
                                        : temporaryRootPath;
    const auto generationPayloadDirectory = cacheEnabled
                                                 ? cacheGenerationLayout.payloadDirectory
                                                 : payloadDirectory;
    const auto cleanup = [&](bool removePublishedRoot = false) {
        layer = nullptr;
        if (removePublishedRoot) {
            std::filesystem::remove(outputPath, error);
        }
        std::filesystem::remove(temporaryRootPath, error);
        std::filesystem::remove(temporaryManifestPath, error);
        std::filesystem::remove(manifestPath, error);
        std::filesystem::remove_all(payloadDirectory, error);
        if (cacheEnabled && !cacheCommitted &&
            cacheGenerationLayout.IsValid()) {
            std::filesystem::remove_all(cacheGenerationLayout.entryDirectory,
                                        error);
        }
        std::filesystem::remove_all(transactionPath, error);
    };

    if (cacheHit) {
        std::string materializeError;
        if (!MaterializeCache(cacheLayout, temporaryRootPath,
                              payloadDirectory, materializeError) ||
            !WriteManifest(temporaryManifestPath, manifestInputName, inputSize,
                           outputPath,
                           payloadDirectory, request, materializeError)) {
            cleanup();
            std::cerr << "Unable to materialize cache hit: "
                      << materializeError << "\n";
            return 1;
        }
        if (interrupted != 0) {
            std::cerr << "Conversion cancelled\n";
            cleanup();
            return 1;
        }
        layer = nullptr;
        std::filesystem::rename(temporaryManifestPath, manifestPath, error);
        if (error) {
            std::cerr << "Unable to publish conversion manifest: "
                      << error.message() << "\n";
            cleanup();
            return 1;
        }
        if (interrupted != 0) {
            std::cerr << "Conversion cancelled\n";
            cleanup();
            return 1;
        }
        std::filesystem::rename(temporaryRootPath, outputPath, error);
        if (error) {
            std::cerr << "Unable to publish generated root layer: "
                      << error.message() << "\n";
            cleanup();
            return 1;
        }
        if (interrupted != 0) {
            std::cerr << "Conversion cancelled\n";
            cleanup(true);
            return 1;
        }
        std::filesystem::remove_all(transactionPath, error);
        std::cout << "Cache hit " << cacheLayout.entryDirectory.string()
                  << "\n";
        std::cout << "Generated " << outputPath.string() << "\n";
        std::cout << "Payloads: " << payloadDirectory.string() << "\n";
        PrintCacheStatistics();
        return 0;
    }

    std::vector<usdpointcloud::PointTileManifestEntry> tileManifestEntries;
    usdgeo::PointCloudPayloadOptions payloadOptions{
        generationPayloadDirectory.string(), generationRootPath.string(),
        request.tileMemoryLimitBytes, request.readOptions.isCancelled, {},
        &tileManifestEntries};
    payloadOptions.spoolDirectory = (transactionPath / "spool").string();
    std::unique_ptr<usdpointcloud::PointStream> plannedStream;
    const auto authored = [&]() {
        if (!request.maxPointsPerTile) {
            return usdgeo::AuthorPointCloudTiledAssetFromStream(
                layer.operator->(), "/PointCloud", *stream, reference,
                {request.tileSize, 0}, payloadOptions, diagnostics);
        }

        if (!adaptiveStreamFactory) {
            return false;
        }
        usdgeo::cache::SourceIdentity authoringIdentity;
        if (source.identity.IsValid()) {
            std::string identityError;
            if (!RefreshSourceIdentity(source, authoringIdentity, identityError) ||
                !SameSourceIdentity(planningIdentity, authoringIdentity)) {
                diagnostics.push_back({
                    usdgeo::DiagnosticCode::DecodeFailure,
                    usdgeo::Severity::Error,
                    "input changed during adaptive tile planning"});
                return false;
            }
        }
        plannedStream = adaptiveStreamFactory();
        if (!plannedStream) return false;
        const usdpointcloud::PointBudgetTileRouter router(adaptiveTilePlan);
        return usdgeo::AuthorPointCloudTiledAssetFromStream(
            layer.operator->(), "/PointCloud", *plannedStream, reference,
            router, payloadOptions, diagnostics);
    }();
    if (!authored || interrupted != 0) {
        if (interrupted != 0) {
            std::cerr << "Conversion cancelled\n";
        }
        if (!authored && diagnostics.empty()) {
            std::cerr << "Unable to author tiled point-cloud asset\n";
        }
        PrintDiagnostics(diagnostics);
        cleanup();
        return 1;
    }
    bool saved = false;
    if (cacheEnabled) {
        const auto cacheLayer =
            pxr::SdfLayer::CreateNew(generationRootPath.string());
        if (cacheLayer) {
            cacheLayer->TransferContent(layer);
            saved = cacheLayer->Save();
        }
    } else {
        layer->SetIdentifier(generationRootPath.string());
        saved = layer->Save();
    }
    if (!saved) {
        std::cerr << "Unable to save generated root layer\n";
        cleanup();
        return 1;
    }
    std::string manifestError;
    if (!WriteTileManifest(
            TileManifestPath(generationPayloadDirectory),
            {tileManifestEntries}, manifestError)) {
        std::cerr << "Unable to write tile manifest: " << manifestError
                  << "\n";
        cleanup();
        return 1;
    }
    const auto generatedManifestPath =
        cacheEnabled ? cacheGenerationLayout.manifest : temporaryManifestPath;
    const auto generatedOutputPath =
        cacheEnabled ? cacheGenerationLayout.rootLayer : outputPath;
    if (!WriteManifest(generatedManifestPath, manifestInputName, inputSize,
                       generatedOutputPath,
                       generationPayloadDirectory, request, manifestError)) {
        std::cerr << "Unable to write conversion manifest: " << manifestError
                  << "\n";
        cleanup();
        return 1;
    }
    if (cacheEnabled) {
        if (!PublishCacheEntry(cacheGenerationLayout, cacheLayout,
                               manifestError)) {
            cleanup();
            std::cerr << "Unable to publish generated cache: "
                      << manifestError << "\n";
            return 1;
        }
        cacheCommitted = true;
        layer = nullptr;
        if (!MaterializeCache(cacheLayout, temporaryRootPath,
                              payloadDirectory, manifestError) ||
            !WriteManifest(temporaryManifestPath, manifestInputName, inputSize,
                           outputPath,
                           payloadDirectory, request, manifestError)) {
            cleanup();
            std::cerr << "Unable to materialize generated cache: "
                      << manifestError << "\n";
            return 1;
        }
        if (interrupted != 0) {
            std::cerr << "Conversion cancelled\n";
            cleanup();
            return 1;
        }
        std::filesystem::rename(temporaryManifestPath, manifestPath, error);
        if (error) {
            std::cerr << "Unable to publish conversion manifest: "
                      << error.message() << "\n";
            cleanup();
            return 1;
        }
        if (interrupted != 0) {
            std::cerr << "Conversion cancelled\n";
            cleanup();
            return 1;
        }
        std::filesystem::rename(temporaryRootPath, outputPath, error);
        if (error) {
            std::cerr << "Unable to publish generated root layer: "
                      << error.message() << "\n";
            cleanup();
            return 1;
        }
        if (interrupted != 0) {
            std::cerr << "Conversion cancelled\n";
            cleanup(true);
            return 1;
        }
        std::filesystem::remove_all(transactionPath, error);
        std::cout << "Generated cache " << cacheLayout.entryDirectory.string()
                  << "\n";
        std::cout << "Generated " << outputPath.string() << "\n";
        std::cout << "Payloads: " << payloadDirectory.string() << "\n";
        PrintCacheStatistics();
        return 0;
    }
    if (interrupted != 0) {
        std::cerr << "Conversion cancelled\n";
        cleanup();
        return 1;
    }
    layer = nullptr;
    std::filesystem::rename(temporaryManifestPath, manifestPath, error);
    if (error) {
        std::cerr << "Unable to publish conversion manifest: "
                  << error.message() << "\n";
        cleanup();
        return 1;
    }
    if (interrupted != 0) {
        std::cerr << "Conversion cancelled\n";
        cleanup();
        return 1;
    }
    std::filesystem::rename(temporaryRootPath, outputPath, error);
    if (error) {
        std::cerr << "Unable to publish generated root layer: "
                  << error.message() << "\n";
        cleanup();
        return 1;
    }
    if (interrupted != 0) {
        std::cerr << "Conversion cancelled\n";
        cleanup(true);
        return 1;
    }
    std::filesystem::remove_all(transactionPath, error);

    std::cout << "Generated " << outputPath.string() << "\n";
    std::cout << "Payloads: " << payloadDirectory.string() << "\n";
    return 0;
}
