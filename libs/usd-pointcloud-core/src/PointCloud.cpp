#include "usdpointcloud/PointCloud.h"

#include <algorithm>
#include <cmath>
#include <limits>
#include <set>

namespace usdpointcloud {

bool PointAttribute::IsValid() const noexcept {
    return !name.empty();
}

bool PointChunk::IsValid() const noexcept {
    if (pointCount == 0) {
        return !bounds.IsValid() && attributes.empty();
    }
    if (!bounds.IsValid()) {
        return false;
    }

    for (const auto& attribute : attributes) {
        if (!attribute.IsValid()) {
            return false;
        }
    }
    for (auto first = attributes.begin(); first != attributes.end(); ++first) {
        if (std::any_of(first + 1, attributes.end(),
                        [&](const PointAttribute& other) {
                            return other.name == first->name;
                        })) {
            return false;
        }
    }
    return true;
}

namespace {

template <typename T>
bool HasPointCountOrIsEmpty(const std::vector<T>& values,
                            std::size_t pointCount) {
    return values.empty() || values.size() == pointCount;
}

bool IsAsciiAlphaNumeric(unsigned char character) {
    return (character >= 'A' && character <= 'Z') ||
           (character >= 'a' && character <= 'z') ||
           (character >= '0' && character <= '9');
}

bool IsAsciiDigit(unsigned char character) {
    return character >= '0' && character <= '9';
}

std::string NormalizeExtraByteName(const std::string& name) {
    std::string result;
    result.reserve(name.size());
    for (const unsigned char character : name) {
        result += IsAsciiAlphaNumeric(character) || character == '_'
                      ? static_cast<char>(character)
                      : '_';
    }
    if (result.empty()) {
        return "extra";
    }
    if (IsAsciiDigit(static_cast<unsigned char>(result.front()))) {
        result.insert(result.begin(), '_');
    }
    return result;
}

std::set<std::string> ReservedPointAttributeNames() {
    return {"xyz", "intensity", "returnNumber", "numberOfReturns",
            "classification", "classificationFlags", "scannerChannel",
            "scanDirectionFlag", "edgeOfFlightLine", "userData",
            "scanAngle", "pointSourceId", "red", "green", "blue",
            "nir", "gpsTime", "waveformDescriptorIndex",
            "waveformDataOffset", "waveformPacketSize",
            "returnPointWaveformLocation", "waveformXt", "waveformYt",
            "waveformZt", "waveformDataExternal", "waveformDataFile"};
}

} // namespace

bool PointData::IsValid() const noexcept {
    const auto pointCount = positions.size();
    const auto hasValidExtraByteValues = [&](std::size_t index,
                                             const auto& values) {
        const auto componentCount = extraByteComponentCounts.empty()
                                        ? std::uint8_t{1}
                                        : extraByteComponentCounts[index];
        return componentCount >= 1 && componentCount <= 3 &&
               (values.empty() ||
                (pointCount <= (std::numeric_limits<std::size_t>::max)() /
                                   componentCount &&
                 values.size() == pointCount * componentCount));
    };
    const bool extraByteComponentCountsValid =
        extraByteComponentCounts.empty() ||
        extraByteComponentCounts.size() == extraBytes.size();
    bool extraByteValuesValid = extraByteComponentCountsValid;
    if (extraByteComponentCountsValid) {
        for (std::size_t index = 0; index < extraBytes.size(); ++index) {
            if (!hasValidExtraByteValues(index, extraBytes[index])) {
                extraByteValuesValid = false;
                break;
            }
        }
    }
        return (colorBitDepth == 8 || colorBitDepth == 16) &&
            HasPointCountOrIsEmpty(intensity, pointCount) &&
           HasPointCountOrIsEmpty(returnNumber, pointCount) &&
           HasPointCountOrIsEmpty(numberOfReturns, pointCount) &&
           HasPointCountOrIsEmpty(classification, pointCount) &&
           HasPointCountOrIsEmpty(classificationFlags, pointCount) &&
           HasPointCountOrIsEmpty(scannerChannel, pointCount) &&
           HasPointCountOrIsEmpty(scanDirectionFlag, pointCount) &&
           HasPointCountOrIsEmpty(edgeOfFlightLine, pointCount) &&
           HasPointCountOrIsEmpty(userData, pointCount) &&
           HasPointCountOrIsEmpty(scanAngle, pointCount) &&
           HasPointCountOrIsEmpty(pointSourceId, pointCount) &&
           HasPointCountOrIsEmpty(red, pointCount) &&
           HasPointCountOrIsEmpty(green, pointCount) &&
           HasPointCountOrIsEmpty(blue, pointCount) &&
           HasPointCountOrIsEmpty(nir, pointCount) &&
           HasPointCountOrIsEmpty(gpsTime, pointCount) &&
           HasPointCountOrIsEmpty(waveformDescriptorIndex, pointCount) &&
           HasPointCountOrIsEmpty(waveformDataOffset, pointCount) &&
           HasPointCountOrIsEmpty(waveformPacketSize, pointCount) &&
           HasPointCountOrIsEmpty(returnPointWaveformLocation, pointCount) &&
           HasPointCountOrIsEmpty(waveformXt, pointCount) &&
           HasPointCountOrIsEmpty(waveformYt, pointCount) &&
           HasPointCountOrIsEmpty(waveformZt, pointCount) &&
           HasPointCountOrIsEmpty(waveformDataExternal, pointCount) &&
           extraByteNames.size() == extraBytes.size() &&
           extraByteComponentCountsValid &&
           extraByteValuesValid &&
           (returnNumber.empty() == numberOfReturns.empty()) &&
           (red.empty() == green.empty()) && (red.empty() == blue.empty()) &&
           (waveformDescriptorIndex.empty() == waveformDataOffset.empty()) &&
           (waveformDescriptorIndex.empty() == waveformPacketSize.empty()) &&
           (waveformDescriptorIndex.empty() ==
            returnPointWaveformLocation.empty()) &&
           (waveformDescriptorIndex.empty() == waveformXt.empty()) &&
           (waveformDescriptorIndex.empty() == waveformYt.empty()) &&
           (waveformDescriptorIndex.empty() == waveformZt.empty()) &&
           (waveformDescriptorIndex.empty() == waveformDataExternal.empty());
}

bool PointCloudAsset::IsValid() const noexcept {
    return reference.IsValid() && bounds.IsValid() && data.IsValid() &&
           chunk.IsValid() && chunk.pointCount == data.positions.size();
}

namespace {

constexpr double kPoseTolerance = 1.0e-9;

bool HasSameGeoReference(const usdgeo::GeoReference& first,
                         const usdgeo::GeoReference& second) {
    return first.epsgCode == second.epsgCode && first.wkt == second.wkt &&
           first.projJson == second.projJson &&
           first.linearUnit == second.linearUnit &&
           first.sourceUpAxis == second.sourceUpAxis &&
           first.stageUpAxis == second.stageUpAxis &&
           first.localOrigin.x == second.localOrigin.x &&
           first.localOrigin.y == second.localOrigin.y &&
           first.localOrigin.z == second.localOrigin.z;
}

bool IsRigidPose(const std::array<double, 16>& pose) {
    if (!std::all_of(pose.begin(), pose.end(),
                     [](double value) { return std::isfinite(value); }) ||
        std::abs(pose[12]) > kPoseTolerance ||
        std::abs(pose[13]) > kPoseTolerance ||
        std::abs(pose[14]) > kPoseTolerance ||
        std::abs(pose[15] - 1.0) > kPoseTolerance) {
        return false;
    }

    const auto dot = [&](int firstRow, int secondRow) {
        double result = 0.0;
        for (int column = 0; column != 3; ++column) {
            result += pose[firstRow * 4 + column] *
                      pose[secondRow * 4 + column];
        }
        return result;
    };
    for (int row = 0; row != 3; ++row) {
        if (std::abs(dot(row, row) - 1.0) > kPoseTolerance) {
            return false;
        }
        for (int otherRow = row + 1; otherRow != 3; ++otherRow) {
            if (std::abs(dot(row, otherRow)) > kPoseTolerance) {
                return false;
            }
        }
    }

    const auto determinant =
        pose[0] * (pose[5] * pose[10] - pose[6] * pose[9]) -
        pose[1] * (pose[4] * pose[10] - pose[6] * pose[8]) +
        pose[2] * (pose[4] * pose[9] - pose[5] * pose[8]);
    return std::abs(determinant - 1.0) <= kPoseTolerance;
}

usdgeo::Vec3d ApplyPose(const std::array<double, 16>& pose,
                        const usdgeo::Vec3d& point) {
    return {pose[0] * point.x + pose[1] * point.y + pose[2] * point.z +
                pose[3],
            pose[4] * point.x + pose[5] * point.y + pose[6] * point.z +
                pose[7],
            pose[8] * point.x + pose[9] * point.y + pose[10] * point.z +
                pose[11]};
}

bool ContainsTransformedBounds(const usdgeo::SpatialBounds& container,
                               const PointCloudScan& scan) {
    for (int x = 0; x != 2; ++x) {
        for (int y = 0; y != 2; ++y) {
            for (int z = 0; z != 2; ++z) {
                const usdgeo::Vec3d corner{
                    x == 0 ? scan.asset.bounds.minimum.x
                           : scan.asset.bounds.maximum.x,
                    y == 0 ? scan.asset.bounds.minimum.y
                           : scan.asset.bounds.maximum.y,
                    z == 0 ? scan.asset.bounds.minimum.z
                           : scan.asset.bounds.maximum.z};
                const auto transformed = ApplyPose(scan.pose, corner);
                const auto tolerance = 1.0e-9 *
                    (std::max)({1.0, std::abs(container.minimum.x),
                                std::abs(container.minimum.y),
                                std::abs(container.minimum.z),
                                std::abs(container.maximum.x),
                                std::abs(container.maximum.y),
                                std::abs(container.maximum.z),
                                std::abs(transformed.x),
                                std::abs(transformed.y),
                                std::abs(transformed.z)});
                if (transformed.x < container.minimum.x - tolerance ||
                    transformed.y < container.minimum.y - tolerance ||
                    transformed.z < container.minimum.z - tolerance ||
                    transformed.x > container.maximum.x + tolerance ||
                    transformed.y > container.maximum.y + tolerance ||
                    transformed.z > container.maximum.z + tolerance) {
                    return false;
                }
            }
        }
    }
    return true;
}

} // namespace

bool PointCloudScan::IsValid() const noexcept {
    return !id.empty() && asset.IsValid() && IsRigidPose(pose);
}

bool PointCloudCollection::IsValid() const noexcept {
    if (!reference.IsValid() || !bounds.IsValid() || scans.empty()) {
        return false;
    }
    std::set<std::string> ids;
    for (const auto& scan : scans) {
        if (!scan.IsValid() ||
            !HasSameGeoReference(reference, scan.asset.reference) ||
            !ids.insert(scan.id).second ||
            !ContainsTransformedBounds(bounds, scan)) {
            return false;
        }
    }
    return true;
}

PointChunk MakePointChunk(const PointData& data,
                         const usdgeo::SpatialBounds& bounds) {
    PointChunk chunk;
    chunk.pointCount = data.positions.size();
    chunk.bounds = bounds;
    const auto add = [&](const char* name, PointAttributeType type,
                         std::size_t size) {
        if (size != 0) {
            chunk.attributes.push_back({name, type});
        }
    };
    add("intensity", PointAttributeType::UInt16, data.intensity.size());
    add("returnNumber", PointAttributeType::UInt8, data.returnNumber.size());
    add("numberOfReturns", PointAttributeType::UInt8,
        data.numberOfReturns.size());
    add("classification", PointAttributeType::UInt8,
        data.classification.size());
    add("classificationFlags", PointAttributeType::UInt8,
        data.classificationFlags.size());
    add("scannerChannel", PointAttributeType::UInt8,
        data.scannerChannel.size());
    add("scanDirectionFlag", PointAttributeType::UInt8,
        data.scanDirectionFlag.size());
    add("edgeOfFlightLine", PointAttributeType::UInt8,
        data.edgeOfFlightLine.size());
    add("userData", PointAttributeType::UInt8, data.userData.size());
    add("scanAngle", PointAttributeType::Int16, data.scanAngle.size());
    add("pointSourceId", PointAttributeType::UInt16,
        data.pointSourceId.size());
    add("red", PointAttributeType::UInt16, data.red.size());
    add("green", PointAttributeType::UInt16, data.green.size());
    add("blue", PointAttributeType::UInt16, data.blue.size());
    add("nir", PointAttributeType::UInt16, data.nir.size());
    add("gpsTime", PointAttributeType::Float64, data.gpsTime.size());
    add("waveformDescriptorIndex", PointAttributeType::UInt8,
        data.waveformDescriptorIndex.size());
    add("waveformDataOffset", PointAttributeType::UInt64,
        data.waveformDataOffset.size());
    add("waveformPacketSize", PointAttributeType::UInt32,
        data.waveformPacketSize.size());
    add("returnPointWaveformLocation", PointAttributeType::Float32,
        data.returnPointWaveformLocation.size());
    add("waveformXt", PointAttributeType::Float32, data.waveformXt.size());
    add("waveformYt", PointAttributeType::Float32, data.waveformYt.size());
    add("waveformZt", PointAttributeType::Float32, data.waveformZt.size());
    add("waveformDataExternal", PointAttributeType::UInt8,
        data.waveformDataExternal.size());
    const auto extraByteNames = NormalizeExtraByteNames(data.extraByteNames);
    for (std::size_t index = 0; index < data.extraBytes.size(); ++index) {
        if (!data.extraBytes[index].empty() && index < extraByteNames.size()) {
            const auto componentCount =
                data.extraByteComponentCounts.empty()
                    ? std::uint8_t{1}
                    : index < data.extraByteComponentCounts.size()
                          ? data.extraByteComponentCounts[index]
                          : std::uint8_t{0};
            if (componentCount < 1 || componentCount > 3) {
                continue;
            }
            const auto type = componentCount == 1 ? PointAttributeType::Float64
                              : componentCount == 2 ? PointAttributeType::Float64Vec2
                                                    : PointAttributeType::Float64Vec3;
            chunk.attributes.push_back({extraByteNames[index], type});
        }
    }
    return chunk;
}

std::vector<std::string> NormalizeExtraByteNames(
    const std::vector<std::string>& names) {
    std::set<std::string> usedNames = ReservedPointAttributeNames();
    std::vector<std::string> result;
    result.reserve(names.size());
    for (const auto& name : names) {
        const auto baseName = NormalizeExtraByteName(name);
        auto normalizedName = baseName;
        std::size_t suffix = 2;
        while (!usedNames.insert(normalizedName).second) {
            normalizedName = baseName + "_" + std::to_string(suffix++);
        }
        result.push_back(std::move(normalizedName));
    }
    return result;
}

bool PointRange::IsValid() const noexcept {
    return pointCount == 0 ||
           firstPoint <= (std::numeric_limits<std::uint64_t>::max)() -
                              pointCount;
}

bool PointReadOptions::IsValid() const noexcept {
    return chunkPointLimit != 0 && memoryBudgetBytes != 0 &&
           range.IsValid() && (!bounds || bounds->IsValid());
}

} // namespace usdpointcloud