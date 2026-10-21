#include "usd_scan.hpp"

#include "duckdb/common/error_data.hpp"
#include "duckdb/common/file_system.hpp"
#include "duckdb/common/multi_file/multi_file_reader.hpp"
#include "duckdb/function/function_set.hpp"
#include "duckdb/logging/logger.hpp"
#include "duckdb/main/client_context.hpp"
#include "duckdb/parallel/task_scheduler.hpp"
#include "duckdb/planner/expression/bound_columnref_expression.hpp"
#include "duckdb/planner/expression/bound_comparison_expression.hpp"
#include "duckdb/planner/expression/bound_conjunction_expression.hpp"
#include "duckdb/planner/expression/bound_constant_expression.hpp"
#include "duckdb/planner/expression/bound_operator_expression.hpp"
#include "duckdb/planner/operator/logical_get.hpp"

#include <pxr/base/tf/stringUtils.h>
#include <pxr/usd/sdf/path.h>
#include <pxr/usd/usd/attribute.h>
#include <pxr/usd/usd/primRange.h>
#include <pxr/usd/usd/relationship.h>

namespace duckdb {

namespace {

//! The number of prims handed to a thread at a time
constexpr idx_t USD_MORSEL_SIZE = 1024;

struct UsdTableFunctionInfo : public TableFunctionInfo {
	explicit UsdTableFunctionInfo(shared_ptr<const UsdTableFunctionDefinition> definition_p)
	    : definition(std::move(definition_p)) {
	}
	shared_ptr<const UsdTableFunctionDefinition> definition;
};

//! A unit of work: a run of prims from one stage (or a whole stage, for stage-level functions)
struct UsdMorsel {
	shared_ptr<UsdStageHandle> stage;
	idx_t file_index = DConstants::INVALID_INDEX;
	idx_t batch_index = 0;
	std::vector<pxr::UsdPrim> prims;
};

class UsdScanGlobalState : public GlobalTableFunctionState {
public:
	explicit UsdScanGlobalState(idx_t max_threads_p) : max_threads(max_threads_p) {
	}

	idx_t MaxThreads() const override {
		return max_threads;
	}

	//! Fills `morsel` with the next unit of work. Returns false once every file has been scanned.
	bool Next(ClientContext &context, const UsdScanBindData &bind_data, UsdMorsel &morsel) {
		lock_guard<mutex> guard(lock);
		while (true) {
			if (!stage) {
				if (next_file >= bind_data.files.size()) {
					return false;
				}
				current_file = next_file++;
				files_started = next_file;
				auto &path = bind_data.files[current_file];
				try {
					stage = UsdStageProvider::Open(context, path, bind_data.options.stage,
					                               bind_data.definition->warn_on_composition_errors);
				} catch (const IOException &ex) {
					if (!bind_data.options.ignore_errors) {
						throw;
					}
					DUCKDB_LOG_INTERNAL(context, "usd", LogLevel::LOG_WARNING,
					                    "Skipping \"" + path + "\": " + ErrorData(ex).RawMessage());
					continue;
				}
				if (bind_data.definition->unit == UsdScanUnit::STAGE) {
					morsel.prims.clear();
					SetMorsel(morsel);
					stage.reset();
					return true;
				}
				if (!StartTraversal(bind_data.options)) {
					stage.reset();
					continue;
				}
			}
			morsel.prims.clear();
			while (position != range.end() && morsel.prims.size() < USD_MORSEL_SIZE) {
				morsel.prims.push_back(*position);
				++position;
			}
			if (morsel.prims.empty()) {
				stage.reset();
				continue;
			}
			SetMorsel(morsel);
			return true;
		}
	}

	//! The percentage of files that have been scanned (the file being scanned counts as not done)
	double Progress(idx_t file_count) const {
		auto started = files_started.load();
		if (file_count == 0 || started == 0) {
			return 0.0;
		}
		return 100.0 * static_cast<double>(started - 1) / static_cast<double>(file_count);
	}

private:
	bool StartTraversal(const UsdScanOptions &options) {
		auto predicate = options.Predicate();
		if (options.root == "/") {
			range = pxr::UsdPrimRange::Stage(stage->stage, predicate);
		} else {
			auto root = stage->stage->GetPrimAtPath(pxr::SdfPath(options.root));
			if (!root || !predicate(root)) {
				// the root does not exist in this file (or is filtered out) - nothing to scan here
				return false;
			}
			range = pxr::UsdPrimRange(root, predicate);
		}
		position = range.begin();
		return true;
	}

