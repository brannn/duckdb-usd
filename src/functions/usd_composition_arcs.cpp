#include "usd_functions.hpp"
#include "usd_scan.hpp"

#include <pxr/base/tf/errorMark.h>
#include <pxr/usd/pcp/node.h>
#include <pxr/usd/pcp/primIndex.h>
#include <pxr/usd/sdf/layer.h>
#include <pxr/usd/sdf/listEditorProxy.h>
#include <pxr/usd/sdf/payload.h>
#include <pxr/usd/sdf/reference.h>
#include <pxr/usd/usd/primCompositionQuery.h>

#include <deque>
#include <unordered_map>

namespace duckdb {

namespace {

enum UsdCompositionArcsColumn : column_t {
	PRIM_PATH,
	ARC_INDEX,
	ARC_TYPE,
	ASSET_PATH,
	TARGET_LAYER,
	TARGET_PRIM_PATH,
	VARIANT_SET,
	VARIANT,
	INTRODUCING_LAYER,
	INTRODUCING_PRIM_PATH,
	IS_ANCESTRAL,
	IS_IMPLICIT,
	HAS_SPECS
};

//! One composition arc, as the composition query reports it. Strings are empty where the column is NULL.
struct ArcInfo {
	const char *arc_type;
	std::string asset_path;
	std::string target_layer;
	std::string target_prim_path;
	std::string variant_set;
	std::string variant;
	std::string introducing_layer;
	pxr::SdfPath introducing_prim_path;
	bool is_ancestral;
	bool is_implicit;
	bool has_specs;
};

using ArcList = std::vector<ArcInfo>;

struct ArcRow {
	pxr::UsdPrim prim;
	const ArcInfo *arc;
	int32_t arc_index;
	//! The arc's introducing prim path, mapped to the prim's own instance for instance proxies
	pxr::SdfPath introducing_prim_path;
};

const char *ArcTypeName(pxr::PcpArcType type) {
	switch (type) {
	case pxr::PcpArcTypeRoot:
		return "root";
	case pxr::PcpArcTypeInherit:
		return "inherit";
	case pxr::PcpArcTypeVariant:
		return "variant";
	case pxr::PcpArcTypeRelocate:
		return "relocate";
	case pxr::PcpArcTypeReference:
		return "reference";
	case pxr::PcpArcTypePayload:
		return "payload";
	case pxr::PcpArcTypeSpecialize:
		return "specialize";
	default:
		return "unknown";
	}
}

std::string LayerIdentifier(const pxr::SdfLayerHandle &layer) {
	return layer ? layer->GetIdentifier() : std::string();
}

//! Whether the composition query can find any arc to report for a prim. Computing the query (a fully expanded prim
//! index) is expensive, and most prims have no arcs of their own. Arcs that contribute opinions are in the prim's
//! cached index; arcs that do not (e.g. inherits of a class nobody defines) are only in the expanded index, but are
//! always authored as metadata. Ancestral arcs without opinions leave no such trace, so with ancestral arcs every
//! prim is queried.
bool MayHaveArcs(const pxr::UsdPrim &prim, bool include_ancestral) {
	if (include_ancestral) {
		return true;
	}
	for (const auto &node : prim.GetPrimIndex().GetNodeRange()) {
		if (node.GetArcType() != pxr::PcpArcTypeRoot && !node.IsDueToAncestor()) {
			return true;
		}
	}
	return prim.HasAuthoredReferences() || prim.HasAuthoredPayloads() || prim.HasAuthoredInherits() ||
	       prim.HasAuthoredSpecializes() || prim.HasVariantSets();
}

class UsdCompositionArcsProducer : public UsdBufferedProducer<ArcRow> {
public:
	explicit UsdCompositionArcsProducer(bool include_ancestral_p) : include_ancestral(include_ancestral_p) {
		if (!include_ancestral) {
			filter.dependencyTypeFilter = pxr::UsdPrimCompositionQuery::DependencyTypeFilter::Direct;
		}
	}

	void BeginStage(const UsdStageHandle &) override {
		prototype_arcs.clear();
	}

