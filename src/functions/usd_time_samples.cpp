#include "usd_functions.hpp"
#include "usd_scan.hpp"

#include <pxr/base/gf/interval.h>
#include <pxr/base/ts/spline.h>
#include <pxr/usd/usd/attribute.h>

namespace duckdb {

namespace {

enum UsdTimeSamplesColumn : column_t { PRIM_PATH, PROPERTY_NAME, TYPE_NAME, TIME, ARRAY_LENGTH, VALUE };

struct TimeSampleRow {
	pxr::UsdPrim prim;
	pxr::UsdAttribute attribute;
	double time;
	//! The value at the sample. Only fetched if a value column is projected.
	pxr::VtValue value;
};

class UsdTimeSamplesProducer : public UsdBufferedProducer<TimeSampleRow> {
public:
	UsdTimeSamplesProducer(const UsdScanOptions &options, const vector<column_t> &columns)
	    : interval(options.start_time, options.end_time), max_array_length(options.max_array_length),
	      fetch_values(UsdIsProjected(columns, VALUE) || UsdIsProjected(columns, ARRAY_LENGTH)), selection(options) {
	}

	void AddPrim(const pxr::UsdPrim &prim) override {
		for (auto &property : selection.Get(prim)) {
			if (!property.Is<pxr::UsdAttribute>()) {
				continue;
			}
			auto attribute = property.As<pxr::UsdAttribute>();
			times.clear();
			if (attribute.HasSpline()) {
				// an attribute animated with a spline has knots rather than time samples (GetKnots(interval) also
				// returns the knots around the interval that shape the curve inside it)
				for (auto &knot : attribute.GetSpline().GetKnots()) {
					if (interval.Contains(knot.GetTime())) {
						times.push_back(knot.GetTime());
					}
				}
			} else {
				attribute.GetTimeSamplesInInterval(interval, &times);
			}
			for (auto time : times) {
				TimeSampleRow row {prim, attribute, time, pxr::VtValue()};
				if (fetch_values) {
					attribute.Get(&row.value, time);
				}
				rows.push_back(std::move(row));
			}
		}
	}

	void Write(column_t column, idx_t offset, idx_t count, Vector &result) override {
		switch (column) {
		case PRIM_PATH:
			WritePrimPaths(offset, count, result);
			break;
		case PROPERTY_NAME:
			UsdWriteStringRefs(rows, offset, count, result,
			                   [](const TimeSampleRow &row) { return &row.attribute.GetName().GetString(); });
			break;
		case TYPE_NAME:
			UsdWriteStrings(rows, offset, count, result, [](const TimeSampleRow &row) {
				return row.attribute.GetTypeName().GetAsToken().GetString();
			});
			break;
		case TIME:
			UsdWriteColumn<double>(rows, offset, count, result, [](const TimeSampleRow &row) { return row.time; });
			break;
		case ARRAY_LENGTH:
			UsdWriteArrayLengths(rows, offset, count, result);
			break;
		case VALUE:
			UsdWriteValues(rows, offset, count, max_array_length, result);
			break;
		default:
			throw InternalException("usd_time_samples: unknown column %d", column);
		}
	}

private:
	pxr::GfInterval interval;
	idx_t max_array_length;
	bool fetch_values;
	UsdPropertySelection selection;
	std::vector<double> times;
};

void AddOptions(TypedKwargs &options) {
	options.Add("start_time", LogicalType::DOUBLE);
	options.Add("end_time", LogicalType::DOUBLE);
}

void BindOptions(const named_argument_map_t &parameters, UsdScanOptions &options) {
	if (auto start_time = UsdGetOption(parameters, "start_time")) {
		options.start_time = DoubleValue::Get(*start_time);
	}
	if (auto end_time = UsdGetOption(parameters, "end_time")) {
		options.end_time = DoubleValue::Get(*end_time);
	}
	if (options.start_time > options.end_time) {
		throw BinderException("The \"start_time\" option cannot be after \"end_time\"");
	}
}

unique_ptr<UsdRowProducer> CreateProducer(ClientContext &, const UsdScanBindData &bind_data,
                                          const vector<column_t> &columns) {
	return make_uniq<UsdTimeSamplesProducer>(bind_data.options, columns);
}

} // namespace

CreateTableFunctionInfo UsdTimeSamplesFunction() {
	UsdTableFunctionDefinition definition;
	definition.name = "usd_time_samples";
	definition.description = "One row per time sample (or spline knot) of each attribute, with its time and typed "
	                         "value.";
	definition.example = "usd_time_samples('scene.usda', start_time := 0, end_time := 24)";
	definition.returns_values = true;
	definition.property_name_column = PROPERTY_NAME;
	definition.columns = {{"prim_path", LogicalType::VARCHAR},   {"property_name", LogicalType::VARCHAR},
	                      {"type_name", LogicalType::VARCHAR},   {"time", LogicalType::DOUBLE},
	                      {"array_length", LogicalType::BIGINT}, {"value", LogicalType::VARIANT()}};
	definition.add_options = AddOptions;
	definition.bind_options = BindOptions;
	definition.create_producer = CreateProducer;
	return UsdCreateScanFunction(std::move(definition));
}

} // namespace duckdb
