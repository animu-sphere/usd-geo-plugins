#include "usdgeoply/UsdGeoPlyFileFormat.h"
#include "usdgeoply/UsdGeoPlyDiagnostics.h"

#include "usdgeo/Diagnostic.h"
#include "usdgeo/PointCloudCache.h"
#include "usdgeo/PointCloudLayer.h"
#include "usdpointcloud/FileFormatArguments.h"
#include "usdpointcloud/Sampling.h"
#include "usdply/Ply.h"

#include <pxr/base/tf/diagnostic.h>
#include <pxr/base/tf/registryManager.h>
#include <pxr/base/vt/value.h>
#include <pxr/usd/pcp/dynamicFileFormatContext.h>

#include <filesystem>
#include <memory>
#include <string>
#include <vector>

PXR_NAMESPACE_OPEN_SCOPE

namespace {

const TfToken DynamicLodField("pc_ply_lod");

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
        detail += " (byte offset " + std::to_string(*diagnostic.byteOffset) + ")";
    }
    if (diagnostic.pointIndex) {
        detail += " (point " + std::to_string(*diagnostic.pointIndex) + ")";
    }
    return detail;
}

usdgeo::GeoReference MakeReference(const usdpointcloud::PointReadRequest& request) {
    usdgeo::GeoReference reference;
    reference.epsgCode = request.epsgCode;
    reference.linearUnit = request.linearUnit;
    reference.sourceUpAxis = request.sourceUpAxis;
    reference.stageUpAxis = request.stageUpAxis;
    return reference;
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

TF_DEFINE_PUBLIC_TOKENS(UsdGeoPlyFileFormatTokens, USDGEOPLY_FILE_FORMAT_TOKENS);

UsdGeoPlyFileFormat::UsdGeoPlyFileFormat()
    : SdfFileFormat(UsdGeoPlyFileFormatTokens->Id,
                    UsdGeoPlyFileFormatTokens->Version,
                    UsdGeoPlyFileFormatTokens->Target,
                    UsdGeoPlyFileFormatTokens->Extension) {}

UsdGeoPlyFileFormat::~UsdGeoPlyFileFormat() = default;

void UsdGeoPlyFileFormat::ComposeFieldsForFileFormatArguments(
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

bool UsdGeoPlyFileFormat::CanFieldChangeAffectFileFormatArguments(
    const TfToken& field,
    const VtValue&,
    const VtValue&,
    const VtValue&) const {
    return field == DynamicLodField;
}

bool UsdGeoPlyFileFormat::CanRead(const std::string& file) const {
    return SdfFileFormat::GetFileExtension(file) == "ply";
}

bool UsdGeoPlyFileFormat::Read(SdfLayer* layer,
                               const std::string& resolvedPath,
                               bool metadataOnly) const {
    if (!layer) {
        TF_RUNTIME_ERROR("%s", usdgeoply::diagnostics::Message(
                                  usdgeoply::diagnostics::InvalidReadRequest,
                                  "pointcloud-ply requires a writable layer")
                                  .c_str());
        return false;
    }
    if (metadataOnly) {
        TF_RUNTIME_ERROR("%s", usdgeoply::diagnostics::Message(
                                  usdgeoply::diagnostics::DecodeFailed,
                                  "PLY metadata-only reads are not available because PLY has no bounds metadata")
                                  .c_str());
        return false;
    }

    usdpointcloud::PointReadRequest request;
    std::vector<usdgeo::Diagnostic> diagnostics;
    if (!usdpointcloud::MakeReadRequest(
            layer->GetFileFormatArguments(), request, diagnostics,
            usdpointcloud::PointReadFormat::Ply)) {
        TF_RUNTIME_ERROR("%s", usdgeoply::diagnostics::Message(
                                  usdgeoply::diagnostics::FormatArgumentInvalid,
                                  "Invalid PLY file-format arguments: " +
                                      DiagnosticDetail(diagnostics,
                                                       "invalid arguments"))
                                  .c_str());
        return false;
    }
    if (!request.epsgCode) {
        TF_RUNTIME_ERROR("%s", usdgeoply::diagnostics::Message(
                                  usdgeoply::diagnostics::CrsRequired,
                                  "PLY requires an explicit epsg file-format argument")
                                  .c_str());
        return false;
    }
    usdpointcloud::PointCloudAsset asset;
    const auto reference = MakeReference(request);
    if (!usdgeo::PointCloudCacheRootFromEnvironment().empty()) {
        bool cacheHit = false;
        std::string cacheError;
        if (!usdgeo::TryLoadPointCloudCache(
                layer, resolvedPath, reference, request, "ply-reader-1",
                cacheHit, cacheError)) {
            TF_RUNTIME_ERROR("%s", usdgeoply::diagnostics::Message(
                                      usdgeoply::diagnostics::PointCloudAuthorFailed,
                                      cacheError)
                                      .c_str());
            return false;
        }
        if (cacheHit) {
            return true;
        }
    }
    if (request.tiled) {
        usdply::PlyHeader header;
        auto stream = usdply::OpenPointStream(
            resolvedPath, request.readOptions, header, diagnostics);
        if (!stream) {
            TF_RUNTIME_ERROR("%s", usdgeoply::diagnostics::Message(
                                      usdgeoply::diagnostics::DecodeFailed,
                                      "Unable to open PLY point stream: " +
                                          DiagnosticDetail(diagnostics,
                                                           "stream open failed"))
                                      .c_str());
            return false;
        }
        auto payloadDirectory = std::filesystem::path(request.payloadDirectory);
        if (payloadDirectory.is_relative()) {
            payloadDirectory = std::filesystem::path(resolvedPath).parent_path() /
                               payloadDirectory;
        }
        SelectedPointStream selected(std::move(stream), request.attributes);
        usdgeo::PointCloudPayloadOptions payloadOptions{
            payloadDirectory.string(), resolvedPath,
            request.tileMemoryLimitBytes};
        payloadOptions.owner =
            usdgeo::PointCloudPayloadOwner(resolvedPath, request);
        if (!usdgeo::AuthorPointCloudTiledAssetFromStream(
            layer, "/PointCloud", selected, reference,
                {request.tileSize, 0}, payloadOptions, diagnostics)) {
            TF_RUNTIME_ERROR("%s", usdgeoply::diagnostics::Message(
                                      usdgeoply::diagnostics::PointCloudAuthorFailed,
                                      DiagnosticDetail(
                                          diagnostics,
                                          "Unable to author tiled PLY point cloud"))
                                      .c_str());
            return false;
        }
        return true;
    }
    if (!usdply::ReadPointCloud(resolvedPath, request.readOptions, reference,
                                asset, diagnostics)) {
        TF_RUNTIME_ERROR("%s", usdgeoply::diagnostics::Message(
                                  usdgeoply::diagnostics::DecodeFailed,
                                  "Unable to read PLY file " + resolvedPath + ": " +
                                      DiagnosticDetail(diagnostics, "read failed"))
                                  .c_str());
        return false;
    }
    std::string selectionError;
    if (!usdpointcloud::SelectPointDataAttributes(
            asset.data, request.attributes, selectionError)) {
        TF_RUNTIME_ERROR("%s", usdgeoply::diagnostics::Message(
                                  usdgeoply::diagnostics::FormatArgumentInvalid,
                                  selectionError)
                                  .c_str());
        return false;
    }
    asset.chunk = usdpointcloud::MakePointChunk(asset.data, asset.bounds);

    bool authored = false;
    usdgeo::PointCloudAuthorFailure authorFailure =
        usdgeo::PointCloudAuthorFailure::PointCloud;
    if (request.lodProfile != usdpointcloud::LodProfile::Off) {
        std::vector<usdpointcloud::PointCloudAsset> levels;
        usdpointcloud::PointLodHierarchy hierarchy;
        if (usdpointcloud::BuildPointLodAssets(
                asset, request.lodProfile, levels, hierarchy, diagnostics)) {
            authored = usdgeo::AuthorPointCloudLodAsset(
                layer, "/PointCloud", levels, hierarchy);
        }
    } else {
        authored = usdgeo::AuthorPointCloudAsset(
            layer, "/PointCloud", asset, authorFailure);
    }
    if (!authored) {
        const char* code = usdgeoply::diagnostics::PointCloudAuthorFailed;
        if (authorFailure == usdgeo::PointCloudAuthorFailure::InvalidLayer ||
            authorFailure == usdgeo::PointCloudAuthorFailure::StageCreation) {
            code = usdgeoply::diagnostics::UsdLayerCreateFailed;
        } else if (authorFailure == usdgeo::PointCloudAuthorFailure::StageMetrics) {
            code = usdgeoply::diagnostics::StageMetricsFailed;
        }
        TF_RUNTIME_ERROR("%s", usdgeoply::diagnostics::Message(
                                  code,
                                  "Unable to author PLY point cloud to USD layer: " +
                                      resolvedPath)
                                  .c_str());
        return false;
    }
    return true;
}

bool UsdGeoPlyFileFormat::WriteToString(const SdfLayer& layer,
                                        std::string* str,
                                        const std::string& comment) const {
    const auto usda = SdfFileFormat::FindByExtension("usda");
    return usda ? usda->WriteToString(layer, str, comment)
                : layer.ExportToString(str);
}

TF_REGISTRY_FUNCTION(TfType) {
    SDF_DEFINE_FILE_FORMAT(UsdGeoPlyFileFormat, SdfFileFormat);
}

PXR_NAMESPACE_CLOSE_SCOPE