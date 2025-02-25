// Copyright 2018 The Chromium Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include "chrome/browser/web_applications/extensions/bookmark_app_installation_task.h"

#include <map>
#include <memory>
#include <utility>
#include <vector>

#include "base/bind.h"
#include "base/bind_helpers.h"
#include "base/callback.h"
#include "base/macros.h"
#include "base/optional.h"
#include "base/run_loop.h"
#include "base/strings/utf_string_conversions.h"
#include "base/test/bind_test_util.h"
#include "base/threading/thread_task_runner_handle.h"
#include "chrome/browser/extensions/bookmark_app_helper.h"
#include "chrome/browser/extensions/extension_service.h"
#include "chrome/browser/extensions/test_extension_system.h"
#include "chrome/browser/installable/installable_data.h"
#include "chrome/browser/web_applications/bookmark_apps/bookmark_app_install_manager.h"
#include "chrome/browser/web_applications/components/web_app_constants.h"
#include "chrome/browser/web_applications/components/web_app_data_retriever.h"
#include "chrome/browser/web_applications/components/web_app_provider_base.h"
#include "chrome/browser/web_applications/extensions/web_app_extension_ids_map.h"
#include "chrome/browser/web_applications/test/test_data_retriever.h"
#include "chrome/common/web_application_info.h"
#include "chrome/test/base/chrome_render_view_host_test_harness.h"
#include "chrome/test/base/testing_profile.h"
#include "content/public/test/test_utils.h"
#include "content/public/test/web_contents_tester.h"
#include "extensions/common/constants.h"
#include "extensions/common/extension_id.h"
#include "testing/gtest/include/gtest/gtest.h"
#include "third_party/blink/public/common/manifest/manifest.h"
#include "third_party/skia/include/core/SkBitmap.h"
#include "url/gurl.h"

namespace extensions {

using Result = BookmarkAppInstallationTask::Result;

namespace {

const char kWebAppTitle[] = "Foo Title";
const char kWebAppUrl[] = "https://foo.example";

}  // namespace

class TestBookmarkAppHelper : public BookmarkAppHelper {
 public:
  TestBookmarkAppHelper(Profile* profile,
                        WebApplicationInfo web_app_info,
                        content::WebContents* contents,
                        WebappInstallSource install_source)
      : BookmarkAppHelper(profile, web_app_info, contents, install_source) {}
  ~TestBookmarkAppHelper() override {}

  void CompleteInstallation() {
    CompleteInstallableCheck();
    content::RunAllTasksUntilIdle();
    CompleteIconDownload();
    content::RunAllTasksUntilIdle();
  }

  void CompleteInstallableCheck() {
    blink::Manifest manifest;
    InstallableData data = {
        {NO_MANIFEST},
        GURL::EmptyGURL(),
        &manifest,
        GURL::EmptyGURL(),
        nullptr,
        false,
        GURL::EmptyGURL(),
        nullptr,
        false,
        false,
    };
    BookmarkAppHelper::OnDidPerformInstallableCheck(data);
  }

  void CompleteIconDownload() {
    BookmarkAppHelper::OnIconsDownloaded(
        true, std::map<GURL, std::vector<SkBitmap>>());
  }

  void FailIconDownload() {
    BookmarkAppHelper::OnIconsDownloaded(
        false, std::map<GURL, std::vector<SkBitmap>>());
  }

 private:
  DISALLOW_COPY_AND_ASSIGN(TestBookmarkAppHelper);
};

class BookmarkAppInstallationTaskTest : public ChromeRenderViewHostTestHarness {
 public:
  BookmarkAppInstallationTaskTest() {}

  ~BookmarkAppInstallationTaskTest() override = default;

  void SetUp() override {
    ChromeRenderViewHostTestHarness::SetUp();

    // CrxInstaller in BookmarkAppInstaller needs an ExtensionService, so
    // create one for the profile.
    TestExtensionSystem* test_system =
        static_cast<TestExtensionSystem*>(ExtensionSystem::Get(profile()));
    test_system->CreateExtensionService(base::CommandLine::ForCurrentProcess(),
                                        profile()->GetPath(),
                                        false /* autoupdate_enabled */);
    SetUpTestingFactories();
  }

 protected:
  const GURL& app_url() const { return app_url_; }

  TestBookmarkAppHelper& test_helper() {
    DCHECK(test_helper_);
    return *test_helper_;
  }

