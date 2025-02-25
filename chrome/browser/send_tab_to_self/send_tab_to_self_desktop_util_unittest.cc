// Copyright 2019 The Chromium Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include "chrome/browser/send_tab_to_self/send_tab_to_self_desktop_util.h"

#include <memory>
#include <string>
#include <utility>
#include <vector>

#include "base/bind.h"
#include "base/strings/utf_string_conversions.h"
#include "base/test/scoped_feature_list.h"
#include "chrome/browser/sync/send_tab_to_self_sync_service_factory.h"
#include "chrome/test/base/browser_with_test_window_test.h"
#include "components/send_tab_to_self/send_tab_to_self_sync_service.h"
#include "components/send_tab_to_self/test_send_tab_to_self_model.h"
#include "components/sync/driver/sync_driver_switches.h"
#include "content/public/browser/navigation_entry.h"
#include "testing/gmock/include/gmock/gmock.h"
#include "testing/gtest/include/gtest/gtest.h"
#include "url/gurl.h"

namespace send_tab_to_self {

namespace {

class SendTabToSelfModelMock : public TestSendTabToSelfModel {
 public:
  SendTabToSelfModelMock() = default;
  ~SendTabToSelfModelMock() override = default;

  MOCK_METHOD3(AddEntry,
               const SendTabToSelfEntry*(const GURL&,
                                         const std::string&,
                                         base::Time));
};

class TestSendTabToSelfSyncService : public SendTabToSelfSyncService {
 public:
  TestSendTabToSelfSyncService() = default;
  ~TestSendTabToSelfSyncService() override = default;

  SendTabToSelfModel* GetSendTabToSelfModel() override {
    return &send_tab_to_self_model_mock_;
  }

 protected:
  SendTabToSelfModelMock send_tab_to_self_model_mock_;
};

std::unique_ptr<KeyedService> BuildTestSendTabToSelfSyncService(
    content::BrowserContext* context) {
  return std::make_unique<TestSendTabToSelfSyncService>();
}

class SendTabToSelfDesktopUtilTest : public BrowserWithTestWindowTest {
 public:
  SendTabToSelfDesktopUtilTest() = default;
  ~SendTabToSelfDesktopUtilTest() override = default;

  void SetUp() override {
    BrowserWithTestWindowTest::SetUp();

    url_ = GURL("https://www.google.com");
    title_ = base::UTF8ToUTF16(base::StringPiece("Google"));
  }

  // Set up all test conditions to let ShouldOfferFeature() return true
  void SetUpAllTrueEnv() {
    scoped_feature_list_.InitAndEnableFeature(switches::kSyncSendTabToSelf);

    AddTab(browser(), url_);
    NavigateAndCommitActiveTabWithTitle(browser(), url_, title_);
  }

 protected:
  base::test::ScopedFeatureList scoped_feature_list_;
  GURL url_;
  base::string16 title_;
};

TEST_F(SendTabToSelfDesktopUtilTest, CreateNewEntry) {
  SetUpAllTrueEnv();
  SendTabToSelfSyncServiceFactory::GetInstance()->SetTestingFactory(
      profile(), base::BindRepeating(&BuildTestSendTabToSelfSyncService));
  content::WebContents* tab =
      browser()->tab_strip_model()->GetActiveWebContents();
  content::NavigationEntry* entry =
      tab->GetController().GetLastCommittedEntry();

  GURL url = entry->GetURL();
  std::string title = base::UTF16ToUTF8(entry->GetTitle());
  base::Time navigation_time = entry->GetTimestamp();

  SendTabToSelfModelMock* model_mock = static_cast<SendTabToSelfModelMock*>(
      SendTabToSelfSyncServiceFactory::GetForProfile(profile())
          ->GetSendTabToSelfModel());

  EXPECT_CALL(*model_mock, AddEntry(url, title, navigation_time))
      .WillOnce(testing::Return(nullptr));

  CreateNewEntry(tab, profile());
}

}  // namespace

}  // namespace send_tab_to_self
