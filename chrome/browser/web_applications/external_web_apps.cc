// Copyright 2018 The Chromium Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include "chrome/browser/web_applications/external_web_apps.h"

#include <map>
#include <memory>
#include <string>
#include <utility>

#include "base/bind.h"
#include "base/callback.h"
#include "base/feature_list.h"
#include "base/files/file_enumerator.h"
#include "base/files/file_path.h"
#include "base/json/json_file_value_serializer.h"
#include "base/no_destructor.h"
#include "base/path_service.h"
#include "base/task/post_task.h"
#include "base/threading/scoped_blocking_call.h"
#include "build/build_config.h"
#include "chrome/browser/apps/user_type_filter.h"
#include "chrome/browser/profiles/profile.h"
#include "chrome/browser/web_applications/components/web_app_constants.h"
#include "chrome/common/chrome_paths.h"
#include "content/public/browser/browser_thread.h"
#include "url/gurl.h"

#if defined(OS_CHROMEOS)
#include "chrome/browser/chromeos/profiles/profile_helper.h"
#endif

namespace {

// kAppUrl is a required string specifying a URL inside the scope of the web
// app that contains a link to the app manifest.
constexpr char kAppUrl[] = "app_url";

// kCreateShortcuts is an optional boolean which controls whether OS
// level shortcuts are created. On Chrome OS this controls whether the app is
// pinned to the shelf.
// The default value of kCreateShortcuts if false.
constexpr char kCreateShortcuts[] = "create_shortcuts";

// kFeatureName is an optional string parameter specifying a feature
// associated with this app. If specified:
//  - if the feature is enabled, the app will be installed
//  - if the feature is not enabled, the app will be removed.
constexpr char kFeatureName[] = "feature_name";

// kLaunchContainer is a required string which can be "window" or "tab"
// and controls what sort of container the web app is launched in.
constexpr char kLaunchContainer[] = "launch_container";
constexpr char kLaunchContainerTab[] = "tab";
constexpr char kLaunchContainerWindow[] = "window";

#if defined(OS_CHROMEOS)
// The sub-directory of the extensions directory in which to scan for external
// web apps (as opposed to external extensions or external ARC apps).
const base::FilePath::CharType kWebAppsSubDirectory[] =
    FILE_PATH_LITERAL("web_apps");
#endif

bool IsFeatureEnabled(const std::string& feature_name) {
  // The feature system ensures there is only ever one Feature instance for each
  // given feature name. To enable multiple apps to be gated by the same field
  // trial this means there needs to be a global map of Features that is used.
  static base::NoDestructor<
      std::map<std::string, std::unique_ptr<base::Feature>>>
      feature_map;
  if (!feature_map->count(feature_name)) {
    // To ensure the string used in the feature (which is a char*) is stable
    // (i.e. is not freed later on), the key of the map is used. So, first
    // insert a null Feature into the map, and then swap it with a real Feature
    // constructed using the pointer from the key.
    auto it = feature_map->insert(std::make_pair(feature_name, nullptr)).first;
    it->second = std::make_unique<base::Feature>(
        base::Feature{it->first.c_str(), base::FEATURE_DISABLED_BY_DEFAULT});
  }

  // Use the feature from the map, not the one in the pair above, as it has a
  // stable address.
  const auto it = feature_map->find(feature_name);
  DCHECK(it != feature_map->end());
  return base::FeatureList::IsEnabled(*it->second);
}

std::vector<web_app::InstallOptions> ScanDir(const base::FilePath& dir,
                                             const std::string& user_type) {
  base::ScopedBlockingCall scoped_blocking_call(FROM_HERE,
                                                base::BlockingType::MAY_BLOCK);
  base::FilePath::StringType extension(FILE_PATH_LITERAL(".json"));
  base::FileEnumerator json_files(dir,
                                  false,  // Recursive.
                                  base::FileEnumerator::FILES);

  std::vector<web_app::InstallOptions> install_options_list;

  for (base::FilePath file = json_files.Next(); !file.empty();
       file = json_files.Next()) {
    if (!file.MatchesExtension(extension)) {
      continue;
    }

    JSONFileValueDeserializer deserializer(file);
    std::string error_msg;
    std::unique_ptr<base::Value> dict =
        deserializer.Deserialize(nullptr, &error_msg);
    if (!dict) {
      LOG(ERROR) << file.value() << " was not valid JSON: " << error_msg;
      continue;
    }
    if (dict->type() != base::Value::Type::DICTIONARY) {
      LOG(ERROR) << file.value() << " was not a dictionary as the top level";
      continue;
    }

    if (!apps::UserTypeMatchesJsonUserType(
            user_type, file.MaybeAsASCII() /* app_id */, dict.get(),
            nullptr /* default_user_types */)) {
      // Already logged.
      continue;
    }

    const base::Value* value =
        dict->FindKeyOfType(kFeatureName, base::Value::Type::STRING);
    if (value) {
      std::string feature_name = value->GetString();
      VLOG(1) << file.value() << " checking feature " << feature_name;
      if (!IsFeatureEnabled(feature_name)) {
        VLOG(1) << file.value() << " feature not enabled";
        continue;
      }
    }

    value = dict->FindKeyOfType(kAppUrl, base::Value::Type::STRING);
    if (!value) {
      LOG(ERROR) << file.value() << " had a missing " << kAppUrl;
      continue;
    }
    GURL app_url(value->GetString());
    if (!app_url.is_valid()) {
      LOG(ERROR) << file.value() << " had an invalid " << kAppUrl;
      continue;
    }

    bool create_shortcuts = false;
    value = dict->FindKey(kCreateShortcuts);
    if (value) {
      if (!value->is_bool()) {
        LOG(ERROR) << file.value() << " had an invalid " << kCreateShortcuts;
        continue;
      }
      create_shortcuts = value->GetBool();
    }

    value = dict->FindKeyOfType(kLaunchContainer, base::Value::Type::STRING);
    if (!value) {
      LOG(ERROR) << file.value() << " had an invalid " << kLaunchContainer;
      continue;
    }
    std::string launch_container_str = value->GetString();
    auto launch_container = web_app::LaunchContainer::kTab;
    if (launch_container_str == kLaunchContainerTab) {
      launch_container = web_app::LaunchContainer::kTab;
    } else if (launch_container_str == kLaunchContainerWindow) {
      launch_container = web_app::LaunchContainer::kWindow;
    } else {
      LOG(ERROR) << file.value() << " had an invalid " << kLaunchContainer;
      continue;
    }

    web_app::InstallOptions install_options(
        std::move(app_url), launch_container,
        web_app::InstallSource::kExternalDefault);
    install_options.create_shortcuts = create_shortcuts;
    install_options.require_manifest = true;

    install_options_list.push_back(std::move(install_options));
  }

  return install_options_list;
}

base::FilePath DetermineScanDir(const Profile* profile) {
  base::FilePath dir;
#if defined(OS_CHROMEOS)
  // As of mid 2018, only Chrome OS has default/external web apps, and
  // chrome::DIR_STANDALONE_EXTERNAL_EXTENSIONS is only defined for OS_LINUX,
  // which includes OS_CHROMEOS.

  if (chromeos::ProfileHelper::IsPrimaryProfile(profile)) {
    // For manual testing, you can change s/STANDALONE/USER/, as writing to
    // "$HOME/.config/chromium/test-user/.config/chromium/External
    // Extensions/web_apps" does not require root ACLs, unlike
    // "/usr/share/chromium/extensions/web_apps".
    if (!base::PathService::Get(chrome::DIR_STANDALONE_EXTERNAL_EXTENSIONS,
                                &dir)) {
      LOG(ERROR) << "ScanForExternalWebApps: base::PathService::Get failed";
    } else {
      dir = dir.Append(kWebAppsSubDirectory);
    }
  }

#endif
  return dir;
}

}  // namespace

