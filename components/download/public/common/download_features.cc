// Copyright 2018 The Chromium Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include "components/download/public/common/download_features.h"

#include "build/build_config.h"

namespace download {
namespace features {

const base::Feature kUseDownloadOfflineContentProvider{
    "UseDownloadOfflineContentProvider", base::FEATURE_DISABLED_BY_DEFAULT};

const base::Feature kDownloadAutoResumptionNative {
  "DownloadsAutoResumptionNative",
#if defined(OS_ANDROID)
      base::FEATURE_ENABLED_BY_DEFAULT
#else
      base::FEATURE_DISABLED_BY_DEFAULT
#endif
};

const base::Feature kParallelDownloading {
  "ParallelDownloading",
#if defined(OS_ANDROID)
      base::FEATURE_ENABLED_BY_DEFAULT
#else
      base::FEATURE_DISABLED_BY_DEFAULT
#endif
};

const base::Feature kDownloadDBForNewDownloads{
    "DownloadDBForNewDownloads", base::FEATURE_ENABLED_BY_DEFAULT};

#if defined(OS_ANDROID)
const base::Feature kRefreshExpirationDate{"RefreshExpirationDate",
                                           base::FEATURE_ENABLED_BY_DEFAULT};
#endif

}  // namespace features
}  // namespace download