	void AddPrim(const pxr::UsdPrim &prim) override {
		if (!prim.IsInstanceProxy()) {
			prim_arcs.push_back(ComputeArcs(prim));
			for (auto &arc : prim_arcs.back()) {
				rows.push_back(
				    {prim, &arc, NumericCast<int32_t>(&arc - prim_arcs.back().data()), arc.introducing_prim_path});
			}
			return;
		}
		// The prims inside instances share the prim index of the instance OpenUSD chose as the prototype's source:
		// they all have the same arcs, so compute them once per prototype prim. The paths of arcs authored on the
		// instance are under the source instance - map them to the proxy's own instance.
		auto &index_path = prim.GetPrimIndex().GetPath();
		auto entry = prototype_arcs.find(index_path);
		if (entry == prototype_arcs.end()) {
			entry = prototype_arcs.emplace(index_path, ComputeArcs(prim)).first;
		}
		auto &arcs = entry->second;
		if (arcs.empty()) {
			return;
		}
		auto source_root = index_path;
		auto prim_root = prim.GetPath();
		while (source_root != prim_root && !source_root.IsAbsoluteRootPath() &&
		       source_root.GetNameToken() == prim_root.GetNameToken()) {
			source_root = source_root.GetParentPath();
			prim_root = prim_root.GetParentPath();
		}
		for (auto &arc : arcs) {
			rows.push_back({prim, &arc, NumericCast<int32_t>(&arc - arcs.data()),
			                arc.introducing_prim_path.ReplacePrefix(source_root, prim_root)});
		}
	}

	void Clear() override {
		UsdBufferedProducer::Clear();
		prim_arcs.clear();
	}

	void Write(column_t column, idx_t offset, idx_t count, Vector &result) override {
		switch (column) {
		case PRIM_PATH:
			WritePrimPaths(offset, count, result);
			break;
		case ARC_INDEX:
			UsdWriteColumn<int32_t>(rows, offset, count, result, [](const ArcRow &row) { return row.arc_index; });
			break;
		case ARC_TYPE:
			UsdWriteStrings(rows, offset, count, result, [](const ArcRow &row) { return row.arc->arc_type; });
			break;
		case ASSET_PATH:
			WriteOptional(offset, count, result, &ArcInfo::asset_path);
			break;
		case TARGET_LAYER:
			WriteOptional(offset, count, result, &ArcInfo::target_layer);
			break;
		case TARGET_PRIM_PATH:
			WriteOptional(offset, count, result, &ArcInfo::target_prim_path);
			break;
		case VARIANT_SET:
			WriteOptional(offset, count, result, &ArcInfo::variant_set);
			break;
		case VARIANT:
			WriteOptional(offset, count, result, &ArcInfo::variant);
			break;
		case INTRODUCING_LAYER:
			WriteOptional(offset, count, result, &ArcInfo::introducing_layer);
			break;
		case INTRODUCING_PRIM_PATH:
			UsdWriteStringRefs(rows, offset, count, result, [](const ArcRow &row) -> const std::string * {
				return row.introducing_prim_path.IsEmpty() ? nullptr : &row.introducing_prim_path.GetString();
			});
			break;
		case IS_ANCESTRAL:
			UsdWriteColumn<bool>(rows, offset, count, result, [](const ArcRow &row) { return row.arc->is_ancestral; });
			break;
		case IS_IMPLICIT:
			UsdWriteColumn<bool>(rows, offset, count, result, [](const ArcRow &row) { return row.arc->is_implicit; });
			break;
		case HAS_SPECS:
			UsdWriteColumn<bool>(rows, offset, count, result, [](const ArcRow &row) { return row.arc->has_specs; });
			break;
		default:
			throw InternalException("usd_composition_arcs: unknown column %d", column);
		}
	}

private:
	ArcList ComputeArcs(const pxr::UsdPrim &prim) const {
		ArcList result;
		if (!MayHaveArcs(prim, include_ancestral)) {
			return result;
		}
		// Unresolvable arcs are reported (again) by the expanded prim index: they are already part of the stage's
		// composition errors, so discard them here
		pxr::TfErrorMark mark;
		pxr::UsdPrimCompositionQuery query(prim, filter);
		for (const auto &arc : query.GetCompositionArcs()) {
			auto arc_type = arc.GetArcType();
			if (arc_type == pxr::PcpArcTypeRoot) {
				continue;
			}
			ArcInfo info;
			info.arc_type = ArcTypeName(arc_type);
			if (arc_type == pxr::PcpArcTypeReference) {
				pxr::SdfReferenceEditorProxy editor;
				pxr::SdfReference reference;
				if (arc.GetIntroducingListEditor(&editor, &reference)) {
					info.asset_path = reference.GetAssetPath();
				}
			} else if (arc_type == pxr::PcpArcTypePayload) {
				pxr::SdfPayloadEditorProxy editor;
				pxr::SdfPayload payload;
				if (arc.GetIntroducingListEditor(&editor, &payload)) {
					info.asset_path = payload.GetAssetPath();
				}
			}
			info.target_layer = LayerIdentifier(arc.GetTargetLayer());
			auto target_path = arc.GetTargetPrimPath();
			info.target_prim_path = target_path.GetString();
			if (arc_type == pxr::PcpArcTypeVariant) {
				auto selection = target_path.GetVariantSelection();
				info.variant_set = selection.first;
				info.variant = selection.second;
			}
			info.introducing_layer = LayerIdentifier(arc.GetIntroducingLayer());
			info.introducing_prim_path = arc.GetIntroducingPrimPath();
			info.is_ancestral = arc.IsAncestral();
			info.is_implicit = arc.IsImplicit();
			info.has_specs = arc.HasSpecs();
			result.push_back(std::move(info));
		}
		mark.Clear();
		return result;
	}

