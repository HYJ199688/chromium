// Copyright 2019 The Chromium Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include "components/download/public/common/simple_download_manager_coordinator.h"

#include <utility>

#include "components/download/public/common/download_item.h"
#include "components/download/public/common/simple_download_manager.h"

namespace download {

SimpleDownloadManagerCoordinator::SimpleDownloadManagerCoordinator()
    : simple_download_manager_(nullptr),
      has_all_history_downloads_(false),
      initialized_(false),
      weak_factory_(this) {}

SimpleDownloadManagerCoordinator::~SimpleDownloadManagerCoordinator() = default;

void SimpleDownloadManagerCoordinator::SetSimpleDownloadManager(
    SimpleDownloadManager* simple_download_manager,
    bool manages_all_history_downloads) {
  DCHECK(simple_download_manager_);
  simple_download_manager_ = simple_download_manager;
  simple_download_manager_->NotifyWhenInitialized(base::BindOnce(
      &SimpleDownloadManagerCoordinator::OnManagerInitialized,
      weak_factory_.GetWeakPtr(), manages_all_history_downloads));
}

void SimpleDownloadManagerCoordinator::AddObserver(Observer* observer) {
  observers_.AddObserver(observer);
}

void SimpleDownloadManagerCoordinator::RemoveObserver(Observer* observer) {
  observers_.RemoveObserver(observer);
}

void SimpleDownloadManagerCoordinator::DownloadUrl(
    std::unique_ptr<DownloadUrlParameters> parameters) {
  if (simple_download_manager_)
    simple_download_manager_->DownloadUrl(std::move(parameters));
}

void SimpleDownloadManagerCoordinator::GetAllDownloads(
    std::vector<DownloadItem*>* downloads) {
  simple_download_manager_->GetAllDownloads(downloads);
}

DownloadItem* SimpleDownloadManagerCoordinator::GetDownloadByGuid(
    const std::string& guid) {
  if (simple_download_manager_)
    return simple_download_manager_->GetDownloadByGuid(guid);
  return nullptr;
}

void SimpleDownloadManagerCoordinator::OnManagerInitialized(
    bool has_all_history_downloads) {
  initialized_ = true;
  has_all_history_downloads_ = has_all_history_downloads;
  for (auto& observer : observers_)
    observer.OnDownloadsInitialized(!has_all_history_downloads);
}

}  // namespace download
