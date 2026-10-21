//===----------------------------------------------------------------------===//
//                         DuckDB USD Extension
//
// usd_scan.hpp
//
// The scan framework shared by all usd_* table functions. A scan walks the
// prims of one or more stages in parallel "morsels"; each function supplies
// a row producer that turns prims (or whole stages) into buffered rows and
// writes the projected columns of those rows.
//===----------------------------------------------------------------------===//

#pragma once

#include "duckdb.hpp"
#include "duckdb/common/vector/array_vector.hpp"
#include "duckdb/common/vector/vector_writer.hpp"
#include "duckdb/function/table_function.hpp"
#include "duckdb/parser/parsed_data/create_table_function_info.hpp"
#include "usd_json.hpp"
#include "usd_stage.hpp"
#include "utf8proc_wrapper.hpp"

#include <pxr/usd/usd/prim.h>
#include <pxr/usd/usd/primFlags.h>
#include <pxr/usd/usd/property.h>
#include <pxr/usd/usd/timeCode.h>

#include <algorithm>
#include <limits>
#include <tuple>
#include <type_traits>
#include <vector>

namespace duckdb {

//! Options shared by the usd_* table functions, plus the few that only some functions read
struct UsdScanOptions {
	UsdStageOptions stage;
	//! Path of the prim whose subtree is scanned
	string root = "/";
	//! Descend into instances (visit instance proxies)
	bool instance_proxies = false;
	//! Skip inactive prims
	bool active_only = false;
	//! Visit abstract prims (classes)
	bool include_abstract = false;
	//! Visit undefined prims (prims that only have "over" opinions)
	bool include_undefined = false;
	//! Add a "filename" column
	bool filename = false;
	//! Skip files that cannot be opened instead of failing
	bool ignore_errors = false;
	//! Time code to read values, transforms and bounds at; UsdTimeCode::Default() when unset
	bool has_time = false;
	double time = 0;
	//! Time codes to read values, transforms and bounds at, one row per time each (adds a "time" column)
	vector<double> times;

	//! Functions that return values: arrays longer than this produce a NULL value (0 = unlimited)
	idx_t max_array_length = 1000;
	//! usd_properties: skip properties without authored opinions (schema fallbacks)
	bool authored_only = false;
	//! usd_relationships: report forwarded targets (through relationships that target relationships)
	bool forwarded = false;
	//! usd_bounds: purposes included in the bounds
	vector<string> purposes {"default", "render"};
	//! usd_bounds: use extentsHint where authored
	bool use_extents_hint = true;
	//! usd_composition_arcs: also report arcs a prim gets through its ancestors
	bool include_ancestral = false;
	//! usd_material_bindings: the material purpose to resolve bindings for (empty: all-purpose)
	string material_purpose;
	//! usd_time_samples: the (closed) interval of the time samples to list
	double start_time = -std::numeric_limits<double>::infinity();
	double end_time = std::numeric_limits<double>::infinity();
	//! Functions with a property name column: read only the properties with these names (set from the query's filters
	//! on that column, see UsdTableFunctionDefinition::property_name_column)
	bool filter_property_names = false;
	vector<string> property_names;

	pxr::UsdTimeCode TimeCode() const;
	pxr::Usd_PrimFlagsPredicate Predicate() const;

	//! Every option. DuckDB treats two scans whose bind data are equal as the same scan, so an option missing here
	//! makes scans that differ only in it share their results.
	auto Tie() const {
		return std::tie(stage, root, instance_proxies, active_only, include_abstract, include_undefined, filename,
		                ignore_errors, has_time, time, times, max_array_length, authored_only, forwarded, purposes,
		                use_extents_hint, include_ancestral, material_purpose, start_time, end_time,
		                filter_property_names, property_names);
	}
	bool operator==(const UsdScanOptions &other) const {
		return Tie() == other.Tie();
	}
};

//! The properties a scan reads from each prim: all of them, or only the ones named by a filter on property_name
class UsdPropertySelection {
public:
	explicit UsdPropertySelection(const UsdScanOptions &options);

	//! The selected properties of `prim`, in name order
	std::vector<pxr::UsdProperty> Get(const pxr::UsdPrim &prim) const;

private:
	bool filter;
	//! The names to look up, in USD's name order (TfDictionaryLessThan)
	std::vector<pxr::TfToken> names;
};

//! Whether a function produces rows per prim or per stage
enum class UsdScanUnit : uint8_t { PRIM, STAGE };

struct UsdColumn {
	string name;
	LogicalType type;
};

//! Produces the rows of one usd_* function. Each scanning thread owns one producer.
class UsdRowProducer {
public:
	virtual ~UsdRowProducer() = default;