	//! Writes a string member of the arc; empty strings are NULL
	void WriteOptional(idx_t offset, idx_t count, Vector &result, std::string ArcInfo::*member) {
		UsdWriteStringRefs(rows, offset, count, result, [member](const ArcRow &row) -> const std::string * {
			auto &value = row.arc->*member;
			return value.empty() ? nullptr : &value;
		});
	}

	bool include_ancestral;
	pxr::UsdPrimCompositionQuery::Filter filter;
	//! The arcs of the prims in the current morsel (a deque, so that rows can point into it)
	std::deque<ArcList> prim_arcs;
	//! The arcs of each prototype prim, by the path of its prim index, for the current stage
	std::unordered_map<pxr::SdfPath, ArcList, pxr::SdfPath::Hash> prototype_arcs;
};

void AddOptions(TypedKwargs &options) {
	options.Add("include_ancestral", LogicalType::BOOLEAN);
}

void BindOptions(const named_argument_map_t &parameters, UsdScanOptions &options) {
	UsdGetBooleanOption(parameters, "include_ancestral", options.include_ancestral);
}

unique_ptr<UsdRowProducer> CreateProducer(ClientContext &, const UsdScanBindData &bind_data, const vector<column_t> &) {
	return make_uniq<UsdCompositionArcsProducer>(bind_data.options.include_ancestral);
}

} // namespace

CreateTableFunctionInfo UsdCompositionArcsFunction() {
	UsdTableFunctionDefinition definition;
	definition.name = "usd_composition_arcs";
	definition.description = "One row per composition arc of each prim: references, payloads, inherits, specializes "
	                         "and variant selections, strongest first.";
	definition.example = "usd_composition_arcs('scene.usda', instance_proxies := true)";
	definition.columns = {{"prim_path", LogicalType::VARCHAR},         {"arc_index", LogicalType::INTEGER},
	                      {"arc_type", LogicalType::VARCHAR},          {"asset_path", LogicalType::VARCHAR},
	                      {"target_layer", LogicalType::VARCHAR},      {"target_prim_path", LogicalType::VARCHAR},
	                      {"variant_set", LogicalType::VARCHAR},       {"variant", LogicalType::VARCHAR},
	                      {"introducing_layer", LogicalType::VARCHAR}, {"introducing_prim_path", LogicalType::VARCHAR},
	                      {"is_ancestral", LogicalType::BOOLEAN},      {"is_implicit", LogicalType::BOOLEAN},
	                      {"has_specs", LogicalType::BOOLEAN}};
	definition.add_options = AddOptions;
	definition.bind_options = BindOptions;
	definition.create_producer = CreateProducer;
	return UsdCreateScanFunction(std::move(definition));
}

} // namespace duckdb
