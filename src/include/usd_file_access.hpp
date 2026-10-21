//===----------------------------------------------------------------------===//
//                         DuckDB USD Extension
//
// usd_file_access.hpp
//
// DuckDB's file access settings for the files USD reads by itself (sublayers,
// references, payloads, value clips): the extension's asset resolver refuses
// the files a database may not read before USD opens them.
//===----------------------------------------------------------------------===//

#pragma once

#include "duckdb.hpp"

#include <pxr/usd/ar/resolverContext.h>

namespace duckdb {

//! The file access settings a stage is opened under. The asset resolver checks every file USD resolves or opens for
//! the stage against them, on whichever thread USD reads it, and records the first file it refuses.
class UsdFileAccess {
public:
	explicit UsdFileAccess(ClientContext &context);

	//! Whether USD may read the file at `path`. Records `path` if not.
	bool Check(const std::string &path);
	//! Throws a PermissionException if a file was refused
	void ThrowIfRefused() const;

	//! Throws a PermissionException unless the database of `context` may read `path`
	static void Verify(ClientContext &context, const string &path);
	//! The resolver context to open the stage at `path` with: USD's default one, plus the settings of `access`
	static pxr::ArResolverContext CreateResolverContext(const string &path, const shared_ptr<UsdFileAccess> &access);

private:
	weak_ptr<DatabaseInstance> db;
	atomic<bool> has_refused {false};
	mutable mutex lock;
	string refused;
};

//! The plugInfo.json that declares the extension's asset resolver to OpenUSD
extern const char USD_RESOLVER_PLUGIN_INFO[];
//! Makes the extension's resolver OpenUSD's asset resolver. Called once its plugInfo.json is registered, before
//! anything uses the resolver.
void UsdDefineResolver();
//! Whether OpenUSD uses the extension's resolver
bool UsdResolverInstalled();

} // namespace duckdb
