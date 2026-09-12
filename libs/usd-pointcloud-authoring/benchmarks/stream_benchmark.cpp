#include "usdgeo/PointCloudLayer.h"
#include "usdcopc/Copc.h"
#include "usdlas/Las.h"
#include "usdlaz/Laz.h"
#include "usdply/Ply.h"

#include <pxr/usd/sdf/layer.h>

#ifdef _WIN32
#include <windows.h>
#include <psapi.h>
#elif defined(__linux__)
#include <unistd.h>
#elif defined(__APPLE__)
#include <mach/mach.h>
#else
#include <sys/resource.h>
#endif

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cmath>
#include <cctype>
#include <cstdint>
#include <filesystem>
#include <iostream>
#include <fstream>
#include <limits>
#include <memory>
#include <string>
#include <thread>
#include <unordered_set>
#include <vector>

namespace {

struct BenchmarkOptions {
    std::string inputPath;
    std::string inputFormat;
    std::string tilingStrategy = "fixed-grid";
    int epsgCode = 0;
    bool epsgSpecified = false;
    std::size_t pointCount = 1'000'000;
    std::size_t chunkPointCount = 65'536;
    double tileSize = 128.0;
    std::size_t memoryLimitBytes = 1 * 1024 * 1024;
    std::size_t maxPointsPerTile = 4096;
    std::size_t minPointsPerTile = 1;
    std::int32_t maxDepth = 16;
};

constexpr std::size_t kPointsPerTile = 4096;
constexpr std::size_t kTilesPerRow = 32;

bool ParseSize(const std::string& text, std::size_t& value) {
    try {
        const auto parsed = std::stoull(text);
        if (parsed == 0 || parsed > std::numeric_limits<std::size_t>::max()) {
            return false;
        }
        value = static_cast<std::size_t>(parsed);
        return true;
    } catch (...) {
        return false;
    }
}

bool ParseDouble(const std::string& text, double& value) {
    try {
        value = std::stod(text);
        return std::isfinite(value) && value > 0.0;
    } catch (...) {
        return false;
    }
}

bool ParseEpsg(const std::string& text, int& value) {
    try {
        const auto parsed = std::stoll(text);
        if (parsed <= 0 || parsed > std::numeric_limits<int>::max()) {
            return false;
        }
        value = static_cast<int>(parsed);
        return true;
    } catch (...) {
        return false;
    }
}

bool ParseOptions(int argc, char** argv, BenchmarkOptions& options) {
    for (int index = 1; index < argc; ++index) {
        if (std::string(argv[index]) == "--help") {
            std::cout
                << "Usage: usdPointCloudAuthoring_stream_benchmark"
                   " [--input PATH] [--format las|laz|copc|ply] [--epsg CODE]"
                   " [--points N]"
                   " [--chunk-points N] [--tiling fixed-grid|adaptive]"
                   " [--tile-size N] [--memory-limit BYTES]"
                   " [--max-points-per-tile N] [--min-points-per-tile N]"
                   " [--max-depth N]\n";
            return false;
        }
        if (index + 1 >= argc) return false;
        const std::string argument(argv[index]);
        const std::string value(argv[++index]);
        if (argument == "--input") {
            options.inputPath = value;
        } else if (argument == "--format") {
            options.inputFormat = value;
        } else if (argument == "--tiling") {
            options.tilingStrategy = value;
            if (options.tilingStrategy != "fixed-grid" &&
                options.tilingStrategy != "adaptive") {
                return false;
            }
        } else if (argument == "--epsg") {
            if (!ParseEpsg(value, options.epsgCode)) return false;
            options.epsgSpecified = true;
        } else if (argument == "--points") {
            if (!ParseSize(value, options.pointCount)) return false;
        } else if (argument == "--chunk-points") {
            if (!ParseSize(value, options.chunkPointCount)) return false;
        } else if (argument == "--tile-size") {
            if (!ParseDouble(value, options.tileSize)) return false;
        } else if (argument == "--memory-limit") {
            if (!ParseSize(value, options.memoryLimitBytes)) return false;
        } else if (argument == "--max-points-per-tile") {
            if (!ParseSize(value, options.maxPointsPerTile)) return false;
        } else if (argument == "--min-points-per-tile") {
            if (!ParseSize(value, options.minPointsPerTile)) return false;
        } else if (argument == "--max-depth") {
            try {
                const auto parsed = std::stoll(value);
                if (parsed < 0 ||
                    parsed > std::numeric_limits<std::int32_t>::max()) {
                    return false;
                }
                options.maxDepth = static_cast<std::int32_t>(parsed);
            } catch (...) {
                return false;
            }
        } else {
            return false;
        }
    }
    return true;
}

std::string Lowercase(std::string value) {
    std::transform(value.begin(), value.end(), value.begin(),
                   [](unsigned char character) {
                       return static_cast<char>(std::tolower(character));
                   });
    return value;
}

std::string InputFormat(const BenchmarkOptions& options) {
    if (!options.inputFormat.empty()) return Lowercase(options.inputFormat);
    const auto extension = std::filesystem::path(options.inputPath).extension();
    return Lowercase(extension.string().substr(1));
}

std::uint64_t ResidentBytes() {
#ifdef _WIN32
    PROCESS_MEMORY_COUNTERS_EX counters{};
    if (!GetProcessMemoryInfo(
            GetCurrentProcess(),
            reinterpret_cast<PROCESS_MEMORY_COUNTERS*>(&counters),
            sizeof(counters))) {
        return 0;
    }
    return static_cast<std::uint64_t>(counters.WorkingSetSize);
#elif defined(__linux__)
    std::ifstream statm("/proc/self/statm");
    std::uint64_t totalPages = 0;
    std::uint64_t residentPages = 0;
    const auto pageSize = sysconf(_SC_PAGESIZE);
    if (pageSize <= 0 || !(statm >> totalPages >> residentPages)) return 0;
    return residentPages * static_cast<std::uint64_t>(pageSize);
#elif defined(__APPLE__)
    mach_task_basic_info info{};
    mach_msg_type_number_t count = MACH_TASK_BASIC_INFO_COUNT;
    if (task_info(mach_task_self(), MACH_TASK_BASIC_INFO,
                  reinterpret_cast<task_info_t>(&info), &count) != KERN_SUCCESS) {
        return 0;
    }
    return static_cast<std::uint64_t>(info.resident_size);
#else
    struct rusage usage {};
    if (getrusage(RUSAGE_SELF, &usage) != 0) return 0;
#ifdef __APPLE__
    return static_cast<std::uint64_t>(usage.ru_maxrss);
#else
    return static_cast<std::uint64_t>(usage.ru_maxrss) * 1024;
#endif
#endif
}

std::uint64_t ProcessWriteBytes() {
#ifdef _WIN32
    IO_COUNTERS counters{};
    if (!GetProcessIoCounters(GetCurrentProcess(), &counters)) return 0;
    return static_cast<std::uint64_t>(counters.WriteTransferCount);
#else
    return 0;
#endif
}

std::unordered_set<std::filesystem::path> ExistingSpoolDirectories() {
    std::unordered_set<std::filesystem::path> result;
    std::error_code error;
    const auto temporaryDirectory = std::filesystem::temp_directory_path(error);
    if (error) return result;
    for (const auto& entry : std::filesystem::directory_iterator(
             temporaryDirectory, error)) {
        if (error) break;
        if (entry.is_directory(error) &&
            entry.path().filename().string().rfind(
                "usdgeo_point_spool_", 0) == 0) {
            result.insert(entry.path());
        }
        error.clear();
    }
    return result;
}

std::uint64_t DirectoryBytes(const std::filesystem::path& directory) {
    std::uint64_t bytes = 0;
    std::error_code error;
    for (const auto& entry : std::filesystem::directory_iterator(
             directory, error)) {
        if (error) break;
        if (entry.is_regular_file(error)) bytes += entry.file_size(error);
        error.clear();
    }
    return bytes;
}

std::uint64_t NewSpoolFileBytes(
    const std::unordered_set<std::filesystem::path>& existingDirectories) {
    std::uint64_t bytes = 0;
    for (const auto& directory : ExistingSpoolDirectories()) {
        if (existingDirectories.count(directory) == 0) {
            bytes += DirectoryBytes(directory);
        }
    }
    return bytes;
}

std::uint64_t PayloadBytes(const std::filesystem::path& directory) {
    std::uint64_t bytes = 0;
    std::error_code error;
    for (const auto& entry : std::filesystem::directory_iterator(
             directory, error)) {
        if (error) break;
        if (entry.is_regular_file(error) && entry.path().extension() == ".usdc") {
            bytes += entry.file_size(error);
        }
        error.clear();
    }
    return bytes;
}

bool CountPayloads(const std::filesystem::path& directory,
                   std::uint64_t& count) {
    count = 0;
    std::error_code error;
    for (const auto& entry : std::filesystem::directory_iterator(
             directory, error)) {
        if (error) return false;
        if (entry.is_regular_file(error) &&
            entry.path().extension() == ".usdc") {
            ++count;
        }
        if (error) return false;
    }
    return true;
}

std::uint64_t DirectoryBytesRecursive(const std::filesystem::path& directory) {
    std::uint64_t bytes = 0;
    std::error_code error;
    for (const auto& entry : std::filesystem::recursive_directory_iterator(
             directory, error)) {
        if (error) break;
        if (entry.is_regular_file(error)) bytes += entry.file_size(error);
        error.clear();
    }
    return bytes;
}

class GeneratedPointStream final : public usdpointcloud::PointStream {
public:
    GeneratedPointStream(std::size_t pointCount, std::size_t chunkPointCount,
                         double tileSize)
        : pointCount_(pointCount),
          chunkPointCount_(chunkPointCount),
          tileSize_(tileSize) {}