	void SetMorsel(UsdMorsel &morsel) {
		morsel.stage = stage;
		morsel.file_index = current_file;
		morsel.batch_index = next_batch_index++;
	}

private:
	mutex lock;
	idx_t max_threads;
	idx_t next_file = 0;
	idx_t current_file = 0;
	atomic<idx_t> files_started {0};
	idx_t next_batch_index = 0;
	shared_ptr<UsdStageHandle> stage;
	pxr::UsdPrimRange range;
	pxr::UsdPrimRange::iterator position;
};

class UsdScanLocalState : public LocalTableFunctionState {
public:
	unique_ptr<UsdRowProducer> producer;
	vector<column_t> column_ids;
	UsdMorsel morsel;
	//! Next prim of the morsel to hand to the producer
	idx_t next_prim = 0;
	//! With the "times" option: the time the morsel's prims are being added at, and the time of each buffered row
	idx_t time_index = 0;
	std::vector<double> row_times;
	//! First buffered row that has not been emitted
	idx_t row_offset = 0;
};

void ParseScanOptions(const named_argument_map_t &parameters, UsdScanOptions &options) {
	UsdGetBooleanOption(parameters, "load_payloads", options.stage.load_payloads);
	UsdGetBooleanOption(parameters, "instance_proxies", options.instance_proxies);
	UsdGetBooleanOption(parameters, "active_only", options.active_only);
	UsdGetBooleanOption(parameters, "include_abstract", options.include_abstract);
	UsdGetBooleanOption(parameters, "include_undefined", options.include_undefined);
	UsdGetBooleanOption(parameters, "filename", options.filename);
	UsdGetBooleanOption(parameters, "ignore_errors", options.ignore_errors);

	if (auto root = UsdGetOption(parameters, "root")) {
		auto root_path = StringValue::Get(*root);
		pxr::SdfPath path(root_path);
		if (path.IsEmpty() || !(path.IsAbsoluteRootPath() || (path.IsAbsolutePath() && path.IsPrimPath()))) {
			throw BinderException("The \"root\" option must be an absolute prim path such as '/World', got '%s'",
			                      root_path);
		}
		options.root = path.GetString();
		// compose only the subtree that is scanned
		options.stage.mask_path = options.root;
	}

	if (auto time = UsdGetOption(parameters, "time")) {
		options.has_time = true;
		options.time = DoubleValue::Get(*time);
	}
	if (auto times = UsdGetOption(parameters, "times")) {
		if (options.has_time) {
			throw BinderException("The \"time\" and \"times\" options cannot be combined");
		}
		for (auto &time : ListValue::GetChildren(*times)) {
			if (time.IsNull()) {
				throw BinderException("The \"times\" option cannot contain NULL");
			}
			options.times.push_back(DoubleValue::Get(time));
		}
		if (options.times.empty()) {
			throw BinderException("The \"times\" option cannot be empty");
		}
	}

	if (auto max_array_length = UsdGetOption(parameters, "max_array_length")) {
		auto value = BigIntValue::Get(*max_array_length);
		if (value < 0) {
			throw BinderException("The \"max_array_length\" option must be 0 (unlimited) or positive");
		}
		options.max_array_length = UnsafeNumericCast<idx_t>(value);
	}
}

//! Whether to scan the file at `path`. A file that cannot be read is an error, or skipped with ignore_errors.
bool AcceptFile(ClientContext &context, const UsdScanOptions &options, const string &path) {
	try {
		UsdStageProvider::ValidatePath(context, path);
		return true;
	} catch (const std::exception &ex) {
		ErrorData error(ex);
		if (!options.ignore_errors ||
		    (error.Type() != ExceptionType::IO && error.Type() != ExceptionType::INVALID_INPUT)) {
			throw;
		}
		DUCKDB_LOG_INTERNAL(context, "usd", LogLevel::LOG_WARNING, "Skipping \"" + path + "\": " + error.RawMessage());
		return false;
	}
}

unique_ptr<FunctionData> UsdScanBind(ClientContext &context, TableFunctionBindInput &input,
                                     vector<LogicalType> &return_types, vector<Identifier> &names) {
	auto &info = input.info->Cast<UsdTableFunctionInfo>();
	auto &definition = *info.definition;
	auto result = make_uniq<UsdScanBindData>(info.definition);

	auto &paths = input.inputs[0];
	if (paths.IsNull()) {
		throw BinderException("%s: the file path cannot be NULL", definition.name);
	}
	if (paths.type().id() == LogicalTypeId::LIST) {
		for (auto &child : ListValue::GetChildren(paths)) {
			if (child.IsNull()) {
				throw BinderException("%s: the list of file paths cannot contain NULL", definition.name);
			}
		}
	}
	ParseScanOptions(input.named_parameters, result->options);
	if (definition.bind_options) {
		definition.bind_options(input.named_parameters, result->options);
	}

	auto multi_file_reader = MultiFileReader::Create(input.table_function);
	for (auto &entry : multi_file_reader->ParseFileList(paths)) {
		// rejected before globbing, which would try to load a file system extension for them
		if (FileSystem::IsRemoteFile(entry.path)) {
			throw NotImplementedException("%s: reading remote USD files is not supported: \"%s\"", definition.name,
			                              entry.path);
		}
		auto file_list =
		    multi_file_reader->CreateFileList(context, vector<string> {entry.path}, FileGlobOptions::ALLOW_EMPTY);
		auto files = file_list->GetAllFiles();
		if (files.empty() && !FileSystem::HasGlob(entry.path)) {
			// a plain path that matches no file: report why it cannot be read (missing, a directory, ...)
			AcceptFile(context, result->options, entry.path);
		}
		for (auto &file : files) {
			if (AcceptFile(context, result->options, file.path)) {
				result->files.push_back(file.path);
			}
		}
	}
	if (result->files.empty()) {
		throw IOException("%s: no USD files found that match %s", definition.name, paths.ToString());
	}

	for (auto &column : definition.columns) {
		names.emplace_back(column.name);
		return_types.push_back(column.type);
	}
	if (!result->options.times.empty()) {
		result->time_column = names.size();
		names.emplace_back("time");
		return_types.push_back(LogicalType::DOUBLE);
	}
	if (result->options.filename) {
		result->filename_column = names.size();
		names.emplace_back("filename");
		return_types.push_back(LogicalType::VARCHAR);
	}
	return std::move(result);
}

unique_ptr<GlobalTableFunctionState> UsdScanInitGlobal(ClientContext &context, TableFunctionInitInput &input) {
	auto &bind_data = input.bind_data->Cast<UsdScanBindData>();
	idx_t max_threads = 1;
	if (bind_data.definition->unit == UsdScanUnit::PRIM) {
		max_threads = NumericCast<idx_t>(TaskScheduler::GetScheduler(context).NumberOfThreads());
	} else {
		max_threads = MinValue<idx_t>(bind_data.files.size(),
		                              NumericCast<idx_t>(TaskScheduler::GetScheduler(context).NumberOfThreads()));
	}
	return make_uniq<UsdScanGlobalState>(MaxValue<idx_t>(max_threads, 1));
}

unique_ptr<LocalTableFunctionState> UsdScanInitLocal(ExecutionContext &context, TableFunctionInitInput &input,
                                                     GlobalTableFunctionState *global_state) {
	auto &bind_data = input.bind_data->Cast<UsdScanBindData>();
	auto result = make_uniq<UsdScanLocalState>();
	result->column_ids = input.column_ids;
	vector<column_t> producer_columns;
	for (auto column_id : input.column_ids) {
		if (!IsVirtualColumn(column_id) && column_id < bind_data.definition->columns.size()) {
			producer_columns.push_back(column_id);
		}
	}
	result->producer = bind_data.definition->create_producer(context.client, bind_data, producer_columns);
	return std::move(result);
}

void UsdScanFunction(ClientContext &context, TableFunctionInput &input, DataChunk &output) {
	auto &bind_data = input.bind_data->Cast<UsdScanBindData>();
	auto &global_state = input.global_state->Cast<UsdScanGlobalState>();
	auto &local_state = input.local_state->Cast<UsdScanLocalState>();
	auto &producer = *local_state.producer;
	UsdDiagnosticScope diagnostics(context);

	auto &times = bind_data.options.times;
	while (true) {
		auto &prims = local_state.morsel.prims;
		while (producer.RowCount() - local_state.row_offset < STANDARD_VECTOR_SIZE &&
		       local_state.next_prim < prims.size()) {
			if (times.empty()) {
				producer.AddPrim(prims[local_state.next_prim++]);
				continue;
			}
			// with "times", the morsel's prims are added at each time in turn: the producer's caches (transforms,
			// bounds) then serve the whole morsel at one time
			auto time = times[local_state.time_index];
			if (local_state.next_prim == 0) {
				producer.SetTime(pxr::UsdTimeCode(time));
			}
			producer.AddPrim(prims[local_state.next_prim++]);
			local_state.row_times.resize(producer.RowCount(), time);
			if (local_state.next_prim == prims.size() && local_state.time_index + 1 < times.size()) {
				local_state.time_index++;
				local_state.next_prim = 0;
			}
		}
		auto available = producer.RowCount() - local_state.row_offset;
		if (available > 0) {
			auto count = MinValue<idx_t>(available, STANDARD_VECTOR_SIZE);
			for (idx_t i = 0; i < local_state.column_ids.size(); i++) {
				auto column_id = local_state.column_ids[i];
				auto &result = output.data[i];
				if (bind_data.time_column.IsValid() && column_id == bind_data.time_column.GetIndex()) {
					auto writer = FlatVector::Writer<double>(result, count);
					for (idx_t row = 0; row < count; row++) {
						writer.WriteValue(local_state.row_times[local_state.row_offset + row]);
					}
				} else if (bind_data.filename_column.IsValid() && column_id == bind_data.filename_column.GetIndex()) {
					result.Reference(Value(local_state.morsel.stage->path), count_t(count));
				} else if (IsVirtualColumn(column_id) || column_id >= bind_data.definition->columns.size()) {
					result.Reference(Value(result.GetType()), count_t(count));
				} else {
					producer.Write(column_id, local_state.row_offset, count, result);
				}
			}
			// USD may have been refused a file it reads while the stage is in use (a value clip)
			local_state.morsel.stage->access->ThrowIfRefused();
			output.SetChildCardinality(count);
			local_state.row_offset += count;
			return;
		}

		// the current morsel is exhausted: move to the next one
		producer.Clear();
		local_state.row_offset = 0;
		local_state.next_prim = 0;
		local_state.time_index = 0;
		local_state.row_times.clear();
		local_state.morsel.prims.clear();
		auto previous_file = local_state.morsel.file_index;
		if (!global_state.Next(context, bind_data, local_state.morsel)) {
			output.SetChildCardinality(0);
			return;
		}
		if (bind_data.definition->unit == UsdScanUnit::STAGE || local_state.morsel.file_index != previous_file) {
			producer.BeginStage(*local_state.morsel.stage);
		}
	}
}

OperatorPartitionData UsdScanGetPartitionData(ClientContext &context, TableFunctionGetPartitionInput &input) {
	if (input.partition_info.RequiresPartitionColumns()) {
		throw InternalException("usd scans do not support partition columns");
	}
	auto &local_state = input.local_state->Cast<UsdScanLocalState>();
	return OperatorPartitionData(local_state.morsel.batch_index);
}

double UsdScanProgress(ClientContext &context, const FunctionData *bind_data_p,
                       const GlobalTableFunctionState *global_state) {
	auto &bind_data = bind_data_p->Cast<UsdScanBindData>();
	return global_state->Cast<UsdScanGlobalState>().Progress(bind_data.files.size());
}

//! Whether `expr` is the column `column` of the scan `get`
bool IsScanColumn(const Expression &expr, const LogicalGet &get, idx_t column) {
	if (expr.GetExpressionClass() != ExpressionClass::BOUND_COLUMN_REF) {
		return false;
	}
	auto &binding = expr.Cast<BoundColumnRefExpression>().Binding();
	auto &column_ids = get.GetColumnIds();
	return binding.table_index == get.table_index && binding.column_index < column_ids.size() &&
	       column_ids[binding.column_index].GetPrimaryIndex() == column;
}

//! If `filter` only compares the property name column with constants (name = 'a', name IN ('a', 'b'), or an OR of
//! those), adds the names it accepts to `names` and returns true
bool CollectPropertyNames(const Expression &filter, const LogicalGet &get, idx_t column, vector<string> &names) {
	auto add_constant = [&names](const Expression &expr) {
		if (expr.GetExpressionClass() != ExpressionClass::BOUND_CONSTANT) {
			return false;
		}
		auto &value = expr.Cast<BoundConstantExpression>().GetValue();
		if (value.type().id() != LogicalTypeId::VARCHAR) {
			return false;
		}
		// NULL is never equal to a name: it accepts none
		if (!value.IsNull()) {
			names.push_back(StringValue::Get(value));
		}
		return true;
	};
	switch (filter.GetExpressionType()) {
	case ExpressionType::COMPARE_EQUAL: {
		if (!BoundComparisonExpression::IsComparison(filter)) {
			return false;
		}
		auto &comparison = filter.Cast<BoundFunctionExpression>();
		auto &left = BoundComparisonExpression::Left(comparison);
		auto &right = BoundComparisonExpression::Right(comparison);
		if (IsScanColumn(left, get, column)) {
			return add_constant(right);
		}
		return IsScanColumn(right, get, column) && add_constant(left);
	}
	case ExpressionType::COMPARE_IN: {
		if (filter.GetExpressionClass() != ExpressionClass::BOUND_OPERATOR) {
			return false;
		}
		auto &in = filter.Cast<BoundOperatorExpression>();
		if (in.GetChildren().empty() || !IsScanColumn(*in.GetChildren()[0], get, column)) {
			return false;
		}
		for (idx_t i = 1; i < in.GetChildren().size(); i++) {
			if (!add_constant(*in.GetChildren()[i])) {
				return false;
			}
		}
		return true;
	}
	case ExpressionType::CONJUNCTION_OR: {
		if (filter.GetExpressionClass() != ExpressionClass::BOUND_CONJUNCTION) {
			return false;
		}
		for (auto &child : filter.Cast<BoundConjunctionExpression>().GetChildren()) {
			if (!CollectPropertyNames(*child, get, column, names)) {
				return false;
			}
		}
		return true;
	}
	default:
		return false;
	}
}

//! Pushes the filters on the property name column into the scan, which then reads only the named properties. Besides
//! reading less, this keeps the query's other filters from seeing other properties' values: DuckDB orders a filter's
//! conditions by cost, so `property_name = 'x' AND value::DOUBLE > 4` may otherwise cast another property's value.
//! The filters stay in the plan (they are cheap on the rows that are left).
void UsdScanPushdownComplexFilter(ClientContext &, LogicalGet &get, FunctionData *bind_data_p,
                                  vector<unique_ptr<Expression>> &filters) {
	auto &bind_data = bind_data_p->Cast<UsdScanBindData>();
	auto column = bind_data.definition->property_name_column.GetIndex();
	auto &options = bind_data.options;
	for (auto &filter : filters) {
		vector<string> names;
		if (!CollectPropertyNames(*filter, get, column, names)) {
			continue;
		}
		if (options.filter_property_names) {
			// the filters all apply: keep the names that every one of them accepts
			vector<string> accepted;
			for (auto &name : names) {
				if (std::find(options.property_names.begin(), options.property_names.end(), name) !=
				    options.property_names.end()) {
					accepted.push_back(name);
				}
			}
			names = std::move(accepted);
		}
		std::sort(names.begin(), names.end());
		names.erase(std::unique(names.begin(), names.end()), names.end());
		options.filter_property_names = true;
		options.property_names = std::move(names);
	}
}

InsertionOrderPreservingMap<string> UsdScanToString(TableFunctionToStringInput &input) {
	InsertionOrderPreservingMap<string> result;
	result["Function"] = StringUtil::Upper(input.table_function.GetName().GetIdentifierName());
	if (input.bind_data) {
		auto &options = input.bind_data->Cast<UsdScanBindData>().options;
		if (options.filter_property_names) {
			result["Property Names"] =
			    options.property_names.empty() ? "(none)" : StringUtil::Join(options.property_names, ", ");
		}
	}
	return result;
}

} // namespace

UsdPropertySelection::UsdPropertySelection(const UsdScanOptions &options) : filter(options.filter_property_names) {
	for (auto &name : options.property_names) {
		// a name that is not a property name cannot match (and would make USD report a coding error)
		if (pxr::SdfPath::IsValidNamespacedIdentifier(name)) {
			names.emplace_back(name);
		}
	}
	std::sort(names.begin(), names.end(), pxr::TfDictionaryLessThan());
}

std::vector<pxr::UsdProperty> UsdPropertySelection::Get(const pxr::UsdPrim &prim) const {
	if (!filter) {
		return prim.GetProperties();
	}
	// looked up by name: GetProperties() would compose the names of all of the prim's properties first
	std::vector<pxr::UsdProperty> result;
	for (auto &name : names) {
		if (auto attribute = prim.GetAttribute(name)) {
			result.push_back(attribute);
		} else if (auto relationship = prim.GetRelationship(name)) {
			result.push_back(relationship);
		}
	}
	return result;
}

pxr::UsdTimeCode UsdScanOptions::TimeCode() const {
	return has_time ? pxr::UsdTimeCode(time) : pxr::UsdTimeCode::Default();
}

pxr::Usd_PrimFlagsPredicate UsdScanOptions::Predicate() const {
	// a default constructed conjunction matches every prim
	pxr::Usd_PrimFlagsConjunction flags;
	if (!include_undefined) {
		flags &= pxr::UsdPrimIsDefined;
	}
	if (!include_abstract) {
		flags &= !pxr::UsdPrimIsAbstract;
	}
	if (active_only) {
		flags &= pxr::UsdPrimIsActive;
	}
	pxr::Usd_PrimFlagsPredicate predicate = flags;
	if (instance_proxies) {
		predicate = pxr::UsdTraverseInstanceProxies(predicate);
	}
	return predicate;
}

unique_ptr<FunctionData> UsdScanBindData::Copy() const {
	auto result = make_uniq<UsdScanBindData>(definition);
	result->files = files;
	result->options = options;
	result->time_column = time_column;
	result->filename_column = filename_column;
	return std::move(result);
}

bool UsdScanBindData::Equals(const FunctionData &other_p) const {
	auto &other = other_p.Cast<UsdScanBindData>();
	return definition == other.definition && files == other.files && options == other.options;
}

CreateTableFunctionInfo UsdCreateScanFunction(UsdTableFunctionDefinition definition_p) {
	auto definition = make_shared_ptr<const UsdTableFunctionDefinition>(std::move(definition_p));
	TableFunction function(Identifier(definition->name),
	                       FunctionSignature().AddPositionalOnly("path", LogicalType::VARCHAR), UsdScanFunction,
	                       UsdScanBind, UsdScanInitGlobal, UsdScanInitLocal);
	function.GetSignature().WithTypedKwargs("options", [definition](TypedKwargs &options) {
		options.Add("root", LogicalType::VARCHAR);
		options.Add("instance_proxies", LogicalType::BOOLEAN);
		options.Add("active_only", LogicalType::BOOLEAN);
		options.Add("include_abstract", LogicalType::BOOLEAN);
		options.Add("include_undefined", LogicalType::BOOLEAN);
		options.Add("load_payloads", LogicalType::BOOLEAN);
		options.Add("filename", LogicalType::BOOLEAN);
		options.Add("ignore_errors", LogicalType::BOOLEAN);
		if (definition->time_dependent) {
			options.Add("time", LogicalType::DOUBLE);
			options.Add("times", LogicalType::LIST(LogicalType::DOUBLE));
		}
		if (definition->returns_values) {
			options.Add("max_array_length", LogicalType::BIGINT);
		}
		if (definition->add_options) {
			definition->add_options(options);
		}
	});
	function.projection_pushdown = true;
	function.get_partition_data = UsdScanGetPartitionData;
	function.table_scan_progress = UsdScanProgress;
	function.to_string = UsdScanToString;
	if (definition->property_name_column.IsValid()) {
		function.pushdown_complex_filter = UsdScanPushdownComplexFilter;
	}
	function.function_info = make_shared_ptr<UsdTableFunctionInfo>(definition);

	CreateTableFunctionInfo info(MultiFileReader::CreateFunctionSet(std::move(function)));
	info.on_conflict = OnCreateConflict::ALTER_ON_CONFLICT;
	FunctionDescription description;
	description.description = definition->description;
	description.examples.push_back(definition->example);
	info.descriptions.push_back(std::move(description));
	return info;
}

optional_ptr<const Value> UsdGetOption(const named_argument_map_t &parameters, const char *name) {
	auto entry = parameters.find(name);
	if (entry == parameters.end()) {
		return nullptr;
	}
	if (entry->second.IsNull()) {
		throw BinderException("The \"%s\" option cannot be NULL", name);
	}
	return &entry->second;
}

void UsdGetBooleanOption(const named_argument_map_t &parameters, const char *name, bool &result) {
	if (auto value = UsdGetOption(parameters, name)) {
		result = BooleanValue::Get(*value);
	}
}

} // namespace duckdb
