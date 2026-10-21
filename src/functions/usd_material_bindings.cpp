#include "usd_functions.hpp"
#include "usd_scan.hpp"

#include <pxr/usd/usdGeom/imageable.h>
#include <pxr/usd/usdGeom/subset.h>
#include <pxr/usd/usdShade/material.h>
#include <pxr/usd/usdShade/materialBindingAPI.h>
#include <pxr/usd/usdShade/tokens.h>

namespace duckdb {

namespace {

enum UsdMaterialBindingsColumn : column_t {
	PRIM_PATH,
	MATERIAL_PATH,
	MATERIAL_FOUND,
	BINDING_TYPE,
	COLLECTION_PATH,
	BINDING_PRIM_PATH,
	INHERITED,
	STRENGTH,
	MATERIAL_PURPOSE
};

//! The material purposes, as USD names them in its documentation (the all-purpose token itself is empty)
const char *PurposeName(const pxr::TfToken &purpose) {
	return purpose.IsEmpty() ? "allPurpose" : purpose.GetText();
}

struct BindingRow {
	pxr::UsdPrim prim;
	//! The winning binding relationship
	pxr::UsdRelationship binding;
	pxr::SdfPath material_path;
	bool material_found;
};

bool IsCollectionBinding(const pxr::UsdRelationship &binding) {
	return pxr::UsdShadeMaterialBindingAPI::CollectionBinding::IsCollectionBindingRel(binding);
}

//! The purpose a binding relationship is for, from its name: material:binding[:purpose] for a direct binding,
//! material:binding:collection[:purpose]:name for a collection-based one
pxr::TfToken BindingPurpose(const pxr::UsdRelationship &binding) {
	auto parts = pxr::SdfPath::TokenizeIdentifierAsTokens(binding.GetName());
	auto purpose_index = IsCollectionBinding(binding) ? 3 : 2;
	auto all_purpose_size = IsCollectionBinding(binding) ? 4 : 2;
	if (parts.size() > static_cast<size_t>(all_purpose_size)) {
		return parts[purpose_index];
	}
	return pxr::UsdShadeTokens->allPurpose;
}

class UsdMaterialBindingsProducer : public UsdBufferedProducer<BindingRow> {
public:
	explicit UsdMaterialBindingsProducer(const UsdScanOptions &options) : purpose(options.material_purpose) {
	}

	void BeginStage(const UsdStageHandle &) override {
		bindings_cache.clear();
		collection_cache.clear();
	}

	void AddPrim(const pxr::UsdPrim &prim) override {
		// bindings apply to what is rendered: materials, shaders and other non-imageable prims are skipped
		if (!prim.IsA<pxr::UsdGeomImageable>() && !prim.IsA<pxr::UsdGeomSubset>()) {
			return;
		}
		pxr::UsdRelationship binding;
		auto material = pxr::UsdShadeMaterialBindingAPI(prim).ComputeBoundMaterial(&bindings_cache, &collection_cache,
		                                                                           purpose, &binding);
		if (!binding) {
			return;
		}
		auto material_path = pxr::UsdShadeMaterialBindingAPI::GetResolvedTargetPathFromBindingRel(binding);
		rows.push_back({prim, binding, material_path, static_cast<bool>(material)});
	}

	void Clear() override {
		UsdBufferedProducer::Clear();
		// keyed by prim path, the bindings cache would grow to every prim of the stage (and of every instance):
		// a morsel's prims share their ancestors, so it is kept for one morsel at a time
		bindings_cache.clear();
	}