    usdpointcloud::PointStreamStatus ReadNext(
        usdpointcloud::PointChunk& chunk,
        usdpointcloud::PointData& data,
        usdgeo::Diagnostic& diagnostic) override {
        diagnostic = {};
        if (index_ == pointCount_) return usdpointcloud::PointStreamStatus::End;

        const auto count = std::min(chunkPointCount_, pointCount_ - index_);
        data.positions.reserve(count);
        data.intensity.reserve(count);
        data.extraByteNames = {"temperature"};
        data.extraByteComponentCounts = {1};
        data.extraBytes.emplace_back();
        data.extraBytes.front().reserve(count);

        for (std::size_t offset = 0; offset < count; ++offset) {
            const auto pointIndex = index_ + offset;
            const auto tileIndex = pointIndex / kPointsPerTile;
            const auto tileX = tileIndex % kTilesPerRow;
            const auto tileY = tileIndex / kTilesPerRow;
            const auto pointInTile = pointIndex % kPointsPerTile;
            const auto localX = static_cast<double>(pointInTile % 64) * 0.25;
            const auto localY = static_cast<double>(pointInTile / 64) * 0.25;
            data.positions.push_back(
                {static_cast<double>(tileX) * tileSize_ + localX + 0.25,
                 static_cast<double>(tileY) * tileSize_ + localY + 0.25,
                 static_cast<double>(pointIndex % 1000)});
            data.intensity.push_back(static_cast<std::uint16_t>(pointIndex));
            data.extraBytes.front().push_back(
                static_cast<double>(pointIndex % 1000) * 0.1);
        }
        usdgeo::SpatialBounds bounds = usdgeo::SpatialBounds::Empty();
        for (const auto& position : data.positions) bounds.Expand(position);
        chunk = usdpointcloud::MakePointChunk(data, bounds);
        index_ += count;
        return usdpointcloud::PointStreamStatus::Chunk;
    }

