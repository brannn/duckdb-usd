#include "usd_prims.hpp"
#include "usd_helpers.hpp"
#include "duckdb/common/exception.hpp"
#include "duckdb/common/string_util.hpp"
#include <pxr/usd/usd/stage.h>
#include <pxr/usd/usd/prim.h>
#include <pxr/usd/usd/modelAPI.h>
#include <pxr/base/tf/token.h>

namespace duckdb {

struct UsdPrimsBindData : public TableFunctionData {
    std::string file_path;

    explicit UsdPrimsBindData(std::string path) : file_path(std::move(path)) {}
};

struct UsdPrimsGlobalState : public GlobalTableFunctionState {
    pxr::UsdStageRefPtr stage;
    std::unique_ptr<UsdPrimIterator> iterator;

    UsdPrimsGlobalState() = default;
};

static unique_ptr<FunctionData> UsdPrimsBind(ClientContext &context, TableFunctionBindInput &input,
                                              vector<LogicalType> &return_types, vector<string> &names) {
    // Expect one parameter: file path
    if (input.inputs.size() != 1) {
        throw BinderException("usd_prims requires exactly one argument: file_path");
    }
    
    if (input.inputs[0].type() != LogicalType::VARCHAR) {
        throw BinderException("usd_prims file_path must be a string");
    }
    
    auto file_path = input.inputs[0].ToString();
    
    // Validate USD file
    if (!UsdStageManager::IsValidUsdFile(file_path)) {
        throw BinderException("Invalid USD file: " + file_path);
    }
    
    // Define output schema - all columns from Phase 2
    names.emplace_back("prim_path");
    return_types.emplace_back(LogicalType::VARCHAR);

    names.emplace_back("parent_path");
    return_types.emplace_back(LogicalType::VARCHAR);

    names.emplace_back("name");
    return_types.emplace_back(LogicalType::VARCHAR);

    names.emplace_back("prim_type");
    return_types.emplace_back(LogicalType::VARCHAR);

    names.emplace_back("kind");
    return_types.emplace_back(LogicalType::VARCHAR);

    names.emplace_back("active");
    return_types.emplace_back(LogicalType::BOOLEAN);

    names.emplace_back("instanceable");
    return_types.emplace_back(LogicalType::BOOLEAN);

    return make_uniq<UsdPrimsBindData>(file_path);
}

static unique_ptr<GlobalTableFunctionState> UsdPrimsInit(ClientContext &context, TableFunctionInitInput &input) {
    auto &bind_data = input.bind_data->Cast<UsdPrimsBindData>();
    auto result = make_uniq<UsdPrimsGlobalState>();

    // Open USD stage
    result->stage = UsdStageManager::OpenStage(bind_data.file_path);

    // Create prim iterator
    result->iterator = make_uniq<UsdPrimIterator>(result->stage);

    return std::move(result);
}

static void UsdPrimsExecute(ClientContext &context, TableFunctionInput &data_p, DataChunk &output) {
    auto &state = data_p.global_state->Cast<UsdPrimsGlobalState>();

    idx_t count = 0;
    auto prim_path_vector = FlatVector::GetData<string_t>(output.data[0]);
    auto parent_path_vector = FlatVector::GetData<string_t>(output.data[1]);
    auto name_vector = FlatVector::GetData<string_t>(output.data[2]);
    auto prim_type_vector = FlatVector::GetData<string_t>(output.data[3]);
    auto kind_vector = FlatVector::GetData<string_t>(output.data[4]);
    auto active_vector = FlatVector::GetData<bool>(output.data[5]);
    auto instanceable_vector = FlatVector::GetData<bool>(output.data[6]);

    // Stream prims in batches
    while (state.iterator->HasNext() && count < STANDARD_VECTOR_SIZE) {
        auto prim = state.iterator->GetNext();

        // Get prim path
        std::string path = prim.GetPath().GetString();
        prim_path_vector[count] = StringVector::AddString(output.data[0], path);

        // Get parent path
        auto parent = prim.GetParent();
        std::string parent_path = parent ? parent.GetPath().GetString() : "";
        parent_path_vector[count] = StringVector::AddString(output.data[1], parent_path);

        // Get prim name
        std::string name = prim.GetName().GetString();
        name_vector[count] = StringVector::AddString(output.data[2], name);

        // Get prim type
        std::string type_name = prim.GetTypeName().GetString();
        if (type_name.empty()) {
            type_name = "<undefined>";
        }
        prim_type_vector[count] = StringVector::AddString(output.data[3], type_name);

        // Get kind metadata
        pxr::TfToken kind_token;
        std::string kind_str;
        if (pxr::UsdModelAPI(prim).GetKind(&kind_token)) {
            kind_str = kind_token.GetString();
        }
        kind_vector[count] = StringVector::AddString(output.data[4], kind_str);

        // Get active status
        active_vector[count] = prim.IsActive();

        // Get instanceable status
        instanceable_vector[count] = prim.IsInstanceable();

        count++;
    }

    output.SetCardinality(count);
}

TableFunction UsdPrimsFunction::GetFunction() {
    TableFunction func("usd_prims", {LogicalType::VARCHAR}, UsdPrimsExecute, UsdPrimsBind, UsdPrimsInit);
    return func;
}

} // namespace duckdb

