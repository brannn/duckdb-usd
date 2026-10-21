//===----------------------------------------------------------------------===//
//                         DuckDB USD Extension
//
// usd_stage.hpp
//
// Opening USD stages: path validation against DuckDB's file access settings,
// capture of USD diagnostics, and sharing of open stages within a query and,
// through DuckDB's object cache, between queries.
//===----------------------------------------------------------------------===//

#pragma once

#include "duckdb.hpp"
#include "usd_file_access.hpp"

#include <pxr/base/tf/errorMark.h>
#include <pxr/usd/usd/stage.h>

namespace duckdb {

//! Options that change how a stage is composed. Stages are only shared between scans with equal options.
struct UsdStageOptions {
	//! Load payloads (UsdStage::LoadAll) or leave them unloaded (UsdStage::LoadNone)
	bool load_payloads = true;
	//! Compose only this prim's subtree (a population mask). "/" composes the whole stage.
	string mask_path = "/";

	//! Identifies the stage at `path` (relative to the working directory) opened with these options
	string CacheKey(const string &path) const;
	bool operator==(const UsdStageOptions &other) const;
};

//! An open, composed stage. It is never modified after opening, so it can be read from several threads at once.
struct UsdStageHandle {
	UsdStageHandle(string path_p, pxr::UsdStageRefPtr stage_p, shared_ptr<UsdFileAccess> access_p)
	    : path(std::move(path_p)), stage(std::move(stage_p)), access(std::move(access_p)) {
	}

	//! The path the stage was opened from, as given by the user (after glob expansion)
	string path;
	pxr::UsdStageRefPtr stage;
	//! The file access settings the stage reads files under. USD can read more files while the stage is in use
	//! (value clips): scans check that none was refused.
	shared_ptr<UsdFileAccess> access;
};

struct UsdStageProvider {
	//! Throws unless `path` is an existing local file in a format USD can read
	static void ValidatePath(ClientContext &context, const string &path);
	//! Opens the stage rooted at `path`, sharing it with other scans of the same query that use equal options, and,
	//! when the usd_stage_cache setting is on, with later queries until a file it read changes. Logs a single
	//! warning per query if the stage has composition errors and `warn_on_composition_errors` is set.
	static shared_ptr<UsdStageHandle> Open(ClientContext &context, const string &path, const UsdStageOptions &options,
	                                       bool warn_on_composition_errors);
	//! Frees the stages kept between queries by the database of `context`
	static void DropCachedStages(ClientContext &context);
};

//! Name of the setting that keeps stages open between queries
constexpr const char *USD_STAGE_CACHE_SETTING = "usd_stage_cache";

//! While alive, USD warnings raised on this thread are written to the DuckDB log of `context` rather than to stderr
class UsdDiagnosticScope {
public:
	explicit UsdDiagnosticScope(ClientContext &context);
	~UsdDiagnosticScope();

private:
	optional_ptr<ClientContext> previous;
};

//! One-time OpenUSD setup: keeps USD diagnostics off stderr. Safe to call more than once.
void UsdInitialize();

//! The errors USD raised since `mark` was set, one per line, and clears them
string UsdTakeErrors(pxr::TfErrorMark &mark);

} // namespace duckdb