    std::uint64_t SourceBytesRead() const noexcept override {
        return static_cast<std::uint64_t>(pointCount_) *
               (sizeof(usdgeo::Vec3d) + sizeof(std::uint16_t) + sizeof(double));
    }

private:
    std::size_t pointCount_;
    std::size_t chunkPointCount_;
    double tileSize_;
    std::size_t index_ = 0;
};

class CountingPointStream final : public usdpointcloud::PointStream {
public:
    CountingPointStream(std::unique_ptr<usdpointcloud::PointStream> stream,
                        std::uint64_t& totalBytes)
        : stream_(std::move(stream)), totalBytes_(totalBytes) {}

    ~CountingPointStream() override {
        if (stream_) totalBytes_ += stream_->SourceBytesRead();
    }

    usdpointcloud::PointStreamStatus ReadNext(
        usdpointcloud::PointChunk& chunk,
        usdpointcloud::PointData& data,
        usdgeo::Diagnostic& diagnostic) override {
        return stream_->ReadNext(chunk, data, diagnostic);
    }

    std::uint64_t SourceBytesRead() const noexcept override {
        return stream_ ? stream_->SourceBytesRead() : 0;
    }

private:
    std::unique_ptr<usdpointcloud::PointStream> stream_;
    std::uint64_t& totalBytes_;
};

} // namespace

