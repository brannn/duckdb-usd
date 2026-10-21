#include "usd_functions.hpp"
#include "usd_scan.hpp"

#include <pxr/usd/usd/relationship.h>

namespace duckdb {

namespace {

enum UsdRelationshipsColumn : column_t { PRIM_PATH, RELATIONSHIP_NAME, TARGET_PATH, TARGET_INDEX };

struct TargetRow {
	pxr::UsdPrim prim;
	pxr::TfToken relationship_name;
	pxr::SdfPath target;
	int32_t index;
};

class UsdRelationshipsProducer : public UsdBufferedProducer<TargetRow> {
public:
	explicit UsdRelationshipsProducer(bool forwarded) : forwarded(forwarded) {
	}

	void AddPrim(const pxr::UsdPrim &prim) override {
		pxr::SdfPathVector targets;
		for (auto &relationship : prim.GetRelationships()) {
			targets.clear();
			if (forwarded) {
				relationship.GetForwardedTargets(&targets);
			} else {
				relationship.GetTargets(&targets);
			}
			for (idx_t i = 0; i < targets.size(); i++) {
				rows.push_back({prim, relationship.GetName(), targets[i], UnsafeNumericCast<int32_t>(i)});
			}
		}
	}

	void Write(column_t column, idx_t offset, idx_t count, Vector &result) override {
		switch (column) {
		case PRIM_PATH:
			WritePrimPaths(offset, count, result);
			break;
		case RELATIONSHIP_NAME:
			UsdWriteStringRefs(rows, offset, count, result,
			                   [](const TargetRow &row) { return &row.relationship_name.GetString(); });
			break;
		case TARGET_PATH:
			UsdWriteStringRefs(rows, offset, count, result,
			                   [](const TargetRow &row) { return &row.target.GetString(); });
			break;
		case TARGET_INDEX:
			UsdWriteColumn<int32_t>(rows, offset, count, result, [](const TargetRow &row) { return row.index; });
			break;
		default:
			throw InternalException("usd_relationships: unknown column %d", column);
		}
	}

private:
	bool forwarded;
};

void AddOptions(TypedKwargs &options) {
	options.Add("forwarded", LogicalType::BOOLEAN);
}

void BindOptions(const named_argument_map_t &parameters, UsdScanOptions &options) {
	UsdGetBooleanOption(parameters, "forwarded", options.forwarded);
}

unique_ptr<UsdRowProducer> CreateProducer(ClientContext &, const UsdScanBindData &bind_data, const vector<column_t> &) {
	return make_uniq<UsdRelationshipsProducer>(bind_data.options.forwarded);
}

} // namespace

CreateTableFunctionInfo UsdRelationshipsFunction() {
	UsdTableFunctionDefinition definition;
	definition.name = "usd_relationships";
	definition.description = "One row per target of each relationship.";
	definition.example = "usd_relationships('scene.usda')";
	definition.columns = {{"prim_path", LogicalType::VARCHAR},
	                      {"relationship_name", LogicalType::VARCHAR},
	                      {"target_path", LogicalType::VARCHAR},
	                      {"target_index", LogicalType::INTEGER}};
	definition.add_options = AddOptions;
	definition.bind_options = BindOptions;
	definition.create_producer = CreateProducer;
	return UsdCreateScanFunction(std::move(definition));
}

} // namespace duckdb
