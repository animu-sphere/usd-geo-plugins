#include "usdgeolas/UsdGeoLasFileFormat.h"
#include "usdgeolas/UsdGeoLasDiagnostics.h"

#include "usdgeo/Diagnostic.h"
#include "usdgeo/PointCloudCache.h"
#include "usdgeo/PointCloudLayer.h"
#include "usdpointcloud/FileFormatArguments.h"
#include "usdpointcloud/Sampling.h"
#include "usdlas/Las.h"

#include <pxr/base/tf/diagnostic.h>
#include <pxr/base/tf/registryManager.h>
#include <pxr/base/vt/value.h>
#include <pxr/usd/pcp/dynamicFileFormatContext.h>

#include <cstdint>
#include <filesystem>
#include <memory>
#include <string>
#include <vector>

PXR_NAMESPACE_OPEN_SCOPE

namespace {

const TfToken DynamicLodField("pc_las_lod");

std::string DynamicTokenValue(const VtValue& value) {
    if (value.IsHolding<TfToken>()) {
        return value.UncheckedGet<TfToken>().GetString();
    }
    if (value.IsHolding<std::string>()) {
        return value.UncheckedGet<std::string>();
    }
    return {};
}

std::string DiagnosticDetail(
    const std::vector<usdgeo::Diagnostic>& diagnostics,
    const std::string& fallback) {
    if (diagnostics.empty()) {
        return fallback;
    }
    const auto& diagnostic = diagnostics.front();
    std::string detail = diagnostic.message;
    if (diagnostic.byteOffset) {
        detail += " (byte offset " +
                  std::to_string(*diagnostic.byteOffset) + ")";
    }
    if (diagnostic.pointIndex) {
        detail += " (point " + std::to_string(*diagnostic.pointIndex) + ")";
    }
    return detail;
}

const char* ReaderDiagnosticCode(
    const std::vector<usdgeo::Diagnostic>& diagnostics,
    usdlas::LasReadFailure failure) {
    if (diagnostics.empty()) {
        return usdgeolas::diagnostics::PointDecodeFailed;
    }
    switch (failure) {
    case usdlas::LasReadFailure::FileOpen:
        return usdgeolas::diagnostics::FileOpenFailed;
    case usdlas::LasReadFailure::FileSize:
        return usdgeolas::diagnostics::FileSizeUnavailable;
    case usdlas::LasReadFailure::PointDataTruncated:
        return usdgeolas::diagnostics::PointDataTruncated;
    case usdlas::LasReadFailure::EvlrOffset:
        return usdgeolas::diagnostics::EvlrOffsetInvalid;
    case usdlas::LasReadFailure::Vlr:
        return usdgeolas::diagnostics::VlrInvalid;
    case usdlas::LasReadFailure::Evlr:
        return usdgeolas::diagnostics::EvlrInvalid;
    case usdlas::LasReadFailure::PointDataSeek:
        return usdgeolas::diagnostics::PointDataSeekFailed;
    case usdlas::LasReadFailure::PointDataRead:
        return usdgeolas::diagnostics::PointReadFailed;
    case usdlas::LasReadFailure::PointDecode:
        return usdgeolas::diagnostics::PointDecodeFailed;
    case usdlas::LasReadFailure::Asset:
        return usdgeolas::diagnostics::BoundsTransformFailed;
    case usdlas::LasReadFailure::InvalidRequest:
        if (diagnostics.front().code == usdgeo::DiagnosticCode::InvalidFormatArgument) {
            return usdgeolas::diagnostics::FormatArgumentInvalid;
        }
        break;
    default:
        break;
    }

    switch (diagnostics.front().code) {
    case usdgeo::DiagnosticCode::NonFiniteCoordinate:
    case usdgeo::DiagnosticCode::DecodeFailure:
        return usdgeolas::diagnostics::PointDecodeFailed;
    case usdgeo::DiagnosticCode::InvalidCrs:
    case usdgeo::DiagnosticCode::UnsupportedExtraBytesType:
        return usdgeolas::diagnostics::VlrInvalid;
    default:
        return usdgeolas::diagnostics::HeaderInvalid;
    }
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

TF_DEFINE_PUBLIC_TOKENS(UsdGeoLasFileFormatTokens, USDGEOLAS_FILE_FORMAT_TOKENS);

UsdGeoLasFileFormat::UsdGeoLasFileFormat()
    : SdfFileFormat(UsdGeoLasFileFormatTokens->Id,
                    UsdGeoLasFileFormatTokens->Version,
                    UsdGeoLasFileFormatTokens->Target,
                    UsdGeoLasFileFormatTokens->Extension) {}

UsdGeoLasFileFormat::~UsdGeoLasFileFormat() = default;

void UsdGeoLasFileFormat::ComposeFieldsForFileFormatArguments(
    const std::string&,
    const PcpDynamicFileFormatContext& context,
    FileFormatArguments* args,
    VtValue*) const {
    VtValue value;
    if (context.ComposeValue(DynamicLodField, &value)) {
        const auto lod = DynamicTokenValue(value);
        if (!lod.empty()) {
            (*args)["lod"] = lod;
        }
    }
}

bool UsdGeoLasFileFormat::CanFieldChangeAffectFileFormatArguments(
    const TfToken& field,
    const VtValue&,
    const VtValue&,
    const VtValue&) const {
    return field == DynamicLodField;
}

bool UsdGeoLasFileFormat::CanRead(const std::string& file) const {
    return SdfFileFormat::GetFileExtension(file) == "las";
}

bool UsdGeoLasFileFormat::Read(SdfLayer* layer,
                            const std::string& resolvedPath,
                            bool metadataOnly) const {
    if (!layer) {
        TF_RUNTIME_ERROR("%s", usdgeolas::diagnostics::Message(
                                  usdgeolas::diagnostics::InvalidReadRequest,
                                  "pointcloud-las requires a writable layer")
                                  .c_str());
        return false;
    }

        const std::string sourcePath = resolvedPath;
    usdpointcloud::PointReadRequest request;
    std::vector<usdgeo::Diagnostic> diagnostics;
        if (!usdpointcloud::MakeReadRequest(
            layer->GetFileFormatArguments(), request, diagnostics,
            usdpointcloud::PointReadFormat::Las)) {
        TF_RUNTIME_ERROR("%s", usdgeolas::diagnostics::Message(
                                  usdgeolas::diagnostics::FormatArgumentInvalid,
                                  "Invalid LAS file-format arguments: " +
                                      DiagnosticDetail(diagnostics,
                                                       "invalid arguments"))
                                  .c_str());
        return false;
    }

    if (!metadataOnly && !usdgeo::PointCloudCacheRootFromEnvironment().empty()) {
        usdlas::LasHeader header;
        usdlas::LasReader reader(sourcePath);
        if (!reader.ReadMetadata(header, diagnostics)) {
            TF_RUNTIME_ERROR("%s", usdgeolas::diagnostics::Message(
                                      ReaderDiagnosticCode(diagnostics,
                                                           reader.FailureKind()),
                                      "Unable to inspect LAS file " + resolvedPath +
                                          ": " + DiagnosticDetail(
                                              diagnostics, "inspection failed"))
                                      .c_str());
            return false;
        }
        usdpointcloud::PointChunk chunk;
        usdgeo::GeoReference reference;
        usdgeo::SpatialBounds bounds;
        std::string metadataError;
        if (!usdlas::BuildPointCloudMetadata(
                header, chunk, reference, bounds, metadataError)) {
            TF_RUNTIME_ERROR("%s", usdgeolas::diagnostics::BoundsTransformFailed);
            return false;
        }
        bool cacheHit = false;
        std::string cacheError;
        if (!usdgeo::TryLoadPointCloudCache(
                layer, sourcePath, reference, request, "las-reader-1",
                cacheHit, cacheError)) {
            TF_RUNTIME_ERROR("%s", usdgeolas::diagnostics::Message(
                                      usdgeolas::diagnostics::PointCloudAuthorFailed,
                                      cacheError)
                                      .c_str());
            return false;
        }
        if (cacheHit) {
            return true;
        }
    }

    if (metadataOnly) {
        usdlas::LasHeader header;
        usdlas::LasReader reader(sourcePath);
        if (!reader.ReadMetadata(header, diagnostics)) {
            TF_RUNTIME_ERROR("%s", usdgeolas::diagnostics::Message(
                                      ReaderDiagnosticCode(diagnostics,
                                                           reader.FailureKind()),
                                      "Unable to inspect LAS file " + resolvedPath +
                                          ": " + DiagnosticDetail(
                                              diagnostics, "inspection failed"))
                                      .c_str());
            return false;
        }
        usdpointcloud::PointChunk chunk;
        usdgeo::GeoReference reference;
        usdgeo::SpatialBounds bounds;
        usdgeo::PointCloudSourceMetadata sourceMetadata{
            header.pointFormat,
            {header.xScale, header.yScale, header.zScale},
            {header.xOffset, header.yOffset, header.zOffset}};
        std::string metadataError;
        if (!usdlas::BuildPointCloudMetadata(
                header, chunk, reference, bounds, metadataError) ||
            !usdgeo::AuthorPointCloudMetadata(
                layer, "/PointCloud", reference, bounds, chunk,
                sourceMetadata)) {
            TF_RUNTIME_ERROR("%s", usdgeolas::diagnostics::PointCloudAuthorFailed);
            return false;
        }
        return true;
    }

    if (request.tiled) {
        usdlas::LasHeader header;
        auto stream = usdlas::OpenLasPointStream(
            sourcePath, request.readOptions, header, diagnostics);
        if (!stream) {
            TF_RUNTIME_ERROR("%s", usdgeolas::diagnostics::Message(
                                      ReaderDiagnosticCode(diagnostics,
                                                           usdlas::LasReadFailure::Header),
                                      "Unable to open LAS point stream: " +
                                          DiagnosticDetail(diagnostics,
                                                           "stream open failed"))
                                      .c_str());
            return false;
        }
        usdpointcloud::PointChunk metadataChunk;
        usdgeo::GeoReference reference;
        usdgeo::SpatialBounds bounds;
        std::string metadataError;
        if (!usdlas::BuildPointCloudMetadata(
                header, metadataChunk, reference, bounds, metadataError)) {
            TF_RUNTIME_ERROR("%s", usdgeolas::diagnostics::BoundsTransformFailed);
            return false;
        }
        std::filesystem::path payloadDirectory(request.payloadDirectory);
        if (payloadDirectory.is_relative()) {
            payloadDirectory = std::filesystem::path(sourcePath).parent_path() /
                               payloadDirectory;
        }
        const auto rootLayerPath = resolvedPath;
        SelectedPointStream selected(std::move(stream), request.attributes);
        usdgeo::PointCloudPayloadOptions payloadOptions{
            payloadDirectory.string(), rootLayerPath,
            request.tileMemoryLimitBytes};
        payloadOptions.owner = usdgeo::PointCloudPayloadOwner(
            resolvedPath, payloadDirectory, layer->GetFileFormatArguments());
        if (!usdgeo::AuthorPointCloudTiledAssetFromStream(
                layer, "/PointCloud", selected, reference,
                {request.tileSize, 0}, payloadOptions, diagnostics)) {
            TF_RUNTIME_ERROR("%s", usdgeolas::diagnostics::Message(
                                      usdgeolas::diagnostics::PointCloudAuthorFailed,
                                      "Unable to author tiled LAS point cloud: " +
                                          DiagnosticDetail(
                                              diagnostics,
                                              "tiled authoring failed"))
                                      .c_str());
            return false;
        }
        return true;
    }

    usdpointcloud::PointCloudAsset asset;
    usdlas::LasReadFailure failure = usdlas::LasReadFailure::None;
    if (!usdlas::ReadPointCloud(
            sourcePath, request.readOptions, request.attributes,
            "LAS CRS unavailable; inspect VLR metadata", asset, failure,
            diagnostics)) {
        TF_RUNTIME_ERROR("%s", usdgeolas::diagnostics::Message(
                                  ReaderDiagnosticCode(diagnostics, failure),
                                  "Unable to read LAS file " + resolvedPath +
                                      ": " +
                                      DiagnosticDetail(diagnostics, "read failed"))
                                  .c_str());
        return false;
    }
    bool authored = false;
    usdgeo::PointCloudAuthorFailure authorFailure =
        usdgeo::PointCloudAuthorFailure::PointCloud;
    if (request.lodProfile != usdpointcloud::LodProfile::Off) {
        std::vector<usdpointcloud::PointCloudAsset> levels;
        usdpointcloud::PointLodHierarchy hierarchy;
        if (!usdpointcloud::BuildPointLodAssets(
                asset, request.lodProfile, levels, hierarchy, diagnostics)) {
            authored = false;
        } else {
            authored = usdgeo::AuthorPointCloudLodAsset(
                layer, "/PointCloud", levels, hierarchy);
        }
    } else {
        authored = usdgeo::AuthorPointCloudAsset(layer, "/PointCloud", asset,
                                                 authorFailure);
    }
    if (!authored) {
        const char* code = usdgeolas::diagnostics::PointCloudAuthorFailed;
        if (authorFailure == usdgeo::PointCloudAuthorFailure::InvalidLayer ||
            authorFailure == usdgeo::PointCloudAuthorFailure::StageCreation) {
            code = usdgeolas::diagnostics::UsdLayerCreateFailed;
        } else if (authorFailure ==
                   usdgeo::PointCloudAuthorFailure::StageMetrics) {
            code = usdgeolas::diagnostics::StageMetricsFailed;
        }
        TF_RUNTIME_ERROR("%s", usdgeolas::diagnostics::Message(
                                  code,
                                  "Unable to author LAS point cloud to USD layer: " +
                                      resolvedPath)
                                  .c_str());
        return false;
    }
    return true;
}

bool UsdGeoLasFileFormat::WriteToString(const SdfLayer& layer,
                                     std::string* str,
                                     const std::string& comment) const {
    const auto usda = SdfFileFormat::FindByExtension("usda");
    return usda ? usda->WriteToString(layer, str, comment)
                : layer.ExportToString(str);
}

TF_REGISTRY_FUNCTION(TfType) {
    SDF_DEFINE_FILE_FORMAT(UsdGeoLasFileFormat, SdfFileFormat);
}

PXR_NAMESPACE_CLOSE_SCOPE
