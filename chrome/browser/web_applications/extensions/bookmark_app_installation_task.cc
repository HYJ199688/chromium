// Copyright 2018 The Chromium Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include "chrome/browser/web_applications/extensions/bookmark_app_installation_task.h"

#include "base/bind.h"
#include "base/callback.h"
#include "chrome/browser/favicon/favicon_utils.h"
#include "chrome/browser/installable/installable_manager.h"
#include "chrome/browser/profiles/profile.h"
#include "chrome/browser/ssl/security_state_tab_helper.h"
#include "chrome/browser/web_applications/components/install_manager.h"
#include "chrome/browser/web_applications/components/web_app_constants.h"
#include "chrome/browser/web_applications/components/web_app_provider_base.h"
#include "content/public/browser/browser_thread.h"
#include "extensions/browser/extension_registry.h"
#include "extensions/common/constants.h"
#include "extensions/common/extension.h"

namespace extensions {

BookmarkAppInstallationTask::Result::Result(
    web_app::InstallResultCode code,
    base::Optional<web_app::AppId> app_id)
    : code(code), app_id(std::move(app_id)) {
  DCHECK_EQ(code == web_app::InstallResultCode::kSuccess, app_id.has_value());
}

BookmarkAppInstallationTask::Result::Result(Result&&) = default;

BookmarkAppInstallationTask::Result::~Result() = default;

// static
void BookmarkAppInstallationTask::CreateTabHelpers(
    content::WebContents* web_contents) {
  InstallableManager::CreateForWebContents(web_contents);
  SecurityStateTabHelper::CreateForWebContents(web_contents);
  favicon::CreateContentFaviconDriverForWebContents(web_contents);
}

BookmarkAppInstallationTask::BookmarkAppInstallationTask(
    Profile* profile,
    web_app::InstallOptions install_options)
    : profile_(profile),
      extension_ids_map_(profile_->GetPrefs()),
      install_options_(std::move(install_options)) {}

BookmarkAppInstallationTask::~BookmarkAppInstallationTask() = default;

void BookmarkAppInstallationTask::Install(content::WebContents* web_contents,
                                          ResultCallback result_callback) {
  DCHECK_CURRENTLY_ON(content::BrowserThread::UI);

  DCHECK_EQ(web_contents->GetBrowserContext(), profile_);

  auto* provider = web_app::WebAppProviderBase::GetProviderBase(profile_);
  DCHECK(provider);

  provider->install_manager().InstallWebAppWithOptions(
      web_contents, install_options_,
      base::BindOnce(&BookmarkAppInstallationTask::OnWebAppInstalled,
                     weak_ptr_factory_.GetWeakPtr(),
                     std::move(result_callback)));
}

void BookmarkAppInstallationTask::OnWebAppInstalled(
    ResultCallback result_callback,
    const web_app::AppId& app_id,
    web_app::InstallResultCode code) {
  ExtensionRegistry* registry = ExtensionRegistry::Get(profile_);
  const Extension* extension = registry->enabled_extensions().GetByID(app_id);

  if (code == web_app::InstallResultCode::kSuccess && extension) {
    extension_ids_map_.Insert(install_options_.url, extension->id(),
                              install_options_.install_source);
    std::move(result_callback)
        .Run(Result(web_app::InstallResultCode::kSuccess, extension->id()));
  } else {
    std::move(result_callback).Run(Result(code, base::nullopt));
  }
}

}  // namespace extensions
