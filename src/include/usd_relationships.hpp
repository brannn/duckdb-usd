#pragma once

#include "duckdb.hpp"

namespace duckdb {

class UsdRelationshipsFunction {
public:
    static TableFunction GetFunction();
};

} // namespace duckdb

