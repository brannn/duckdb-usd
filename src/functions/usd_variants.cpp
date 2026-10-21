#include "usd_functions.hpp"
#include "usd_scan.hpp"

#include <pxr/usd/usd/variantSets.h>

namespace duckdb {

namespace {

enum UsdVariantsColumn : column_t { PRIM_PATH, VARIANT_SET, SELECTION, VARIANTS };

struct VariantSetRow {
	pxr::UsdPrim prim;
	std::string variant_set;
	std::string selection;
	std::vector<std::string> variants;
};

class UsdVariantsProducer : public UsdBufferedProducer<VariantSetRow> {
public:
	void AddPrim(const pxr::UsdPrim &prim) override {
		if (!prim.HasVariantSets()) {
			return;
		}
		auto variant_sets = prim.GetVariantSets();
		for (auto &name : variant_sets.GetNames()) {
			auto variant_set = variant_sets.GetVariantSet(name);
			rows.push_back({prim, name, variant_set.GetVariantSelection(), variant_set.GetVariantNames()});
		}
	}

	void Write(column_t column, idx_t offset, idx_t count, Vector &result) override {
		switch (column) {
		case PRIM_PATH:
			WritePrimPaths(offset, count, result);
			break;
		case VARIANT_SET:
			UsdWriteStringRefs(rows, offset, count, result, [](const VariantSetRow &row) { return &row.variant_set; });
			break;
		case SELECTION:
			UsdWriteStringRefs(rows, offset, count, result, [](const VariantSetRow &row) -> const std::string * {
				return row.selection.empty() ? nullptr : &row.selection;
			});
			break;
		case VARIANTS:
			UsdWriteStringLists(rows, offset, count, result,
			                    [](const VariantSetRow &row, std::vector<std::string> &out) {
				                    out = row.variants;
				                    return true;
			                    });
			break;
		default:
			throw InternalException("usd_variants: unknown column %d", column);
		}
	}
};

unique_ptr<UsdRowProducer> CreateProducer(ClientContext &, const UsdScanBindData &, const vector<column_t> &) {
	return make_uniq<UsdVariantsProducer>();
}

} // namespace

CreateTableFunctionInfo UsdVariantsFunction() {
	UsdTableFunctionDefinition definition;
	definition.name = "usd_variants";
	definition.description = "One row per variant set of each prim, with its selection and variants.";
	definition.example = "usd_variants('scene.usda')";
	definition.columns = {{"prim_path", LogicalType::VARCHAR},
	                      {"variant_set", LogicalType::VARCHAR},
	                      {"selection", LogicalType::VARCHAR},
	                      {"variants", LogicalType::LIST(LogicalType::VARCHAR)}};
	definition.create_producer = CreateProducer;
	return UsdCreateScanFunction(std::move(definition));
}

} // namespace duckdb
