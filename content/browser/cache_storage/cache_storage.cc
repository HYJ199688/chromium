// Copyright 2014 The Chromium Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include "content/browser/cache_storage/cache_storage.h"

#include <stddef.h>

#include <set>
#include <string>
#include <utility>

#include "base/barrier_closure.h"
#include "base/bind.h"
#include "base/bind_helpers.h"
#include "base/files/file_util.h"
#include "base/files/memory_mapped_file.h"
#include "base/guid.h"
#include "base/hash/sha1.h"
#include "base/location.h"
#include "base/memory/ptr_util.h"
#include "base/memory/ref_counted.h"
#include "base/metrics/histogram_macros.h"
#include "base/numerics/safe_conversions.h"
#include "base/sequenced_task_runner.h"
#include "base/single_thread_task_runner.h"
#include "base/stl_util.h"
#include "base/strings/string_number_conversions.h"
#include "base/strings/string_util.h"
#include "base/threading/thread_task_runner_handle.h"
#include "base/trace_event/trace_event.h"
#include "base/trace_event/traced_value.h"
#include "content/browser/cache_storage/cache_storage.pb.h"
#include "content/browser/cache_storage/cache_storage_cache.h"
#include "content/browser/cache_storage/cache_storage_cache_handle.h"
#include "content/browser/cache_storage/cache_storage_histogram_utils.h"
#include "content/browser/cache_storage/cache_storage_index.h"
#include "content/browser/cache_storage/cache_storage_manager.h"
#include "content/browser/cache_storage/cache_storage_quota_client.h"
#include "content/browser/cache_storage/cache_storage_scheduler.h"
#include "content/browser/cache_storage/cache_storage_trace_utils.h"
#include "content/common/background_fetch/background_fetch_types.h"
#include "content/public/browser/browser_thread.h"
#include "crypto/symmetric_key.h"
#include "net/base/directory_lister.h"
#include "net/base/net_errors.h"
#include "storage/browser/blob/blob_storage_context.h"
#include "storage/browser/quota/padding_key.h"
#include "storage/browser/quota/quota_manager_proxy.h"
#include "third_party/blink/public/mojom/quota/quota_types.mojom.h"

using blink::mojom::CacheStorageError;
using blink::mojom::StorageType;
using crypto::SymmetricKey;

namespace content {

namespace {

std::string HexedHash(const std::string& value) {
  std::string value_hash = base::SHA1HashString(value);
  std::string valued_hexed_hash = base::ToLowerASCII(
      base::HexEncode(value_hash.c_str(), value_hash.length()));
  return valued_hexed_hash;
}

void SizeRetrievedFromAllCaches(std::unique_ptr<int64_t> accumulator,
                                CacheStorage::SizeCallback callback) {
  base::ThreadTaskRunnerHandle::Get()->PostTask(
      FROM_HERE, base::BindOnce(std::move(callback), *accumulator));
}

}  // namespace

const char CacheStorage::kIndexFileName[] = "index.txt";
constexpr int64_t CacheStorage::kSizeUnknown;

struct CacheStorage::CacheMatchResponse {
  CacheMatchResponse() = default;
  ~CacheMatchResponse() = default;

  CacheStorageError error;
  blink::mojom::FetchAPIResponsePtr response;
  std::unique_ptr<storage::BlobDataHandle> blob_data_handle;
};

// Handles the loading and clean up of CacheStorageCache objects.
class CacheStorage::CacheLoader {
 public:
  using CacheCallback =
      base::OnceCallback<void(std::unique_ptr<CacheStorageCache>)>;
  using BoolCallback = base::OnceCallback<void(bool)>;
  using CacheStorageIndexLoadCallback =
      base::OnceCallback<void(std::unique_ptr<CacheStorageIndex>)>;

  CacheLoader(
      base::SequencedTaskRunner* cache_task_runner,
      storage::QuotaManagerProxy* quota_manager_proxy,
      base::WeakPtr<storage::BlobStorageContext> blob_context,
      CacheStorage* cache_storage,
      const url::Origin& origin,
      CacheStorageOwner owner)
      : cache_task_runner_(cache_task_runner),
        quota_manager_proxy_(quota_manager_proxy),
        blob_context_(blob_context),
        cache_storage_(cache_storage),
        origin_(origin),
        owner_(owner) {
    DCHECK(!origin_.opaque());
  }

  virtual ~CacheLoader() {}

  // Creates a CacheStorageCache with the given name. It does not attempt to
  // load the backend, that happens lazily when the cache is used.
  virtual std::unique_ptr<CacheStorageCache> CreateCache(
      const std::string& cache_name,
      int64_t cache_size,
      int64_t cache_padding,
      std::unique_ptr<SymmetricKey> cache_padding_key) = 0;

  // Deletes any pre-existing cache of the same name and then loads it.
  virtual void PrepareNewCacheDestination(const std::string& cache_name,
                                          CacheCallback callback) = 0;

  // After the backend has been deleted, do any extra house keeping such as
  // removing the cache's directory.
  virtual void CleanUpDeletedCache(CacheStorageCache* cache) = 0;

  // Writes the cache index to disk if applicable.
  virtual void WriteIndex(const CacheStorageIndex& index,
                          BoolCallback callback) = 0;

  // Loads the cache index from disk if applicable.
  virtual void LoadIndex(CacheStorageIndexLoadCallback callback) = 0;

  // Called when CacheStorage has created a cache. Used to hold onto a handle to
  // the cache if necessary.
  virtual void NotifyCacheCreated(const std::string& cache_name,
                                  CacheStorageCacheHandle cache_handle) {}

  // Notification that the cache for |cache_handle| has been doomed. If the
  // loader is holding a handle to the cache, it should drop it now.
  virtual void NotifyCacheDoomed(CacheStorageCacheHandle cache_handle) {}

 protected:
  scoped_refptr<base::SequencedTaskRunner> cache_task_runner_;

  // Owned by CacheStorage which owns this.
  storage::QuotaManagerProxy* quota_manager_proxy_;

  base::WeakPtr<storage::BlobStorageContext> blob_context_;

  // Raw pointer is safe because this object is owned by cache_storage_.
  CacheStorage* cache_storage_;

  url::Origin origin_;
  CacheStorageOwner owner_;
};

// Creates memory-only ServiceWorkerCaches. Because these caches have no
// persistent storage it is not safe to free them from memory if they might be
// used again. Therefore this class holds a reference to each cache until the
// cache is doomed.
class CacheStorage::MemoryLoader : public CacheStorage::CacheLoader {
 public:
  MemoryLoader(base::SequencedTaskRunner* cache_task_runner,
               storage::QuotaManagerProxy* quota_manager_proxy,
               base::WeakPtr<storage::BlobStorageContext> blob_context,
               CacheStorage* cache_storage,
               const url::Origin& origin,
               CacheStorageOwner owner)
      : CacheLoader(cache_task_runner,
                    quota_manager_proxy,
                    blob_context,
                    cache_storage,
                    origin,
                    owner) {}