	void Write(column_t column, idx_t offset, idx_t count, Vector &result) override {
		switch (column) {
		case PRIM_PATH:
			WritePrimPaths(offset, count, result);
			break;
		case MATERIAL_PATH:
			UsdWriteStringRefs(rows, offset, count, result,
			                   [](const BindingRow &row) { return &row.material_path.GetString(); });
			break;
		case MATERIAL_FOUND:
			UsdWriteColumn<bool>(rows, offset, count, result, [](const BindingRow &row) { return row.material_found; });
			break;
		case BINDING_TYPE:
			UsdWriteStrings(rows, offset, count, result, [](const BindingRow &row) {
				return IsCollectionBinding(row.binding) ? "collection" : "direct";
			});
			break;
		case COLLECTION_PATH:
			UsdWriteStrings(rows, offset, count, result, [](const BindingRow &row, std::string &out) {
				if (!IsCollectionBinding(row.binding)) {
					return false;
				}
				out = pxr::UsdShadeMaterialBindingAPI::CollectionBinding(row.binding).GetCollectionPath().GetString();
				return true;
			});
			break;
		case BINDING_PRIM_PATH:
			UsdWriteStringRefs(rows, offset, count, result,
			                   [](const BindingRow &row) { return &row.binding.GetPrimPath().GetString(); });
			break;
		case INHERITED:
			UsdWriteColumn<bool>(rows, offset, count, result,
			                     [](const BindingRow &row) { return row.binding.GetPrimPath() != row.prim.GetPath(); });
			break;
		case STRENGTH:
			UsdWriteStrings(rows, offset, count, result, [](const BindingRow &row) {
				return pxr::UsdShadeMaterialBindingAPI::GetMaterialBindingStrength(row.binding).GetString();
			});
			break;
		case MATERIAL_PURPOSE:
			UsdWriteStrings(rows, offset, count, result,
			                [](const BindingRow &row) { return PurposeName(BindingPurpose(row.binding)); });
			break;
		default:
			throw InternalException("usd_material_bindings: unknown column %d", column);
		}
	}

private:
	const pxr::TfToken purpose;
	pxr::UsdShadeMaterialBindingAPI::BindingsCache bindings_cache;
	pxr::UsdShadeMaterialBindingAPI::CollectionQueryCache collection_cache;
};

void AddOptions(TypedKwargs &options) {
	options.Add("material_purpose", LogicalType::VARCHAR);
}

void BindOptions(const named_argument_map_t &parameters, UsdScanOptions &options) {
	auto purpose = UsdGetOption(parameters, "material_purpose");
	if (!purpose) {
		return;
	}
	auto name = StringValue::Get(*purpose);
	if (name == "allPurpose") {
		options.material_purpose = "";
	} else if (name == "full" || name == "preview") {
		options.material_purpose = name;
	} else {
		throw BinderException("Unknown material purpose '%s' - expected 'allPurpose', 'full' or 'preview'", name);
	}
}

unique_ptr<UsdRowProducer> CreateProducer(ClientContext &, const UsdScanBindData &bind_data, const vector<column_t> &) {
	return make_uniq<UsdMaterialBindingsProducer>(bind_data.options);
}

} // namespace

CreateTableFunctionInfo UsdMaterialBindingsFunction() {
	UsdTableFunctionDefinition definition;
	definition.name = "usd_material_bindings";
	definition.description = "The material each prim is bound to, and the binding that decides it.";
	definition.example = "usd_material_bindings('scene.usda', material_purpose := 'preview')";
	definition.columns = {{"prim_path", LogicalType::VARCHAR},       {"material_path", LogicalType::VARCHAR},
	                      {"material_found", LogicalType::BOOLEAN},  {"binding_type", LogicalType::VARCHAR},
	                      {"collection_path", LogicalType::VARCHAR}, {"binding_prim_path", LogicalType::VARCHAR},
	                      {"inherited", LogicalType::BOOLEAN},       {"strength", LogicalType::VARCHAR},
	                      {"material_purpose", LogicalType::VARCHAR}};
	definition.add_options = AddOptions;
	definition.bind_options = BindOptions;
	definition.create_producer = CreateProducer;
	return UsdCreateScanFunction(std::move(definition));
}

} // namespace duckdb
