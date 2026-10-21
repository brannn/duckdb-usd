#include "usd_plugins.hpp"

#include "usd_file_access.hpp"
#include "usd_stage.hpp"

#include <pxr/base/arch/fileSystem.h>
#include <pxr/base/plug/registry.h>
#include <pxr/base/tf/errorMark.h>
#include <pxr/base/tf/fileUtils.h>
#include <pxr/base/tf/pathUtils.h>
#include <pxr/base/tf/stringUtils.h>
#include <pxr/usd/sdf/fileFormat.h>
#include <pxr/usd/sdf/schema.h>
#include <pxr/usd/usd/schemaRegistry.h>

#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <fstream>
#include <mutex>
#include <unistd.h>

namespace duckdb {

namespace {

//! The directory the resources were written to, removed when the process exits
std::string resource_directory;

void RemoveResourceDirectory() {
	if (!resource_directory.empty()) {
		pxr::TfRmTree(resource_directory, [](const std::string &, const std::string &) {});
	}
}

std::string CreateResourceDirectory() {
	// for hosts without a writable temporary directory: a directory of the user's choosing
	auto configured = std::getenv("DUCKDB_USD_RESOURCE_DIR");
	if (configured && *configured) {
		return pxr::TfStringCatPaths(configured, std::string("duckdb-usd-") + USD_EMBEDDED_RESOURCE_HASH);
	}
	// a private (mode 0700) directory for this process
	auto directory = pxr::ArchMakeTmpSubdir(pxr::ArchGetTmpDir(), "duckdb-usd");
	if (directory.empty()) {
		throw IOException("Cannot create a temporary directory under \"%s\" for the OpenUSD plugin resources - set "
		                  "DUCKDB_USD_RESOURCE_DIR to a writable directory",
		                  pxr::ArchGetTmpDir());
	}
	resource_directory = directory;
	std::atexit(RemoveResourceDirectory);
	return directory;
}

void WriteResource(const std::string &directory, const UsdEmbeddedResource &resource) {
	auto path = pxr::TfStringCatPaths(directory, resource.path);
	auto parent = pxr::TfGetPathName(path);
	if (!pxr::TfIsDir(parent) && !pxr::TfMakeDirs(parent, -1, true)) {
		throw IOException("Cannot create directory \"%s\" for the OpenUSD plugin resources", parent);
	}
	// written next to the file and renamed into place: another process may be reading a shared resource directory
	auto temporary_path = path + "." + std::to_string(getpid()) + ".tmp";
	{
		std::ofstream out(temporary_path, std::ios::binary | std::ios::trunc);
		out.write(reinterpret_cast<const char *>(resource.data), static_cast<std::streamsize>(resource.size));
		if (!out) {
			std::remove(temporary_path.c_str());
			throw IOException("Cannot write the OpenUSD plugin resource \"%s\"", path);
		}
	}
	if (std::rename(temporary_path.c_str(), path.c_str()) != 0) {
		std::remove(temporary_path.c_str());
		throw IOException("Cannot write the OpenUSD plugin resource \"%s\"", path);
	}
}

void RegisterPlugins() {
	auto directory = CreateResourceDirectory();
	std::vector<std::string> plugin_infos;
	std::vector<std::string> plugin_names {"duckdb_usd"};
	for (idx_t i = 0; i < USD_EMBEDDED_RESOURCE_COUNT; i++) {
		auto &resource = USD_EMBEDDED_RESOURCES[i];
		WriteResource(directory, resource);
		if (pxr::TfGetBaseName(resource.path) == "plugInfo.json") {
			plugin_infos.push_back(pxr::TfStringCatPaths(directory, resource.path));
			// each library's plugin is named after its directory, e.g. usdGeom/resources/plugInfo.json
			std::string path(resource.path);
			plugin_names.push_back(path.substr(0, path.find('/')));
		}
	}
	// the extension's own asset resolver (see usd_file_access.hpp)
	UsdEmbeddedResource resolver_info {"duckdb_usd/resources/plugInfo.json",
	                                   reinterpret_cast<const unsigned char *>(USD_RESOLVER_PLUGIN_INFO),
	                                   strlen(USD_RESOLVER_PLUGIN_INFO)};
	WriteResource(directory, resolver_info);
	plugin_infos.push_back(pxr::TfStringCatPaths(directory, resolver_info.path));

	pxr::TfErrorMark mark;
	auto &registry = pxr::PlugRegistry::GetInstance();
	registry.RegisterPlugins(plugin_infos);
	for (auto &name : plugin_names) {
		if (!registry.GetPluginWithName(name)) {
			throw IOException("The OpenUSD plugin \"%s\" failed to register from \"%s\"", name, directory);
		}
	}
	UsdDefineResolver();
	// Touch everything that reads the resources now: a plugin that is missing later would abort the process,
	// while here it can still be reported as an error
	if (!pxr::SdfFileFormat::FindByExtension("usda") || !pxr::SdfFileFormat::FindByExtension("usdc") ||
	    !pxr::SdfFileFormat::FindByExtension("usdz")) {
		throw IOException("The OpenUSD file formats (usda, usdc, usdz) are not registered");
	}
	pxr::SdfSchema::GetInstance();
	pxr::UsdSchemaRegistry::GetInstance();
	if (!UsdResolverInstalled()) {
		throw IOException("OpenUSD does not use the extension's asset resolver");
	}
	if (!mark.IsClean()) {
		throw IOException("OpenUSD reported errors while registering its plugins:\n%s", UsdTakeErrors(mark));
	}
}

} // namespace

void UsdRegisterPlugins() {
	// if registration throws, the next call tries again
	static std::once_flag registered;
	std::call_once(registered, RegisterPlugins);
}

} // namespace duckdb