  std::unique_ptr<CacheStorageCache> CreateCache(
      const std::string& cache_name,
      int64_t cache_size,
      int64_t cache_padding,
      std::unique_ptr<SymmetricKey> cache_padding_key) override {
    return CacheStorageCache::CreateMemoryCache(
        origin_, owner_, cache_name, cache_storage_, quota_manager_proxy_,
        blob_context_, storage::CopyDefaultPaddingKey());
  }

  void PrepareNewCacheDestination(const std::string& cache_name,
                                  CacheCallback callback) override {
    std::unique_ptr<CacheStorageCache> cache =
        CreateCache(cache_name, 0 /*cache_size*/, 0 /* cache_padding */,
                    storage::CopyDefaultPaddingKey());
    std::move(callback).Run(std::move(cache));
  }

  void CleanUpDeletedCache(CacheStorageCache* cache) override {}

  void WriteIndex(const CacheStorageIndex& index,
                  BoolCallback callback) override {
    std::move(callback).Run(true);
  }

  void LoadIndex(CacheStorageIndexLoadCallback callback) override {
    std::move(callback).Run(std::make_unique<CacheStorageIndex>());
  }

  void NotifyCacheCreated(const std::string& cache_name,
                          CacheStorageCacheHandle cache_handle) override {
    DCHECK(!base::ContainsKey(cache_handles_, cache_name));
    cache_handles_.insert(std::make_pair(cache_name, std::move(cache_handle)));
  }

  void NotifyCacheDoomed(CacheStorageCacheHandle cache_handle) override {
    DCHECK(
        base::ContainsKey(cache_handles_, cache_handle.value()->cache_name()));
    cache_handles_.erase(cache_handle.value()->cache_name());
  }

 private:
  typedef std::map<std::string, CacheStorageCacheHandle> CacheHandles;
  ~MemoryLoader() override {}

  // Keep a reference to each cache to ensure that it's not freed before the
  // client calls CacheStorage::Delete or the CacheStorage is
  // freed.
  CacheHandles cache_handles_;
};

class CacheStorage::SimpleCacheLoader : public CacheStorage::CacheLoader {
 public:
  SimpleCacheLoader(const base::FilePath& origin_path,
                    base::SequencedTaskRunner* cache_task_runner,
                    storage::QuotaManagerProxy* quota_manager_proxy,
                    base::WeakPtr<storage::BlobStorageContext> blob_context,
                    CacheStorage* cache_storage,
                    const url::Origin& origin,
                    CacheStorageOwner owner)
      : CacheLoader(cache_task_runner,
                    quota_manager_proxy,
                    blob_context,
                    cache_storage,
                    origin,
                    owner),
        origin_path_(origin_path),
        weak_ptr_factory_(this) {}

  std::unique_ptr<CacheStorageCache> CreateCache(
      const std::string& cache_name,
      int64_t cache_size,
      int64_t cache_padding,
      std::unique_ptr<SymmetricKey> cache_padding_key) override {
    DCHECK_CURRENTLY_ON(BrowserThread::IO);
    DCHECK(base::ContainsKey(cache_name_to_cache_dir_, cache_name));

    std::string cache_dir = cache_name_to_cache_dir_[cache_name];
    base::FilePath cache_path = origin_path_.AppendASCII(cache_dir);
    return CacheStorageCache::CreatePersistentCache(
        origin_, owner_, cache_name, cache_storage_, cache_path,
        quota_manager_proxy_, blob_context_, cache_size, cache_padding,
        std::move(cache_padding_key));
  }

  void PrepareNewCacheDestination(const std::string& cache_name,
                                  CacheCallback callback) override {
    DCHECK_CURRENTLY_ON(BrowserThread::IO);

    PostTaskAndReplyWithResult(
        cache_task_runner_.get(), FROM_HERE,
        base::BindOnce(&SimpleCacheLoader::PrepareNewCacheDirectoryInPool,
                       origin_path_),
        base::BindOnce(&SimpleCacheLoader::PrepareNewCacheCreateCache,
                       weak_ptr_factory_.GetWeakPtr(), cache_name,
                       std::move(callback)));
  }

  // Runs on the cache_task_runner_.
  static std::string PrepareNewCacheDirectoryInPool(
      const base::FilePath& origin_path) {
    std::string cache_dir;
    base::FilePath cache_path;
    do {
      cache_dir = base::GenerateGUID();
      cache_path = origin_path.AppendASCII(cache_dir);
    } while (base::PathExists(cache_path));

    return base::CreateDirectory(cache_path) ? cache_dir : "";
  }

  void PrepareNewCacheCreateCache(const std::string& cache_name,
                                  CacheCallback callback,
                                  const std::string& cache_dir) {
    if (cache_dir.empty()) {
      std::move(callback).Run(nullptr);
      return;
    }

    cache_name_to_cache_dir_[cache_name] = cache_dir;
    std::move(callback).Run(CreateCache(cache_name, CacheStorage::kSizeUnknown,
                                        CacheStorage::kSizeUnknown,
                                        storage::CopyDefaultPaddingKey()));
  }

  void CleanUpDeletedCache(CacheStorageCache* cache) override {
    DCHECK_CURRENTLY_ON(BrowserThread::IO);
    DCHECK(base::ContainsKey(doomed_cache_to_path_, cache));

    base::FilePath cache_path =
        origin_path_.AppendASCII(doomed_cache_to_path_[cache]);
    doomed_cache_to_path_.erase(cache);

    cache_task_runner_->PostTask(
        FROM_HERE,
        base::BindOnce(&SimpleCacheLoader::CleanUpDeleteCacheDirInPool,
                       cache_path));
  }

  static void CleanUpDeleteCacheDirInPool(const base::FilePath& cache_path) {
    base::DeleteFile(cache_path, true /* recursive */);
  }

  void WriteIndex(const CacheStorageIndex& index,
                  BoolCallback callback) override {
    DCHECK_CURRENTLY_ON(BrowserThread::IO);

    // 1. Create the index file as a string. (WriteIndex)
    // 2. Write the file to disk. (WriteIndexWriteToFileInPool)

    proto::CacheStorageIndex protobuf_index;
    // GetURL().spec() is used here rather than Serialize() to ensure
    // backwards compatibility with older data. The serializations are
    // subtly different, e.g. Origin does not include a trailing "/".
    // TODO(crbug.com/809329): Add a test for validating fields in the proto
    protobuf_index.set_origin(origin_.GetURL().spec());

    for (const auto& cache_metadata : index.ordered_cache_metadata()) {
      DCHECK(base::ContainsKey(cache_name_to_cache_dir_, cache_metadata.name));

      proto::CacheStorageIndex::Cache* index_cache = protobuf_index.add_cache();
      index_cache->set_name(cache_metadata.name);
      index_cache->set_cache_dir(cache_name_to_cache_dir_[cache_metadata.name]);
      if (cache_metadata.size == CacheStorage::kSizeUnknown)
        index_cache->clear_size();
      else
        index_cache->set_size(cache_metadata.size);
      index_cache->set_padding_key(cache_metadata.padding_key);
      index_cache->set_padding(cache_metadata.padding);
      index_cache->set_padding_version(
          CacheStorageCache::GetResponsePaddingVersion());
    }

    std::string serialized;
    bool success = protobuf_index.SerializeToString(&serialized);
    DCHECK(success);

    base::FilePath tmp_path = origin_path_.AppendASCII("index.txt.tmp");
    base::FilePath index_path =
        origin_path_.AppendASCII(CacheStorage::kIndexFileName);

    PostTaskAndReplyWithResult(
        cache_task_runner_.get(), FROM_HERE,
        base::BindOnce(&SimpleCacheLoader::WriteIndexWriteToFileInPool,
                       tmp_path, index_path, serialized),
        std::move(callback));
  }

