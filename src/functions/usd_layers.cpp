#include "usd_functions.hpp"
#include "usd_scan.hpp"

#include <pxr/usd/sdf/fileFormat.h>
#include <pxr/usd/sdf/layer.h>

#include <algorithm>

namespace duckdb {

namespace {

enum UsdLayersColumn : column_t {
	LAYER_IDENTIFIER,
	RESOLVED_PATH,
	FORMAT,
	IS_ROOT,
	IS_ANONYMOUS,
	SUBLAYERS,
	DEFAULT_PRIM,
	DOCUMENTATION
};

struct LayerRow {
	pxr::SdfLayerRefPtr layer;
	bool is_root;
};

class UsdLayersProducer : public UsdBufferedProducer<LayerRow> {
public:
	void BeginStage(const UsdStageHandle &handle) override {
		auto root = handle.stage->GetRootLayer();
		auto session = handle.stage->GetSessionLayer();
		std::vector<pxr::SdfLayerRefPtr> others;
		for (auto &layer : handle.stage->GetUsedLayers()) {
			// the session layer is an empty, in-memory layer every stage gets
			if (layer != root && layer != session) {
				others.emplace_back(layer);
			}
		}
		std::sort(others.begin(), others.end(), [](const pxr::SdfLayerRefPtr &a, const pxr::SdfLayerRefPtr &b) {
			return a->GetIdentifier() < b->GetIdentifier();
		});
		rows.push_back({root, true});
		for (auto &layer : others) {
			rows.push_back({layer, false});
		}
	}

	void Write(column_t column, idx_t offset, idx_t count, Vector &result) override {
		switch (column) {
		case LAYER_IDENTIFIER:
			UsdWriteStringRefs(rows, offset, count, result,
			                   [](const LayerRow &row) { return &row.layer->GetIdentifier(); });
			break;
		case RESOLVED_PATH:
			UsdWriteStringRefs(rows, offset, count, result, [](const LayerRow &row) -> const std::string * {
				auto &path = row.layer->GetRealPath();
				return path.empty() ? nullptr : &path;
			});
			break;
		case FORMAT:
			UsdWriteStrings(rows, offset, count, result, [](const LayerRow &row, std::string &out) {
				auto &format = row.layer->GetFileFormat();
				if (!format) {
					return false;
				}
				out = format->GetFormatId().GetString();
				return true;
			});
			break;
		case IS_ROOT:
			UsdWriteColumn<bool>(rows, offset, count, result, [](const LayerRow &row) { return row.is_root; });
			break;
		case IS_ANONYMOUS:
			UsdWriteColumn<bool>(rows, offset, count, result,
			                     [](const LayerRow &row) { return row.layer->IsAnonymous(); });
			break;
		case SUBLAYERS:
			UsdWriteStringLists(rows, offset, count, result, [](const LayerRow &row, std::vector<std::string> &out) {
				for (const std::string &path : row.layer->GetSubLayerPaths()) {
					out.push_back(path);
				}
				return true;
			});
			break;
		case DEFAULT_PRIM:
			UsdWriteStrings(rows, offset, count, result, [](const LayerRow &row, std::string &out) {
				auto default_prim = row.layer->GetDefaultPrim();
				if (default_prim.IsEmpty()) {
					return false;
				}
				out = default_prim.GetString();
				return true;
			});
			break;
		case DOCUMENTATION:
			UsdWriteStrings(rows, offset, count, result, [](const LayerRow &row, std::string &out) {
				out = row.layer->GetDocumentation();
				return !out.empty();
			});
			break;
		default:
			throw InternalException("usd_layers: unknown column %d", column);
		}
	}
};

unique_ptr<UsdRowProducer> CreateProducer(ClientContext &, const UsdScanBindData &, const vector<column_t> &) {
	return make_uniq<UsdLayersProducer>();
}

} // namespace

CreateTableFunctionInfo UsdLayersFunction() {
	UsdTableFunctionDefinition definition;
	definition.name = "usd_layers";
	definition.description =
	    "One row per layer a stage uses: the root layer, its sublayers and the layers of references and payloads.";
	definition.example = "usd_layers('scene.usda')";
	definition.unit = UsdScanUnit::STAGE;
	definition.columns = {
	    {"layer_identifier", LogicalType::VARCHAR}, {"resolved_path", LogicalType::VARCHAR},
	    {"format", LogicalType::VARCHAR},           {"is_root", LogicalType::BOOLEAN},
	    {"is_anonymous", LogicalType::BOOLEAN},     {"sublayers", LogicalType::LIST(LogicalType::VARCHAR)},
	    {"default_prim", LogicalType::VARCHAR},     {"documentation", LogicalType::VARCHAR}};
	definition.create_producer = CreateProducer;
	return UsdCreateScanFunction(std::move(definition));
}

} // namespace duckdb
