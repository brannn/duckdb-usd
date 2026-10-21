#include "usd_functions.hpp"
#include "usd_scan.hpp"

#include <pxr/usd/usd/attribute.h>
#include <pxr/usd/usd/relationship.h>

namespace duckdb {

namespace {

enum UsdPropertiesColumn : column_t {
	PRIM_PATH,
	PROPERTY_NAME,
	PROPERTY_KIND,
	TYPE_NAME,
	IS_ARRAY,
	IS_CUSTOM,
	IS_AUTHORED,
	VARIABILITY,
	TIME_SAMPLE_COUNT,
	ARRAY_LENGTH,
	VALUE
};

struct PropertyRow {
	pxr::UsdPrim prim;
	pxr::UsdProperty property;
	bool is_attribute;
	//! The resolved value (attributes) or targets (relationships). Only fetched if a value column is projected.
	pxr::VtValue value;
};

class UsdPropertiesProducer : public UsdBufferedProducer<PropertyRow> {
public:
	UsdPropertiesProducer(const UsdScanOptions &options, const vector<column_t> &columns)
	    : time(options.TimeCode()), authored_only(options.authored_only), max_array_length(options.max_array_length),
	      fetch_values(UsdIsProjected(columns, VALUE) || UsdIsProjected(columns, ARRAY_LENGTH)), selection(options) {
	}

	void SetTime(pxr::UsdTimeCode time_p) override {
		time = time_p;
	}

	void AddPrim(const pxr::UsdPrim &prim) override {
		for (auto &property : selection.Get(prim)) {
			if (authored_only && !property.IsAuthored()) {
				continue;
			}
			PropertyRow row {prim, property, property.Is<pxr::UsdAttribute>(), pxr::VtValue()};
			if (fetch_values) {
				if (row.is_attribute) {
					property.As<pxr::UsdAttribute>().Get(&row.value, time);
				} else {
					pxr::SdfPathVector targets;
					property.As<pxr::UsdRelationship>().GetTargets(&targets);
					// a string array, so that the value is a list of paths and array_length counts the targets
					pxr::VtStringArray paths;
					paths.reserve(targets.size());
					for (auto &target : targets) {
						paths.push_back(target.GetString());
					}
					row.value = pxr::VtValue(std::move(paths));
				}
			}
			rows.push_back(std::move(row));
		}
	}

	void Write(column_t column, idx_t offset, idx_t count, Vector &result) override {
		switch (column) {
		case PRIM_PATH:
			WritePrimPaths(offset, count, result);
			break;
		case PROPERTY_NAME:
			UsdWriteStringRefs(rows, offset, count, result,
			                   [](const PropertyRow &row) { return &row.property.GetName().GetString(); });
			break;
		case PROPERTY_KIND:
			UsdWriteStrings(rows, offset, count, result,
			                [](const PropertyRow &row) { return row.is_attribute ? "attribute" : "relationship"; });
			break;
		case TYPE_NAME:
			UsdWriteStrings(rows, offset, count, result, [](const PropertyRow &row, std::string &out) {
				if (!row.is_attribute) {
					return false;
				}
				out = row.property.As<pxr::UsdAttribute>().GetTypeName().GetAsToken().GetString();
				return true;
			});
			break;
		case IS_ARRAY:
			UsdWriteColumn<bool>(rows, offset, count, result, [](const PropertyRow &row, bool &out) {
				if (!row.is_attribute) {
					return false;
				}
				out = row.property.As<pxr::UsdAttribute>().GetTypeName().IsArray();
				return true;
			});
			break;
		case IS_CUSTOM:
			UsdWriteColumn<bool>(rows, offset, count, result,
			                     [](const PropertyRow &row) { return row.property.IsCustom(); });
			break;
		case IS_AUTHORED:
			UsdWriteColumn<bool>(rows, offset, count, result,
			                     [](const PropertyRow &row) { return row.property.IsAuthored(); });
			break;
		case VARIABILITY:
			UsdWriteStrings(rows, offset, count, result, [](const PropertyRow &row) {
				auto variability = row.is_attribute ? row.property.As<pxr::UsdAttribute>().GetVariability()
				                                    : pxr::SdfVariabilityUniform;
				return variability == pxr::SdfVariabilityUniform ? "uniform" : "varying";
			});
			break;
		case TIME_SAMPLE_COUNT:
			UsdWriteColumn<int64_t>(rows, offset, count, result, [](const PropertyRow &row, int64_t &out) {
				if (!row.is_attribute) {
					return false;
				}
				out = UnsafeNumericCast<int64_t>(row.property.As<pxr::UsdAttribute>().GetNumTimeSamples());
				return true;
			});
			break;
		case ARRAY_LENGTH:
			UsdWriteArrayLengths(rows, offset, count, result);
			break;
		case VALUE:
			UsdWriteValues(rows, offset, count, max_array_length, result);
			break;
		default:
			throw InternalException("usd_properties: unknown column %d", column);
		}
	}

private:
	pxr::UsdTimeCode time;
	bool authored_only;
	idx_t max_array_length;
	bool fetch_values;
	UsdPropertySelection selection;
};

void AddOptions(TypedKwargs &options) {
	options.Add("authored_only", LogicalType::BOOLEAN);
}

void BindOptions(const named_argument_map_t &parameters, UsdScanOptions &options) {
	UsdGetBooleanOption(parameters, "authored_only", options.authored_only);
}

unique_ptr<UsdRowProducer> CreateProducer(ClientContext &, const UsdScanBindData &bind_data,
                                          const vector<column_t> &columns) {
	return make_uniq<UsdPropertiesProducer>(bind_data.options, columns);
}

} // namespace

CreateTableFunctionInfo UsdPropertiesFunction() {
	UsdTableFunctionDefinition definition;
	definition.name = "usd_properties";
	definition.description = "One row per property (attribute or relationship) of each prim, with its type, flags, "
	                         "time samples and typed value.";
	definition.example = "usd_properties('scene.usda', authored_only := true)";
	definition.time_dependent = true;
	definition.returns_values = true;
	definition.property_name_column = PROPERTY_NAME;
	definition.columns = {{"prim_path", LogicalType::VARCHAR},        {"property_name", LogicalType::VARCHAR},
	                      {"property_kind", LogicalType::VARCHAR},    {"type_name", LogicalType::VARCHAR},
	                      {"is_array", LogicalType::BOOLEAN},         {"is_custom", LogicalType::BOOLEAN},
	                      {"is_authored", LogicalType::BOOLEAN},      {"variability", LogicalType::VARCHAR},
	                      {"time_sample_count", LogicalType::BIGINT}, {"array_length", LogicalType::BIGINT},
	                      {"value", LogicalType::VARIANT()}};
	definition.add_options = AddOptions;
	definition.bind_options = BindOptions;
	definition.create_producer = CreateProducer;
	return UsdCreateScanFunction(std::move(definition));
}

} // namespace duckdb
