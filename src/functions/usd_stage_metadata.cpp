#include "usd_functions.hpp"
#include "usd_json.hpp"
#include "usd_scan.hpp"

#include <pxr/usd/sdf/layer.h>
#include <pxr/usd/usdGeom/metrics.h>

namespace duckdb {

namespace {

enum UsdStageMetadataColumn : column_t {
	DEFAULT_PRIM,
	UP_AXIS,
	METERS_PER_UNIT,
	START_TIME_CODE,
	END_TIME_CODE,
	TIME_CODES_PER_SECOND,
	FRAMES_PER_SECOND,
	LAYER_COUNT,
	DOCUMENTATION,
	CUSTOM_LAYER_DATA
};

class UsdStageMetadataProducer : public UsdBufferedProducer<pxr::UsdStageRefPtr> {
public:
	void BeginStage(const UsdStageHandle &handle) override {
		rows.push_back(handle.stage);
	}

	void Write(column_t column, idx_t offset, idx_t count, Vector &result) override {
		using pxr::UsdStageRefPtr;
		switch (column) {
		case DEFAULT_PRIM:
			UsdWriteStrings(rows, offset, count, result, [](const UsdStageRefPtr &stage, std::string &out) {
				auto default_prim = stage->GetRootLayer()->GetDefaultPrim();
				if (default_prim.IsEmpty()) {
					return false;
				}
				out = default_prim.GetString();
				return true;
			});
			break;
		case UP_AXIS:
			UsdWriteStrings(rows, offset, count, result,
			                [](const UsdStageRefPtr &stage) { return pxr::UsdGeomGetStageUpAxis(stage).GetString(); });
			break;
		case METERS_PER_UNIT:
			UsdWriteColumn<double>(rows, offset, count, result, [](const UsdStageRefPtr &stage) {
				return pxr::UsdGeomGetStageMetersPerUnit(stage);
			});
			break;
		case START_TIME_CODE:
			UsdWriteColumn<double>(rows, offset, count, result, [](const UsdStageRefPtr &stage, double &out) {
				out = stage->GetStartTimeCode();
				return stage->HasAuthoredTimeCodeRange();
			});
			break;
		case END_TIME_CODE:
			UsdWriteColumn<double>(rows, offset, count, result, [](const UsdStageRefPtr &stage, double &out) {
				out = stage->GetEndTimeCode();
				return stage->HasAuthoredTimeCodeRange();
			});
			break;
		case TIME_CODES_PER_SECOND:
			UsdWriteColumn<double>(rows, offset, count, result,
			                       [](const UsdStageRefPtr &stage) { return stage->GetTimeCodesPerSecond(); });
			break;
		case FRAMES_PER_SECOND:
			UsdWriteColumn<double>(rows, offset, count, result,
			                       [](const UsdStageRefPtr &stage) { return stage->GetFramesPerSecond(); });
			break;
		case LAYER_COUNT:
			UsdWriteColumn<int64_t>(rows, offset, count, result, [](const UsdStageRefPtr &stage) {
				auto session = stage->GetSessionLayer();
				int64_t layer_count = 0;
				for (auto &layer : stage->GetUsedLayers()) {
					layer_count += layer != session;
				}
				return layer_count;
			});
			break;
		case DOCUMENTATION:
			UsdWriteStrings(rows, offset, count, result, [](const UsdStageRefPtr &stage, std::string &out) {
				out = stage->GetRootLayer()->GetDocumentation();
				return !out.empty();
			});
			break;
		case CUSTOM_LAYER_DATA:
			UsdWriteVariants(rows, offset, count, result, [](const UsdStageRefPtr &stage, std::string &out) {
				auto data = stage->GetRootLayer()->GetCustomLayerData();
				if (data.empty()) {
					return false;
				}
				UsdJson::Append(pxr::VtValue(std::move(data)), out);
				return true;
			});
			break;
		default:
			throw InternalException("usd_stage_metadata: unknown column %d", column);
		}
	}
};

unique_ptr<UsdRowProducer> CreateProducer(ClientContext &, const UsdScanBindData &, const vector<column_t> &) {
	return make_uniq<UsdStageMetadataProducer>();
}

} // namespace

CreateTableFunctionInfo UsdStageMetadataFunction() {
	UsdTableFunctionDefinition definition;
	definition.name = "usd_stage_metadata";
	definition.description =
	    "One row per stage: default prim, up axis, meters per unit, time codes and custom layer data.";
	definition.example = "usd_stage_metadata('scenes/*.usd', filename := true)";
	definition.unit = UsdScanUnit::STAGE;
	definition.columns = {{"default_prim", LogicalType::VARCHAR},     {"up_axis", LogicalType::VARCHAR},
	                      {"meters_per_unit", LogicalType::DOUBLE},   {"start_time_code", LogicalType::DOUBLE},
	                      {"end_time_code", LogicalType::DOUBLE},     {"time_codes_per_second", LogicalType::DOUBLE},
	                      {"frames_per_second", LogicalType::DOUBLE}, {"layer_count", LogicalType::BIGINT},
	                      {"documentation", LogicalType::VARCHAR},    {"custom_layer_data", LogicalType::VARIANT()}};
	definition.create_producer = CreateProducer;
	return UsdCreateScanFunction(std::move(definition));
}

} // namespace duckdb