namespace web_app {

std::vector<web_app::InstallOptions> ScanDirForExternalWebAppsForTesting(
    const base::FilePath& dir,
    Profile* profile) {
  return ScanDir(dir, apps::DetermineUserType(profile));
}

void ScanForExternalWebApps(Profile* profile,
                            ScanForExternalWebAppsCallback callback) {
  DCHECK_CURRENTLY_ON(content::BrowserThread::UI);
  const base::FilePath dir = DetermineScanDir(profile);
  if (dir.empty()) {
    std::move(callback).Run(std::vector<web_app::InstallOptions>());
    return;
  }
  // Do a two-part callback dance, across different TaskRunners.
  //
  // 1. Schedule ScanDir to happen on a background thread, so that we don't
  // block the UI thread. When that's done,
  // base::PostTaskWithTraitsAndReplyWithResult will bounce us back to the
  // originating thread (the UI thread).
  //
  // 2. In |callback|, forward the vector of InstallOptions on to the
  // pending_app_manager_, which can only be called on the UI thread.
  base::PostTaskWithTraitsAndReplyWithResult(
      FROM_HERE,
      {base::MayBlock(), base::TaskPriority::BEST_EFFORT,
       base::TaskShutdownBehavior::SKIP_ON_SHUTDOWN},
      base::BindOnce(&ScanDir, dir, apps::DetermineUserType(profile)),
      std::move(callback));
}

}  //  namespace web_app
