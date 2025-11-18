#include "usd_properties.hpp"
#include "usd_helpers.hpp"
#include "duckdb/common/exception.hpp"
#include "duckdb/common/string_util.hpp"
#include <pxr/usd/usd/stage.h>
#include <pxr/usd/usd/prim.h>
#include <pxr/usd/usd/attribute.h>
#include <pxr/usd/usd/relationship.h>
#include <pxr/base/vt/value.h>
#include <sstream>
#include <filesystem>

namespace duckdb {

struct UsdPropertiesBindData : public TableFunctionData {
    std::string file_path;

    explicit UsdPropertiesBindData(std::string path) : file_path(std::move(path)) {}
};

struct UsdPropertiesGlobalState : public GlobalTableFunctionState {
    pxr::UsdStageRefPtr stage;
    std::unique_ptr<UsdPrimIterator> prim_iterator;
    pxr::UsdPrim current_prim;
    std::vector<pxr::UsdProperty> current_properties;
    size_t property_index = 0;
    bool has_current_prim = false;

    UsdPropertiesGlobalState() = default;
};

// Helper function to stringify USD attribute values
static std::string StringifyValue(const pxr::VtValue &value) {
    if (value.IsEmpty()) {
        return "";
    }

    std::ostringstream oss;
    oss << value;
    return oss.str();
}

static unique_ptr<FunctionData> UsdPropertiesBind(ClientContext &context, TableFunctionBindInput &input,
                                                    vector<LogicalType> &return_types, vector<string> &names) {
    // Validate input
    if (input.inputs.size() != 1) {
        throw BinderException("usd_properties requires exactly 1 argument (file_path)");
    }

    auto file_path = input.inputs[0].GetValue<string>();

    // Check for empty path
    if (file_path.empty() || file_path.find_first_not_of(" \t\n\r") == std::string::npos) {
        throw BinderException("usd_properties: file_path cannot be empty");
    }

    // Check if file exists
    if (!std::filesystem::exists(file_path)) {
        throw BinderException("usd_properties: USD file not found: " + file_path);
    }

    // Check if it's a directory
    if (std::filesystem::is_directory(file_path)) {
        throw BinderException("usd_properties: path is a directory, not a file: " + file_path);
    }

    // Validate file extension
    if (!UsdStageManager::IsValidUsdFile(file_path)) {
        throw BinderException("usd_properties: file must have a USD extension (.usd, .usda, .usdc, .usdz): " + file_path);
    }

    // Define output schema
    names = {"prim_path", "prop_name", "prop_kind", "usd_type_name", "is_array", "is_time_sampled", "default_value"};
    return_types = {LogicalType::VARCHAR, LogicalType::VARCHAR, LogicalType::VARCHAR,
                    LogicalType::VARCHAR, LogicalType::BOOLEAN, LogicalType::BOOLEAN, LogicalType::VARCHAR};

    return make_uniq<UsdPropertiesBindData>(file_path);
}

static unique_ptr<GlobalTableFunctionState> UsdPropertiesInit(ClientContext &context, TableFunctionInitInput &input) {
    auto &bind_data = input.bind_data->Cast<UsdPropertiesBindData>();
    auto result = make_uniq<UsdPropertiesGlobalState>();

    // Open USD stage
    result->stage = UsdStageManager::OpenStage(bind_data.file_path);

    // Create prim iterator
    result->prim_iterator = make_uniq<UsdPrimIterator>(result->stage);

    // Load properties for the first prim
    if (result->prim_iterator->HasNext()) {
        result->current_prim = result->prim_iterator->GetNext();
        result->current_properties = result->current_prim.GetProperties();
        result->property_index = 0;
        result->has_current_prim = true;
    }

    return std::move(result);
}

static void UsdPropertiesExecute(ClientContext &context, TableFunctionInput &data_p, DataChunk &output) {
    auto &state = data_p.global_state->Cast<UsdPropertiesGlobalState>();
    idx_t output_idx = 0;

    while (output_idx < STANDARD_VECTOR_SIZE) {
        // Check if we need to move to the next prim
        while (state.property_index >= state.current_properties.size()) {
            if (!state.prim_iterator->HasNext()) {
                // No more prims, we're done
                output.SetCardinality(output_idx);
                return;
            }

            // Move to next prim
            state.current_prim = state.prim_iterator->GetNext();
            state.current_properties = state.current_prim.GetProperties();
            state.property_index = 0;
            state.has_current_prim = true;
        }

        // Get current property
        auto prop = state.current_properties[state.property_index];

        // Extract property information
        std::string prim_path = state.current_prim.GetPath().GetString();
        std::string prop_name = prop.GetName().GetString();
        std::string prop_kind;
        std::string usd_type_name;
        bool is_array = false;
        bool is_time_sampled = false;
        std::string default_value;

        // Check if it's an attribute or relationship
        if (prop.Is<pxr::UsdAttribute>()) {
            auto attr = prop.As<pxr::UsdAttribute>();
            prop_kind = "attribute";

            auto type_name = attr.GetTypeName();
            usd_type_name = type_name.GetAsToken().GetString();
            is_array = type_name.IsArray();
            is_time_sampled = attr.ValueMightBeTimeVarying();

            // Get default value
            pxr::VtValue value;
            if (attr.Get(&value)) {
                default_value = StringifyValue(value);
            }
        } else if (prop.Is<pxr::UsdRelationship>()) {
            prop_kind = "relationship";
            usd_type_name = "relationship";
            is_array = false;
            is_time_sampled = false;
            default_value = "";
        }

        // Write to output
        FlatVector::GetData<string_t>(output.data[0])[output_idx] = StringVector::AddString(output.data[0], prim_path);
        FlatVector::GetData<string_t>(output.data[1])[output_idx] = StringVector::AddString(output.data[1], prop_name);
        FlatVector::GetData<string_t>(output.data[2])[output_idx] = StringVector::AddString(output.data[2], prop_kind);
        FlatVector::GetData<string_t>(output.data[3])[output_idx] = StringVector::AddString(output.data[3], usd_type_name);


        FlatVector::GetData<bool>(output.data[4])[output_idx] = is_array;
        FlatVector::GetData<bool>(output.data[5])[output_idx] = is_time_sampled;
        FlatVector::GetData<string_t>(output.data[6])[output_idx] = StringVector::AddString(output.data[6], default_value);

        output_idx++;
        state.property_index++;
    }

    output.SetCardinality(output_idx);
}

TableFunction UsdPropertiesFunction::GetFunction() {
    TableFunction func("usd_properties", {LogicalType::VARCHAR}, UsdPropertiesExecute, UsdPropertiesBind, UsdPropertiesInit);
    return func;
}

} // namespace duckdb
