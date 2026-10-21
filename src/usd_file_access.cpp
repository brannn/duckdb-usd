#include "usd_file_access.hpp"

#include "duckdb/main/client_context.hpp"
#include "duckdb/main/config.hpp"

#include <pxr/base/tf/pathUtils.h>
#include <pxr/base/tf/type.h>
#include <pxr/usd/ar/asset.h>
#include <pxr/usd/ar/defaultResolver.h>
#include <pxr/usd/ar/defineResolver.h>
#include <pxr/usd/ar/defineResolverContext.h>
#include <pxr/usd/ar/packageUtils.h>
#include <pxr/usd/ar/resolver.h>

namespace duckdb {

//! The resolver context object that carries a stage's file access settings to the resolver
struct UsdFileAccessContext {
	shared_ptr<UsdFileAccess> access;

	bool operator<(const UsdFileAccessContext &other) const {
		return access.get() < other.access.get();
	}
	bool operator==(const UsdFileAccessContext &other) const {
		return access.get() == other.access.get();
	}
};

size_t hash_value(const UsdFileAccessContext &context) {
	return std::hash<const void *>()(context.access.get());
}

} // namespace duckdb

PXR_NAMESPACE_OPEN_SCOPE
AR_DECLARE_RESOLVER_CONTEXT(duckdb::UsdFileAccessContext);
PXR_NAMESPACE_CLOSE_SCOPE

namespace duckdb {

//! OpenUSD's default resolver, except that the files the bound UsdFileAccessContext refuses look missing
class UsdAccessCheckingResolver : public pxr::ArDefaultResolver {
protected:
	pxr::ArResolvedPath _Resolve(const std::string &asset_path) const override {
		auto access = CurrentAccess();
		if (!access) {
			return ArDefaultResolver::_Resolve(asset_path);
		}
		// an absolute path is checked before it is looked up, so a refused file looks the same whether it exists or not
		auto file = FilePath(asset_path);
		if (!pxr::TfIsRelativePath(file) && !access->Check(file)) {
			return pxr::ArResolvedPath();
		}
		auto resolved_path = ArDefaultResolver::_Resolve(asset_path);
		if (!resolved_path.empty() && !access->Check(FilePath(resolved_path))) {
			return pxr::ArResolvedPath();
		}
		return resolved_path;
	}

	std::shared_ptr<pxr::ArAsset> _OpenAsset(const pxr::ArResolvedPath &resolved_path) const override {
		auto access = CurrentAccess();
		if (access && !access->Check(FilePath(resolved_path))) {
			return nullptr;
		}
		return ArDefaultResolver::_OpenAsset(resolved_path);
	}

private:
	UsdFileAccess *CurrentAccess() const {
		auto context = _GetCurrentContextObject<UsdFileAccessContext>();
		return context ? context->access.get() : nullptr;
	}

	//! The file `path` is in: the package, for a path inside a .usdz
	static std::string FilePath(const std::string &path) {
		return pxr::ArIsPackageRelativePath(path) ? pxr::ArSplitPackageRelativePathOuter(path).first : path;
	}
};

UsdFileAccess::UsdFileAccess(ClientContext &context) : db(context.db) {
}

bool UsdFileAccess::Check(const std::string &path) {
	auto database = db.lock();
	if (database && DBConfig::GetConfig(*database).CanAccessFile(path, FileType::FILE_TYPE_REGULAR)) {
		return true;
	}
	lock_guard<mutex> guard(lock);
	if (refused.empty()) {
		refused = path;
	}
	has_refused = true;
	return false;
}

void UsdFileAccess::ThrowIfRefused() const {
	if (!has_refused) {
		return;
	}
	lock_guard<mutex> guard(lock);
	throw PermissionException("Cannot access file \"%s\" - file system operations are disabled by configuration",
	                          refused);
}

void UsdFileAccess::Verify(ClientContext &context, const string &path) {
	if (!DBConfig::GetConfig(context).CanAccessFile(path, FileType::FILE_TYPE_REGULAR)) {
		throw PermissionException("Cannot access file \"%s\" - file system operations are disabled by configuration",
		                          path);
	}
}

pxr::ArResolverContext UsdFileAccess::CreateResolverContext(const string &path,
                                                            const shared_ptr<UsdFileAccess> &access) {
	return pxr::ArResolverContext(pxr::ArGetResolver().CreateDefaultContextForAsset(path),
	                              UsdFileAccessContext {access});
}

const char USD_RESOLVER_PLUGIN_INFO[] = R"({
    "Plugins": [
        {
            "Info": {
                "Types": {
                    "duckdb::UsdAccessCheckingResolver": {
                        "bases": ["ArDefaultResolver"],
                        "implementsContexts": true
                    }
                }
            },
            "LibraryPath": "",
            "Name": "duckdb_usd",
            "ResourcePath": "resources",
            "Root": "..",
            "Type": "library"
        }
    ]
}
)";

void UsdDefineResolver() {
	pxr::Ar_DefineResolver<UsdAccessCheckingResolver, pxr::ArDefaultResolver>();
	pxr::ArSetPreferredResolver(pxr::TfType::Find<UsdAccessCheckingResolver>().GetTypeName());
}

bool UsdResolverInstalled() {
	return dynamic_cast<UsdAccessCheckingResolver *>(&pxr::ArGetUnderlyingResolver()) != nullptr;
}

} // namespace duckdb
