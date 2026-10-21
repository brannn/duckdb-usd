#include "usd_functions.hpp"
#include "usd_scan.hpp"

#include <pxr/base/gf/matrix4d.h>
#include <pxr/base/gf/rotation.h>
#include <pxr/base/gf/transform.h>
#include <pxr/usd/usdGeom/xformable.h>
#include <pxr/usd/usdGeom/xformCache.h>

#include <algorithm>

namespace duckdb {

namespace {

enum UsdXformsColumn : column_t {
	PRIM_PATH,
	X,
	Y,
	Z,
	ROTATION,
	SCALE,
	HAS_ROTATION,
	HAS_SCALE,
	WORLD_MATRIX,
	LOCAL_MATRIX,
	RESETS_XFORM_STACK,
	XFORM_OP_ORDER,
	TIME_VARYING
};

constexpr double USD_XFORM_EPSILON = 1e-6;

//! Whether a component of `vector` differs from `value` by more than USD_XFORM_EPSILON
bool Differs(const pxr::GfVec3d &vector, double value) {
	for (idx_t i = 0; i < 3; i++) {
		if (std::abs(vector[i] - value) > USD_XFORM_EPSILON) {
			return true;
		}
	}
	return false;
}

struct XformRow {
	pxr::UsdPrim prim;
	pxr::UsdTimeCode time;
	pxr::GfMatrix4d world;

	//! The rotation of the world transform, as XYZ Euler angles in degrees
	const pxr::GfVec3d &Rotation() const {
		Decompose();
		return rotation;
	}

	//! The scale of the world transform
	const pxr::GfVec3d &Scale() const {
		Decompose();
		return scale;
	}

private:
	//! Decomposes the world transform into rotation and scale, on first use
	void Decompose() const {
		if (decomposed) {
			return;
		}
		pxr::GfTransform transform(world);
		// decomposing about Z, Y, X yields the angles of an XYZ rotation order, in reverse
		auto angles =
		    transform.GetRotation().Decompose(pxr::GfVec3d::ZAxis(), pxr::GfVec3d::YAxis(), pxr::GfVec3d::XAxis());
		rotation = pxr::GfVec3d(Snap(angles[2]), Snap(angles[1]), Snap(angles[0]));
		auto factors = transform.GetScale();
		scale = pxr::GfVec3d(Snap(factors[0]), Snap(factors[1]), Snap(factors[2]));
		decomposed = true;
	}

	//! Rounds away the floating point noise of the decomposition (e.g. 29.999999999999986) and turns -0 into 0
	static double Snap(double value) {
		auto rounded = std::round(value * 1e9) / 1e9;
		return rounded == 0 ? 0.0 : rounded;
	}

	mutable bool decomposed = false;
	mutable pxr::GfVec3d rotation;
	mutable pxr::GfVec3d scale;
};

class UsdXformsProducer : public UsdBufferedProducer<XformRow> {
public:
	explicit UsdXformsProducer(pxr::UsdTimeCode time) : time(time), cache(time) {
	}

	void BeginStage(const UsdStageHandle &) override {
		cache.Clear();
	}

	void SetTime(pxr::UsdTimeCode time_p) override {
		time = time_p;
		cache.SetTime(time);
	}

	void AddPrim(const pxr::UsdPrim &prim) override {
		if (!prim.IsA<pxr::UsdGeomXformable>()) {
			return;
		}
		XformRow row;
		row.prim = prim;
		row.time = time;
		row.world = cache.GetLocalToWorldTransform(prim);
		rows.push_back(std::move(row));
	}

