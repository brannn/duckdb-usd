#include "usd_stage.hpp"

#include "duckdb/common/file_system.hpp"
#include "duckdb/logging/logger.hpp"
#include "duckdb/main/client_context.hpp"
#include "duckdb/main/client_context_state.hpp"
#include "duckdb/storage/buffer_manager.hpp"
#include "duckdb/storage/object_cache.hpp"

#include <pxr/base/arch/fileSystem.h>
#include <pxr/base/tf/diagnosticMgr.h>
#include <pxr/base/tf/errorMark.h>
#include <pxr/base/tf/pathUtils.h>
#include <pxr/usd/ar/packageUtils.h>
#include <pxr/usd/ar/resolver.h>
#include <pxr/usd/ar/resolverContextBinder.h>
#include <pxr/usd/pcp/errors.h>
#include <pxr/usd/sdf/layer.h>
#include <pxr/usd/usd/primRange.h>
#include <pxr/usd/usd/stagePopulationMask.h>

#include <set>

namespace duckdb {

namespace {

thread_local ClientContext *diagnostic_context = nullptr;

//! Keeps USD from printing to stderr. Diagnostics are logged at debug level only: they arrive in the middle of USD
//! code (possibly on a USD worker thread), where a warning-level entry could throw (warnings_as_errors).
class UsdDiagnosticDelegate : public pxr::TfDiagnosticMgr::Delegate {
public:
	void IssueError(const pxr::TfError &error) override {
		Log(error.GetCommentary());
	}
	void IssueFatalError(const pxr::TfCallContext &, const std::string &message) override {
		Log(message);
	}
	void IssueStatus(const pxr::TfStatus &status) override {
		Log(status.GetCommentary());
	}
	void IssueWarning(const pxr::TfWarning &warning) override {
		Log(warning.GetCommentary());
	}

private:
	static void Log(const std::string &message) {
		if (!diagnostic_context) {
			// raised outside of a scan (e.g. on a USD worker thread) - there is no query to attribute it to
			return;
		}
		try {
			DUCKDB_LOG_INTERNAL(*diagnostic_context, "usd", LogLevel::LOG_DEBUG, message);
		} catch (...) { // NOLINT: an exception must never propagate into USD
		}
	}
};

//! Stages opened by the scans of the running query. Entries are weak, so a stage lives only as long as a scan
//! uses it, and the map is cleared when the query ends.
class UsdQueryStageCache : public ClientContextState {
public:
	static UsdQueryStageCache &Get(ClientContext &context) {
		return *context.registered_state->GetOrCreate<UsdQueryStageCache>("usd_query_stage_cache");
	}

	shared_ptr<UsdStageHandle> Find(const string &key) {
		lock_guard<mutex> guard(lock);
		auto entry = stages.find(key);
		if (entry == stages.end()) {
			return nullptr;
		}
		auto handle = entry->second.lock();
		if (!handle) {
			stages.erase(entry);
		}
		return handle;
	}

	void Insert(const string &key, const shared_ptr<UsdStageHandle> &handle) {
		lock_guard<mutex> guard(lock);
		stages[key] = handle;
	}

	void QueryEnd() override {
		lock_guard<mutex> guard(lock);
		stages.clear();
	}

private:
	mutex lock;
	unordered_map<string, weak_ptr<UsdStageHandle>> stages;
};

//! A version of a file: its modification time and size. (Modification times alone can miss a change: on Linux they
//! advance in steps of a few milliseconds.)
struct UsdFileStamp {
	pxr::ArTimestamp time;
	int64_t size = -1;

	bool operator==(const UsdFileStamp &other) const {
		return time == other.time && size == other.size;
	}
	bool operator!=(const UsdFileStamp &other) const {
		return !(*this == other);
	}
};

//! The current stamp of the file at `resolved_path` (of its package, for a file inside a .usdz). Called with a
//! resolver context bound if the path came from one.
UsdFileStamp GetFileStamp(const std::string &asset_path, const pxr::ArResolvedPath &resolved_path) {
	UsdFileStamp stamp;
	if (resolved_path.empty()) {
		return stamp;
	}
	stamp.time = pxr::ArGetResolver().GetModificationTimestamp(asset_path, resolved_path);
	auto &path = resolved_path.GetPathString();
	auto file = pxr::ArIsPackageRelativePath(path) ? pxr::ArSplitPackageRelativePathOuter(path).first : path;
	stamp.size = pxr::ArchGetFileLength(file.c_str());
	return stamp;
}

//! The stamp of each loaded layer's file when the layer was read. USD keeps one copy of each loaded layer, shared by
//! every stage that uses its file, and opening a stage reuses that copy, even when its file changed since: while a
//! cached stage (or one a running query uses) keeps a layer alive, newly opened stages get its old contents. These
//! stamps tell which layers are out of date.
class UsdLayerStamps {
public:
	static UsdLayerStamps &Get() {
		// never freed: it holds layer handles, which must not be released during static destruction
		static auto *instance = new UsdLayerStamps();
		return *instance;
	}