  static bool WriteIndexWriteToFileInPool(const base::FilePath& tmp_path,
                                          const base::FilePath& index_path,
                                          const std::string& data) {
    int bytes_written = base::WriteFile(tmp_path, data.c_str(), data.size());
    if (bytes_written != base::checked_cast<int>(data.size())) {
      base::DeleteFile(tmp_path, /* recursive */ false);
      return false;
    }

    // Atomically rename the temporary index file to become the real one.
    return base::ReplaceFile(tmp_path, index_path, nullptr);
  }

  void LoadIndex(CacheStorageIndexLoadCallback callback) override {
    DCHECK_CURRENTLY_ON(BrowserThread::IO);

    PostTaskAndReplyWithResult(
        cache_task_runner_.get(), FROM_HERE,
        base::BindOnce(&SimpleCacheLoader::ReadAndMigrateIndexInPool,
                       origin_path_),
        base::BindOnce(&SimpleCacheLoader::LoadIndexDidReadIndex,
                       weak_ptr_factory_.GetWeakPtr(), std::move(callback)));
  }

  void LoadIndexDidReadIndex(CacheStorageIndexLoadCallback callback,
                             proto::CacheStorageIndex protobuf_index) {
    DCHECK_CURRENTLY_ON(BrowserThread::IO);

    std::unique_ptr<std::set<std::string>> cache_dirs(
        new std::set<std::string>);

    auto index = std::make_unique<CacheStorageIndex>();
    for (int i = 0, max = protobuf_index.cache_size(); i < max; ++i) {
      const proto::CacheStorageIndex::Cache& cache = protobuf_index.cache(i);
      DCHECK(cache.has_cache_dir());
      int64_t cache_size =
          cache.has_size() ? cache.size() : CacheStorage::kSizeUnknown;
      int64_t cache_padding;
      if (cache.has_padding()) {
        if (cache.has_padding_version() &&
            cache.padding_version() ==
                CacheStorageCache::GetResponsePaddingVersion()) {
          cache_padding = cache.padding();
        } else {
          // The padding algorithm version changed so set to unknown to force
          // recalculation.
          cache_padding = CacheStorage::kSizeUnknown;
        }
      } else {
        cache_padding = CacheStorage::kSizeUnknown;
      }

      std::string cache_padding_key =
          cache.has_padding_key() ? cache.padding_key()
                                  : storage::SerializeDefaultPaddingKey();

      index->Insert(CacheStorageIndex::CacheMetadata(
          cache.name(), cache_size, cache_padding,
          std::move(cache_padding_key)));
      cache_name_to_cache_dir_[cache.name()] = cache.cache_dir();
      cache_dirs->insert(cache.cache_dir());
    }

    cache_task_runner_->PostTask(
        FROM_HERE, base::BindOnce(&DeleteUnreferencedCachesInPool, origin_path_,
                                  std::move(cache_dirs)));
    std::move(callback).Run(std::move(index));
  }

  void NotifyCacheDoomed(CacheStorageCacheHandle cache_handle) override {
    DCHECK(base::ContainsKey(cache_name_to_cache_dir_,
                             cache_handle.value()->cache_name()));
    auto iter =
        cache_name_to_cache_dir_.find(cache_handle.value()->cache_name());
    doomed_cache_to_path_[cache_handle.value()] = iter->second;
    cache_name_to_cache_dir_.erase(iter);
  }

 private:
  friend class MigratedLegacyCacheDirectoryNameTest;
  ~SimpleCacheLoader() override {}

  // Iterates over the caches and deletes any directory not found in
  // |cache_dirs|. Runs on cache_task_runner_
  static void DeleteUnreferencedCachesInPool(
      const base::FilePath& cache_base_dir,
      std::unique_ptr<std::set<std::string>> cache_dirs) {
    base::FileEnumerator file_enum(cache_base_dir, false /* recursive */,
                                   base::FileEnumerator::DIRECTORIES);
    std::vector<base::FilePath> dirs_to_delete;
    {
      base::FilePath cache_path;
      while (!(cache_path = file_enum.Next()).empty()) {
        if (!base::ContainsKey(*cache_dirs,
                               cache_path.BaseName().AsUTF8Unsafe()))
          dirs_to_delete.push_back(cache_path);
      }
    }

    for (const base::FilePath& cache_path : dirs_to_delete)
      base::DeleteFile(cache_path, true /* recursive */);
  }

  // Runs on cache_task_runner_
  static proto::CacheStorageIndex ReadAndMigrateIndexInPool(
      const base::FilePath& origin_path) {
    const base::FilePath index_path =
        origin_path.AppendASCII(CacheStorage::kIndexFileName);

    proto::CacheStorageIndex index;
    std::string body;
    if (!base::ReadFileToString(index_path, &body) ||
        !index.ParseFromString(body))
      return proto::CacheStorageIndex();
    body.clear();

    base::File::Info file_info;
    base::Time index_last_modified;
    if (GetFileInfo(index_path, &file_info))
      index_last_modified = file_info.last_modified;
    bool index_modified = false;

    // Look for caches that have no cache_dir. Give any such caches a directory
    // with a random name and move them there. Then, rewrite the index file.
    // Additionally invalidate the size of any index entries where the cache was
    // modified after the index (making it out-of-date).
    for (int i = 0, max = index.cache_size(); i < max; ++i) {
      const proto::CacheStorageIndex::Cache& cache = index.cache(i);
      if (cache.has_cache_dir()) {
        if (cache.has_size()) {
          base::FilePath cache_dir = origin_path.AppendASCII(cache.cache_dir());
          if (!GetFileInfo(cache_dir, &file_info) ||
              index_last_modified <= file_info.last_modified) {
            // Index is older than this cache, so invalidate index entries that
            // may change as a result of cache operations.
            index.mutable_cache(i)->clear_size();
          }
        }
      } else {
        // Find a new home for the cache.
        base::FilePath legacy_cache_path =
            origin_path.AppendASCII(HexedHash(cache.name()));
        std::string cache_dir;
        base::FilePath cache_path;
        do {
          cache_dir = base::GenerateGUID();
          cache_path = origin_path.AppendASCII(cache_dir);
        } while (base::PathExists(cache_path));

        if (!base::Move(legacy_cache_path, cache_path)) {
          // If the move fails then the cache is in a bad state. Return an empty
          // index so that the CacheStorage can start fresh. The unreferenced
          // caches will be discarded later in initialization.
          return proto::CacheStorageIndex();
        }

        index.mutable_cache(i)->set_cache_dir(cache_dir);
        index.mutable_cache(i)->clear_size();
        index_modified = true;
      }
    }

    if (index_modified) {
      base::FilePath tmp_path = origin_path.AppendASCII("index.txt.tmp");
      if (!index.SerializeToString(&body) ||
          !WriteIndexWriteToFileInPool(tmp_path, index_path, body)) {
        return proto::CacheStorageIndex();
      }
    }

    return index;
  }

