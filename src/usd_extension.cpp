#include "usd_extension.hpp"

#include "usd_functions.hpp"
#include "usd_plugins.hpp"
#include "usd_stage.hpp"

#include "duckdb/common/file_system.hpp"
#include "duckdb/function/replacement_scan.hpp"
#include "duckdb/main/config.hpp"
#include "duckdb/main/extension/extension_loader.hpp"
#include "duckdb/parser/expression/constant_expression.hpp"
#include "duckdb/parser/expression/function_expression.hpp"
#include "duckdb/parser/tableref/table_function_ref.hpp"

namespace duckdb {

namespace {

//! Lets `FROM 'scene.usda'` read the prims of a USD file
unique_ptr<TableRef> UsdReplacementScan(ClientContext &context, ReplacementScanInput &input,
                                        optional_ptr<ReplacementScanData> data) {
	auto table_name = ReplacementScan::GetFullPath(input);
	if (!ReplacementScan::CanReplace(table_name, {"usd", "usda", "usdc", "usdz"})) {
		return nullptr;
	}
	auto table_function = make_uniq<TableFunctionRef>();
	vector<unique_ptr<ParsedExpression>> children;
	children.push_back(ConstantExpression::String(table_name));
	table_function->function = make_uniq<FunctionExpression>("usd_prims", std::move(children));
	if (!FileSystem::HasGlob(table_name)) {
		auto &fs = FileSystem::GetFileSystem(context);
		table_function->alias = Identifier(fs.ExtractBaseName(table_name));
	}
	return std::move(table_function);
}

//! Turning the stage cache off also frees the stages it holds
void SetStageCache(ClientContext &context, SetScope scope, Value &parameter) {
	if (parameter.IsNull()) {
		throw InvalidInputException("The \"%s\" setting cannot be NULL", USD_STAGE_CACHE_SETTING);
	}
	if (!BooleanValue::Get(parameter)) {
		UsdStageProvider::DropCachedStages(context);
	}
}

void LoadInternal(ExtensionLoader &loader) {
	// before anything else touches OpenUSD
	UsdRegisterPlugins();
	UsdInitialize();

	loader.SetDescription("Query Universal Scene Description (USD) files with SQL");
	loader.RegisterFunction(UsdPrimsFunction());
	loader.RegisterFunction(UsdPropertiesFunction());
	loader.RegisterFunction(UsdRelationshipsFunction());
	loader.RegisterFunction(UsdXformsFunction());
	loader.RegisterFunction(UsdBoundsFunction());
	loader.RegisterFunction(UsdVariantsFunction());
	loader.RegisterFunction(UsdLayersFunction());
	loader.RegisterFunction(UsdStageMetadataFunction());
	loader.RegisterFunction(UsdCompositionErrorsFunction());
	loader.RegisterFunction(UsdCompositionArcsFunction());
	loader.RegisterFunction(UsdMaterialBindingsFunction());
	loader.RegisterFunction(UsdTimeSamplesFunction());
	loader.RegisterFunction(UsdVersionFunction());

	auto &config = DBConfig::GetConfig(loader.GetDatabaseInstance());
	config.replacement_scans.emplace_back(UsdReplacementScan);
	config.AddExtensionOption(USD_STAGE_CACHE_SETTING,
	                          "Keep opened USD stages in memory between queries, until a file they read changes. "
	                          "Cached stages count toward memory_limit and are evicted when DuckDB needs the memory; "
	                          "turning this off frees them.",
	                          LogicalType::BOOLEAN, Value::BOOLEAN(true), SetStageCache);
}

} // namespace

void UsdExtension::Load(ExtensionLoader &loader) {
	LoadInternal(loader);
}

std::string UsdExtension::Name() {
	return "usd";
}

std::string UsdExtension::Version() const {
#ifdef EXT_VERSION_USD
	return EXT_VERSION_USD;
#else
	return "";
#endif
}

} // namespace duckdb

extern "C" {

DUCKDB_CPP_EXTENSION_ENTRY(usd, loader) {
	duckdb::LoadInternal(loader);
}
}
