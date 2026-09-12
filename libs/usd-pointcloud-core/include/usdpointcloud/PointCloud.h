#pragma once

#include "usdgeo/Diagnostic.h"
#include "usdgeo/GeoReference.h"
#include "usdgeo/SpatialBounds.h"

#include <array>
#include <cstdint>
#include <functional>
#include <optional>
#include <string>
#include <vector>

namespace usdpointcloud {

enum class PointAttributeType {
    Int32,
    Int16,
    UInt8,
    UInt16,
    UInt32,
    UInt64,
    Float32,
    Float64,
    Float64Vec2,
    Float64Vec3
};

struct PointAttribute {
    std::string name;
    PointAttributeType type = PointAttributeType::Float32;

    bool IsValid() const noexcept;
};

struct PointChunk {
    std::uint64_t pointCount = 0;
    usdgeo::SpatialBounds bounds = usdgeo::SpatialBounds::Empty();
    std::vector<PointAttribute> attributes;

    bool IsValid() const noexcept;
};

struct PointData {
    std::vector<usdgeo::Vec3d> positions;
    std::vector<std::uint16_t> intensity;
    std::vector<std::uint8_t> returnNumber;
    std::vector<std::uint8_t> numberOfReturns;
    std::vector<std::uint8_t> classification;
    std::vector<std::uint8_t> classificationFlags;
    std::vector<std::uint8_t> scannerChannel;
    std::vector<std::uint8_t> scanDirectionFlag;
    std::vector<std::uint8_t> edgeOfFlightLine;
    std::vector<std::uint8_t> userData;
    std::vector<std::int16_t> scanAngle;
    std::vector<std::uint16_t> pointSourceId;
    std::vector<std::uint16_t> red;
    std::vector<std::uint16_t> green;
    std::vector<std::uint16_t> blue;
    std::uint8_t colorBitDepth = 16;
    std::vector<std::uint16_t> nir;
    std::vector<double> gpsTime;
    std::vector<std::uint8_t> waveformDescriptorIndex;
    std::vector<std::uint64_t> waveformDataOffset;
    std::vector<std::uint32_t> waveformPacketSize;
    std::vector<float> returnPointWaveformLocation;
    std::vector<float> waveformXt;
    std::vector<float> waveformYt;
    std::vector<float> waveformZt;
    std::vector<std::uint8_t> waveformDataExternal;
    std::string waveformDataFile;
    std::vector<std::string> extraByteNames;
    std::vector<std::uint8_t> extraByteComponentCounts;
    std::vector<std::vector<double>> extraBytes;

    bool IsValid() const noexcept;
};

struct PointCloudAsset {
    usdgeo::GeoReference reference;
    usdgeo::SpatialBounds bounds = usdgeo::SpatialBounds::Empty();
    PointChunk chunk;
    PointData data;

    bool IsValid() const noexcept;
};

struct PointCloudScan {
    std::string id;
    PointCloudAsset asset;
    std::array<double, 16> pose{
        1.0, 0.0, 0.0, 0.0,
        0.0, 1.0, 0.0, 0.0,
        0.0, 0.0, 1.0, 0.0,
        0.0, 0.0, 0.0, 1.0};

    bool IsValid() const noexcept;
};

struct PointCloudCollection {
    usdgeo::GeoReference reference;
    usdgeo::SpatialBounds bounds = usdgeo::SpatialBounds::Empty();
    std::vector<PointCloudScan> scans;

    bool IsValid() const noexcept;
};

PointChunk MakePointChunk(const PointData& data,
                         const usdgeo::SpatialBounds& bounds);

std::vector<std::string> NormalizeExtraByteNames(
    const std::vector<std::string>& names);

struct PointRange {
    std::uint64_t firstPoint = 0;
    std::uint64_t pointCount = 0;

    bool IsValid() const noexcept;
};

struct PointReadOptions {
    std::size_t chunkPointLimit = 65536;
    std::size_t memoryBudgetBytes = 64 * 1024 * 1024;
    PointRange range;
    std::optional<usdgeo::SpatialBounds> bounds;
    std::vector<std::uint8_t> classifications;
    std::function<bool()> isCancelled;

    bool IsValid() const noexcept;
};

enum class PointStreamStatus {
    Chunk,
    End,
    Error,
};

class PointStream {
public:
    virtual ~PointStream() = default;

    virtual PointStreamStatus ReadNext(
        PointChunk& chunk,
        PointData& data,
        usdgeo::Diagnostic& diagnostic) = 0;

    virtual std::uint64_t SourceBytesRead() const noexcept { return 0; }
};

} // namespace usdpointcloud