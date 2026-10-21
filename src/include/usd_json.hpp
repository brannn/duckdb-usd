//===----------------------------------------------------------------------===//
//                         DuckDB USD Extension
//
// usd_json.hpp
//
// Serialization of USD values (VtValue) to JSON, so that they can be cast to
// DuckDB's VARIANT type in a single vectorized step.
//===----------------------------------------------------------------------===//

#pragma once

#include "duckdb.hpp"

#include <pxr/base/vt/value.h>

namespace duckdb {

struct UsdJson {
	//! Appends the JSON representation of `value` to `out`. Non-finite floats become null, and types without a JSON
	//! mapping are written as their USD string representation.
	static void Append(const pxr::VtValue &value, string &out);
	//! The number of elements of an array-valued value, or DConstants::INVALID_INDEX for scalars and empty values
	static idx_t ArrayLength(const pxr::VtValue &value);
};

} // namespace duckdb
