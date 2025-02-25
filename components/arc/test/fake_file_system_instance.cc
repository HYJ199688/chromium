// Copyright 2016 The Chromium Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include "components/arc/test/fake_file_system_instance.h"

#include <string.h>
#include <unistd.h>

#include <sstream>
#include <utility>

#include "base/bind.h"
#include "base/bind_helpers.h"
#include "base/files/file.h"
#include "base/files/file_path.h"
#include "base/files/file_util.h"
#include "base/files/scoped_file.h"
#include "base/location.h"
#include "base/logging.h"
#include "base/optional.h"
#include "base/threading/thread_task_runner_handle.h"
#include "mojo/public/cpp/system/platform_handle.h"

namespace arc {

namespace {

base::ScopedFD CreateRegularFileDescriptor(const std::string& content,
                                           const base::FilePath& temp_dir) {
  base::FilePath path;
  bool create_success = base::CreateTemporaryFileInDir(temp_dir, &path);
  DCHECK(create_success);
  int written_size = base::WriteFile(path, content.data(), content.size());
  DCHECK_EQ(static_cast<int>(content.size()), written_size);
  base::File file(path, base::File::FLAG_OPEN | base::File::FLAG_READ);
  DCHECK(file.IsValid());
  return base::ScopedFD(file.TakePlatformFile());
}

base::ScopedFD CreateStreamFileDescriptor(const std::string& content) {
  int fds[2];
  int ret = pipe(fds);
  DCHECK_EQ(0, ret);
  base::ScopedFD fd_read(fds[0]);
  base::ScopedFD fd_write(fds[1]);
  bool write_success =
      base::WriteFileDescriptor(fd_write.get(), content.data(), content.size());
  DCHECK(write_success);
  return fd_read;
}

mojom::DocumentPtr MakeDocument(const FakeFileSystemInstance::Document& doc) {
  mojom::DocumentPtr document = mojom::Document::New();
  document->document_id = doc.document_id;
  document->display_name = doc.display_name;
  document->mime_type = doc.mime_type;
  document->size = doc.size;
  document->last_modified = doc.last_modified;
  return document;
}

mojom::RootPtr MakeRoot(const FakeFileSystemInstance::Root& in_root) {
  mojom::RootPtr root = mojom::Root::New();
  root->authority = in_root.authority;
  root->root_id = in_root.root_id;
  root->document_id = in_root.document_id;
  root->title = in_root.title;
  return root;
}

// Generates unique document ID on each calls.
std::string GenerateDocumentId() {
  static int count = 0;
  std::ostringstream ss;
  ss << "doc_" << count++;
  return ss.str();
}

}  // namespace

FakeFileSystemInstance::File::File(const std::string& url,
                                   const std::string& content,
                                   const std::string& mime_type,
                                   Seekable seekable)
    : url(url), content(content), mime_type(mime_type), seekable(seekable) {}

FakeFileSystemInstance::File::File(const File& that) = default;

FakeFileSystemInstance::File::~File() = default;

FakeFileSystemInstance::Document::Document(
    const std::string& authority,
    const std::string& document_id,
    const std::string& parent_document_id,
    const std::string& display_name,
    const std::string& mime_type,
    int64_t size,
    uint64_t last_modified)
    : authority(authority),
      document_id(document_id),
      parent_document_id(parent_document_id),
      display_name(display_name),
      mime_type(mime_type),
      size(size),
      last_modified(last_modified) {}

FakeFileSystemInstance::Document::Document(const Document& that) = default;

FakeFileSystemInstance::Document::~Document() = default;

FakeFileSystemInstance::Root::Root(const std::string& authority,
                                   const std::string& root_id,
                                   const std::string& document_id,
                                   const std::string& title)
    : authority(authority),
      root_id(root_id),
      document_id(document_id),
      title(title) {}

FakeFileSystemInstance::Root::Root(const Root& that) = default;

FakeFileSystemInstance::Root::~Root() = default;

FakeFileSystemInstance::FakeFileSystemInstance() {
  bool temp_dir_created = temp_dir_.CreateUniqueTempDir();
  DCHECK(temp_dir_created);
}

FakeFileSystemInstance::~FakeFileSystemInstance() {
  DCHECK_CALLED_ON_VALID_THREAD(thread_checker_);
}

bool FakeFileSystemInstance::InitCalled() {
  return host_.is_bound();
}

void FakeFileSystemInstance::AddFile(const File& file) {
  DCHECK_CALLED_ON_VALID_THREAD(thread_checker_);
  DCHECK_EQ(0u, files_.count(std::string(file.url)));
  files_.insert(std::make_pair(std::string(file.url), file));
}

void FakeFileSystemInstance::AddDocument(const Document& document) {
  DCHECK_CALLED_ON_VALID_THREAD(thread_checker_);
  DocumentKey key(document.authority, document.document_id);
  DCHECK_EQ(0u, documents_.count(key));
  documents_.insert(std::make_pair(key, document));
  child_documents_[key];  // Allocate a vector.
  if (!document.parent_document_id.empty()) {
    DocumentKey parent_key(document.authority, document.parent_document_id);
    DCHECK_EQ(1u, documents_.count(parent_key));
    child_documents_[parent_key].push_back(key);
  }
}

void FakeFileSystemInstance::AddRecentDocument(const std::string& root_id,
                                               const Document& document) {
  DCHECK_CALLED_ON_VALID_THREAD(thread_checker_);
  RootKey key(document.authority, root_id);
  recent_documents_[key].push_back(document);
}

void FakeFileSystemInstance::AddRoot(const Root& root) {
  DCHECK_CALLED_ON_VALID_THREAD(thread_checker_);
  roots_.push_back(root);
}

void FakeFileSystemInstance::TriggerWatchers(
    const std::string& authority,
    const std::string& document_id,
    storage::WatcherManager::ChangeType type) {
  DCHECK_CALLED_ON_VALID_THREAD(thread_checker_);
  if (!host_) {
    LOG(ERROR) << "FileSystemHost is not available.";
    return;
  }
  auto iter = document_to_watchers_.find(DocumentKey(authority, document_id));
  if (iter == document_to_watchers_.end())
    return;
  for (int64_t watcher_id : iter->second) {
    host_->OnDocumentChanged(watcher_id, type);
  }
}

bool FakeFileSystemInstance::DocumentExists(const std::string& authority,
                                            const std::string& document_id) {
  DCHECK_CALLED_ON_VALID_THREAD(thread_checker_);
  DocumentKey key(authority, document_id);
  return documents_.find(key) != documents_.end();
}

bool FakeFileSystemInstance::DocumentExists(const std::string& authority,
                                            const std::string& root_document_id,
                                            const base::FilePath& path) {
  DCHECK_CALLED_ON_VALID_THREAD(thread_checker_);
  std::vector<std::string> path_components;
  path.GetComponents(&path_components);
  std::string document_id =
      FindChildDocumentId(authority, root_document_id, path_components);
  return DocumentExists(authority, document_id);
}

FakeFileSystemInstance::Document FakeFileSystemInstance::GetDocument(
    const std::string& authority,
    const std::string& document_id) {
  DCHECK_CALLED_ON_VALID_THREAD(thread_checker_);
  DocumentKey key(authority, document_id);
  auto iter = documents_.find(key);
  DCHECK(iter != documents_.end());
  return iter->second;
}

FakeFileSystemInstance::Document FakeFileSystemInstance::GetDocument(
    const std::string& authority,
    const std::string& root_document_id,
    const base::FilePath& path) {
  DCHECK_CALLED_ON_VALID_THREAD(thread_checker_);
  std::vector<std::string> path_components;
  path.GetComponents(&path_components);
  std::string document_id =
      FindChildDocumentId(authority, root_document_id, path_components);
  return GetDocument(authority, document_id);
}

void FakeFileSystemInstance::AddWatcher(const std::string& authority,
                                        const std::string& document_id,
                                        AddWatcherCallback callback) {
  DCHECK_CALLED_ON_VALID_THREAD(thread_checker_);
  DocumentKey key(authority, document_id);
  auto iter = documents_.find(key);
  if (iter == documents_.end()) {
    base::ThreadTaskRunnerHandle::Get()->PostTask(
        FROM_HERE, base::BindOnce(std::move(callback), -1));
    return;
  }
  int64_t watcher_id = next_watcher_id_++;
  document_to_watchers_[key].insert(watcher_id);
  watcher_to_document_.insert(std::make_pair(watcher_id, key));
  base::ThreadTaskRunnerHandle::Get()->PostTask(
      FROM_HERE, base::BindOnce(std::move(callback), watcher_id));
}

void FakeFileSystemInstance::GetFileSize(const std::string& url,
                                         GetFileSizeCallback callback) {
  DCHECK_CALLED_ON_VALID_THREAD(thread_checker_);
  auto iter = files_.find(url);
  if (iter == files_.end()) {
    base::ThreadTaskRunnerHandle::Get()->PostTask(
        FROM_HERE, base::BindOnce(std::move(callback), -1));
    return;
  }
  const File& file = iter->second;
  base::ThreadTaskRunnerHandle::Get()->PostTask(
      FROM_HERE, base::BindOnce(std::move(callback), file.content.size()));
}

void FakeFileSystemInstance::GetMimeType(const std::string& url,
                                         GetMimeTypeCallback callback) {
  DCHECK_CALLED_ON_VALID_THREAD(thread_checker_);
  auto iter = files_.find(url);
  if (iter == files_.end()) {
    base::ThreadTaskRunnerHandle::Get()->PostTask(
        FROM_HERE, base::BindOnce(std::move(callback), base::nullopt));
    return;
  }
  const File& file = iter->second;
  base::ThreadTaskRunnerHandle::Get()->PostTask(
      FROM_HERE, base::BindOnce(std::move(callback), file.mime_type));
}

void FakeFileSystemInstance::OpenFileToRead(const std::string& url,
                                            OpenFileToReadCallback callback) {
  DCHECK_CALLED_ON_VALID_THREAD(thread_checker_);
  auto iter = files_.find(url);
  if (iter == files_.end()) {
    base::ThreadTaskRunnerHandle::Get()->PostTask(
        FROM_HERE, base::BindOnce(std::move(callback), mojo::ScopedHandle()));
    return;
  }
  const File& file = iter->second;
  base::ScopedFD fd =
      file.seekable == File::Seekable::YES
          ? CreateRegularFileDescriptor(file.content, temp_dir_.GetPath())
          : CreateStreamFileDescriptor(file.content);
  mojo::ScopedHandle wrapped_handle =
      mojo::WrapPlatformHandle(mojo::PlatformHandle(std::move(fd)));
  DCHECK(wrapped_handle.is_valid());
  base::ThreadTaskRunnerHandle::Get()->PostTask(
      FROM_HERE,
      base::BindOnce(std::move(callback), std::move(wrapped_handle)));
}

void FakeFileSystemInstance::GetDocument(const std::string& authority,
                                         const std::string& document_id,
                                         GetDocumentCallback callback) {
  DCHECK_CALLED_ON_VALID_THREAD(thread_checker_);
  auto iter = documents_.find(DocumentKey(authority, document_id));
  if (iter == documents_.end()) {
    base::ThreadTaskRunnerHandle::Get()->PostTask(
        FROM_HERE, base::BindOnce(std::move(callback), mojom::DocumentPtr()));
    return;
  }
  base::ThreadTaskRunnerHandle::Get()->PostTask(
      FROM_HERE,
      base::BindOnce(std::move(callback), MakeDocument(iter->second)));
}

void FakeFileSystemInstance::GetChildDocuments(
    const std::string& authority,
    const std::string& parent_document_id,
    GetChildDocumentsCallback callback) {
  DCHECK_CALLED_ON_VALID_THREAD(thread_checker_);
  ++get_child_documents_count_;
  auto child_iter =
      child_documents_.find(DocumentKey(authority, parent_document_id));
  if (child_iter == child_documents_.end()) {
    base::ThreadTaskRunnerHandle::Get()->PostTask(
        FROM_HERE, base::BindOnce(std::move(callback), base::nullopt));
    return;
  }
  std::vector<mojom::DocumentPtr> children;
  for (const auto& child_key : child_iter->second) {
    auto doc_iter = documents_.find(child_key);
    DCHECK(doc_iter != documents_.end());
    children.emplace_back(MakeDocument(doc_iter->second));
  }
  base::ThreadTaskRunnerHandle::Get()->PostTask(
      FROM_HERE, base::BindOnce(std::move(callback),
                                base::make_optional(std::move(children))));
}

void FakeFileSystemInstance::GetRecentDocuments(
    const std::string& authority,
    const std::string& root_id,
    GetRecentDocumentsCallback callback) {
  DCHECK_CALLED_ON_VALID_THREAD(thread_checker_);
  auto recent_iter = recent_documents_.find(RootKey(authority, root_id));
  if (recent_iter == recent_documents_.end()) {
    base::ThreadTaskRunnerHandle::Get()->PostTask(
        FROM_HERE, base::BindOnce(std::move(callback), base::nullopt));
    return;
  }
  std::vector<mojom::DocumentPtr> recents;
  for (const Document& document : recent_iter->second)
    recents.emplace_back(MakeDocument(document));
  base::ThreadTaskRunnerHandle::Get()->PostTask(
      FROM_HERE, base::BindOnce(std::move(callback),
                                base::make_optional(std::move(recents))));
}

void FakeFileSystemInstance::GetRoots(GetRootsCallback callback) {
  DCHECK_CALLED_ON_VALID_THREAD(thread_checker_);
  std::vector<mojom::RootPtr> roots;
  for (const Root& root : roots_)
    roots.emplace_back(MakeRoot(root));
  base::ThreadTaskRunnerHandle::Get()->PostTask(
      FROM_HERE, base::BindOnce(std::move(callback),
                                base::make_optional(std::move(roots))));
}

void FakeFileSystemInstance::DeleteDocument(const std::string& authority,
                                            const std::string& document_id,
                                            DeleteDocumentCallback callback) {
  DCHECK_CALLED_ON_VALID_THREAD(thread_checker_);
  DocumentKey key(authority, document_id);
  if (!documents_.erase(key)) {
    base::ThreadTaskRunnerHandle::Get()->PostTask(
        FROM_HERE, base::BindOnce(std::move(callback), false));
    return;
  }
  size_t erased = child_documents_.erase(key);
  DCHECK_NE(0u, erased);
  base::ThreadTaskRunnerHandle::Get()->PostTask(
      FROM_HERE, base::BindOnce(std::move(callback), true));
}

void FakeFileSystemInstance::RenameDocument(const std::string& authority,
                                            const std::string& document_id,
                                            const std::string& display_name,
                                            RenameDocumentCallback callback) {
  DCHECK_CALLED_ON_VALID_THREAD(thread_checker_);
  DocumentKey key(authority, document_id);
  auto iter = documents_.find(key);
  DCHECK(iter != documents_.end());
  iter->second.display_name = display_name;
  base::ThreadTaskRunnerHandle::Get()->PostTask(
      FROM_HERE,
      base::BindOnce(std::move(callback), MakeDocument(iter->second)));
}

void FakeFileSystemInstance::CreateDocument(
    const std::string& authority,
    const std::string& parent_document_id,
    const std::string& mime_type,
    const std::string& display_name,
    CreateDocumentCallback callback) {
  DCHECK_CALLED_ON_VALID_THREAD(thread_checker_);
  std::string document_id = GenerateDocumentId();
  Document document(authority, document_id, parent_document_id, display_name,
                    mime_type, 0, 0);
  AddDocument(document);
  base::ThreadTaskRunnerHandle::Get()->PostTask(
      FROM_HERE, base::BindOnce(std::move(callback), MakeDocument(document)));
}

void FakeFileSystemInstance::CopyDocument(
    const std::string& authority,
    const std::string& source_document_id,
    const std::string& target_parent_document_id,
    CopyDocumentCallback callback) {
  DCHECK_CALLED_ON_VALID_THREAD(thread_checker_);
  DocumentKey source_key(authority, source_document_id);
  auto iter = documents_.find(source_key);
  DCHECK(iter != documents_.end());
  Document target_document(iter->second);
  target_document.document_id = target_document.display_name;
  target_document.parent_document_id = target_parent_document_id;
  AddDocument(target_document);
  base::ThreadTaskRunnerHandle::Get()->PostTask(
      FROM_HERE,
      base::BindOnce(std::move(callback), MakeDocument(target_document)));
}

void FakeFileSystemInstance::MoveDocument(
    const std::string& authority,
    const std::string& source_document_id,
    const std::string& source_parent_document_id,
    const std::string& target_parent_document_id,
    MoveDocumentCallback callback) {
  DCHECK_CALLED_ON_VALID_THREAD(thread_checker_);
  DocumentKey source_key(authority, source_document_id);
  DocumentKey source_parent_key(authority, source_parent_document_id);
  DocumentKey target_parent_key(authority, target_parent_document_id);
  for (auto iter = child_documents_[source_parent_key].begin();
       iter != child_documents_[source_parent_key].end(); iter++) {
    if (*iter == source_key) {
      child_documents_[source_parent_key].erase(iter);
      break;
    }
  }
  child_documents_[target_parent_key].push_back(source_key);
  auto iter = documents_.find(source_key);
  DCHECK(iter != documents_.end());
  iter->second.parent_document_id = target_parent_document_id;
  base::ThreadTaskRunnerHandle::Get()->PostTask(
      FROM_HERE,
      base::BindOnce(std::move(callback), MakeDocument(iter->second)));
}

void FakeFileSystemInstance::InitDeprecated(mojom::FileSystemHostPtr host) {
  Init(std::move(host), base::DoNothing());
}

void FakeFileSystemInstance::Init(mojom::FileSystemHostPtr host,
                                  InitCallback callback) {
  DCHECK_CALLED_ON_VALID_THREAD(thread_checker_);
  DCHECK(host);
  DCHECK(!host_);
  host_ = std::move(host);
  std::move(callback).Run();
}

void FakeFileSystemInstance::RemoveWatcher(int64_t watcher_id,
                                           RemoveWatcherCallback callback) {
  DCHECK_CALLED_ON_VALID_THREAD(thread_checker_);
  auto iter = watcher_to_document_.find(watcher_id);
  if (iter == watcher_to_document_.end()) {
    base::ThreadTaskRunnerHandle::Get()->PostTask(
        FROM_HERE, base::BindOnce(std::move(callback), false));
    return;
  }
  document_to_watchers_[iter->second].erase(watcher_id);
  watcher_to_document_.erase(iter);
  base::ThreadTaskRunnerHandle::Get()->PostTask(
      FROM_HERE, base::BindOnce(std::move(callback), true));
}

void FakeFileSystemInstance::RequestMediaScan(
    const std::vector<std::string>& paths) {
  DCHECK_CALLED_ON_VALID_THREAD(thread_checker_);
  // Do nothing and pretend we scaned them.
}

void FakeFileSystemInstance::OpenUrlsWithPermission(
    mojom::OpenUrlsRequestPtr request,
    OpenUrlsWithPermissionCallback callback) {
  DCHECK_CALLED_ON_VALID_THREAD(thread_checker_);
}

std::string FakeFileSystemInstance::FindChildDocumentId(
    const std::string& authority,
    const std::string& parent_document_id,
    const std::vector<std::string>& components) {
  if (components.empty())
    return parent_document_id;

  auto children_iter =
      child_documents_.find(DocumentKey(authority, parent_document_id));
  if (children_iter == child_documents_.end())
    return std::string();

  for (DocumentKey key : children_iter->second) {
    auto iter = documents_.find(key);
    if (iter == documents_.end())
      continue;

    if (iter->second.display_name == components[0]) {
      std::vector<std::string> next_components(components.begin() + 1,
                                               components.end());
      return FindChildDocumentId(authority, iter->second.document_id,
                                 next_components);
    }
  }
  return std::string();
}

}  // namespace arc