	void Write(column_t column, idx_t offset, idx_t count, Vector &result) override {
		switch (column) {
		case PRIM_PATH:
			WritePrimPaths(offset, count, result);
			break;
		case X:
		case Y:
		case Z: {
			auto axis = column - X;
			UsdWriteColumn<double>(rows, offset, count, result,
			                       [axis](const XformRow &row) { return row.world.ExtractTranslation()[axis]; });
			break;
		}
		case ROTATION:
			UsdWriteDoubleArrays(rows, offset, count, 3, result, [](const XformRow &row, double *out) {
				std::copy_n(row.Rotation().data(), 3, out);
				return true;
			});
			break;
		case SCALE:
			UsdWriteDoubleArrays(rows, offset, count, 3, result, [](const XformRow &row, double *out) {
				std::copy_n(row.Scale().data(), 3, out);
				return true;
			});
			break;
		case HAS_ROTATION:
			UsdWriteColumn<bool>(rows, offset, count, result,
			                     [](const XformRow &row) { return Differs(row.Rotation(), 0); });
			break;
		case HAS_SCALE:
			UsdWriteColumn<bool>(rows, offset, count, result,
			                     [](const XformRow &row) { return Differs(row.Scale(), 1); });
			break;
		case WORLD_MATRIX:
			UsdWriteDoubleArrays(rows, offset, count, 16, result, [](const XformRow &row, double *out) {
				std::copy_n(row.world.GetArray(), 16, out);
				return true;
			});
			break;
		case LOCAL_MATRIX:
			UsdWriteDoubleArrays(rows, offset, count, 16, result, [](const XformRow &row, double *out) {
				pxr::GfMatrix4d local;
				bool resets;
				if (!pxr::UsdGeomXformable(row.prim).GetLocalTransformation(&local, &resets, row.time)) {
					return false;
				}
				std::copy_n(local.GetArray(), 16, out);
				return true;
			});
			break;
		case RESETS_XFORM_STACK:
			UsdWriteColumn<bool>(rows, offset, count, result, [](const XformRow &row) {
				return pxr::UsdGeomXformable(row.prim).GetResetXformStack();
			});
			break;
		case XFORM_OP_ORDER:
			UsdWriteStringLists(rows, offset, count, result, [](const XformRow &row, std::vector<std::string> &out) {
				pxr::VtTokenArray order;
				pxr::UsdGeomXformable(row.prim).GetXformOpOrderAttr().Get(&order);
				for (auto &op : order) {
					out.push_back(op.GetString());
				}
				return true;
			});
			break;
		case TIME_VARYING:
			UsdWriteColumn<bool>(rows, offset, count, result, [](const XformRow &row) {
				return pxr::UsdGeomXformable(row.prim).TransformMightBeTimeVarying();
			});
			break;
		default:
			throw InternalException("usd_xforms: unknown column %d", column);
		}
	}

private:
	pxr::UsdTimeCode time;
	pxr::UsdGeomXformCache cache;
};

unique_ptr<UsdRowProducer> CreateProducer(ClientContext &, const UsdScanBindData &bind_data, const vector<column_t> &) {
	return make_uniq<UsdXformsProducer>(bind_data.options.TimeCode());
}

} // namespace

CreateTableFunctionInfo UsdXformsFunction() {
	UsdTableFunctionDefinition definition;
	definition.name = "usd_xforms";
	definition.description = "The world-space position, rotation, scale and matrices of each xformable prim.";
	definition.example = "usd_xforms('scene.usda', root := '/World')";
	definition.time_dependent = true;
	definition.columns = {{"prim_path", LogicalType::VARCHAR},
	                      {"x", LogicalType::DOUBLE},
	                      {"y", LogicalType::DOUBLE},
	                      {"z", LogicalType::DOUBLE},
	                      {"rotation", LogicalType::ARRAY(LogicalType::DOUBLE, 3)},
	                      {"scale", LogicalType::ARRAY(LogicalType::DOUBLE, 3)},
	                      {"has_rotation", LogicalType::BOOLEAN},
	                      {"has_scale", LogicalType::BOOLEAN},
	                      {"world_matrix", LogicalType::ARRAY(LogicalType::DOUBLE, 16)},
	                      {"local_matrix", LogicalType::ARRAY(LogicalType::DOUBLE, 16)},
	                      {"resets_xform_stack", LogicalType::BOOLEAN},
	                      {"xform_op_order", LogicalType::LIST(LogicalType::VARCHAR)},
	                      {"time_varying", LogicalType::BOOLEAN}};
	definition.create_producer = CreateProducer;
	return UsdCreateScanFunction(std::move(definition));
}

} // namespace duckdb
