#include "usd_functions.hpp"
#include "usd_scan.hpp"

#include <pxr/usd/pcp/errors.h>

namespace duckdb {

namespace {

enum UsdCompositionErrorsColumn : column_t { ERROR_TYPE, PRIM_PATH, MESSAGE };

//! PcpErrorType is not registered with TfEnum, so its names are spelled out here (without the PcpErrorType_ prefix)
const char *ErrorTypeName(pxr::PcpErrorType type) {
	switch (type) {
	case pxr::PcpErrorType_ArcCycle:
		return "ArcCycle";
	case pxr::PcpErrorType_ArcPermissionDenied:
		return "ArcPermissionDenied";
	case pxr::PcpErrorType_ArcToProhibitedChild:
		return "ArcToProhibitedChild";
	case pxr::PcpErrorType_IndexCapacityExceeded:
		return "IndexCapacityExceeded";
	case pxr::PcpErrorType_ArcCapacityExceeded:
		return "ArcCapacityExceeded";
	case pxr::PcpErrorType_ArcNamespaceDepthCapacityExceeded:
		return "ArcNamespaceDepthCapacityExceeded";
	case pxr::PcpErrorType_InconsistentPropertyType:
		return "InconsistentPropertyType";
	case pxr::PcpErrorType_InconsistentAttributeType:
		return "InconsistentAttributeType";
	case pxr::PcpErrorType_InconsistentAttributeVariability:
		return "InconsistentAttributeVariability";
	case pxr::PcpErrorType_InternalAssetPath:
		return "InternalAssetPath";
	case pxr::PcpErrorType_InvalidPrimPath:
		return "InvalidPrimPath";
	case pxr::PcpErrorType_InvalidAssetPath:
		return "InvalidAssetPath";
	case pxr::PcpErrorType_InvalidInstanceTargetPath:
		return "InvalidInstanceTargetPath";
	case pxr::PcpErrorType_InvalidExternalTargetPath:
		return "InvalidExternalTargetPath";
	case pxr::PcpErrorType_InvalidTargetPath:
		return "InvalidTargetPath";
	case pxr::PcpErrorType_InvalidReferenceOffset:
		return "InvalidReferenceOffset";
	case pxr::PcpErrorType_InvalidSublayerOffset:
		return "InvalidSublayerOffset";
	case pxr::PcpErrorType_InvalidSublayerOwnership:
		return "InvalidSublayerOwnership";
	case pxr::PcpErrorType_InvalidSublayerPath:
		return "InvalidSublayerPath";
	case pxr::PcpErrorType_InvalidVariantSelection:
		return "InvalidVariantSelection";
	case pxr::PcpErrorType_MutedAssetPath:
		return "MutedAssetPath";
	case pxr::PcpErrorType_InvalidAuthoredRelocation:
		return "InvalidAuthoredRelocation";
	case pxr::PcpErrorType_InvalidConflictingRelocation:
		return "InvalidConflictingRelocation";
	case pxr::PcpErrorType_InvalidSameTargetRelocations:
		return "InvalidSameTargetRelocations";
	case pxr::PcpErrorType_OpinionAtRelocationSource:
		return "OpinionAtRelocationSource";
	case pxr::PcpErrorType_PrimPermissionDenied:
		return "PrimPermissionDenied";
	case pxr::PcpErrorType_PropertyPermissionDenied:
		return "PropertyPermissionDenied";
	case pxr::PcpErrorType_SublayerCycle:
		return "SublayerCycle";
	case pxr::PcpErrorType_TargetPermissionDenied:
		return "TargetPermissionDenied";
	case pxr::PcpErrorType_UnresolvedPrimPath:
		return "UnresolvedPrimPath";
	case pxr::PcpErrorType_VariableExpressionError:
		return "VariableExpressionError";
	default:
		return "Unknown";
	}
}

class UsdCompositionErrorsProducer : public UsdBufferedProducer<pxr::PcpErrorBasePtr> {
public:
	void BeginStage(const UsdStageHandle &handle) override {
		for (auto &error : handle.stage->GetCompositionErrors()) {
			rows.push_back(error);
		}
	}

	void Write(column_t column, idx_t offset, idx_t count, Vector &result) override {
		using pxr::PcpErrorBasePtr;
		switch (column) {
		case ERROR_TYPE:
			UsdWriteStrings(rows, offset, count, result,
			                [](const PcpErrorBasePtr &error) { return ErrorTypeName(error->errorType); });
			break;
		case PRIM_PATH:
			UsdWriteStrings(rows, offset, count, result, [](const PcpErrorBasePtr &error, std::string &out) {
				auto &path = error->rootSite.path;
				if (path.IsEmpty()) {
					return false;
				}
				out = path.GetString();
				return true;
			});
			break;
		case MESSAGE:
			UsdWriteStrings(rows, offset, count, result,
			                [](const PcpErrorBasePtr &error) { return error->ToString(); });
			break;
		default:
			throw InternalException("usd_composition_errors: unknown column %d", column);
		}
	}
};

unique_ptr<UsdRowProducer> CreateProducer(ClientContext &, const UsdScanBindData &, const vector<column_t> &) {
	return make_uniq<UsdCompositionErrorsProducer>();
}

} // namespace

CreateTableFunctionInfo UsdCompositionErrorsFunction() {
	UsdTableFunctionDefinition definition;
	definition.name = "usd_composition_errors";
	definition.description =
	    "One row per error USD encountered while composing a stage, such as a reference to a missing file.";
	definition.example = "usd_composition_errors('scene.usda')";
	definition.unit = UsdScanUnit::STAGE;
	// reporting the errors is the point of this function
	definition.warn_on_composition_errors = false;
	definition.columns = {
	    {"error_type", LogicalType::VARCHAR}, {"prim_path", LogicalType::VARCHAR}, {"message", LogicalType::VARCHAR}};
	definition.create_producer = CreateProducer;
	return UsdCreateScanFunction(std::move(definition));
}

} // namespace duckdb