	//! Called whenever the scan moves to another stage. Stage-level functions buffer their rows here.
	virtual void BeginStage(const UsdStageHandle &handle) {
	}
	//! Buffers the rows of one prim (prim-level functions)
	virtual void AddPrim(const pxr::UsdPrim &prim) {
	}
	//! With the "times" option, called before the prims of a morsel are added at each of the times (functions
	//! that depend on time)
	virtual void SetTime(pxr::UsdTimeCode time) {
	}
	//! The number of buffered rows
	virtual idx_t RowCount() const = 0;
	//! Writes column `column` of the buffered rows [offset, offset + count) to `result`
	virtual void Write(column_t column, idx_t offset, idx_t count, Vector &result) = 0;
	//! Drops all buffered rows
	virtual void Clear() = 0;
};

//! Whether `columns`, the columns a producer must write, include `column`
inline bool UsdIsProjected(const vector<column_t> &columns, column_t column) {
	return std::find(columns.begin(), columns.end(), column) != columns.end();
}

struct UsdScanBindData;

struct UsdTableFunctionDefinition {
	string name;
	//! What the function returns (shown by duckdb_functions()), and an example call
	string description;
	string example;
	UsdScanUnit unit = UsdScanUnit::PRIM;
	//! The columns, in the order of the function's column enum
	vector<UsdColumn> columns;
	//! Log a warning when a scanned stage has composition errors
	bool warn_on_composition_errors = true;
	//! Whether the rows depend on time: accepts the "time" and "times" options
	bool time_dependent = false;
	//! Whether the function returns USD values (a VARIANT column): accepts the "max_array_length" option
	bool returns_values = false;
	//! The column that holds each row's property name, if any. Filters that compare it with constants (=, IN) are
	//! pushed into the scan: the producer then reads only those properties (UsdScanOptions::property_names), and the
	//! query's other filters only see rows of those properties.
	optional_idx property_name_column;
	//! Declares the named parameters only this function accepts
	void (*add_options)(TypedKwargs &options) = nullptr;
	//! Reads the named parameters only this function accepts
	void (*bind_options)(const named_argument_map_t &parameters, UsdScanOptions &options) = nullptr;
	//! Creates a producer that writes (at least) the given columns
	unique_ptr<UsdRowProducer> (*create_producer)(ClientContext &context, const UsdScanBindData &bind_data,
	                                              const vector<column_t> &columns) = nullptr;
};

struct UsdScanBindData : public TableFunctionData {
	explicit UsdScanBindData(shared_ptr<const UsdTableFunctionDefinition> definition_p)
	    : definition(std::move(definition_p)) {
	}

	shared_ptr<const UsdTableFunctionDefinition> definition;
	vector<string> files;
	UsdScanOptions options;
	//! Index of the "time" column, with the "times" option
	optional_idx time_column;
	//! Index of the "filename" column, if requested
	optional_idx filename_column;

