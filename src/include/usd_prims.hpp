#pragma once

#include "duckdb.hpp"
#include "duckdb/function/table_function.hpp"

namespace duckdb {

class UsdPrimsFunction {
public:
    static TableFunction GetFunction();
};

} // namespace duckdb

