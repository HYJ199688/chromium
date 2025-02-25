// Copyright 2019 The Chromium Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#ifndef COMPONENTS_DOWNLOAD_PUBLIC_COMMON_SIMPLE_DOWNLOAD_MANAGER_H_
#define COMPONENTS_DOWNLOAD_PUBLIC_COMMON_SIMPLE_DOWNLOAD_MANAGER_H_

#include <memory>
#include <string>
#include <vector>

#include "components/download/public/common/download_export.h"
#include "components/download/public/common/download_url_parameters.h"

namespace download {

class DownloadItem;

// Download manager that provides simple functionalities for callers to carry
// out a download task.
class COMPONENTS_DOWNLOAD_EXPORT SimpleDownloadManager {
 public:
  class Observer {
   public:
    Observer() = default;
    virtual ~Observer() = default;

    virtual void OnDownloadsInitialized(bool active_downloads_only) {}
    virtual void OnManagerGoingDown() {}
    virtual void OnDownloadCreated(DownloadItem* item) {}
    virtual void OnDownloadUpdated(DownloadItem* item) {}
    virtual void OnDownloadOpened(DownloadItem* item) {}
    virtual void OnDownloadRemoved(DownloadItem* item) {}

   private:
    DISALLOW_COPY_AND_ASSIGN(Observer);
  };

  SimpleDownloadManager();
  virtual ~SimpleDownloadManager();

  // Calls the callback if this object becomes initialized.
  void NotifyWhenInitialized(base::OnceClosure callback);

  // Download a URL given by the |params|. Returns true if the download could
  // take place, or false otherwise.
  virtual bool DownloadUrl(
      std::unique_ptr<DownloadUrlParameters> parameters) = 0;

  using DownloadVector = std::vector<DownloadItem*>;
  // Add all download items to |downloads|, no matter the type or state, without
  // clearing |downloads| first.
  virtual void GetAllDownloads(DownloadVector* downloads) = 0;

  // Get the download item for |guid|.
  virtual DownloadItem* GetDownloadByGuid(const std::string& guid) = 0;

 protected:
  // Called when the manager is initailized.
  void OnInitialized();

  // Whether this object is initialized.
  bool initialized_ = false;

 private:
  // Callbacks to call once this object is initialized.
  std::vector<base::OnceClosure> on_initialized_callbacks_;
};

}  // namespace download

#endif  // COMPONENTS_DOWNLOAD_PUBLIC_COMMON_SIMPLE_DOWNLOAD_MANAGER_H_
