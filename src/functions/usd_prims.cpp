#include "usd_functions.hpp"
#include "usd_json.hpp"
#include "usd_scan.hpp"

#include <pxr/usd/sdf/schema.h>
#include <pxr/usd/usd/modelAPI.h>

namespace duckdb {

namespace {

enum UsdPrimsColumn : column_t {
	PRIM_PATH,
	PARENT_PATH,
	NAME,
	DEPTH,
	PRIM_TYPE,
	KIND,
	SPECIFIER,
	ACTIVE,
	DEFINED,
	ABSTRACT,
	INSTANCEABLE,
	INSTANCE,
	INSTANCE_PROXY,
	HAS_PAYLOAD,
	LOADED,
	APPLIED_SCHEMAS,
	CUSTOM_DATA,
	ASSET_INFO
};

const char *SpecifierName(pxr::SdfSpecifier specifier) {
	switch (specifier) {
	case pxr::SdfSpecifierDef:
		return "def";
	case pxr::SdfSpecifierOver:
		return "over";
	case pxr::SdfSpecifierClass:
		return "class";
	default:
		return nullptr;
	}
}

//! Appends the authored value of a dictionary metadata field as JSON. GetCustomData() is not used because it also
//! returns the documentation the schema registry adds to every typed prim.
bool AppendAuthoredMetadata(const pxr::UsdPrim &prim, const pxr::TfToken &key, std::string &out) {
	auto metadata = prim.GetAllAuthoredMetadata();
	auto entry = metadata.find(key);
	if (entry == metadata.end() || entry->second.IsEmpty()) {
		return false;
	}
	UsdJson::Append(entry->second, out);
	return true;
}

class UsdPrimsProducer : public UsdBufferedProducer<pxr::UsdPrim> {
public:
	void AddPrim(const pxr::UsdPrim &prim) override {
		rows.push_back(prim);
	}