	//! Whether the file of `layer` changed since the layer was read. The first call for a layer records the file's
	//! current stamp, so it must be made right after the layer is loaded.
	bool Changed(const pxr::SdfLayerHandle &layer) {
		auto &resolved_path = layer->GetResolvedPath();
		if (resolved_path.empty()) {
			return false;
		}
		auto current = GetFileStamp(resolved_path.GetPathString(), resolved_path);
		lock_guard<mutex> guard(lock);
		if (layers.size() >= prune_at) {
			Prune();
		}
		auto &entry = layers[layer->GetIdentifier()];
		if (entry.layer != layer) {
			// first seen (or reloaded after the previous copy was freed)
			entry.layer = layer;
			entry.stamp = current;
			return false;
		}
		return entry.stamp != current;
	}

private:
	struct Entry {
		//! Weak: expires when USD frees the layer
		pxr::SdfLayerHandle layer;
		UsdFileStamp stamp;
	};

	void Prune() {
		for (auto it = layers.begin(); it != layers.end();) {
			it = it->second.layer ? std::next(it) : layers.erase(it);
		}
		prune_at = MaxValue<idx_t>(1024, 2 * layers.size());
	}

	mutex lock;
	unordered_map<string, Entry> layers;
	idx_t prune_at = 1024;
};

//! The layers of `stage` whose files changed since they were read
pxr::SdfLayerHandleVector ChangedLayers(const pxr::UsdStageRefPtr &stage) {
	auto &stamps = UsdLayerStamps::Get();
	pxr::SdfLayerHandleVector changed;
	for (auto &layer : stage->GetUsedLayers()) {
		if (stamps.Changed(layer)) {
			changed.push_back(layer);
		}
	}
	return changed;
}

//! An asset that a stage failed to load: a missing reference, payload or sublayer, or a file USD could not read
struct UsdMissingAsset {
	std::string identifier;
	//! Where the asset resolved to (empty if it did not resolve), and that file's stamp
	pxr::ArResolvedPath resolved_path;
	UsdFileStamp stamp;
};

//! The assets the composition errors of `stage` report as missing or unreadable
vector<UsdMissingAsset> FindMissingAssets(const pxr::UsdStageRefPtr &stage, const pxr::PcpErrorVector &errors) {
	std::set<std::string> identifiers;
	for (auto &error : errors) {
		pxr::SdfLayerHandle anchor;
		std::string asset_path;
		if (auto asset_error = std::dynamic_pointer_cast<pxr::PcpErrorInvalidAssetPath>(error)) {
			anchor = asset_error->sourceLayer;
			asset_path = asset_error->assetPath;
		} else if (auto sublayer_error = std::dynamic_pointer_cast<pxr::PcpErrorInvalidSublayerPath>(error)) {
			anchor = sublayer_error->layer;
			asset_path = sublayer_error->sublayerPath;
		}
		if (anchor && !asset_path.empty()) {
			identifiers.insert(anchor->ComputeAbsolutePath(asset_path));
		}
	}
	vector<UsdMissingAsset> assets;
	pxr::ArResolverContextBinder binder(stage->GetPathResolverContext());
	auto &resolver = pxr::ArGetResolver();
	for (auto &identifier : identifiers) {
		UsdMissingAsset asset;
		asset.identifier = identifier;
		asset.resolved_path = resolver.Resolve(identifier);
		asset.stamp = GetFileStamp(identifier, asset.resolved_path);
		assets.push_back(std::move(asset));
	}
	return assets;
}

//! Memory a composed stage keeps, measured on large real scenes: about 1 to 3 KB per prim (prototype prims included)
//! and half a megabyte per layer. DuckDB evicts cached stages by this estimate, so it errs high.
constexpr idx_t STAGE_BYTES_PER_PRIM = 2560;
constexpr idx_t STAGE_BYTES_PER_LAYER = 512 * 1024;

idx_t EstimateStageMemory(const pxr::UsdStageRefPtr &stage) {
	idx_t prims = 0;
	auto count = [&prims](const pxr::UsdPrimRange &range) {
		for (auto it = range.begin(); it != range.end(); ++it) {
			prims++;
		}
	};
	count(pxr::UsdPrimRange::Stage(stage, pxr::UsdPrimAllPrimsPredicate));
	for (auto &prototype : stage->GetPrototypes()) {
		count(pxr::UsdPrimRange(prototype, pxr::UsdPrimAllPrimsPredicate));
	}
	return prims * STAGE_BYTES_PER_PRIM + stage->GetUsedLayers().size() * STAGE_BYTES_PER_LAYER;
}

//! Identifies a database's object cache, so that the stages it holds can be told apart from other databases'. Kept in
//! that cache, never evicted.
class UsdStageCacheOwner : public ObjectCacheEntry {
public:
	static string ObjectType() {
		return "usd_stage_cache_owner";
	}
	string GetObjectType() override {
		return ObjectType();
	}
	optional_idx GetEstimatedCacheMemory() const override {
		return optional_idx();
	}

