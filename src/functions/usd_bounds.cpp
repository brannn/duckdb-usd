#include "usd_functions.hpp"
#include "usd_scan.hpp"

#include <pxr/base/gf/range3d.h>
#include <pxr/usd/usdGeom/bboxCache.h>
#include <pxr/usd/usdGeom/imageable.h>

namespace duckdb {

namespace {

enum UsdBoundsColumn : column_t { PRIM_PATH, MIN_X, MIN_Y, MIN_Z, MAX_X, MAX_Y, MAX_Z };

struct BoundsRow {
	pxr::UsdPrim prim;
	pxr::GfRange3d range;
};

pxr::TfTokenVector PurposeTokens(const vector<string> &purposes) {
	pxr::TfTokenVector result;
	for (auto &purpose : purposes) {
		result.emplace_back(purpose);
	}
	return result;
}

class UsdBoundsProducer : public UsdBufferedProducer<BoundsRow> {
public:
	explicit UsdBoundsProducer(const UsdScanOptions &options)
	    : cache(options.TimeCode(), PurposeTokens(options.purposes), options.use_extents_hint) {
	}

	void BeginStage(const UsdStageHandle &) override {
		cache.Clear();
	}

	void SetTime(pxr::UsdTimeCode time) override {
		cache.SetTime(time);
	}

	void AddPrim(const pxr::UsdPrim &prim) override {
		if (!prim.IsA<pxr::UsdGeomImageable>()) {
			return;
		}
		rows.push_back({prim, cache.ComputeWorldBound(prim).ComputeAlignedRange()});
	}

	void Write(column_t column, idx_t offset, idx_t count, Vector &result) override {
		if (column == PRIM_PATH) {
			WritePrimPaths(offset, count, result);
			return;
		}
		if (column < MIN_X || column > MAX_Z) {
			throw InternalException("usd_bounds: unknown column %d", column);
		}
		bool is_min = column <= MIN_Z;
		auto axis = is_min ? column - MIN_X : column - MAX_X;
		UsdWriteColumn<double>(rows, offset, count, result, [is_min, axis](const BoundsRow &row, double &out) {
			if (row.range.IsEmpty()) {
				return false;
			}
			out = is_min ? row.range.GetMin()[axis] : row.range.GetMax()[axis];
			return true;
		});
	}

private:
	pxr::UsdGeomBBoxCache cache;
};

void AddOptions(TypedKwargs &options) {
	options.Add("purposes", LogicalType::LIST(LogicalType::VARCHAR));
	options.Add("use_extents_hint", LogicalType::BOOLEAN);
}

void BindOptions(const named_argument_map_t &parameters, UsdScanOptions &options) {
	if (auto purposes = UsdGetOption(parameters, "purposes")) {
		options.purposes.clear();
		for (auto &purpose : ListValue::GetChildren(*purposes)) {
			if (purpose.IsNull()) {
				throw BinderException("The \"purposes\" option cannot contain NULL");
			}
			auto name = StringValue::Get(purpose);
			if (name != "default" && name != "render" && name != "proxy" && name != "guide") {
				throw BinderException("Unknown purpose '%s' - expected 'default', 'render', 'proxy' or 'guide'", name);
			}
			options.purposes.push_back(name);
		}
	}
	UsdGetBooleanOption(parameters, "use_extents_hint", options.use_extents_hint);
}

unique_ptr<UsdRowProducer> CreateProducer(ClientContext &, const UsdScanBindData &bind_data, const vector<column_t> &) {
	return make_uniq<UsdBoundsProducer>(bind_data.options);
}

} // namespace

CreateTableFunctionInfo UsdBoundsFunction() {
	UsdTableFunctionDefinition definition;
	definition.name = "usd_bounds";
	definition.description = "The world-space axis-aligned bounding box of each imageable prim.";
	definition.example = "usd_bounds('scene.usda', purposes := ['default', 'render', 'proxy'])";
	definition.time_dependent = true;
	definition.columns = {{"prim_path", LogicalType::VARCHAR}, {"min_x", LogicalType::DOUBLE},
	                      {"min_y", LogicalType::DOUBLE},      {"min_z", LogicalType::DOUBLE},
	                      {"max_x", LogicalType::DOUBLE},      {"max_y", LogicalType::DOUBLE},
	                      {"max_z", LogicalType::DOUBLE}};
	definition.add_options = AddOptions;
	definition.bind_options = BindOptions;
	definition.create_producer = CreateProducer;
	return UsdCreateScanFunction(std::move(definition));
}

} // namespace duckdb