	unique_ptr<FunctionData> Copy() const override;
	bool Equals(const FunctionData &other) const override;
};

//! Creates the table function (with overloads for a path, a list of paths and a VARIANT) for a definition, with its
//! description
CreateTableFunctionInfo UsdCreateScanFunction(UsdTableFunctionDefinition definition);

//! The named parameter `name`, or nullptr if it is not given. Throws a BinderException if it is NULL.
optional_ptr<const Value> UsdGetOption(const named_argument_map_t &parameters, const char *name);
//! Reads the BOOLEAN named parameter `name` into `result`, if it is given
void UsdGetBooleanOption(const named_argument_map_t &parameters, const char *name, bool &result);

//===--------------------------------------------------------------------===//
// Column writing helpers for producers that buffer rows in a std::vector
//===--------------------------------------------------------------------===//
inline string_t UsdStringRef(const std::string &value) {
	return string_t(value.data(), UnsafeNumericCast<uint32_t>(value.size()));
}

//! USD strings need not be valid UTF-8, which VARCHAR requires: replaces invalid bytes with '?'
inline void UsdMakeValidUtf8(std::string &value) {
	if (Utf8Proc::Analyze(value.data(), value.size()) == UnicodeType::INVALID) {
		Utf8Proc::MakeValid(&value[0], value.size());
	}
}

//! Gets the value of a column for `row` with the column's getter, which is either `get(row, value)`, which sets
//! `value` and returns false for NULL, or `get(row)`, which returns a value that is never NULL
template <class ROW, class T, class F>
bool UsdGetValue(F &get, const ROW &row, T &value) {
	if constexpr (std::is_invocable<F &, const ROW &>::value) {
		value = get(row);
		return true;
	} else {
		return get(row, value);
	}
}

//! Writes a fixed-width column, with a getter as described at UsdGetValue
template <class T, class ROW, class F>
void UsdWriteColumn(const std::vector<ROW> &rows, idx_t offset, idx_t count, Vector &result, F &&get) {
	auto writer = FlatVector::Writer<T>(result, count);
	T value;
	for (idx_t i = 0; i < count; i++) {
		if (UsdGetValue(get, rows[offset + i], value)) {
			writer.WriteValue(value);
		} else {
			writer.WriteNull();
		}
	}
}

//! Writes a VARCHAR column, with a getter as described at UsdGetValue
template <class ROW, class F>
void UsdWriteStrings(const std::vector<ROW> &rows, idx_t offset, idx_t count, Vector &result, F &&get) {
	auto writer = FlatVector::Writer<string_t>(result, count);
	std::string value;
	for (idx_t i = 0; i < count; i++) {
		value.clear();
		if (UsdGetValue(get, rows[offset + i], value)) {
			UsdMakeValidUtf8(value);
			writer.WriteValue(UsdStringRef(value));
		} else {
			writer.WriteNull();
		}
	}
}

//! Writes a VARCHAR column from strings owned elsewhere (e.g. interned USD tokens and paths) without an extra copy.
//! `get(row)` returns a pointer to the string, or nullptr to write NULL.
template <class ROW, class F>
void UsdWriteStringRefs(const std::vector<ROW> &rows, idx_t offset, idx_t count, Vector &result, F &&get) {
	auto writer = FlatVector::Writer<string_t>(result, count);
	std::string valid;
	for (idx_t i = 0; i < count; i++) {
		const std::string *value = get(rows[offset + i]);
		if (value && Utf8Proc::Analyze(value->data(), value->size()) == UnicodeType::INVALID) {
			valid = *value;
			UsdMakeValidUtf8(valid);
			value = &valid;
		}
		if (value) {
			writer.WriteValue(UsdStringRef(*value));
		} else {
			writer.WriteNull();
		}
	}
}

//! Writes a VARCHAR[] column. `get(row, values)` returns false to write NULL.
template <class ROW, class F>
void UsdWriteStringLists(const std::vector<ROW> &rows, idx_t offset, idx_t count, Vector &result, F &&get) {
	auto writer = FlatVector::Writer<VectorListType<string_t>>(result, count);
	std::vector<std::string> values;
	for (idx_t i = 0; i < count; i++) {
		values.clear();
		if (!get(rows[offset + i], values)) {
			writer.WriteNull();
			continue;
		}
		auto list = writer.WriteList(values.size());
		for (auto &value : values) {
			UsdMakeValidUtf8(value);
			list.child_writer.WriteValue(UsdStringRef(value));
		}
	}
}

//! Writes a DOUBLE[width] (ARRAY) column. `get(row, values)` fills `width` doubles, or returns false to write NULL.
template <class ROW, class F>
void UsdWriteDoubleArrays(const std::vector<ROW> &rows, idx_t offset, idx_t count, idx_t width, Vector &result,
                          F &&get) {
	auto &child = ArrayVector::GetChildMutable(result);
	auto child_writer = FlatVector::Writer<double>(child, count * width);
	auto &validity = FlatVector::ValidityMutable(result);
	double values[16];
	D_ASSERT(width <= 16);
	for (idx_t i = 0; i < count; i++) {
		bool valid = get(rows[offset + i], values);
		if (!valid) {
			validity.SetInvalid(i);
		}
		for (idx_t j = 0; j < width; j++) {
			child_writer.WriteValue(valid ? values[j] : 0);
		}
	}
}

//! Writes a VARIANT column from JSON text. `get(row, json)` appends JSON to `json`, or returns false to write NULL.
template <class ROW, class F>
void UsdWriteVariants(const std::vector<ROW> &rows, idx_t offset, idx_t count, Vector &result, F &&get) {
	Vector json_vector(LogicalType::JSON(), count);
	UsdWriteStrings(rows, offset, count, json_vector, std::forward<F>(get));
	VectorOperations::DefaultCast(json_vector, result, count);
}

//! Writes the values of rows with a `value` member (a pxr::VtValue) to a VARIANT column. Empty values, and arrays
//! longer than `max_array_length` unless it is 0, are NULL.
template <class ROW>
void UsdWriteValues(const std::vector<ROW> &rows, idx_t offset, idx_t count, idx_t max_array_length, Vector &result) {
	UsdWriteVariants(rows, offset, count, result, [max_array_length](const ROW &row, std::string &out) {
		if (row.value.IsEmpty()) {
			return false;
		}
		auto length = UsdJson::ArrayLength(row.value);
		if (max_array_length > 0 && length != DConstants::INVALID_INDEX && length > max_array_length) {
			return false;
		}
		UsdJson::Append(row.value, out);
		return true;
	});
}

//! Writes the number of elements of the array values of rows with a `value` member (NULL for other values)
template <class ROW>
void UsdWriteArrayLengths(const std::vector<ROW> &rows, idx_t offset, idx_t count, Vector &result) {
	UsdWriteColumn<int64_t>(rows, offset, count, result, [](const ROW &row, int64_t &out) {
		auto length = UsdJson::ArrayLength(row.value);
		if (length == DConstants::INVALID_INDEX) {
			return false;
		}
		out = UnsafeNumericCast<int64_t>(length);
		return true;
	});
}

//! A producer that buffers its rows in a std::vector
template <class ROW>
class UsdBufferedProducer : public UsdRowProducer {
public:
	idx_t RowCount() const override {
		return rows.size();
	}

	void Clear() override {
		rows.clear();
	}

protected:
	//! Writes the prim paths of rows with a `prim` member
	void WritePrimPaths(idx_t offset, idx_t count, Vector &result) const {
		UsdWriteStringRefs(rows, offset, count, result, [](const ROW &row) { return &row.prim.GetPath().GetString(); });
	}

	std::vector<ROW> rows;
};

} // namespace duckdb