	void Write(column_t column, idx_t offset, idx_t count, Vector &result) override {
		using pxr::UsdPrim;
		switch (column) {
		case PRIM_PATH:
			UsdWriteStringRefs(rows, offset, count, result,
			                   [](const UsdPrim &prim) { return &prim.GetPath().GetString(); });
			break;
		case PARENT_PATH:
			UsdWriteStrings(rows, offset, count, result,
			                [](const UsdPrim &prim) { return prim.GetPath().GetParentPath().GetString(); });
			break;
		case NAME:
			UsdWriteStringRefs(rows, offset, count, result,
			                   [](const UsdPrim &prim) { return &prim.GetName().GetString(); });
			break;
		case DEPTH:
			UsdWriteColumn<int32_t>(rows, offset, count, result, [](const UsdPrim &prim) {
				return UnsafeNumericCast<int32_t>(prim.GetPath().GetPathElementCount());
			});
			break;
		case PRIM_TYPE:
			UsdWriteStringRefs(rows, offset, count, result, [](const UsdPrim &prim) -> const std::string * {
				auto &type_name = prim.GetTypeName();
				return type_name.IsEmpty() ? nullptr : &type_name.GetString();
			});
			break;
		case KIND:
			UsdWriteStrings(rows, offset, count, result, [](const UsdPrim &prim, std::string &out) {
				pxr::TfToken kind;
				if (!pxr::UsdModelAPI(prim).GetKind(&kind) || kind.IsEmpty()) {
					return false;
				}
				out = kind.GetString();
				return true;
			});
			break;
		case SPECIFIER:
			UsdWriteStrings(rows, offset, count, result, [](const UsdPrim &prim, std::string &out) {
				auto name = SpecifierName(prim.GetSpecifier());
				if (!name) {
					return false;
				}
				out = name;
				return true;
			});
			break;
		case ACTIVE:
			UsdWriteColumn<bool>(rows, offset, count, result, [](const UsdPrim &prim) { return prim.IsActive(); });
			break;
		case DEFINED:
			UsdWriteColumn<bool>(rows, offset, count, result, [](const UsdPrim &prim) { return prim.IsDefined(); });
			break;
		case ABSTRACT:
			UsdWriteColumn<bool>(rows, offset, count, result, [](const UsdPrim &prim) { return prim.IsAbstract(); });
			break;
		case INSTANCEABLE:
			UsdWriteColumn<bool>(rows, offset, count, result,
			                     [](const UsdPrim &prim) { return prim.IsInstanceable(); });
			break;
		case INSTANCE:
			UsdWriteColumn<bool>(rows, offset, count, result, [](const UsdPrim &prim) { return prim.IsInstance(); });
			break;
		case INSTANCE_PROXY:
			UsdWriteColumn<bool>(rows, offset, count, result,
			                     [](const UsdPrim &prim) { return prim.IsInstanceProxy(); });
			break;
		case HAS_PAYLOAD:
			UsdWriteColumn<bool>(rows, offset, count, result,
			                     [](const UsdPrim &prim) { return prim.HasAuthoredPayloads(); });
			break;
		case LOADED:
			UsdWriteColumn<bool>(rows, offset, count, result, [](const UsdPrim &prim) { return prim.IsLoaded(); });
			break;
		case APPLIED_SCHEMAS:
			UsdWriteStringLists(rows, offset, count, result, [](const UsdPrim &prim, std::vector<std::string> &out) {
				for (auto &schema : prim.GetAppliedSchemas()) {
					out.push_back(schema.GetString());
				}
				return true;
			});
			break;
		case CUSTOM_DATA:
			UsdWriteVariants(rows, offset, count, result, [](const UsdPrim &prim, std::string &out) {
				return prim.HasAuthoredCustomData() && AppendAuthoredMetadata(prim, pxr::SdfFieldKeys->CustomData, out);
			});
			break;
		case ASSET_INFO:
			UsdWriteVariants(rows, offset, count, result, [](const UsdPrim &prim, std::string &out) {
				return prim.HasAuthoredAssetInfo() && AppendAuthoredMetadata(prim, pxr::SdfFieldKeys->AssetInfo, out);
			});
			break;
		default:
			throw InternalException("usd_prims: unknown column %d", column);
		}
	}
};

unique_ptr<UsdRowProducer> CreateProducer(ClientContext &, const UsdScanBindData &, const vector<column_t> &) {
	return make_uniq<UsdPrimsProducer>();
}

} // namespace

CreateTableFunctionInfo UsdPrimsFunction() {
	UsdTableFunctionDefinition definition;
	definition.name = "usd_prims";
	definition.description = "One row per prim of a USD stage: its path, type, kind, specifier, activation, "
	                         "instancing, payloads, applied schemas, custom data and asset info.";
	definition.example = "usd_prims('scene.usda')";
	definition.columns = {
	    {"prim_path", LogicalType::VARCHAR},      {"parent_path", LogicalType::VARCHAR},
	    {"name", LogicalType::VARCHAR},           {"depth", LogicalType::INTEGER},
	    {"prim_type", LogicalType::VARCHAR},      {"kind", LogicalType::VARCHAR},
	    {"specifier", LogicalType::VARCHAR},      {"active", LogicalType::BOOLEAN},
	    {"defined", LogicalType::BOOLEAN},        {"abstract", LogicalType::BOOLEAN},
	    {"instanceable", LogicalType::BOOLEAN},   {"instance", LogicalType::BOOLEAN},
	    {"instance_proxy", LogicalType::BOOLEAN}, {"has_payload", LogicalType::BOOLEAN},
	    {"loaded", LogicalType::BOOLEAN},         {"applied_schemas", LogicalType::LIST(LogicalType::VARCHAR)},
	    {"custom_data", LogicalType::VARIANT()},  {"asset_info", LogicalType::VARIANT()}};
	definition.create_producer = CreateProducer;
	return UsdCreateScanFunction(std::move(definition));
}

} // namespace duckdb
