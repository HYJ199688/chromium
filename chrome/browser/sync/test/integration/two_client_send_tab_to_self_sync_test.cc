// Copyright 2019 The Chromium Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include "base/run_loop.h"
#include "chrome/browser/history/history_service_factory.h"
#include "chrome/browser/sync/send_tab_to_self_sync_service_factory.h"
#include "chrome/browser/sync/test/integration/send_tab_to_self_helper.h"
#include "chrome/browser/sync/test/integration/sync_test.h"
#include "components/history/core/browser/history_service.h"
#include "components/send_tab_to_self/send_tab_to_self_model.h"
#include "components/send_tab_to_self/send_tab_to_self_sync_service.h"
#include "components/sync/driver/sync_driver_switches.h"
#include "testing/gmock/include/gmock/gmock.h"
#include "url/gurl.h"

class TwoClientSendTabToSelfSyncTest : public SyncTest {
 public:
  TwoClientSendTabToSelfSyncTest() : SyncTest(TWO_CLIENT) {
    scoped_list_.InitAndEnableFeature(switches::kSyncSendTabToSelf);
  }

  ~TwoClientSendTabToSelfSyncTest() override {}

 private:
  base::test::ScopedFeatureList scoped_list_;

  DISALLOW_COPY_AND_ASSIGN(TwoClientSendTabToSelfSyncTest);
};

IN_PROC_BROWSER_TEST_F(TwoClientSendTabToSelfSyncTest,
                       AddedUrlFoundWhenBothClientsAlreadySyncing) {
  const GURL kUrl("https://www.example.com");
  const base::Time kHistoryEntryTime = base::Time::Now();
  const std::string kTitle("example");
  const base::Time kTime = base::Time::FromDoubleT(1);

  ASSERT_TRUE(SetupSync()) << "SetupSync() failed.";

  history::HistoryService* history_service =
      HistoryServiceFactory::GetForProfile(GetProfile(0),
                                           ServiceAccessType::EXPLICIT_ACCESS);
  history_service->AddPage(kUrl, kHistoryEntryTime, history::SOURCE_BROWSED);

  base::RunLoop run_loop;
  history_service->FlushForTest(run_loop.QuitClosure());
  run_loop.Run();

  send_tab_to_self::SendTabToSelfModel* model0 =
      SendTabToSelfSyncServiceFactory::GetForProfile(GetProfile(0))
          ->GetSendTabToSelfModel();

  ASSERT_TRUE(model0->AddEntry(kUrl, kTitle, kTime));

  send_tab_to_self::SendTabToSelfSyncService* service1 =
      SendTabToSelfSyncServiceFactory::GetForProfile(GetProfile(1));

  EXPECT_TRUE(
      send_tab_to_self_helper::SendTabToSelfUrlChecker(service1, kUrl).Wait());
}

IN_PROC_BROWSER_TEST_F(TwoClientSendTabToSelfSyncTest,
                       ModelsMatchAfterAddWhenBothClientsAlreadySyncing) {
  const GURL kGurl0("https://www.example0.com");
  const std::string kTitle0("example0");
  const base::Time kTime0 = base::Time::FromDoubleT(1);

  const GURL kGurl1("https://www.example1.com");
  const std::string kTitle1("example1");
  const base::Time kTime1 = base::Time::FromDoubleT(2);

  const GURL kGurl2("https://www.example2.com");
  const std::string kTitle2("example2");
  const base::Time kTime2 = base::Time::FromDoubleT(3);

  ASSERT_TRUE(SetupSync()) << "SetupSync() failed.";

  send_tab_to_self::SendTabToSelfModel* model0 =
      SendTabToSelfSyncServiceFactory::GetForProfile(GetProfile(0))
          ->GetSendTabToSelfModel();

  ASSERT_TRUE(model0->AddEntry(kGurl0, kTitle0, kTime0));

  ASSERT_TRUE(model0->AddEntry(kGurl1, kTitle1, kTime1));

  ASSERT_TRUE(SendTabToSelfSyncServiceFactory::GetForProfile(GetProfile(1))
                  ->GetSendTabToSelfModel()
                  ->AddEntry(kGurl2, kTitle2, kTime2));

  EXPECT_TRUE(send_tab_to_self_helper::SendTabToSelfModelEqualityChecker(
                  SendTabToSelfSyncServiceFactory::GetForProfile(GetProfile(1)),
                  SendTabToSelfSyncServiceFactory::GetForProfile(GetProfile(0)))
                  .Wait());
}