	static shared_ptr<UsdStageCacheOwner> Get(ObjectCache &object_cache) {
		return object_cache.GetOrCreate<UsdStageCacheOwner>(ObjectType());
	}
};

//! A stage kept between queries in DuckDB's object cache
class UsdCachedStage : public ObjectCacheEntry {
public:
	UsdCachedStage(const shared_ptr<UsdStageCacheOwner> &owner_p, string key_p, shared_ptr<UsdStageHandle> handle_p,
	               string open_errors_p, idx_t composition_error_count_p, vector<UsdMissingAsset> missing_assets_p,
	               idx_t estimated_memory_p)
	    : owner(owner_p), key(std::move(key_p)), estimated_memory(estimated_memory_p),
	      open_errors(std::move(open_errors_p)), composition_error_count(composition_error_count_p),
	      missing_assets(std::move(missing_assets_p)), resolver_context(handle_p->stage->GetPathResolverContext()),
	      handle(std::move(handle_p)) {
		Touch();
	}

	static string ObjectType() {
		return "usd_stage";
	}
	string GetObjectType() override {
		return ObjectType();
	}
	optional_idx GetEstimatedCacheMemory() const override {
		return estimated_memory;
	}

	bool BelongsTo(const shared_ptr<UsdStageCacheOwner> &cache_owner) const {
		return owner.lock() == cache_owner;
	}

	//! Marks the stage as used now, for choosing which cached stages to drop first
	void Touch() {
		static atomic<idx_t> clock {0};
		last_used = ++clock;
	}

	//! The stage, or nullptr once released
	shared_ptr<UsdStageHandle> GetHandle() {
		lock_guard<mutex> guard(lock);
		return handle;
	}

	//! Drops the cache's reference to the stage (queries using it keep it alive until they end)
	void Release() {
		shared_ptr<UsdStageHandle> released;
		{
			lock_guard<mutex> guard(lock);
			released = std::move(handle);
		}
		// a stage can take a while to free: not under the lock
	}

	//! Whether an asset the stage failed to load now resolves elsewhere, or its file changed
	bool MissingAssetsChanged() const {
		if (missing_assets.empty()) {
			return false;
		}
		pxr::ArResolverContextBinder binder(resolver_context);
		auto &resolver = pxr::ArGetResolver();
		for (auto &asset : missing_assets) {
			auto resolved_path = resolver.Resolve(asset.identifier);
			if (resolved_path != asset.resolved_path || GetFileStamp(asset.identifier, resolved_path) != asset.stamp) {
				return true;
			}
		}
		return false;
	}

	//! The object cache holding this entry
	const weak_ptr<UsdStageCacheOwner> owner;
	//! The key of this entry in its object cache (without the type prefix)
	const string key;
	const idx_t estimated_memory;
	atomic<idx_t> last_used {0};
	//! Errors USD raised while opening the stage, logged again by each query that uses it
	const string open_errors;
	const idx_t composition_error_count;
	const vector<UsdMissingAsset> missing_assets;

private:
	const pxr::ArResolverContext resolver_context;
	mutex lock;
	shared_ptr<UsdStageHandle> handle;
};

//! Every stage cached by any database of the process, so that the stages keeping a changed layer alive can be
//! released (USD's layers are shared by the whole process)
class UsdCachedStageRegistry {
public:
	static UsdCachedStageRegistry &Get() {
		static auto *instance = new UsdCachedStageRegistry();
		return *instance;
	}