  const base::FilePath origin_path_;
  std::map<std::string, std::string> cache_name_to_cache_dir_;
  std::map<CacheStorageCache*, std::string> doomed_cache_to_path_;

  base::WeakPtrFactory<SimpleCacheLoader> weak_ptr_factory_;
};

CacheStorage::CacheStorage(
    const base::FilePath& path,
    bool memory_only,
    base::SequencedTaskRunner* cache_task_runner,
    scoped_refptr<storage::QuotaManagerProxy> quota_manager_proxy,
    base::WeakPtr<storage::BlobStorageContext> blob_context,
    CacheStorageManager* cache_storage_manager,
    const url::Origin& origin,
    CacheStorageOwner owner)
    : initialized_(false),
      initializing_(false),
      memory_only_(memory_only),
      scheduler_(
          new CacheStorageScheduler(CacheStorageSchedulerClient::kStorage)),
      origin_path_(path),
      cache_task_runner_(cache_task_runner),
      quota_manager_proxy_(quota_manager_proxy),
      origin_(origin),
      owner_(owner),
      cache_storage_manager_(cache_storage_manager),
      weak_factory_(this) {
  if (memory_only)
    cache_loader_.reset(new MemoryLoader(cache_task_runner_.get(),
                                         quota_manager_proxy.get(),
                                         blob_context, this, origin, owner));
  else
    cache_loader_.reset(new SimpleCacheLoader(
        origin_path_, cache_task_runner_.get(), quota_manager_proxy.get(),
        blob_context, this, origin, owner));
}

CacheStorage::~CacheStorage() {
}

CacheStorageHandle CacheStorage::CreateHandle() {
  return CacheStorageHandle(weak_factory_.GetWeakPtr());
}

void CacheStorage::AddHandleRef() {
  DCHECK_CALLED_ON_VALID_SEQUENCE(sequence_checker_);
  handle_ref_count_ += 1;
}

void CacheStorage::DropHandleRef() {
  DCHECK_CALLED_ON_VALID_SEQUENCE(sequence_checker_);
  DCHECK(handle_ref_count_ > 0);
  handle_ref_count_ -= 1;
  if (!handle_ref_count_ && cache_storage_manager_) {
    ReleaseUnreferencedCaches();
    cache_storage_manager_->CacheStorageUnreferenced(this, origin_, owner_);
  }
}

void CacheStorage::OpenCache(const std::string& cache_name,
                             int64_t trace_id,
                             CacheAndErrorCallback callback) {
  DCHECK_CURRENTLY_ON(BrowserThread::IO);

  if (!initialized_)
    LazyInit();

  quota_manager_proxy_->NotifyStorageAccessed(
      CacheStorageQuotaClient::GetIDFromOwner(owner_), origin_,
      StorageType::kTemporary);

  // TODO: Hold a handle to this CacheStorage instance while executing
  //       operations to better support use by internal code that may
  //       start a single operation without explicitly maintaining a
  //       handle.
  scheduler_->ScheduleOperation(
      CacheStorageSchedulerOp::kOpen,
      base::BindOnce(&CacheStorage::OpenCacheImpl, weak_factory_.GetWeakPtr(),
                     cache_name, trace_id,
                     scheduler_->WrapCallbackToRunNext(std::move(callback))));
}

void CacheStorage::HasCache(const std::string& cache_name,
                            int64_t trace_id,
                            BoolAndErrorCallback callback) {
  DCHECK_CURRENTLY_ON(BrowserThread::IO);

  if (!initialized_)
    LazyInit();

  quota_manager_proxy_->NotifyStorageAccessed(
      CacheStorageQuotaClient::GetIDFromOwner(owner_), origin_,
      StorageType::kTemporary);

  scheduler_->ScheduleOperation(
      CacheStorageSchedulerOp::kHas,
      base::BindOnce(&CacheStorage::HasCacheImpl, weak_factory_.GetWeakPtr(),
                     cache_name, trace_id,
                     scheduler_->WrapCallbackToRunNext(std::move(callback))));
}

void CacheStorage::DoomCache(const std::string& cache_name,
                             int64_t trace_id,
                             ErrorCallback callback) {
  DCHECK_CURRENTLY_ON(BrowserThread::IO);

  if (!initialized_)
    LazyInit();

  quota_manager_proxy_->NotifyStorageAccessed(
      CacheStorageQuotaClient::GetIDFromOwner(owner_), origin_,
      StorageType::kTemporary);

  scheduler_->ScheduleOperation(
      CacheStorageSchedulerOp::kDelete,
      base::BindOnce(&CacheStorage::DoomCacheImpl, weak_factory_.GetWeakPtr(),
                     cache_name, trace_id,
                     scheduler_->WrapCallbackToRunNext(std::move(callback))));
}

void CacheStorage::EnumerateCaches(int64_t trace_id,
                                   EnumerateCachesCallback callback) {
  DCHECK_CURRENTLY_ON(BrowserThread::IO);

  if (!initialized_)
    LazyInit();

  quota_manager_proxy_->NotifyStorageAccessed(
      CacheStorageQuotaClient::GetIDFromOwner(owner_), origin_,
      StorageType::kTemporary);

  scheduler_->ScheduleOperation(
      CacheStorageSchedulerOp::kKeys,
      base::BindOnce(&CacheStorage::EnumerateCachesImpl,
                     weak_factory_.GetWeakPtr(), trace_id,
                     scheduler_->WrapCallbackToRunNext(std::move(callback))));
}

void CacheStorage::MatchCache(const std::string& cache_name,
                              blink::mojom::FetchAPIRequestPtr request,
                              blink::mojom::CacheQueryOptionsPtr match_options,
                              int64_t trace_id,
                              CacheStorageCache::ResponseCallback callback) {
  DCHECK_CURRENTLY_ON(BrowserThread::IO);

  if (!initialized_)
    LazyInit();

  quota_manager_proxy_->NotifyStorageAccessed(
      CacheStorageQuotaClient::GetIDFromOwner(owner_), origin_,
      StorageType::kTemporary);

  scheduler_->ScheduleOperation(
      CacheStorageSchedulerOp::kMatch,
      base::BindOnce(&CacheStorage::MatchCacheImpl, weak_factory_.GetWeakPtr(),
                     cache_name, std::move(request), std::move(match_options),
                     trace_id,
                     scheduler_->WrapCallbackToRunNext(std::move(callback))));
}

void CacheStorage::MatchAllCaches(
    blink::mojom::FetchAPIRequestPtr request,
    blink::mojom::CacheQueryOptionsPtr match_options,
    int64_t trace_id,
    CacheStorageCache::ResponseCallback callback) {
  DCHECK_CURRENTLY_ON(BrowserThread::IO);

  if (!initialized_)
    LazyInit();

  quota_manager_proxy_->NotifyStorageAccessed(
      CacheStorageQuotaClient::GetIDFromOwner(owner_), origin_,
      StorageType::kTemporary);

  scheduler_->ScheduleOperation(
      CacheStorageSchedulerOp::kMatchAll,
      base::BindOnce(&CacheStorage::MatchAllCachesImpl,
                     weak_factory_.GetWeakPtr(), std::move(request),
                     std::move(match_options), trace_id,
                     scheduler_->WrapCallbackToRunNext(std::move(callback))));
}

void CacheStorage::WriteToCache(const std::string& cache_name,
                                blink::mojom::FetchAPIRequestPtr request,
                                blink::mojom::FetchAPIResponsePtr response,
                                int64_t trace_id,
                                CacheStorage::ErrorCallback callback) {
  DCHECK_CURRENTLY_ON(BrowserThread::IO);

  if (!initialized_)
    LazyInit();

  quota_manager_proxy_->NotifyStorageAccessed(
      CacheStorageQuotaClient::GetIDFromOwner(owner_), origin_,
      StorageType::kTemporary);

  scheduler_->ScheduleOperation(
      CacheStorageSchedulerOp::kPut,
      base::BindOnce(&CacheStorage::WriteToCacheImpl,
                     weak_factory_.GetWeakPtr(), cache_name, std::move(request),
                     std::move(response), trace_id,
                     scheduler_->WrapCallbackToRunNext(std::move(callback))));
}

void CacheStorage::GetSizeThenCloseAllCaches(SizeCallback callback) {
  DCHECK_CURRENTLY_ON(BrowserThread::IO);

  if (!initialized_)
    LazyInit();

  scheduler_->ScheduleOperation(
      CacheStorageSchedulerOp::kSizeThenClose,
      base::BindOnce(&CacheStorage::GetSizeThenCloseAllCachesImpl,
                     weak_factory_.GetWeakPtr(),
                     scheduler_->WrapCallbackToRunNext(std::move(callback))));
}

void CacheStorage::Size(CacheStorage::SizeCallback callback) {
  DCHECK_CURRENTLY_ON(BrowserThread::IO);

  if (!initialized_)
    LazyInit();

  scheduler_->ScheduleOperation(
      CacheStorageSchedulerOp::kSize,
      base::BindOnce(&CacheStorage::SizeImpl, weak_factory_.GetWeakPtr(),
                     scheduler_->WrapCallbackToRunNext(std::move(callback))));
}

void CacheStorage::ResetManager() {
  cache_storage_manager_ = nullptr;
}

void CacheStorage::NotifyCacheContentChanged(const std::string& cache_name) {
  if (cache_storage_manager_)
    cache_storage_manager_->NotifyCacheContentChanged(origin_, cache_name);
}

void CacheStorage::ScheduleWriteIndex() {
  static const int64_t kWriteIndexDelaySecs = 5;
  DCHECK_CURRENTLY_ON(BrowserThread::IO);
  index_write_task_.Reset(base::BindOnce(&CacheStorage::WriteIndex,
                                         weak_factory_.GetWeakPtr(),
                                         base::DoNothing::Once<bool>()));
  base::ThreadTaskRunnerHandle::Get()->PostDelayedTask(
      FROM_HERE, index_write_task_.callback(),
      base::TimeDelta::FromSeconds(kWriteIndexDelaySecs));
}

void CacheStorage::WriteIndex(base::OnceCallback<void(bool)> callback) {
  DCHECK_CURRENTLY_ON(BrowserThread::IO);
  scheduler_->ScheduleOperation(
      CacheStorageSchedulerOp::kWriteIndex,
      base::BindOnce(&CacheStorage::WriteIndexImpl, weak_factory_.GetWeakPtr(),
                     scheduler_->WrapCallbackToRunNext(std::move(callback))));
}

void CacheStorage::WriteIndexImpl(base::OnceCallback<void(bool)> callback) {
  DCHECK_CURRENTLY_ON(BrowserThread::IO);
  cache_loader_->WriteIndex(*cache_index_, std::move(callback));
}

bool CacheStorage::InitiateScheduledIndexWriteForTest(
    base::OnceCallback<void(bool)> callback) {
  DCHECK_CURRENTLY_ON(BrowserThread::IO);
  if (index_write_pending()) {
    index_write_task_.Cancel();
    WriteIndex(std::move(callback));
    return true;
  }
  std::move(callback).Run(true /* success */);
  return false;
}

void CacheStorage::CacheSizeUpdated(const CacheStorageCache* cache) {
  // Should not be called for doomed caches.
  DCHECK(!base::ContainsKey(doomed_caches_,
                            const_cast<CacheStorageCache*>(cache)));
  DCHECK_NE(cache->cache_padding(), kSizeUnknown);
  bool size_changed =
      cache_index_->SetCacheSize(cache->cache_name(), cache->cache_size());
  bool padding_changed = cache_index_->SetCachePadding(cache->cache_name(),
                                                       cache->cache_padding());
  if (size_changed || padding_changed)
    ScheduleWriteIndex();
}

void CacheStorage::StartAsyncOperationForTesting() {
  scheduler_->ScheduleOperation(CacheStorageSchedulerOp::kTest,
                                base::DoNothing());
}

void CacheStorage::CompleteAsyncOperationForTesting() {
  scheduler_->CompleteOperationAndRunNext();
}

// Init is run lazily so that it is called on the proper MessageLoop.
void CacheStorage::LazyInit() {
  DCHECK_CURRENTLY_ON(BrowserThread::IO);
  DCHECK(!initialized_);

  if (initializing_)
    return;

  DCHECK(!scheduler_->ScheduledOperations());

  initializing_ = true;
  scheduler_->ScheduleOperation(
      CacheStorageSchedulerOp::kInit,
      base::BindOnce(&CacheStorage::LazyInitImpl, weak_factory_.GetWeakPtr()));
}

void CacheStorage::LazyInitImpl() {
  DCHECK_CURRENTLY_ON(BrowserThread::IO);
  DCHECK(!initialized_);
  DCHECK(initializing_);

  // 1. Get the cache index (async call)
  // 2. For each cache name, load the cache (async call)
  // 3. Once each load is complete, update the map variables.
  // 4. Call the list of waiting callbacks.

  cache_loader_->LoadIndex(base::BindOnce(&CacheStorage::LazyInitDidLoadIndex,
                                          weak_factory_.GetWeakPtr()));
}

void CacheStorage::LazyInitDidLoadIndex(
    std::unique_ptr<CacheStorageIndex> index) {
  DCHECK_CURRENTLY_ON(BrowserThread::IO);
  DCHECK(cache_map_.empty());

  for (const auto& cache_metadata : index->ordered_cache_metadata()) {
    cache_map_.insert(std::make_pair(cache_metadata.name, nullptr));
  }

  DCHECK(!cache_index_);
  cache_index_ = std::move(index);

  initializing_ = false;
  initialized_ = true;

  scheduler_->CompleteOperationAndRunNext();
}

void CacheStorage::OpenCacheImpl(const std::string& cache_name,
                                 int64_t trace_id,
                                 CacheAndErrorCallback callback) {
  TRACE_EVENT_WITH_FLOW1("CacheStorage", "CacheStorage::OpenCacheImpl",
                         TRACE_ID_GLOBAL(trace_id),
                         TRACE_EVENT_FLAG_FLOW_IN | TRACE_EVENT_FLAG_FLOW_OUT,
                         "cache_name", cache_name);
  CacheStorageCacheHandle cache_handle = GetLoadedCache(cache_name);
  if (cache_handle.value()) {
    std::move(callback).Run(std::move(cache_handle),
                            CacheStorageError::kSuccess);
    return;
  }

  cache_loader_->PrepareNewCacheDestination(
      cache_name, base::BindOnce(&CacheStorage::CreateCacheDidCreateCache,
                                 weak_factory_.GetWeakPtr(), cache_name,
                                 trace_id, std::move(callback)));
}

void CacheStorage::CreateCacheDidCreateCache(
    const std::string& cache_name,
    int64_t trace_id,
    CacheAndErrorCallback callback,
    std::unique_ptr<CacheStorageCache> cache) {
  DCHECK_CURRENTLY_ON(BrowserThread::IO);

  TRACE_EVENT_WITH_FLOW0("CacheStorage",
                         "CacheStorage::CreateCacheDidCreateCache",
                         TRACE_ID_GLOBAL(trace_id),
                         TRACE_EVENT_FLAG_FLOW_IN | TRACE_EVENT_FLAG_FLOW_OUT);

  UMA_HISTOGRAM_BOOLEAN("ServiceWorkerCache.CreateCacheStorageResult",
                        static_cast<bool>(cache));

  if (!cache) {
    std::move(callback).Run(
        CacheStorageCacheHandle(),
        MakeErrorStorage(ErrorStorageType::kDidCreateNullCache));
    return;
  }

  CacheStorageCache* cache_ptr = cache.get();

  cache_map_.insert(std::make_pair(cache_name, std::move(cache)));
  cache_index_->Insert(CacheStorageIndex::CacheMetadata(
      cache_name, cache_ptr->cache_size(), cache_ptr->cache_padding(),
      cache_ptr->cache_padding_key()->key()));

  CacheStorageCacheHandle handle = cache_ptr->CreateHandle();
  cache_loader_->WriteIndex(
      *cache_index_,
      base::BindOnce(&CacheStorage::CreateCacheDidWriteIndex,
                     weak_factory_.GetWeakPtr(), std::move(callback),
                     cache_ptr->CreateHandle(), trace_id));

  cache_loader_->NotifyCacheCreated(cache_name, std::move(handle));
  if (cache_storage_manager_)
    cache_storage_manager_->NotifyCacheListChanged(origin_);
}

void CacheStorage::CreateCacheDidWriteIndex(
    CacheAndErrorCallback callback,
    CacheStorageCacheHandle cache_handle,
    int64_t trace_id,
    bool success) {
  DCHECK_CURRENTLY_ON(BrowserThread::IO);
  DCHECK(cache_handle.value());

  TRACE_EVENT_WITH_FLOW0("CacheStorage",
                         "CacheStorage::CreateCacheDidWriteIndex",
                         TRACE_ID_GLOBAL(trace_id),
                         TRACE_EVENT_FLAG_FLOW_IN | TRACE_EVENT_FLAG_FLOW_OUT);

  // TODO(jkarlin): Handle !success.

  std::move(callback).Run(std::move(cache_handle), CacheStorageError::kSuccess);
}

void CacheStorage::HasCacheImpl(const std::string& cache_name,
                                int64_t trace_id,
                                BoolAndErrorCallback callback) {
  TRACE_EVENT_WITH_FLOW1("CacheStorage", "CacheStorage::HasCacheImpl",
                         TRACE_ID_GLOBAL(trace_id),
                         TRACE_EVENT_FLAG_FLOW_IN | TRACE_EVENT_FLAG_FLOW_OUT,
                         "cache_name", cache_name);
  bool has_cache = base::ContainsKey(cache_map_, cache_name);
  std::move(callback).Run(has_cache, CacheStorageError::kSuccess);
}

void CacheStorage::DoomCacheImpl(const std::string& cache_name,
                                 int64_t trace_id,
                                 ErrorCallback callback) {
  TRACE_EVENT_WITH_FLOW1("CacheStorage", "CacheStorage::DoomCacheImpl",
                         TRACE_ID_GLOBAL(trace_id),
                         TRACE_EVENT_FLAG_FLOW_IN | TRACE_EVENT_FLAG_FLOW_OUT,
                         "cache_name", cache_name);
  CacheStorageCacheHandle cache_handle = GetLoadedCache(cache_name);
  if (!cache_handle.value()) {
    base::ThreadTaskRunnerHandle::Get()->PostTask(
        FROM_HERE,
        base::BindOnce(std::move(callback), CacheStorageError::kErrorNotFound));
    return;
  }

  cache_handle.value()->SetObserver(nullptr);
  cache_index_->DoomCache(cache_name);
  cache_loader_->WriteIndex(
      *cache_index_,
      base::BindOnce(&CacheStorage::DeleteCacheDidWriteIndex,
                     weak_factory_.GetWeakPtr(), std::move(cache_handle),
                     std::move(callback), trace_id));
}

void CacheStorage::DeleteCacheDidWriteIndex(
    CacheStorageCacheHandle cache_handle,
    ErrorCallback callback,
    int64_t trace_id,
    bool success) {
  DCHECK_CURRENTLY_ON(BrowserThread::IO);

  TRACE_EVENT_WITH_FLOW0("CacheStorage",
                         "CacheStorage::DeleteCacheDidWriteIndex",
                         TRACE_ID_GLOBAL(trace_id),
                         TRACE_EVENT_FLAG_FLOW_IN | TRACE_EVENT_FLAG_FLOW_OUT);

  if (!success) {
    // Undo any changes if the index couldn't be written to disk.
    cache_index_->RestoreDoomedCache();
    cache_handle.value()->SetObserver(this);
    std::move(callback).Run(
        MakeErrorStorage(ErrorStorageType::kDeleteCacheFailed));
    return;
  }

  cache_index_->FinalizeDoomedCache();

  auto map_iter = cache_map_.find(cache_handle.value()->cache_name());
  DCHECK(map_iter != cache_map_.end());

  doomed_caches_.insert(
      std::make_pair(map_iter->second.get(), std::move(map_iter->second)));
  cache_map_.erase(map_iter);

  cache_loader_->NotifyCacheDoomed(std::move(cache_handle));
  if (cache_storage_manager_)
    cache_storage_manager_->NotifyCacheListChanged(origin_);

  std::move(callback).Run(CacheStorageError::kSuccess);
}

// Call this once the last handle to a doomed cache is gone. It's okay if this
// doesn't get to complete before shutdown, the cache will be removed from disk
// on next startup in that case.
void CacheStorage::DeleteCacheFinalize(CacheStorageCache* doomed_cache) {
  doomed_cache->Size(base::BindOnce(&CacheStorage::DeleteCacheDidGetSize,
                                    weak_factory_.GetWeakPtr(), doomed_cache));
}

void CacheStorage::DeleteCacheDidGetSize(CacheStorageCache* doomed_cache,
                                         int64_t cache_size) {
  quota_manager_proxy_->NotifyStorageModified(
      CacheStorageQuotaClient::GetIDFromOwner(owner_), origin_,
      StorageType::kTemporary, -1 * cache_size);

  cache_loader_->CleanUpDeletedCache(doomed_cache);
  auto doomed_caches_iter = doomed_caches_.find(doomed_cache);
  DCHECK(doomed_caches_iter != doomed_caches_.end());
  doomed_caches_.erase(doomed_caches_iter);
}

void CacheStorage::EnumerateCachesImpl(int64_t trace_id,
                                       EnumerateCachesCallback callback) {
  TRACE_EVENT_WITH_FLOW0("CacheStorage", "CacheStorage::EnumerateCachesImpl",
                         TRACE_ID_GLOBAL(trace_id),
                         TRACE_EVENT_FLAG_FLOW_IN | TRACE_EVENT_FLAG_FLOW_OUT);

  std::vector<std::string> list;

  for (const auto& metadata : cache_index_->ordered_cache_metadata()) {
    list.push_back(metadata.name);
  }

  std::move(callback).Run(std::move(list));
}

void CacheStorage::MatchCacheImpl(
    const std::string& cache_name,
    blink::mojom::FetchAPIRequestPtr request,
    blink::mojom::CacheQueryOptionsPtr match_options,
    int64_t trace_id,
    CacheStorageCache::ResponseCallback callback) {
  TRACE_EVENT_WITH_FLOW2(
      "CacheStorage", "CacheStorage::MatchCacheImpl", TRACE_ID_GLOBAL(trace_id),
      TRACE_EVENT_FLAG_FLOW_IN | TRACE_EVENT_FLAG_FLOW_OUT, "cache_name",
      cache_name, "request", CacheStorageTracedValue(request));

  CacheStorageCacheHandle cache_handle = GetLoadedCache(cache_name);

  if (!cache_handle.value()) {
    std::move(callback).Run(CacheStorageError::kErrorCacheNameNotFound,
                            nullptr);
    return;
  }

  // Pass the cache handle along to the callback to keep the cache open until
  // match is done.
  CacheStorageCache* cache_ptr = cache_handle.value();
  cache_ptr->Match(
      std::move(request), std::move(match_options), trace_id,
      base::BindOnce(&CacheStorage::MatchCacheDidMatch,
                     weak_factory_.GetWeakPtr(), std::move(cache_handle),
                     trace_id, std::move(callback)));
}

void CacheStorage::MatchCacheDidMatch(
    CacheStorageCacheHandle cache_handle,
    int64_t trace_id,
    CacheStorageCache::ResponseCallback callback,
    CacheStorageError error,
    blink::mojom::FetchAPIResponsePtr response) {
  TRACE_EVENT_WITH_FLOW0("CacheStorage", "CacheStorage::MatchCacheDidMatch",
                         TRACE_ID_GLOBAL(trace_id),
                         TRACE_EVENT_FLAG_FLOW_IN | TRACE_EVENT_FLAG_FLOW_OUT);
  std::move(callback).Run(error, std::move(response));
}

void CacheStorage::MatchAllCachesImpl(
    blink::mojom::FetchAPIRequestPtr request,
    blink::mojom::CacheQueryOptionsPtr match_options,
    int64_t trace_id,
    CacheStorageCache::ResponseCallback callback) {
  TRACE_EVENT_WITH_FLOW0("CacheStorage", "CacheStorage::MatchAllCachesImpl",
                         TRACE_ID_GLOBAL(trace_id),
                         TRACE_EVENT_FLAG_FLOW_IN | TRACE_EVENT_FLAG_FLOW_OUT);

  std::vector<CacheMatchResponse>* match_responses =
      new std::vector<CacheMatchResponse>(cache_index_->num_entries());

  base::RepeatingClosure barrier_closure = base::BarrierClosure(
      cache_index_->num_entries(),
      base::BindOnce(
          &CacheStorage::MatchAllCachesDidMatchAll, weak_factory_.GetWeakPtr(),
          base::WrapUnique(match_responses), trace_id, std::move(callback)));

  size_t idx = 0;
  for (const auto& cache_metadata : cache_index_->ordered_cache_metadata()) {
    CacheStorageCacheHandle cache_handle = GetLoadedCache(cache_metadata.name);
    DCHECK(cache_handle.value());

    CacheStorageCache* cache_ptr = cache_handle.value();
    cache_ptr->Match(
        BackgroundFetchSettledFetch::CloneRequest(request),
        match_options ? match_options->Clone() : nullptr, trace_id,
        base::BindOnce(&CacheStorage::MatchAllCachesDidMatch,
                       weak_factory_.GetWeakPtr(), std::move(cache_handle),
                       &match_responses->at(idx), barrier_closure, trace_id));
    idx++;
  }
}

void CacheStorage::MatchAllCachesDidMatch(
    CacheStorageCacheHandle cache_handle,
    CacheMatchResponse* out_match_response,
    const base::RepeatingClosure& barrier_closure,
    int64_t trace_id,
    CacheStorageError error,
    blink::mojom::FetchAPIResponsePtr response) {
  TRACE_EVENT_WITH_FLOW0("CacheStorage", "CacheStorage::MatchAllCachesDidMatch",
                         TRACE_ID_GLOBAL(trace_id),
                         TRACE_EVENT_FLAG_FLOW_IN | TRACE_EVENT_FLAG_FLOW_OUT);
  out_match_response->error = error;
  out_match_response->response = std::move(response);
  barrier_closure.Run();
}

void CacheStorage::MatchAllCachesDidMatchAll(
    std::unique_ptr<std::vector<CacheMatchResponse>> match_responses,
    int64_t trace_id,
    CacheStorageCache::ResponseCallback callback) {
  TRACE_EVENT_WITH_FLOW0("CacheStorage",
                         "CacheStorage::MatchAllCachesDidMatchAll",
                         TRACE_ID_GLOBAL(trace_id),
                         TRACE_EVENT_FLAG_FLOW_IN | TRACE_EVENT_FLAG_FLOW_OUT);
  for (CacheMatchResponse& match_response : *match_responses) {
    if (match_response.error == CacheStorageError::kErrorNotFound)
      continue;
    std::move(callback).Run(match_response.error,
                            std::move(match_response.response));
    return;
  }
  std::move(callback).Run(CacheStorageError::kErrorNotFound, nullptr);
}

void CacheStorage::WriteToCacheImpl(const std::string& cache_name,
                                    blink::mojom::FetchAPIRequestPtr request,
                                    blink::mojom::FetchAPIResponsePtr response,
                                    int64_t trace_id,
                                    CacheStorage::ErrorCallback callback) {
  TRACE_EVENT_WITH_FLOW2("CacheStorage", "CacheStorage::WriteToCacheImpl",
                         TRACE_ID_GLOBAL(trace_id),
                         TRACE_EVENT_FLAG_FLOW_IN | TRACE_EVENT_FLAG_FLOW_OUT,
                         "cache_name", cache_name, "request",
                         CacheStorageTracedValue(request));

  CacheStorageCacheHandle cache_handle = GetLoadedCache(cache_name);

  if (!cache_handle.value()) {
    std::move(callback).Run(CacheStorageError::kErrorCacheNameNotFound);
    return;
  }

  CacheStorageCache* cache_ptr = cache_handle.value();
  DCHECK(cache_ptr);

  cache_ptr->Put(std::move(request), std::move(response), trace_id,
                 std::move(callback));
}

CacheStorageCacheHandle CacheStorage::GetLoadedCache(
    const std::string& cache_name) {
  DCHECK_CURRENTLY_ON(BrowserThread::IO);
  DCHECK(initialized_);

  auto map_iter = cache_map_.find(cache_name);
  if (map_iter == cache_map_.end())
    return CacheStorageCacheHandle();

  CacheStorageCache* cache = map_iter->second.get();

  if (!cache) {
    const CacheStorageIndex::CacheMetadata* metadata =
        cache_index_->GetMetadata(cache_name);
    DCHECK(metadata);
    std::unique_ptr<CacheStorageCache> new_cache = cache_loader_->CreateCache(
        cache_name, metadata->size, metadata->padding,
        storage::DeserializePaddingKey(metadata->padding_key));
    CacheStorageCache* cache_ptr = new_cache.get();
    map_iter->second = std::move(new_cache);

    return cache_ptr->CreateHandle();
  }

  return cache->CreateHandle();
}

void CacheStorage::SizeRetrievedFromCache(CacheStorageCacheHandle cache_handle,
                                          base::OnceClosure closure,
                                          int64_t* accumulator,
                                          int64_t size) {
  if (doomed_caches_.find(cache_handle.value()) == doomed_caches_.end())
    cache_index_->SetCacheSize(cache_handle.value()->cache_name(), size);
  *accumulator += size;
  std::move(closure).Run();
}

void CacheStorage::GetSizeThenCloseAllCachesImpl(SizeCallback callback) {
  DCHECK_CURRENTLY_ON(BrowserThread::IO);
  DCHECK(initialized_);

  std::unique_ptr<int64_t> accumulator(new int64_t(0));
  int64_t* accumulator_ptr = accumulator.get();

  base::RepeatingClosure barrier_closure = base::BarrierClosure(
      cache_index_->num_entries() + doomed_caches_.size(),
      base::BindOnce(&SizeRetrievedFromAllCaches, std::move(accumulator),
                     std::move(callback)));

  for (const auto& cache_metadata : cache_index_->ordered_cache_metadata()) {
    auto cache_handle = GetLoadedCache(cache_metadata.name);
    CacheStorageCache* cache = cache_handle.value();
    cache->GetSizeThenClose(base::BindOnce(
        &CacheStorage::SizeRetrievedFromCache, weak_factory_.GetWeakPtr(),
        std::move(cache_handle), barrier_closure, accumulator_ptr));
  }

  for (const auto& cache_it : doomed_caches_) {
    CacheStorageCache* cache = cache_it.first;
    cache->GetSizeThenClose(base::BindOnce(
        &CacheStorage::SizeRetrievedFromCache, weak_factory_.GetWeakPtr(),
        cache->CreateHandle(), barrier_closure, accumulator_ptr));
  }
}

void CacheStorage::SizeImpl(SizeCallback callback) {
  DCHECK_CURRENTLY_ON(BrowserThread::IO);
  DCHECK(initialized_);

  if (cache_index_->GetPaddedStorageSize() != kSizeUnknown) {
    base::ThreadTaskRunnerHandle::Get()->PostTask(
        FROM_HERE, base::BindOnce(std::move(callback),
                                  cache_index_->GetPaddedStorageSize()));
    return;
  }

  std::unique_ptr<int64_t> accumulator(new int64_t(0));
  int64_t* accumulator_ptr = accumulator.get();

  base::RepeatingClosure barrier_closure = base::BarrierClosure(
      cache_index_->num_entries(),
      base::BindOnce(&SizeRetrievedFromAllCaches, std::move(accumulator),
                     std::move(callback)));

  for (const auto& cache_metadata : cache_index_->ordered_cache_metadata()) {
    if (cache_metadata.size != CacheStorage::kSizeUnknown) {
      *accumulator_ptr += cache_metadata.size;
      barrier_closure.Run();
      continue;
    }
    CacheStorageCacheHandle cache_handle = GetLoadedCache(cache_metadata.name);
    CacheStorageCache* cache = cache_handle.value();
    cache->Size(base::BindOnce(
        &CacheStorage::SizeRetrievedFromCache, weak_factory_.GetWeakPtr(),
        std::move(cache_handle), barrier_closure, accumulator_ptr));
  }
}

void CacheStorage::CacheUnreferenced(CacheStorageCache* cache) {
  DCHECK(cache);
  DCHECK(cache->IsUnreferenced());
  auto doomed_caches_it = doomed_caches_.find(cache);
  if (doomed_caches_it != doomed_caches_.end()) {
    // The last reference to a doomed cache is gone, perform clean up.
    DeleteCacheFinalize(cache);
    return;
  }

  // Opportunistically keep warmed caches open when the CacheStorage is
  // still actively referenced.  Repeatedly opening and closing simple
  // disk_cache backends can be quite slow.  This is easy to trigger when
  // a site uses caches.match() frequently because the a Cache object is
  // never exposed to script to explicitly hold the backend open.
  if (handle_ref_count_)
    return;

  // The CacheStorage is not actively being referenced.  Close the cache
  // immediately.
  auto cache_map_it = cache_map_.find(cache->cache_name());
  DCHECK(cache_map_it != cache_map_.end());

  cache_map_it->second.reset();
}

void CacheStorage::ReleaseUnreferencedCaches() {
  DCHECK_CURRENTLY_ON(BrowserThread::IO);
  for (auto& entry : cache_map_) {
    if (entry.second && entry.second->IsUnreferenced())
      entry.second.reset();
  }
}

}  // namespace content
