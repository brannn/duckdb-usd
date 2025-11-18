#include "usd_xforms.hpp"
#include "usd_helpers.hpp"

#include <pxr/usd/usd/prim.h>
#include <pxr/usd/usd/primRange.h>
#include <pxr/usd/usdGeom/xformable.h>
#include <pxr/usd/usdGeom/xformCache.h>
#include <pxr/base/gf/matrix4d.h>
#include <pxr/base/gf/vec3d.h>

namespace duckdb {

// Bind data structure
struct UsdXformsBindData : public TableFunctionData {
    std::string file_path;
    explicit UsdXformsBindData(std::string path) : file_path(std::move(path)) {}
};

// Global state for iteration
struct UsdXformsGlobalState : public GlobalTableFunctionState {
    pxr::UsdStageRefPtr stage;
    std::unique_ptr<UsdPrimIterator> prim_iterator;
    std::unique_ptr<pxr::UsdGeomXformCache> xform_cache;
    bool finished = false;

    UsdXformsGlobalState() = default;
};

// Bind function
static unique_ptr<FunctionData> UsdXformsBind(ClientContext &context, TableFunctionBindInput &input,
                                              vector<LogicalType> &return_types, vector<string> &names) {
    // Validate input
    if (input.inputs.size() != 1) {
        throw BinderException("usd_xforms requires exactly one argument: file_path");
    }

    auto file_path = input.inputs[0].GetValue<string>();

    // Validate file extension
    if (!UsdStageManager::IsValidUsdFile(file_path)) {
        throw InvalidInputException("File must have a USD extension (.usd, .usda, .usdc, .usdz)");
    }

    // Define output schema
    return_types = {
        LogicalType::VARCHAR,  // prim_path
        LogicalType::DOUBLE,   // x
        LogicalType::DOUBLE,   // y
        LogicalType::DOUBLE,   // z
        LogicalType::BOOLEAN,  // has_rotation
        LogicalType::BOOLEAN   // has_scale
    };

    names = {"prim_path", "x", "y", "z", "has_rotation", "has_scale"};

    return make_uniq<UsdXformsBindData>(file_path);
}

// Init function
static unique_ptr<GlobalTableFunctionState> UsdXformsInit(ClientContext &context, TableFunctionInitInput &input) {
    auto &bind_data = input.bind_data->Cast<UsdXformsBindData>();
    auto state = make_uniq<UsdXformsGlobalState>();

    // Open USD stage
    state->stage = UsdStageManager::OpenStage(bind_data.file_path);

    // Create prim iterator
    state->prim_iterator = std::make_unique<UsdPrimIterator>(state->stage);

    // Create XformCache for efficient transform computation at default time
    state->xform_cache = std::make_unique<pxr::UsdGeomXformCache>(pxr::UsdTimeCode::Default());

    return std::move(state);
}

// Execute function
static void UsdXformsExecute(ClientContext &context, TableFunctionInput &data_p, DataChunk &output) {
    auto &state = data_p.global_state->Cast<UsdXformsGlobalState>();

    if (state.finished) {
        output.SetCardinality(0);
        return;
    }

    idx_t count = 0;

    auto prim_path_data = FlatVector::GetData<string_t>(output.data[0]);
    auto x_data = FlatVector::GetData<double>(output.data[1]);
    auto y_data = FlatVector::GetData<double>(output.data[2]);
    auto z_data = FlatVector::GetData<double>(output.data[3]);
    auto has_rotation_data = FlatVector::GetData<bool>(output.data[4]);
    auto has_scale_data = FlatVector::GetData<bool>(output.data[5]);

    while (count < STANDARD_VECTOR_SIZE && state.prim_iterator->HasNext()) {
        auto prim = state.prim_iterator->GetNext();

        // Check if prim is Xformable
        if (!prim.IsA<pxr::UsdGeomXformable>()) {
            continue;
        }

        // Get world-space transform
        pxr::GfMatrix4d world_transform = state.xform_cache->GetLocalToWorldTransform(prim);

        // Extract translation
        pxr::GfVec3d translation = world_transform.ExtractTranslation();

        // Decompose matrix to detect rotation and scale
        // Factor() decomposes into: M = r * s * -r * u * t
        // where r is rotation, s is scale, u may contain shear, t is translation
        pxr::GfMatrix4d r, u, p;
        pxr::GfVec3d s, t;
        bool has_rotation = false;
        bool has_scale = false;

        if (world_transform.Factor(&r, &s, &u, &t, &p)) {
            // Check if there's non-identity rotation
            // Compare rotation matrix rows with identity matrix
            pxr::GfMatrix4d identity;
            identity.SetIdentity();
            auto r0 = r.GetRow3(0);
            auto r1 = r.GetRow3(1);
            auto r2 = r.GetRow3(2);
            auto i0 = identity.GetRow3(0);
            auto i1 = identity.GetRow3(1);
            auto i2 = identity.GetRow3(2);

            has_rotation = !pxr::GfIsClose(r0[0], i0[0], 1e-6) || !pxr::GfIsClose(r0[1], i0[1], 1e-6) || !pxr::GfIsClose(r0[2], i0[2], 1e-6) ||
                          !pxr::GfIsClose(r1[0], i1[0], 1e-6) || !pxr::GfIsClose(r1[1], i1[1], 1e-6) || !pxr::GfIsClose(r1[2], i1[2], 1e-6) ||
                          !pxr::GfIsClose(r2[0], i2[0], 1e-6) || !pxr::GfIsClose(r2[1], i2[1], 1e-6) || !pxr::GfIsClose(r2[2], i2[2], 1e-6);

            // Check if there's non-uniform or non-identity scale
            // Scale factors of (1, 1, 1) mean no scale
            has_scale = !pxr::GfIsClose(s[0], 1.0, 1e-6) ||
                       !pxr::GfIsClose(s[1], 1.0, 1e-6) ||
                       !pxr::GfIsClose(s[2], 1.0, 1e-6);
        }

        // Emit row
        prim_path_data[count] = StringVector::AddString(output.data[0], prim.GetPath().GetString());
        x_data[count] = translation[0];
        y_data[count] = translation[1];
        z_data[count] = translation[2];
        has_rotation_data[count] = has_rotation;
        has_scale_data[count] = has_scale;

        count++;
    }

    if (!state.prim_iterator->HasNext()) {
        state.finished = true;
    }

    output.SetCardinality(count);
}

// Get the table function
TableFunction UsdXformsFunction::GetFunction() {
    TableFunction func("usd_xforms", {LogicalType::VARCHAR}, UsdXformsExecute, UsdXformsBind, UsdXformsInit);
    return func;
}

} // namespace duckdb