	void Add(const shared_ptr<UsdCachedStage> &stage) {
		lock_guard<mutex> guard(lock);
		// forget the stages that have since left their cache
		stages.erase(std::remove_if(stages.begin(), stages.end(),
		                            [](const weak_ptr<UsdCachedStage> &entry) { return entry.expired(); }),
		             stages.end());
		stages.push_back(stage);
	}

	vector<shared_ptr<UsdCachedStage>> Snapshot() {
		lock_guard<mutex> guard(lock);
		vector<shared_ptr<UsdCachedStage>> result;
		for (auto &entry : stages) {
			auto stage = entry.lock();
			if (stage) {
				result.push_back(std::move(stage));
			}
		}
		return result;
	}

private:
	mutex lock;
	vector<weak_ptr<UsdCachedStage>> stages;
};

//! Removes `entry` from `object_cache`, if it is there (it may belong to another database's cache)
void RemoveFromObjectCache(ObjectCache &object_cache, UsdCachedStage &entry) {
	if (object_cache.GetWithTypePrefix<UsdCachedStage>(entry.key).get() == &entry) {
		object_cache.DeleteWithTypePrefix<UsdCachedStage>(entry.key);
	}
}

//! Makes room for caching a stage of `estimated_memory` bytes, dropping the least recently used cached stages, and
//! returns whether it fits. Cached stages take at most half of memory_limit, and never push DuckDB over the limit:
//! DuckDB evicts cached objects when it needs memory, but many threads evicting at once can fail a query.
bool MakeRoom(ClientContext &context, ObjectCache &object_cache, idx_t estimated_memory) {
	auto &buffer_manager = BufferManager::GetBufferManager(context);
	auto limit = buffer_manager.GetMaxMemory();
	auto owner = UsdStageCacheOwner::Get(object_cache);
	vector<shared_ptr<UsdCachedStage>> cached;
	idx_t cached_memory = 0;
	for (auto &entry : UsdCachedStageRegistry::Get().Snapshot()) {
		if (entry->BelongsTo(owner)) {
			cached_memory += entry->estimated_memory;
			cached.push_back(std::move(entry));
		}
	}
	std::sort(cached.begin(), cached.end(),
	          [](const shared_ptr<UsdCachedStage> &a, const shared_ptr<UsdCachedStage> &b) {
		          return a->last_used < b->last_used;
	          });
	auto fits = [&]() {
		return cached_memory + estimated_memory <= limit / 2 &&
		       buffer_manager.GetUsedMemory() + estimated_memory <= limit;
	};
	for (auto &entry : cached) {
		if (fits()) {
			break;
		}
		RemoveFromObjectCache(object_cache, *entry);
		cached_memory -= entry->estimated_memory;
	}
	return fits();
}

//! Releases every cached stage that uses one of `layers`, so that USD frees those layers and reads them again
void ReleaseStagesUsing(ClientContext &context, const pxr::SdfLayerHandleVector &layers) {
	pxr::SdfLayerHandleSet changed(layers.begin(), layers.end());
	auto &object_cache = ObjectCache::GetObjectCache(context);
	for (auto &entry : UsdCachedStageRegistry::Get().Snapshot()) {
		auto handle = entry->GetHandle();
		if (!handle) {
			continue;
		}
		bool uses_changed_layer = false;
		for (auto &layer : handle->stage->GetUsedLayers()) {
			if (changed.count(layer)) {
				uses_changed_layer = true;
				break;
			}
		}
		handle.reset();
		if (uses_changed_layer) {
			entry->Release();
			RemoveFromObjectCache(object_cache, *entry);
		}
	}
}

bool StageCacheEnabled(ClientContext &context) {
	Value enabled;
	if (!context.TryGetCurrentSetting(USD_STAGE_CACHE_SETTING, enabled) || enabled.IsNull()) {
		return true;
	}
	return BooleanValue::Get(enabled);
}

//! Opens the stage, reading only the files `access` allows; `errors` receives the errors USD raised
pxr::UsdStageRefPtr OpenStage(const string &path, const UsdStageOptions &options,
                              const shared_ptr<UsdFileAccess> &access, string &errors) {
	auto load = options.load_payloads ? pxr::UsdStage::LoadAll : pxr::UsdStage::LoadNone;
	auto resolver_context = UsdFileAccess::CreateResolverContext(path, access);
	pxr::UsdStageRefPtr stage;
	pxr::TfErrorMark mark;
	if (options.mask_path == "/") {
		stage = pxr::UsdStage::Open(path, resolver_context, load);
	} else {
		pxr::UsdStagePopulationMask mask({pxr::SdfPath(options.mask_path)});
		stage = pxr::UsdStage::OpenMasked(path, resolver_context, mask, load);
	}
	errors = UsdTakeErrors(mark);
	// before anything reports USD's errors, which can quote the files they are about
	access->ThrowIfRefused();
	if (!stage) {
		throw IOException("Failed to open USD stage \"%s\"%s", path, errors.empty() ? "" : ": " + errors);
	}
	return stage;
}

//! USD shares loaded layers between stages, including layers another database loaded: makes sure the stage uses no
//! layer DuckDB may not access
void VerifyCanAccessLayers(ClientContext &context, const pxr::UsdStageRefPtr &stage) {
	for (auto &layer : stage->GetUsedLayers()) {
		auto &real_path = layer->GetRealPath();
		if (!real_path.empty()) {
			UsdFileAccess::Verify(context, real_path);
		}
	}
}

void LogOpenWarnings(ClientContext &context, const string &path, const string &open_errors,
                     idx_t composition_error_count) {
	if (!open_errors.empty()) {
		DUCKDB_LOG_INTERNAL(context, "usd", LogLevel::LOG_WARNING, "Opening \"" + path + "\": " + open_errors);
	}
	if (composition_error_count > 0) {
		DUCKDB_LOG_INTERNAL(context, "usd", LogLevel::LOG_WARNING,
		                    StringUtil::Format("USD stage \"%s\" has %d composition error(s), such as unresolved "
		                                       "references. Query usd_composition_errors('%s') for details.",
		                                       path, composition_error_count, path));
	}
}

//! The cached stage for `key`, if it is still up to date with the files it read
shared_ptr<UsdStageHandle> FindCachedStage(ClientContext &context, ObjectCache &object_cache, const string &path,
                                           const string &key, bool warn_on_composition_errors) {
	auto entry = object_cache.GetWithTypePrefix<UsdCachedStage>(key);
	if (!entry) {
		return nullptr;
	}
	auto handle = entry->GetHandle();
	if (handle) {
		auto changed = ChangedLayers(handle->stage);
		if (changed.empty() && !entry->MissingAssetsChanged()) {
			handle->access->ThrowIfRefused();
			VerifyCanAccessLayers(context, handle->stage);
			entry->Touch();
			LogOpenWarnings(context, path, entry->open_errors,
			                warn_on_composition_errors ? entry->composition_error_count : 0);
			return handle;
		}
		handle.reset();
		if (!changed.empty()) {
			// this stage, and any other that shares the changed layers, must let go of them before they can be reread
			ReleaseStagesUsing(context, changed);
		}
	}
	entry->Release();
	RemoveFromObjectCache(object_cache, *entry);
	return nullptr;
}

} // namespace

string UsdStageOptions::CacheKey(const string &path) const {
	return StringUtil::Format("%s|%s|%s", load_payloads ? "all" : "none", mask_path, pxr::TfAbsPath(path));
}

bool UsdStageOptions::operator==(const UsdStageOptions &other) const {
	return load_payloads == other.load_payloads && mask_path == other.mask_path;
}

void UsdStageProvider::ValidatePath(ClientContext &context, const string &path) {
	auto trimmed = path;
	StringUtil::Trim(trimmed);
	if (trimmed.empty()) {
		throw InvalidInputException("USD file path cannot be empty");
	}
	auto &fs = FileSystem::GetFileSystem(context);
	if (!fs.FileExists(path)) {
		if (fs.DirectoryExists(path)) {
			throw InvalidInputException("Expected a USD file but \"%s\" is a directory", path);
		}
		throw IOException("USD file not found: \"%s\"", path);
	}
	if (!pxr::UsdStage::IsSupportedFile(path)) {
		throw InvalidInputException("Not a USD file: \"%s\" (expected .usd, .usda, .usdc or .usdz)", path);
	}
}

shared_ptr<UsdStageHandle> UsdStageProvider::Open(ClientContext &context, const string &path,
                                                  const UsdStageOptions &options, bool warn_on_composition_errors) {
	auto &query_stages = UsdQueryStageCache::Get(context);
	auto key = options.CacheKey(path);
	auto handle = query_stages.Find(key);
	if (handle) {
		return handle;
	}

	UsdFileAccess::Verify(context, path);
	optional_ptr<ObjectCache> object_cache;
	if (StageCacheEnabled(context)) {
		object_cache = ObjectCache::GetObjectCache(context);
		handle = FindCachedStage(context, *object_cache, path, key, warn_on_composition_errors);
		if (handle) {
			query_stages.Insert(key, handle);
			return handle;
		}
	}

	auto access = make_shared_ptr<UsdFileAccess>(context);
	string open_errors;
	auto stage = OpenStage(path, options, access, open_errors);
	auto changed = ChangedLayers(stage);
	if (!changed.empty()) {
		// USD reused layers that other stages keep alive, with the contents their files had before they changed:
		// release the cached stages that hold them, and open the stage again
		ReleaseStagesUsing(context, changed);
		stage = nullptr;
		stage = OpenStage(path, options, access, open_errors);
		changed = ChangedLayers(stage);
	}
	VerifyCanAccessLayers(context, stage);
	auto composition_errors = stage->GetCompositionErrors();
	LogOpenWarnings(context, path, open_errors, warn_on_composition_errors ? composition_errors.size() : 0);
	if (!changed.empty()) {
		// the layers are still held by a running query (an earlier scan of this one, or another connection's)
		DUCKDB_LOG_INTERNAL(context, "usd", LogLevel::LOG_WARNING,
		                    StringUtil::Format("%d file(s) used by USD stage \"%s\" changed while they were being "
		                                       "read: this query reads their previous contents",
		                                       changed.size(), path));
	}

	handle = make_shared_ptr<UsdStageHandle>(path, stage, std::move(access));
	if (object_cache && changed.empty()) {
		auto estimated_memory = EstimateStageMemory(stage);
		if (MakeRoom(context, *object_cache, estimated_memory)) {
			auto entry = make_shared_ptr<UsdCachedStage>(
			    UsdStageCacheOwner::Get(*object_cache), key, handle, open_errors, composition_errors.size(),
			    FindMissingAssets(stage, composition_errors), estimated_memory);
			object_cache->PutWithTypePrefix<UsdCachedStage>(key, entry);
			UsdCachedStageRegistry::Get().Add(entry);
		} else {
			DUCKDB_LOG_INTERNAL(context, "usd", LogLevel::LOG_INFO,
			                    StringUtil::Format("USD stage \"%s\" is not cached: it needs about %s, more than "
			                                       "half of memory_limit or than the memory not in use",
			                                       path, StringUtil::BytesToHumanReadableString(estimated_memory)));
		}
	}
	query_stages.Insert(key, handle);
	return handle;
}

void UsdStageProvider::DropCachedStages(ClientContext &context) {
	auto &object_cache = ObjectCache::GetObjectCache(context);
	auto owner = UsdStageCacheOwner::Get(object_cache);
	for (auto &entry : UsdCachedStageRegistry::Get().Snapshot()) {
		if (entry->BelongsTo(owner)) {
			RemoveFromObjectCache(object_cache, *entry);
		}
	}
}

UsdDiagnosticScope::UsdDiagnosticScope(ClientContext &context) : previous(diagnostic_context) {
	diagnostic_context = &context;
}

UsdDiagnosticScope::~UsdDiagnosticScope() {
	diagnostic_context = previous.get();
}

string UsdTakeErrors(pxr::TfErrorMark &mark) {
	string message;
	for (auto it = mark.GetBegin(); it != mark.GetEnd(); ++it) {
		if (!message.empty()) {
			message += "\n";
		}
		message += it->GetCommentary();
	}
	mark.Clear();
	return message;
}

void UsdInitialize() {
	static std::once_flag initialized;
	std::call_once(initialized, [] {
		// intentionally never freed: the diagnostic manager may outlive any static we could tie it to
		pxr::TfDiagnosticMgr::GetInstance().AddDelegate(new UsdDiagnosticDelegate());
	});
}

} // namespace duckdb
