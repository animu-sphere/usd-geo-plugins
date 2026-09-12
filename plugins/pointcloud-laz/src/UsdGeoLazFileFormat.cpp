#include "usdgeolaz/UsdGeoLazFileFormat.h"
#include "usdgeolaz/UsdGeoLazDiagnostics.h"

#include "usdgeo/Diagnostic.h"
#include "usdgeo/PointCloudCache.h"
#include "usdgeo/PointCloudLayer.h"
#include "usdpointcloud/FileFormatArguments.h"
#include "usdpointcloud/Sampling.h"
#include "usdlaz/Laz.h"

#include <pxr/base/tf/diagnostic.h>
#include <pxr/base/tf/registryManager.h>
#include <pxr/base/vt/value.h>
#include <pxr/usd/pcp/dynamicFileFormatContext.h>
#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <limits>
#include <memory>
#include <string>
#include <vector>

PXR_NAMESPACE_OPEN_SCOPE

namespace {

const TfToken DynamicLodField("pc_laz_lod");

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

const char* ReaderDiagnosticCode(usdlaz::LazReadFailure failure) {
    switch (failure) {
    case usdlaz::LazReadFailure::FileOpen:
        return usdgeolaz::diagnostics::FileOpenFailed;
    case usdlaz::LazReadFailure::InvalidRequest:
        return usdgeolaz::diagnostics::FormatArgumentInvalid;
    case usdlaz::LazReadFailure::Asset:
        return usdgeolaz::diagnostics::BoundsTransformFailed;
    case usdlaz::LazReadFailure::Decode:
    case usdlaz::LazReadFailure::None:
        return usdgeolaz::diagnostics::DecodeFailed;
    }
    return usdgeolaz::diagnostics::DecodeFailed;
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

TF_DEFINE_PUBLIC_TOKENS(UsdGeoLazFileFormatTokens, USDGEOLAZ_FILE_FORMAT_TOKENS);

UsdGeoLazFileFormat::UsdGeoLazFileFormat()
    : SdfFileFormat(UsdGeoLazFileFormatTokens->Id,
                    UsdGeoLazFileFormatTokens->Version,
                    UsdGeoLazFileFormatTokens->Target,
                    UsdGeoLazFileFormatTokens->Extension) {}

UsdGeoLazFileFormat::~UsdGeoLazFileFormat() = default;

void UsdGeoLazFileFormat::ComposeFieldsForFileFormatArguments(
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

bool UsdGeoLazFileFormat::CanFieldChangeAffectFileFormatArguments(
    const TfToken& field,
    const VtValue&,
    const VtValue&,
    const VtValue&) const {
    return field == DynamicLodField;
}

bool UsdGeoLazFileFormat::CanRead(const std::string& file) const {
    return SdfFileFormat::GetFileExtension(file) == "laz";
}

bool UsdGeoLazFileFormat::Read(SdfLayer* layer,
                            const std::string& resolvedPath,
                            bool metadataOnly) const {
    if (!layer) {
        TF_RUNTIME_ERROR("%s", usdgeolaz::diagnostics::Message(
                                  usdgeolaz::diagnostics::InvalidReadRequest,
                                  "pointcloud-laz requires a writable layer")
                                  .c_str());
        return false;
    }

        const std::string sourcePath = resolvedPath;
    usdpointcloud::PointReadRequest request;
    std::vector<usdgeo::Diagnostic> diagnostics;
        if (!usdpointcloud::MakeReadRequest(
            layer->GetFileFormatArguments(), request, diagnostics,
            usdpointcloud::PointReadFormat::Laz)) {
        TF_RUNTIME_ERROR("%s", usdgeolaz::diagnostics::Message(
                                  usdgeolaz::diagnostics::FormatArgumentInvalid,
                                  "Invalid LAZ file-format arguments: " +
                                      DiagnosticDetail(diagnostics,
                                                       "invalid arguments"))
                                  .c_str());
        return false;
    }

    if (!metadataOnly && !usdgeo::PointCloudCacheRootFromEnvironment().empty()) {
        auto decoder = usdlaz::CreateFileDecoder(sourcePath, diagnostics);
        if (!decoder) {
            TF_RUNTIME_ERROR("%s", usdgeolaz::diagnostics::FileOpenFailed);
            return false;
        }
        usdlaz::LazReader reader(std::move(decoder));
        usdlas::LasHeader header;
        if (!reader.ReadMetadata(header, diagnostics)) {
            TF_RUNTIME_ERROR("%s", usdgeolaz::diagnostics::Message(
                                      usdgeolaz::diagnostics::DecodeFailed,
                                      "Unable to inspect LAZ file " + resolvedPath +
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
            TF_RUNTIME_ERROR("%s", usdgeolaz::diagnostics::PointCloudAuthorFailed);
            return false;
        }
        bool cacheHit = false;
        std::string cacheError;
        if (!usdgeo::TryLoadPointCloudCache(
                layer, sourcePath, reference, request, "laz-reader-1",
                cacheHit, cacheError)) {
            TF_RUNTIME_ERROR("%s", usdgeolaz::diagnostics::Message(
                                      usdgeolaz::diagnostics::PointCloudAuthorFailed,
                                      cacheError)
                                      .c_str());
            return false;
        }
        if (cacheHit) {
            return true;
        }
    }

    if (metadataOnly) {
        auto decoder = usdlaz::CreateFileDecoder(sourcePath, diagnostics);
        if (!decoder) {
            TF_RUNTIME_ERROR("%s", usdgeolaz::diagnostics::Message(
                                      usdgeolaz::diagnostics::FileOpenFailed,
                                      "Unable to open LAZ file " + sourcePath +
                                          ": " + DiagnosticDetail(
                                              diagnostics, "decoder could not be created"))
                                      .c_str());
            return false;
        }
        usdlaz::LazReader reader(std::move(decoder));
        usdlas::LasHeader header;
        if (!reader.ReadMetadata(header, diagnostics)) {
            TF_RUNTIME_ERROR("%s", usdgeolaz::diagnostics::Message(
                                      usdgeolaz::diagnostics::DecodeFailed,
                                      "Unable to inspect LAZ file " + resolvedPath +
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
            TF_RUNTIME_ERROR("%s", usdgeolaz::diagnostics::PointCloudAuthorFailed);
            return false;
        }
        return true;
    }

    if (request.tiled) {
        usdlas::LasHeader header;
        auto stream = usdlaz::OpenLazPointStream(
            sourcePath, request.readOptions, header, diagnostics);
        if (!stream) {
            TF_RUNTIME_ERROR("%s", usdgeolaz::diagnostics::Message(
                                      usdgeolaz::diagnostics::DecodeFailed,
                                      "Unable to open LAZ point stream: " +
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
            TF_RUNTIME_ERROR("%s", usdgeolaz::diagnostics::PointCloudAuthorFailed);
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
        payloadOptions.owner =
            usdgeo::PointCloudPayloadOwner(resolvedPath, request);
        if (!usdgeo::AuthorPointCloudTiledAssetFromStream(
                layer, "/PointCloud", selected, reference,
                {request.tileSize, 0}, payloadOptions, diagnostics)) {
            TF_RUNTIME_ERROR("%s", usdgeolaz::diagnostics::Message(
                                      usdgeolaz::diagnostics::PointCloudAuthorFailed,
                                      "Unable to author tiled LAZ point cloud: " +
                                          DiagnosticDetail(
                                              diagnostics,
                                              "tiled authoring failed"))
                                      .c_str());
            return false;
        }
        return true;
    }

    usdpointcloud::PointCloudAsset asset;
    usdlaz::LazReadFailure failure = usdlaz::LazReadFailure::None;
    if (!usdlaz::ReadPointCloud(
            sourcePath, request.readOptions, request.attributes,
            "LAZ CRS unavailable; inspect VLR metadata", asset, failure,
            diagnostics)) {
        TF_RUNTIME_ERROR("%s", usdgeolaz::diagnostics::Message(
                                  ReaderDiagnosticCode(failure),
                                  "Unable to decode LAZ file " + resolvedPath +
                                      ": " +
                                      DiagnosticDetail(diagnostics, "decode failed"))
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
        const char* code = usdgeolaz::diagnostics::PointCloudAuthorFailed;
        if (authorFailure == usdgeo::PointCloudAuthorFailure::InvalidLayer ||
            authorFailure == usdgeo::PointCloudAuthorFailure::StageCreation) {
            code = usdgeolaz::diagnostics::UsdLayerCreateFailed;
        } else if (authorFailure ==
                   usdgeo::PointCloudAuthorFailure::StageMetrics) {
            code = usdgeolaz::diagnostics::StageMetricsFailed;
        }
        TF_RUNTIME_ERROR("%s", usdgeolaz::diagnostics::Message(
                                  code,
                                  "Unable to author LAZ point cloud to USD layer: " +
                                      resolvedPath)
                                  .c_str());
        return false;
    }
    return true;
}

bool UsdGeoLazFileFormat::WriteToString(const SdfLayer& layer,
                                     std::string* str,
                                     const std::string& comment) const {
    const auto usda = SdfFileFormat::FindByExtension("usda");
    return usda ? usda->WriteToString(layer, str, comment)
                : layer.ExportToString(str);
}

TF_REGISTRY_FUNCTION(TfType) {
    SDF_DEFINE_FILE_FORMAT(UsdGeoLazFileFormat, SdfFileFormat);
}

PXR_NAMESPACE_CLOSE_SCOPE
