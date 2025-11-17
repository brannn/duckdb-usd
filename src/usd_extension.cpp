#include "usd_extension.hpp"
#include "duckdb.hpp"
#include "duckdb/common/exception.hpp"
#include "duckdb/function/table_function.hpp"

namespace duckdb {

// Simple test table function for Phase 0 validation
struct UsdTestFunctionData : public TableFunctionData {
};

struct UsdTestGlobalState : public GlobalTableFunctionState {
    UsdTestGlobalState() : finished(false) {
    }
    bool finished;
};

static unique_ptr<FunctionData> UsdTestBind(ClientContext &context, TableFunctionBindInput &input,
                                             vector<LogicalType> &return_types, vector<string> &names) {
    names.emplace_back("message");
    return_types.emplace_back(LogicalType::VARCHAR);

    names.emplace_back("version");
    return_types.emplace_back(LogicalType::VARCHAR);

    return make_uniq<UsdTestFunctionData>();
}

static unique_ptr<GlobalTableFunctionState> UsdTestInit(ClientContext &context, TableFunctionInitInput &input) {
    return make_uniq<UsdTestGlobalState>();
}

static void UsdTestFunction(ClientContext &context, TableFunctionInput &data_p, DataChunk &output) {
    auto &state = data_p.global_state->Cast<UsdTestGlobalState>();

    if (state.finished) {
        // Already returned data
        return;
    }

    idx_t row_count = 1;
    output.SetCardinality(row_count);

    auto message_vector = FlatVector::GetData<string_t>(output.data[0]);
    auto version_vector = FlatVector::GetData<string_t>(output.data[1]);

    message_vector[0] = StringVector::AddString(output.data[0], "DuckDB USD Extension - Phase 0");
    version_vector[0] = StringVector::AddString(output.data[1], "0.1.0");

    state.finished = true;
}

static void LoadInternal(ExtensionLoader &loader) {
    // Register usd_test() table function
    TableFunction usd_test_func("usd_test", {}, UsdTestFunction, UsdTestBind, UsdTestInit);
    loader.RegisterFunction(usd_test_func);
}

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
    return "0.1.0";
#endif
}

} // namespace duckdb

extern "C" {

DUCKDB_CPP_EXTENSION_ENTRY(usd, loader) {
    duckdb::LoadInternal(loader);
}

}


