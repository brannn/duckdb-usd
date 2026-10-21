//===----------------------------------------------------------------------===//
//                         DuckDB USD Extension
//
// usd_functions.hpp
//
// The SQL functions provided by the extension.
//===----------------------------------------------------------------------===//

#pragma once

#include "duckdb.hpp"
#include "duckdb/parser/parsed_data/create_scalar_function_info.hpp"
#include "duckdb/parser/parsed_data/create_table_function_info.hpp"

namespace duckdb {

CreateTableFunctionInfo UsdPrimsFunction();
CreateTableFunctionInfo UsdPropertiesFunction();
CreateTableFunctionInfo UsdRelationshipsFunction();
CreateTableFunctionInfo UsdXformsFunction();
CreateTableFunctionInfo UsdBoundsFunction();
CreateTableFunctionInfo UsdVariantsFunction();
CreateTableFunctionInfo UsdLayersFunction();
CreateTableFunctionInfo UsdStageMetadataFunction();
CreateTableFunctionInfo UsdCompositionErrorsFunction();
CreateTableFunctionInfo UsdCompositionArcsFunction();
CreateTableFunctionInfo UsdMaterialBindingsFunction();
CreateTableFunctionInfo UsdTimeSamplesFunction();

CreateScalarFunctionInfo UsdVersionFunction();

} // namespace duckdb
