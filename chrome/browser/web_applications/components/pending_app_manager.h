// Copyright 2018 The Chromium Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#ifndef CHROME_BROWSER_WEB_APPLICATIONS_COMPONENTS_PENDING_APP_MANAGER_H_
#define CHROME_BROWSER_WEB_APPLICATIONS_COMPONENTS_PENDING_APP_MANAGER_H_

#include <memory>
#include <ostream>
#include <string>
#include <vector>

#include "base/callback_forward.h"
#include "base/macros.h"
#include "chrome/browser/web_applications/components/install_options.h"
#include "url/gurl.h"

namespace web_app {

enum class InstallResultCode;

// PendingAppManager installs, uninstalls, and updates apps.
//
// Implementations of this class should perform each set of operations serially
// in the order in which they arrive. For example, if an uninstall request gets
// queued while an update request for the same app is pending, implementations
// should wait for the update request to finish before uninstalling the app.
class PendingAppManager {
 public:
  using OnceInstallCallback =
      base::OnceCallback<void(const GURL& app_url, InstallResultCode code)>;
  using RepeatingInstallCallback =
      base::RepeatingCallback<void(const GURL& app_url,
                                   InstallResultCode code)>;
  using UninstallCallback =
      base::RepeatingCallback<void(const GURL& app_url, bool succeeded)>;

  PendingAppManager();
  virtual ~PendingAppManager();

  // Queues an installation operation with the highest priority. Essentially
  // installing the app immediately if there are no ongoing operations or
  // installing the app right after the current operation finishes. Runs its
  // callback with the URL in |install_options| and with the id of the installed
  // app or an empty string if the installation fails.
  //
  // Fails if the same operation has been queued before. Should only be used in
  // response to a user action e.g. the user clicked an install button.
  virtual void Install(InstallOptions install_options,
                       OnceInstallCallback callback) = 0;

  // Adds a task to the queue of operations for each InstallOptions in
  // |install_options_list|. Runs |callback| with the URL of the corresponding
  // InstallOptions in |install_options_list| and with the id of the installed
  // app or an empty string if the installation fails. Runs |callback| for every
  // completed installation - whether or not the installation actually
  // succeeded.
  virtual void InstallApps(std::vector<InstallOptions> install_options_list,
                           const RepeatingInstallCallback& callback) = 0;

  // Adds a task to the queue of operations for each GURL in
  // |uninstall_urls|. Runs |callback| with the URL of the corresponding
  // app in |uninstall_urls| and with a bool indicating whether or not the
  // uninstall succeeded. Runs |callback| for every completed uninstallation -
  // whether or not the uninstallation actually succeeded.
  virtual void UninstallApps(std::vector<GURL> uninstall_urls,
                             const UninstallCallback& callback) = 0;

  // Returns the URLs of those apps installed from |install_source|.
  virtual std::vector<GURL> GetInstalledAppUrls(
      InstallSource install_source) const = 0;

  // Installs an app for each InstallOptions in |desired_apps_install_options|
  // and uninstalls any apps in GetInstalledAppUrls(install_source) that are not
  // in |desired_apps_install_options|'s URLs.
  //
  // All apps in |desired_apps_install_options| should have |install_source| as
  // their source.
  //
  // Note that this returns after queueing work (installation and
  // uninstallation) to be done. It does not wait until that work is complete.
  void SynchronizeInstalledApps(
      std::vector<InstallOptions> desired_apps_install_options,
      InstallSource install_source);

  // Returns the app id for |url| if the PendingAppManager is aware of it.
  virtual base::Optional<std::string> LookupAppId(const GURL& url) const = 0;

 private:
  DISALLOW_COPY_AND_ASSIGN(PendingAppManager);
};

}  // namespace web_app

#endif  // CHROME_BROWSER_WEB_APPLICATIONS_COMPONENTS_PENDING_APP_MANAGER_H_
