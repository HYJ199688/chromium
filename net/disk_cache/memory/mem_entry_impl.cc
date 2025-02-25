// Copyright (c) 2012 The Chromium Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include "net/disk_cache/memory/mem_entry_impl.h"

#include <algorithm>
#include <utility>

#include "base/bind.h"
#include "base/logging.h"
#include "base/metrics/histogram_macros.h"
#include "base/numerics/safe_math.h"
#include "base/strings/stringprintf.h"
#include "base/values.h"
#include "net/base/interval.h"
#include "net/base/io_buffer.h"
#include "net/base/net_errors.h"
#include "net/disk_cache/memory/mem_backend_impl.h"
#include "net/disk_cache/net_log_parameters.h"
#include "net/log/net_log_event_type.h"
#include "net/log/net_log_source_type.h"

using base::Time;

namespace disk_cache {

namespace {

const int kSparseData = 1;

// Maximum size of a sparse entry is 2 to the power of this number.
const int kMaxSparseEntryBits = 12;

// Sparse entry has maximum size of 4KB.
const int kMaxSparseEntrySize = 1 << kMaxSparseEntryBits;

// This enum is used for histograms, so only append to the end.
enum MemEntryWriteResult {
  MEM_ENTRY_WRITE_RESULT_SUCCESS = 0,
  MEM_ENTRY_WRITE_RESULT_INVALID_ARGUMENT = 1,
  MEM_ENTRY_WRITE_RESULT_OVER_MAX_ENTRY_SIZE = 2,
  MEM_ENTRY_WRITE_RESULT_EXCEEDED_CACHE_STORAGE_SIZE = 3,
  MEM_ENTRY_WRITE_RESULT_MAX = 4,
};

void RecordWriteResult(MemEntryWriteResult result) {
  UMA_HISTOGRAM_ENUMERATION("MemCache.WriteResult", result,
                            MEM_ENTRY_WRITE_RESULT_MAX);
}

// Convert global offset to child index.
int ToChildIndex(int64_t offset) {
  return static_cast<int>(offset >> kMaxSparseEntryBits);
}

// Convert global offset to offset in child entry.
int ToChildOffset(int64_t offset) {
  return static_cast<int>(offset & (kMaxSparseEntrySize - 1));
}

// Returns a name for a child entry given the base_name of the parent and the
// child_id.  This name is only used for logging purposes.
// If the entry is called entry_name, child entries will be named something
// like Range_entry_name:YYY where YYY is the number of the particular child.
std::string GenerateChildName(const std::string& base_name, int child_id) {
  return base::StringPrintf("Range_%s:%i", base_name.c_str(), child_id);
}

// Returns NetLog parameters for the creation of a MemEntryImpl. A separate
// function is needed because child entries don't store their key().
std::unique_ptr<base::Value> NetLogEntryCreationCallback(
    const MemEntryImpl* entry,
    net::NetLogCaptureMode /* capture_mode */) {
  std::unique_ptr<base::DictionaryValue> dict(new base::DictionaryValue());
  std::string key;
  switch (entry->type()) {
    case MemEntryImpl::PARENT_ENTRY:
      key = entry->key();
      break;
    case MemEntryImpl::CHILD_ENTRY:
      key = GenerateChildName(entry->parent()->key(), entry->child_id());
      break;
  }
  dict->SetString("key", key);
  dict->SetBoolean("created", true);
  return std::move(dict);
}

}  // namespace

MemEntryImpl::MemEntryImpl(base::WeakPtr<MemBackendImpl> backend,
                           const std::string& key,
                           net::NetLog* net_log)
    : MemEntryImpl(backend,
                   key,
                   0,        // child_id
                   nullptr,  // parent
                   net_log) {
  Open();
  // Just creating the entry (without any data) could cause the storage to
  // grow beyond capacity, but we allow such infractions.
  backend_->ModifyStorageSize(GetStorageSize());
}

MemEntryImpl::MemEntryImpl(base::WeakPtr<MemBackendImpl> backend,
                           int child_id,
                           MemEntryImpl* parent,
                           net::NetLog* net_log)
    : MemEntryImpl(backend,
                   std::string(),  // key
                   child_id,
                   parent,
                   net_log) {
  (*parent_->children_)[child_id] = this;
}

void MemEntryImpl::Open() {
  // Only a parent entry can be opened.
  DCHECK_EQ(PARENT_ENTRY, type());
  ++ref_count_;
  DCHECK_GE(ref_count_, 1);
  DCHECK(!doomed_);
}

bool MemEntryImpl::InUse() const {
  if (type() == CHILD_ENTRY)
    return parent_->InUse();

  return ref_count_ > 0;
}

int MemEntryImpl::GetStorageSize() const {
  int storage_size = static_cast<int32_t>(key_.size());
  for (const auto& i : data_)
    storage_size += i.size();
  return storage_size;
}

void MemEntryImpl::UpdateStateOnUse(EntryModified modified_enum) {
  if (!doomed_ && backend_)
    backend_->OnEntryUpdated(this);

  last_used_ = Time::Now();
  if (modified_enum == ENTRY_WAS_MODIFIED)
    last_modified_ = last_used_;
}

void MemEntryImpl::Doom() {
  if (!doomed_) {
    doomed_ = true;
    if (backend_)
      backend_->OnEntryDoomed(this);
    net_log_.AddEvent(net::NetLogEventType::ENTRY_DOOM);
  }
  if (!ref_count_)
    delete this;
}

void MemEntryImpl::Close() {
  DCHECK_EQ(PARENT_ENTRY, type());
  --ref_count_;
  if (ref_count_ == 0 && !doomed_) {
    // At this point the user is clearly done writing, so make sure there isn't
    // wastage due to exponential growth of vector for main data stream.
    Compact();
    if (children_) {
      for (const auto& child_info : *children_) {
        if (child_info.second != this)
          child_info.second->Compact();
      }
    }
  }
  DCHECK_GE(ref_count_, 0);
  if (!ref_count_ && doomed_)
    delete this;
}

std::string MemEntryImpl::GetKey() const {
  // A child entry doesn't have key so this method should not be called.
  DCHECK_EQ(PARENT_ENTRY, type());
  return key_;
}

Time MemEntryImpl::GetLastUsed() const {
  return last_used_;
}

Time MemEntryImpl::GetLastModified() const {
  return last_modified_;
}

int32_t MemEntryImpl::GetDataSize(int index) const {
  if (index < 0 || index >= kNumStreams)
    return 0;
  return data_[index].size();
}

int MemEntryImpl::ReadData(int index,
                           int offset,
                           IOBuffer* buf,
                           int buf_len,
                           CompletionOnceCallback callback) {
  if (net_log_.IsCapturing()) {
    net_log_.BeginEvent(
        net::NetLogEventType::ENTRY_READ_DATA,
        CreateNetLogReadWriteDataCallback(index, offset, buf_len, false));
  }

  int result = InternalReadData(index, offset, buf, buf_len);

  if (net_log_.IsCapturing()) {
    net_log_.EndEvent(net::NetLogEventType::ENTRY_READ_DATA,
                      CreateNetLogReadWriteCompleteCallback(result));
  }
  return result;
}

int MemEntryImpl::WriteData(int index,
                            int offset,
                            IOBuffer* buf,
                            int buf_len,
                            CompletionOnceCallback callback,
                            bool truncate) {
  if (net_log_.IsCapturing()) {
    net_log_.BeginEvent(
        net::NetLogEventType::ENTRY_WRITE_DATA,
        CreateNetLogReadWriteDataCallback(index, offset, buf_len, truncate));
  }

  int result = InternalWriteData(index, offset, buf, buf_len, truncate);

  if (net_log_.IsCapturing()) {
    net_log_.EndEvent(net::NetLogEventType::ENTRY_WRITE_DATA,
                      CreateNetLogReadWriteCompleteCallback(result));
  }
  return result;
}

int MemEntryImpl::ReadSparseData(int64_t offset,
                                 IOBuffer* buf,
                                 int buf_len,
                                 CompletionOnceCallback callback) {
  if (net_log_.IsCapturing()) {
    net_log_.BeginEvent(net::NetLogEventType::SPARSE_READ,
                        CreateNetLogSparseOperationCallback(offset, buf_len));
  }
  int result = InternalReadSparseData(offset, buf, buf_len);
  if (net_log_.IsCapturing())
    net_log_.EndEvent(net::NetLogEventType::SPARSE_READ);
  return result;
}

int MemEntryImpl::WriteSparseData(int64_t offset,
                                  IOBuffer* buf,
                                  int buf_len,
                                  CompletionOnceCallback callback) {
  if (net_log_.IsCapturing()) {
    net_log_.BeginEvent(net::NetLogEventType::SPARSE_WRITE,
                        CreateNetLogSparseOperationCallback(offset, buf_len));
  }
  int result = InternalWriteSparseData(offset, buf, buf_len);
  if (net_log_.IsCapturing())
    net_log_.EndEvent(net::NetLogEventType::SPARSE_WRITE);
  return result;
}

int MemEntryImpl::GetAvailableRange(int64_t offset,
                                    int len,
                                    int64_t* start,
                                    CompletionOnceCallback callback) {
  if (net_log_.IsCapturing()) {
    net_log_.BeginEvent(net::NetLogEventType::SPARSE_GET_RANGE,
                        CreateNetLogSparseOperationCallback(offset, len));
  }
  int result = InternalGetAvailableRange(offset, len, start);
  if (net_log_.IsCapturing()) {
    net_log_.EndEvent(
        net::NetLogEventType::SPARSE_GET_RANGE,
        CreateNetLogGetAvailableRangeResultCallback(*start, result));
  }
  return result;
}

bool MemEntryImpl::CouldBeSparse() const {
  DCHECK_EQ(PARENT_ENTRY, type());
  return (children_.get() != nullptr);
}

net::Error MemEntryImpl::ReadyForSparseIO(CompletionOnceCallback callback) {
  return net::OK;
}

void MemEntryImpl::SetLastUsedTimeForTest(base::Time time) {
  last_used_ = time;
}

size_t MemEntryImpl::EstimateMemoryUsage() const {
  // Subtlety: the entries in children_ are not double counted, as the entry
  // pointers won't be followed by EstimateMemoryUsage.
  return base::trace_event::EstimateMemoryUsage(data_) +
         base::trace_event::EstimateMemoryUsage(key_) +
         base::trace_event::EstimateMemoryUsage(children_);
}

// ------------------------------------------------------------------------

MemEntryImpl::MemEntryImpl(base::WeakPtr<MemBackendImpl> backend,
                           const ::std::string& key,
                           int child_id,
                           MemEntryImpl* parent,
                           net::NetLog* net_log)
    : key_(key),
      ref_count_(0),
      child_id_(child_id),
      child_first_pos_(0),
      parent_(parent),
      last_modified_(Time::Now()),
      last_used_(last_modified_),
      backend_(backend),
      doomed_(false) {
  backend_->OnEntryInserted(this);
  net_log_ = net::NetLogWithSource::Make(
      net_log, net::NetLogSourceType::MEMORY_CACHE_ENTRY);
  net_log_.BeginEvent(net::NetLogEventType::DISK_CACHE_MEM_ENTRY_IMPL,
                      base::Bind(&NetLogEntryCreationCallback, this));
}

MemEntryImpl::~MemEntryImpl() {
  if (backend_)
    backend_->ModifyStorageSize(-GetStorageSize());

  if (type() == PARENT_ENTRY) {
    if (children_) {
      EntryMap children;
      children_->swap(children);

      for (auto& it : children) {
        // Since |this| is stored in the map, it should be guarded against
        // double dooming, which will result in double destruction.
        if (it.second != this)
          it.second->Doom();
      }
    }
  } else {
    parent_->children_->erase(child_id_);
  }
  net_log_.EndEvent(net::NetLogEventType::DISK_CACHE_MEM_ENTRY_IMPL);
}

int MemEntryImpl::InternalReadData(int index, int offset, IOBuffer* buf,
                                   int buf_len) {
  DCHECK(type() == PARENT_ENTRY || index == kSparseData);

  if (index < 0 || index >= kNumStreams || buf_len < 0)
    return net::ERR_INVALID_ARGUMENT;

  int entry_size = data_[index].size();
  if (offset >= entry_size || offset < 0 || !buf_len)
    return 0;

  int end_offset;
  if (!base::CheckAdd(offset, buf_len).AssignIfValid(&end_offset) ||
      end_offset > entry_size)
    buf_len = entry_size - offset;

  UpdateStateOnUse(ENTRY_WAS_NOT_MODIFIED);
  std::copy(data_[index].begin() + offset,
            data_[index].begin() + offset + buf_len, buf->data());
  return buf_len;
}

int MemEntryImpl::InternalWriteData(int index, int offset, IOBuffer* buf,
                                    int buf_len, bool truncate) {
  DCHECK(type() == PARENT_ENTRY || index == kSparseData);
  if (!backend_) {
    // We have to fail writes after the backend is destroyed since we can't
    // ensure we wouldn't use too much memory if it's gone.
    RecordWriteResult(MEM_ENTRY_WRITE_RESULT_EXCEEDED_CACHE_STORAGE_SIZE);
    return net::ERR_INSUFFICIENT_RESOURCES;
  }

  if (index < 0 || index >= kNumStreams) {
    RecordWriteResult(MEM_ENTRY_WRITE_RESULT_INVALID_ARGUMENT);
    return net::ERR_INVALID_ARGUMENT;
  }

  if (offset < 0 || buf_len < 0) {
    RecordWriteResult(MEM_ENTRY_WRITE_RESULT_INVALID_ARGUMENT);
    return net::ERR_INVALID_ARGUMENT;
  }

  int max_file_size = backend_->MaxFileSize();

  int end_offset;
  if (offset > max_file_size || buf_len > max_file_size ||
      !base::CheckAdd(offset, buf_len).AssignIfValid(&end_offset) ||
      end_offset > max_file_size) {
    RecordWriteResult(MEM_ENTRY_WRITE_RESULT_OVER_MAX_ENTRY_SIZE);
    return net::ERR_FAILED;
  }

  int old_data_size = data_[index].size();
  if (truncate || old_data_size < end_offset) {
    int delta = end_offset - old_data_size;
    backend_->ModifyStorageSize(delta);
    if (backend_->HasExceededStorageSize()) {
      backend_->ModifyStorageSize(-delta);
      RecordWriteResult(MEM_ENTRY_WRITE_RESULT_EXCEEDED_CACHE_STORAGE_SIZE);
      return net::ERR_INSUFFICIENT_RESOURCES;
    }

    data_[index].resize(end_offset);

    // Zero fill any hole.
    if (old_data_size < offset) {
      std::fill(data_[index].begin() + old_data_size,
                data_[index].begin() + offset, 0);
    }
  }

  UpdateStateOnUse(ENTRY_WAS_MODIFIED);
  RecordWriteResult(MEM_ENTRY_WRITE_RESULT_SUCCESS);

  if (!buf_len)
    return 0;

  std::copy(buf->data(), buf->data() + buf_len, data_[index].begin() + offset);
  return buf_len;
}

int MemEntryImpl::InternalReadSparseData(int64_t offset,
                                         IOBuffer* buf,
                                         int buf_len) {
  DCHECK_EQ(PARENT_ENTRY, type());

  if (!InitSparseInfo())
    return net::ERR_CACHE_OPERATION_NOT_SUPPORTED;

  // Check that offset + buf_len does not overflow. This ensures that
  // offset + io_buf->BytesConsumed() never overflows below.
  if (offset < 0 || buf_len < 0 || !base::CheckAdd(offset, buf_len).IsValid())
    return net::ERR_INVALID_ARGUMENT;

  // We will keep using this buffer and adjust the offset in this buffer.
  scoped_refptr<net::DrainableIOBuffer> io_buf =
      base::MakeRefCounted<net::DrainableIOBuffer>(buf, buf_len);

  // Iterate until we have read enough.
  while (io_buf->BytesRemaining()) {
    MemEntryImpl* child = GetChild(offset + io_buf->BytesConsumed(), false);

    // No child present for that offset.
    if (!child)
      break;

    // We then need to prepare the child offset and len.
    int child_offset = ToChildOffset(offset + io_buf->BytesConsumed());

    // If we are trying to read from a position that the child entry has no data
    // we should stop.
    if (child_offset < child->child_first_pos_)
      break;
    if (net_log_.IsCapturing()) {
      net_log_.BeginEvent(
          net::NetLogEventType::SPARSE_READ_CHILD_DATA,
          CreateNetLogSparseReadWriteCallback(child->net_log_.source(),
                                              io_buf->BytesRemaining()));
    }
    int ret =
        child->ReadData(kSparseData, child_offset, io_buf.get(),
                        io_buf->BytesRemaining(), CompletionOnceCallback());
    if (net_log_.IsCapturing()) {
      net_log_.EndEventWithNetErrorCode(
          net::NetLogEventType::SPARSE_READ_CHILD_DATA, ret);
    }

    // If we encounter an error in one entry, return immediately.
    if (ret < 0)
      return ret;
    else if (ret == 0)
      break;

    // Increment the counter by number of bytes read in the child entry.
    io_buf->DidConsume(ret);
  }

  UpdateStateOnUse(ENTRY_WAS_NOT_MODIFIED);
  return io_buf->BytesConsumed();
}

int MemEntryImpl::InternalWriteSparseData(int64_t offset,
                                          IOBuffer* buf,
                                          int buf_len) {
  DCHECK_EQ(PARENT_ENTRY, type());

  if (!InitSparseInfo())
    return net::ERR_CACHE_OPERATION_NOT_SUPPORTED;

  // We can't generally do this without the backend since we need it to create
  // child entries.
  if (!backend_)
    return net::ERR_FAILED;

  // Check that offset + buf_len does not overflow. This ensures that
  // offset + io_buf->BytesConsumed() never overflows below.
  if (offset < 0 || buf_len < 0 || !base::CheckAdd(offset, buf_len).IsValid())
    return net::ERR_INVALID_ARGUMENT;

  scoped_refptr<net::DrainableIOBuffer> io_buf =
      base::MakeRefCounted<net::DrainableIOBuffer>(buf, buf_len);

  // This loop walks through child entries continuously starting from |offset|
  // and writes blocks of data (of maximum size kMaxSparseEntrySize) into each
  // child entry until all |buf_len| bytes are written. The write operation can
  // start in the middle of an entry.
  while (io_buf->BytesRemaining()) {
    MemEntryImpl* child = GetChild(offset + io_buf->BytesConsumed(), true);
    int child_offset = ToChildOffset(offset + io_buf->BytesConsumed());

    // Find the right amount to write, this evaluates the remaining bytes to
    // write and remaining capacity of this child entry.
    int write_len = std::min(static_cast<int>(io_buf->BytesRemaining()),
                             kMaxSparseEntrySize - child_offset);

    // Keep a record of the last byte position (exclusive) in the child.
    int data_size = child->GetDataSize(kSparseData);

    if (net_log_.IsCapturing()) {
      net_log_.BeginEvent(net::NetLogEventType::SPARSE_WRITE_CHILD_DATA,
                          CreateNetLogSparseReadWriteCallback(
                              child->net_log_.source(), write_len));
    }

    // Always writes to the child entry. This operation may overwrite data
    // previously written.
    // TODO(hclam): if there is data in the entry and this write is not
    // continuous we may want to discard this write.
    int ret = child->WriteData(kSparseData, child_offset, io_buf.get(),
                               write_len, CompletionOnceCallback(), true);
    if (net_log_.IsCapturing()) {
      net_log_.EndEventWithNetErrorCode(
          net::NetLogEventType::SPARSE_WRITE_CHILD_DATA, ret);
    }
    if (ret < 0)
      return ret;
    else if (ret == 0)
      break;

    // Keep a record of the first byte position in the child if the write was
    // not aligned nor continuous. This is to enable witting to the middle
    // of an entry and still keep track of data off the aligned edge.
    if (data_size != child_offset)
      child->child_first_pos_ = child_offset;

    // Adjust the offset in the IO buffer.
    io_buf->DidConsume(ret);
  }

  UpdateStateOnUse(ENTRY_WAS_MODIFIED);
  return io_buf->BytesConsumed();
}

int MemEntryImpl::InternalGetAvailableRange(int64_t offset,
                                            int len,
                                            int64_t* start) {
  DCHECK_EQ(PARENT_ENTRY, type());
  DCHECK(start);

  if (!InitSparseInfo())
    return net::ERR_CACHE_OPERATION_NOT_SUPPORTED;

  if (offset < 0 || len < 0 || !start)
    return net::ERR_INVALID_ARGUMENT;

  // If offset + len overflows, this will just be the empty interval.
  net::Interval<int64_t> requested(offset, offset + len);

  // Find the first relevant child, if any --- may have to skip over
  // one entry as it may be before the range (consider, for example,
  // if the request is for [2048, 10000), while [0, 1024) is a valid range
  // for the entry).
  EntryMap::const_iterator i = children_->lower_bound(ToChildIndex(offset));
  if (i != children_->cend() && !ChildInterval(i).Intersects(requested))
    ++i;
  net::Interval<int64_t> found;
  if (i != children_->cend() &&
      requested.Intersects(ChildInterval(i), &found)) {
    // Found something relevant; now just need to expand this out if next
    // children are contiguous and relevant to the request.
    while (true) {
      ++i;
      net::Interval<int64_t> relevant_in_next_child;
      if (i == children_->cend() ||
          !requested.Intersects(ChildInterval(i), &relevant_in_next_child) ||
          relevant_in_next_child.min() != found.max()) {
        break;
      }

      found.SpanningUnion(relevant_in_next_child);
    }
    *start = found.min();
    return found.Length();
  }

  *start = offset;
  return 0;
}

bool MemEntryImpl::InitSparseInfo() {
  DCHECK_EQ(PARENT_ENTRY, type());

  if (!children_) {
    // If we already have some data in sparse stream but we are being
    // initialized as a sparse entry, we should fail.
    if (GetDataSize(kSparseData))
      return false;
    children_.reset(new EntryMap());

    // The parent entry stores data for the first block, so save this object to
    // index 0.
    (*children_)[0] = this;
  }
  return true;
}

MemEntryImpl* MemEntryImpl::GetChild(int64_t offset, bool create) {
  DCHECK_EQ(PARENT_ENTRY, type());
  int index = ToChildIndex(offset);
  auto i = children_->find(index);
  if (i != children_->end())
    return i->second;
  if (create)
    return new MemEntryImpl(backend_, index, this, net_log_.net_log());
  return nullptr;
}

net::Interval<int64_t> MemEntryImpl::ChildInterval(
    MemEntryImpl::EntryMap::const_iterator i) {
  DCHECK(i != children_->cend());
  const MemEntryImpl* child = i->second;
  // The valid range in child is [child_first_pos_, DataSize), since the child
  // entry ops just use standard disk_cache::Entry API, so DataSize is
  // not aware of any hole in the beginning.
  int64_t child_responsibility_start = (i->first) * kMaxSparseEntrySize;
  return net::Interval<int64_t>(
      child_responsibility_start + child->child_first_pos_,
      child_responsibility_start + child->GetDataSize(kSparseData));
}

void MemEntryImpl::Compact() {
  // Stream 0 should already be fine since it's written out in a single WriteData().
  data_[1].shrink_to_fit();
  data_[2].shrink_to_fit();
}

}  // namespace disk_cache