 private:
  void SetUpTestingFactories() {
    auto* provider = web_app::WebAppProviderBase::GetProviderBase(profile());
    BookmarkAppInstallManager* install_manager =
        static_cast<BookmarkAppInstallManager*>(&provider->install_manager());

    install_manager->SetBookmarkAppHelperFactoryForTesting(
        base::BindLambdaForTesting(
            [this](Profile* profile, const WebApplicationInfo& web_app_info,
                   content::WebContents* web_contents,
                   WebappInstallSource install_source) {
              auto test_helper = std::make_unique<TestBookmarkAppHelper>(
                  profile, web_app_info, web_contents, install_source);
              test_helper_ = test_helper.get();

              return std::unique_ptr<BookmarkAppHelper>(std::move(test_helper));
            }));

    install_manager->SetDataRetrieverFactoryForTesting(
        base::BindLambdaForTesting([this]() {
          WebApplicationInfo info;
          info.app_url = app_url_;
          info.title = base::UTF8ToUTF16(kWebAppTitle);
          auto data_retriever = std::make_unique<web_app::TestDataRetriever>(
              std::make_unique<WebApplicationInfo>(std::move(info)));

          return std::unique_ptr<web_app::WebAppDataRetriever>(
              std::move(data_retriever));
        }));
  }

  TestBookmarkAppHelper* test_helper_ = nullptr;
  GURL app_url_{kWebAppUrl};

