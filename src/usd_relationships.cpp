#include "usd_relationships.hpp"
#include "usd_helpers.hpp"

#include <pxr/usd/usd/prim.h>
#include <pxr/usd/usd/primRange.h>
#include <pxr/usd/usd/relationship.h>
#include <pxr/usd/sdf/path.h>

namespace duckdb {

// Bind data structure
struct UsdRelationshipsBindData : public TableFunctionData {
    std::string file_path;
    explicit UsdRelationshipsBindData(std::string path) : file_path(std::move(path)) {}
};

// Global state for iteration
struct UsdRelationshipsGlobalState : public GlobalTableFunctionState {
    pxr::UsdStageRefPtr stage;
    std::unique_ptr<UsdPrimIterator> prim_iterator;
    pxr::UsdPrim current_prim;
    std::vector<pxr::UsdRelationship> current_relationships;
    size_t relationship_index = 0;
    pxr::SdfPathVector current_targets;
    size_t target_index = 0;
    bool has_current_prim = false;

    UsdRelationshipsGlobalState() = default;
};

// Bind function
static unique_ptr<FunctionData> UsdRelationshipsBind(ClientContext &context, TableFunctionBindInput &input,
                                                      vector<LogicalType> &return_types, vector<string> &names) {
    // Validate input
    if (input.inputs.size() != 1) {
        throw BinderException("usd_relationships requires exactly one argument: file_path");
    }

    auto file_path = input.inputs[0].GetValue<string>();

    // Validate file extension
    if (!UsdStageManager::IsValidUsdFile(file_path)) {
        throw InvalidInputException("File must have a USD extension (.usd, .usda, .usdc, .usdz)");
    }

    // Define output schema
    return_types = {
        LogicalType::VARCHAR,  // prim_path
        LogicalType::VARCHAR,  // rel_name
        LogicalType::VARCHAR,  // target_path
        LogicalType::INTEGER   // target_index
    };

    names = {"prim_path", "rel_name", "target_path", "target_index"};

    return make_uniq<UsdRelationshipsBindData>(file_path);
}

// Init function
static unique_ptr<GlobalTableFunctionState> UsdRelationshipsInit(ClientContext &context, TableFunctionInitInput &input) {
    auto &bind_data = input.bind_data->Cast<UsdRelationshipsBindData>();
    auto state = make_uniq<UsdRelationshipsGlobalState>();

    // Open USD stage
    state->stage = UsdStageManager::OpenStage(bind_data.file_path);

    // Create prim iterator
    state->prim_iterator = std::make_unique<UsdPrimIterator>(state->stage);

    // Load first prim's relationships
    if (state->prim_iterator->HasNext()) {
        state->current_prim = state->prim_iterator->GetNext();
        state->current_relationships = state->current_prim.GetRelationships();
        state->relationship_index = 0;
        state->target_index = 0;
        state->has_current_prim = true;

        // Load first relationship's targets if available
        if (!state->current_relationships.empty()) {
            state->current_relationships[0].GetTargets(&state->current_targets);
        }
    }

    return std::move(state);
}

// Execute function
static void UsdRelationshipsExecute(ClientContext &context, TableFunctionInput &data_p, DataChunk &output) {
    auto &state = data_p.global_state->Cast<UsdRelationshipsGlobalState>();
    idx_t count = 0;

    auto prim_path_data = FlatVector::GetData<string_t>(output.data[0]);
    auto rel_name_data = FlatVector::GetData<string_t>(output.data[1]);
    auto target_path_data = FlatVector::GetData<string_t>(output.data[2]);
    auto target_index_data = FlatVector::GetData<int32_t>(output.data[3]);

    while (count < STANDARD_VECTOR_SIZE && state.has_current_prim) {
        // Check if we have targets to emit
        if (state.relationship_index < state.current_relationships.size() &&
            state.target_index < state.current_targets.size()) {

            auto &rel = state.current_relationships[state.relationship_index];
            auto &target = state.current_targets[state.target_index];

            // Convert target to absolute path if it's relative
            pxr::SdfPath absolute_target = target;
            if (target.IsAbsolutePath()) {
                absolute_target = target;
            } else {
                absolute_target = target.MakeAbsolutePath(state.current_prim.GetPath());
            }

            // Emit row
            prim_path_data[count] = StringVector::AddString(output.data[0], state.current_prim.GetPath().GetString());
            rel_name_data[count] = StringVector::AddString(output.data[1], rel.GetName().GetString());
            target_path_data[count] = StringVector::AddString(output.data[2], absolute_target.GetString());
            target_index_data[count] = static_cast<int32_t>(state.target_index);

            count++;
            state.target_index++;
        } else if (state.relationship_index < state.current_relationships.size()) {
            // Move to next relationship
            state.relationship_index++;
            state.target_index = 0;

            if (state.relationship_index < state.current_relationships.size()) {
                // Load next relationship's targets
                state.current_targets.clear();
                state.current_relationships[state.relationship_index].GetTargets(&state.current_targets);
            }
        } else {
            // Move to next prim
            if (state.prim_iterator->HasNext()) {
                state.current_prim = state.prim_iterator->GetNext();
                state.current_relationships = state.current_prim.GetRelationships();
                state.relationship_index = 0;
                state.target_index = 0;

                // Load first relationship's targets if available
                if (!state.current_relationships.empty()) {
                    state.current_targets.clear();
                    state.current_relationships[0].GetTargets(&state.current_targets);
                } else {
                    state.current_targets.clear();
                }
            } else {
                // No more prims
                state.has_current_prim = false;
            }
        }
    }

    output.SetCardinality(count);
}

// Get the table function
TableFunction UsdRelationshipsFunction::GetFunction() {
    TableFunction func("usd_relationships", {LogicalType::VARCHAR}, UsdRelationshipsExecute, UsdRelationshipsBind, UsdRelationshipsInit);
    return func;
}

} // namespace duckdb