int main(int argc, char** argv) {
    BenchmarkOptions options;
    if (!ParseOptions(argc, argv, options)) {
        return argc > 1 && std::string(argv[1]) == "--help" ? 0 : 2;
    }

    const auto stamp = std::chrono::steady_clock::now().time_since_epoch().count();
    const auto outputDirectory = std::filesystem::temp_directory_path() /
                                 ("usdgeo_stream_benchmark_" +
                                  std::to_string(stamp));
    const auto rootLayerPath = outputDirectory / "PointCloud.usda";
    std::error_code error;
    std::filesystem::create_directories(outputDirectory, error);
    if (error) {
        std::cerr << "unable to create benchmark output directory\n";
        return 1;
    }

    const auto existingSpoolDirectories = ExistingSpoolDirectories();
    std::atomic<bool> sampling{true};
    std::atomic<std::uint64_t> peakResident{ResidentBytes()};
    std::atomic<std::uint64_t> peakSpoolFileBytes{0};
    const auto sample = [&]() {
        while (sampling.load(std::memory_order_relaxed)) {
            peakResident.store(
                std::max(peakResident.load(std::memory_order_relaxed),
                         ResidentBytes()),
                std::memory_order_relaxed);
            peakSpoolFileBytes.store(
                std::max(peakSpoolFileBytes.load(std::memory_order_relaxed),
                         NewSpoolFileBytes(existingSpoolDirectories)),
                std::memory_order_relaxed);
            std::this_thread::sleep_for(std::chrono::milliseconds(10));
        }
    };
    const auto baselineResident = ResidentBytes();
    const auto writeBytesBefore = ProcessWriteBytes();
    std::thread sampler(sample);

    const auto start = std::chrono::steady_clock::now();
    const auto layer = pxr::SdfLayer::CreateAnonymous("stream_benchmark.usda");
    if (!layer) {
        sampling.store(false, std::memory_order_relaxed);
        sampler.join();
        std::filesystem::remove_all(outputDirectory, error);
        return 1;
    }
    usdgeo::GeoReference reference;
    std::unique_ptr<usdpointcloud::PointStream> stream;
    const auto openStream = [&]()
        -> std::unique_ptr<usdpointcloud::PointStream> {
        if (options.inputPath.empty()) {
            return std::make_unique<GeneratedPointStream>(
                options.pointCount, options.chunkPointCount, options.tileSize);
        }
        const auto format = InputFormat(options);
        usdpointcloud::PointReadOptions readOptions;
        readOptions.chunkPointLimit = options.chunkPointCount;
        readOptions.memoryBudgetBytes = options.memoryLimitBytes;
        std::vector<usdgeo::Diagnostic> openDiagnostics;
        if (format == "las") {
            usdlas::LasHeader header;
            return usdlas::OpenLasPointStream(
                options.inputPath, readOptions, header, openDiagnostics);
        }
        if (format == "laz") {
            usdlas::LasHeader header;
            return usdlaz::OpenLazPointStream(
                options.inputPath, readOptions, header, openDiagnostics);
        }
        if (format == "copc") {
            usdcopc::CopcHeader header;
            return usdcopc::OpenCopcPointStream(
                options.inputPath, readOptions, header, openDiagnostics);
        }
        if (format == "ply" && options.epsgSpecified) {
            usdply::PlyHeader header;
            return usdply::OpenPointStream(
                options.inputPath, readOptions, header, openDiagnostics);
        }
        return nullptr;
    };
    std::uint64_t pointCount = options.pointCount;
    if (options.inputPath.empty()) {
        reference.epsgCode = 26910;
        stream = openStream();
    } else {
        const auto format = InputFormat(options);
        usdpointcloud::PointReadOptions readOptions;
        readOptions.chunkPointLimit = options.chunkPointCount;
        readOptions.memoryBudgetBytes = options.memoryLimitBytes;
        std::vector<usdgeo::Diagnostic> openDiagnostics;
        usdpointcloud::PointChunk metadataChunk;
        usdgeo::SpatialBounds metadataBounds;
        std::string metadataError;
        bool metadataValid = true;
        std::uint64_t sourcePointCount = 0;
        if (format == "las") {
            usdlas::LasHeader header;
            stream = usdlas::OpenLasPointStream(
                options.inputPath, readOptions, header, openDiagnostics);
            sourcePointCount = header.pointCount;
            metadataValid = usdlas::BuildPointCloudMetadata(
                header, metadataChunk, reference, metadataBounds, metadataError);
        } else if (format == "laz") {
            usdlas::LasHeader header;
            stream = usdlaz::OpenLazPointStream(
                options.inputPath, readOptions, header, openDiagnostics);
            sourcePointCount = header.pointCount;
            metadataValid = usdlas::BuildPointCloudMetadata(
                header, metadataChunk, reference, metadataBounds, metadataError);
        } else if (format == "copc") {
            usdcopc::CopcHeader header;
            stream = usdcopc::OpenCopcPointStream(
                options.inputPath, readOptions, header, openDiagnostics);
            sourcePointCount = header.las.pointCount;
            metadataValid = usdlas::BuildPointCloudMetadata(
                header.las, metadataChunk, reference, metadataBounds, metadataError);
        } else if (format == "ply") {
            if (!options.epsgSpecified) {
                std::cerr << "--epsg is required for PLY input\n";
                sampling.store(false, std::memory_order_relaxed);
                sampler.join();
                std::filesystem::remove_all(outputDirectory, error);
                return 2;
            }
            usdply::PlyHeader header;
            stream = usdply::OpenPointStream(
                options.inputPath, readOptions, header, openDiagnostics);
            reference.epsgCode = options.epsgCode;
            for (const auto& element : header.elements) {
                if (element.name == "vertex") {
                    sourcePointCount = element.count;
                    break;
                }
            }
        } else {
            std::cerr << "unsupported input format: " << format << '\n';
        }
        if (!stream || !metadataValid || sourcePointCount == 0) {
            for (const auto& diagnostic : openDiagnostics) {
                std::cerr << diagnostic.message << '\n';
            }
            if (!metadataError.empty()) std::cerr << metadataError << '\n';
            sampling.store(false, std::memory_order_relaxed);
            sampler.join();
            std::filesystem::remove_all(outputDirectory, error);
            return 1;
        }
        pointCount = sourcePointCount;
    }
    if (!stream) {
        std::cerr << "unable to open input stream\n";
        sampling.store(false, std::memory_order_relaxed);
        sampler.join();
        std::filesystem::remove_all(outputDirectory, error);
        return 1;
    }
    std::vector<usdgeo::Diagnostic> diagnostics;
    usdpointcloud::SpoolIoStats spoolIoStats;
    std::vector<usdpointcloud::PointTileManifestEntry> manifestEntries;
    std::uint64_t initialSourceReadBytes = 0;
    std::uint64_t plannedSourceReadBytes = 0;
    std::unique_ptr<usdpointcloud::TileRouter> adaptiveRouter;
    usdpointcloud::FixedGridTileRouter fixedRouter(
        {options.tileSize, 0});
    const usdpointcloud::TileRouter* router = &fixedRouter;
    if (options.tilingStrategy == "adaptive") {
        const usdpointcloud::PointBudgetConfig budgetConfig{
            options.maxPointsPerTile, options.minPointsPerTile,
            options.maxDepth};
        if (!options.inputPath.empty()) {
            initialSourceReadBytes = stream->SourceBytesRead();
        }
        stream.reset();
        const usdpointcloud::PointStreamFactory streamFactory = [&]() {
            auto plannedStream = openStream();
            if (!plannedStream) return plannedStream;
            return std::unique_ptr<usdpointcloud::PointStream>(
                std::make_unique<CountingPointStream>(
                    std::move(plannedStream), plannedSourceReadBytes));
        };
        usdpointcloud::PointBudgetPlan plan;
        if (!usdpointcloud::BuildPointBudgetPlan(
                streamFactory, budgetConfig, plan, diagnostics)) {
            for (const auto& diagnostic : diagnostics) {
                std::cerr << diagnostic.message << '\n';
            }
            sampling.store(false, std::memory_order_relaxed);
            sampler.join();
            std::filesystem::remove_all(outputDirectory, error);
            return 1;
        }
        adaptiveRouter = std::make_unique<usdpointcloud::PointBudgetTileRouter>(
            plan);
        if (!adaptiveRouter->IsValid()) {
            std::cerr << "adaptive tile plan produced an invalid router\n";
            sampling.store(false, std::memory_order_relaxed);
            sampler.join();
            std::filesystem::remove_all(outputDirectory, error);
            return 1;
        }
        router = adaptiveRouter.get();
        stream = openStream();
    }
    if (!stream) {
        std::cerr << "unable to reopen input stream\n";
        sampling.store(false, std::memory_order_relaxed);
        sampler.join();
        std::filesystem::remove_all(outputDirectory, error);
        return 1;
    }
    const auto authored = usdgeo::AuthorPointCloudTiledAssetFromStream(
        layer.operator->(), "/PointCloud", *stream, reference,
        *router,
        {outputDirectory.string(), rootLayerPath.string(),
         options.memoryLimitBytes, {}, {}, &manifestEntries, &spoolIoStats},
        diagnostics);
    const auto exported = authored && layer->Export(rootLayerPath.string());
    std::uint64_t tileCount = 0;
    const auto countedPayloads = exported &&
                                 CountPayloads(outputDirectory, tileCount);
    const auto succeeded = authored && exported && countedPayloads;
    const auto elapsed = std::chrono::duration<double>(
                             std::chrono::steady_clock::now() - start)
                             .count();

    sampling.store(false, std::memory_order_relaxed);
    sampler.join();
    peakResident.store(
        std::max(peakResident.load(std::memory_order_relaxed), ResidentBytes()),
        std::memory_order_relaxed);
    peakSpoolFileBytes.store(
        std::max(peakSpoolFileBytes.load(std::memory_order_relaxed),
                 NewSpoolFileBytes(existingSpoolDirectories)),
        std::memory_order_relaxed);
    const auto outputBytes = DirectoryBytesRecursive(outputDirectory);
    const auto payloadBytes = PayloadBytes(outputDirectory);
    const auto sourceReadBytes = initialSourceReadBytes +
                                 plannedSourceReadBytes +
                                 stream->SourceBytesRead();
    const auto totalIoBytes = sourceReadBytes + spoolIoStats.bytesWritten +
                              spoolIoStats.bytesRead + payloadBytes;
    const auto writeBytesAfter = ProcessWriteBytes();
    std::uint64_t minimumTilePoints = 0;
    std::uint64_t maximumTilePoints = 0;
    std::uint64_t totalTilePoints = 0;
    std::int32_t treeDepth = 0;
    std::uint64_t minimumPayloadBytes = 0;
    std::uint64_t maximumPayloadBytes = 0;
    std::uint64_t totalPayloadBytes = 0;
    std::size_t tileManifestCount = 0;
    for (const auto& entry : manifestEntries) {
        if (entry.lod != 0) continue;
        const auto entryPayloadPath = outputDirectory / entry.payloadPath;
        std::error_code payloadError;
        const std::uint64_t entryPayloadBytes = std::filesystem::file_size(
            entryPayloadPath, payloadError);
        if (payloadError) continue;
        ++tileManifestCount;
        minimumTilePoints = tileManifestCount == 1
            ? entry.pointCount
            : std::min(minimumTilePoints, entry.pointCount);
        maximumTilePoints = std::max(maximumTilePoints, entry.pointCount);
        totalTilePoints += entry.pointCount;
        treeDepth = std::max(treeDepth, entry.id.level);
        minimumPayloadBytes = tileManifestCount == 1
            ? entryPayloadBytes
            : std::min(minimumPayloadBytes, entryPayloadBytes);
        maximumPayloadBytes = std::max(maximumPayloadBytes, entryPayloadBytes);
        totalPayloadBytes += entryPayloadBytes;
    }
    std::cout << "format=" << (options.inputPath.empty()
                                   ? "generated"
                                   : InputFormat(options))
              << " input="
              << (options.inputPath.empty() ? "generated" : options.inputPath)
              << " points=" << pointCount
              << " chunk_points=" << options.chunkPointCount
              << " tiling=" << options.tilingStrategy
              << " tile_size=" << options.tileSize
              << " tile_count=" << tileCount
              << " tile_manifest_count=" << tileManifestCount
              << " tile_point_min=" << minimumTilePoints
              << " tile_point_max=" << maximumTilePoints
              << " tile_point_average="
              << (tileManifestCount == 0
                    ? 0.0
                    : static_cast<double>(totalTilePoints) /
                        static_cast<double>(tileManifestCount))
              << " tile_payload_min_bytes=" << minimumPayloadBytes
              << " tile_payload_max_bytes=" << maximumPayloadBytes
              << " tile_payload_average_bytes="
              << (tileManifestCount == 0
                    ? 0.0
                    : static_cast<double>(totalPayloadBytes) /
                        static_cast<double>(tileManifestCount))
              << " tree_depth=" << treeDepth
              << " memory_limit_bytes=" << options.memoryLimitBytes
              << " elapsed_seconds=" << elapsed
              << " baseline_rss_bytes=" << baselineResident
              << " peak_rss_bytes=" << peakResident.load()
              << " rss_delta_bytes="
              << (peakResident.load() > baselineResident
                      ? peakResident.load() - baselineResident
                      : 0)
              << " peak_spool_file_bytes=" << peakSpoolFileBytes.load()
              << " payload_bytes=" << payloadBytes
              << " source_read_bytes=" << sourceReadBytes
              << " spool_bytes_written=" << spoolIoStats.bytesWritten
              << " spool_bytes_read=" << spoolIoStats.bytesRead
              << " io_amplification="
              << (sourceReadBytes == 0
                    ? 0.0
                    : static_cast<double>(totalIoBytes) /
                        static_cast<double>(sourceReadBytes))
              << " output_bytes=" << outputBytes
              << " process_write_bytes="
              << (writeBytesAfter >= writeBytesBefore
                      ? writeBytesAfter - writeBytesBefore
                      : 0)
              << " success=" << (succeeded ? "true" : "false") << '\n';
    for (const auto& diagnostic : diagnostics) {
        std::cerr << diagnostic.message << '\n';
    }
    std::filesystem::remove_all(outputDirectory, error);
    return succeeded ? 0 : 1;
}