  DISALLOW_COPY_AND_ASSIGN(BookmarkAppInstallationTaskTest);
};

TEST_F(BookmarkAppInstallationTaskTest,
       WebAppOrShortcutFromContents_InstallationSucceeds) {
  auto task = std::make_unique<BookmarkAppInstallationTask>(
      profile(),
      web_app::InstallOptions(app_url(), web_app::LaunchContainer::kDefault,
                              web_app::InstallSource::kInternal));

  bool callback_called = false;
  task->Install(
      web_contents(),
      base::BindLambdaForTesting(
          [&](BookmarkAppInstallationTask::Result result) {
            base::Optional<std::string> id =
                web_app::ExtensionIdsMap(profile()->GetPrefs())
                    .LookupExtensionId(app_url());

            EXPECT_EQ(web_app::InstallResultCode::kSuccess, result.code);
            EXPECT_TRUE(result.app_id.has_value());

            EXPECT_EQ(result.app_id.value(), id.value());

            EXPECT_TRUE(test_helper().create_shortcuts());
            EXPECT_FALSE(test_helper().forced_launch_type().has_value());
            EXPECT_TRUE(test_helper().is_default_app());
            EXPECT_FALSE(test_helper().is_policy_installed_app());
            callback_called = true;
          }));

  content::RunAllTasksUntilIdle();

  test_helper().CompleteInstallableCheck();
  content::RunAllTasksUntilIdle();

  test_helper().CompleteIconDownload();
  content::RunAllTasksUntilIdle();

  EXPECT_TRUE(callback_called);
}

TEST_F(BookmarkAppInstallationTaskTest,
       WebAppOrShortcutFromContents_InstallationFails) {
  auto task = std::make_unique<BookmarkAppInstallationTask>(
      profile(),
      web_app::InstallOptions(app_url(), web_app::LaunchContainer::kWindow,
                              web_app::InstallSource::kInternal));

  bool callback_called = false;
  task->Install(web_contents(),
                base::BindLambdaForTesting(
                    [&](BookmarkAppInstallationTask::Result result) {
                      base::Optional<std::string> id =
                          web_app::ExtensionIdsMap(profile()->GetPrefs())
                              .LookupExtensionId(app_url());

                      EXPECT_NE(web_app::InstallResultCode::kSuccess,
                                result.code);
                      EXPECT_FALSE(result.app_id.has_value());

                      EXPECT_FALSE(id.has_value());
                      callback_called = true;
                    }));

  content::RunAllTasksUntilIdle();

  test_helper().CompleteInstallableCheck();
  content::RunAllTasksUntilIdle();

  test_helper().FailIconDownload();
  content::RunAllTasksUntilIdle();

  EXPECT_TRUE(callback_called);
}

TEST_F(BookmarkAppInstallationTaskTest,
       WebAppOrShortcutFromContents_NoShortcuts) {
  web_app::InstallOptions install_options(app_url(),
                                          web_app::LaunchContainer::kWindow,
                                          web_app::InstallSource::kInternal);
  install_options.create_shortcuts = false;
  auto task = std::make_unique<BookmarkAppInstallationTask>(
      profile(), std::move(install_options));

  bool callback_called = false;
  task->Install(web_contents(),
                base::BindLambdaForTesting(
                    [&](BookmarkAppInstallationTask::Result result) {
                      EXPECT_EQ(web_app::InstallResultCode::kSuccess,
                                result.code);
                      EXPECT_TRUE(result.app_id.has_value());

                      EXPECT_FALSE(test_helper().create_shortcuts());
                      callback_called = true;
                    }));

  content::RunAllTasksUntilIdle();

  test_helper().CompleteInstallation();
  EXPECT_TRUE(callback_called);
}

TEST_F(BookmarkAppInstallationTaskTest,
       WebAppOrShortcutFromContents_ForcedContainerWindow) {
  auto install_options =
      web_app::InstallOptions(app_url(), web_app::LaunchContainer::kWindow,
                              web_app::InstallSource::kInternal);
  auto task = std::make_unique<BookmarkAppInstallationTask>(
      profile(), std::move(install_options));

  bool callback_called = false;
  task->Install(web_contents(),
                base::BindLambdaForTesting(
                    [&](BookmarkAppInstallationTask::Result result) {
                      EXPECT_EQ(web_app::InstallResultCode::kSuccess,
                                result.code);
                      EXPECT_TRUE(result.app_id.has_value());

                      EXPECT_EQ(LAUNCH_TYPE_WINDOW,
                                test_helper().forced_launch_type().value());
                      callback_called = true;
                    }));

  content::RunAllTasksUntilIdle();

  test_helper().CompleteInstallation();
  EXPECT_TRUE(callback_called);
}

TEST_F(BookmarkAppInstallationTaskTest,
       WebAppOrShortcutFromContents_ForcedContainerTab) {
  auto install_options =
      web_app::InstallOptions(app_url(), web_app::LaunchContainer::kTab,
                              web_app::InstallSource::kInternal);
  auto task = std::make_unique<BookmarkAppInstallationTask>(
      profile(), std::move(install_options));

  bool callback_called = false;
  task->Install(web_contents(),
                base::BindLambdaForTesting(
                    [&](BookmarkAppInstallationTask::Result result) {
                      EXPECT_EQ(web_app::InstallResultCode::kSuccess,
                                result.code);
                      EXPECT_TRUE(result.app_id.has_value());

                      EXPECT_EQ(LAUNCH_TYPE_REGULAR,
                                test_helper().forced_launch_type().value());
                      callback_called = true;
                    }));

  content::RunAllTasksUntilIdle();

  test_helper().CompleteInstallation();
  EXPECT_TRUE(callback_called);
}

TEST_F(BookmarkAppInstallationTaskTest,
       WebAppOrShortcutFromContents_DefaultApp) {
  auto install_options =
      web_app::InstallOptions(app_url(), web_app::LaunchContainer::kDefault,
                              web_app::InstallSource::kInternal);
  auto task = std::make_unique<BookmarkAppInstallationTask>(
      profile(), std::move(install_options));

  bool callback_called = false;
  task->Install(web_contents(),
                base::BindLambdaForTesting(
                    [&](BookmarkAppInstallationTask::Result result) {
                      EXPECT_EQ(web_app::InstallResultCode::kSuccess,
                                result.code);
                      EXPECT_TRUE(result.app_id.has_value());

                      EXPECT_TRUE(test_helper().is_default_app());
                      callback_called = true;
                    }));

  content::RunAllTasksUntilIdle();

  test_helper().CompleteInstallation();
  EXPECT_TRUE(callback_called);
}

TEST_F(BookmarkAppInstallationTaskTest,
       WebAppOrShortcutFromContents_AppFromPolicy) {
  auto install_options =
      web_app::InstallOptions(app_url(), web_app::LaunchContainer::kDefault,
                              web_app::InstallSource::kExternalPolicy);
  auto task = std::make_unique<BookmarkAppInstallationTask>(
      profile(), std::move(install_options));

  bool callback_called = false;
  task->Install(web_contents(),
                base::BindLambdaForTesting(
                    [&](BookmarkAppInstallationTask::Result result) {
                      EXPECT_EQ(web_app::InstallResultCode::kSuccess,
                                result.code);
                      EXPECT_TRUE(result.app_id.has_value());

                      EXPECT_TRUE(test_helper().is_policy_installed_app());
                      callback_called = true;
                    }));

  content::RunAllTasksUntilIdle();

  test_helper().CompleteInstallation();
  EXPECT_TRUE(callback_called);
}

}  // namespace extensions
