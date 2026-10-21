#include "usd_functions.hpp"

#include <pxr/pxr.h>

namespace duckdb {

namespace {

void UsdVersion(DataChunk &args, ExpressionState &state, Vector &result) {
	auto version = StringUtil::Format("%d.%02d", PXR_MINOR_VERSION, PXR_PATCH_VERSION);
	result.Reference(Value(version), count_t(args.size()));
}

} // namespace

CreateScalarFunctionInfo UsdVersionFunction() {
	CreateScalarFunctionInfo info(ScalarFunction("usd_version", {}, LogicalType::VARCHAR, UsdVersion));
	info.on_conflict = OnCreateConflict::ALTER_ON_CONFLICT;
	FunctionDescription description;
	description.description = "The version of OpenUSD the extension was built with.";
	description.examples.push_back("usd_version()");
	info.descriptions.push_back(std::move(description));
	return info;
}

} // namespace duckdb
