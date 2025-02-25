// Copyright 2018 The Chromium Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include "base/macros.h"
#include "base/test/scoped_feature_list.h"
#include "build/build_config.h"
#include "chrome/browser/defaults.h"
#include "chrome/browser/profiles/profile.h"
#include "chrome/browser/sync/test/integration/profile_sync_service_harness.h"
#include "chrome/browser/sync/test/integration/single_client_status_change_checker.h"
#include "chrome/browser/sync/test/integration/sync_test.h"
#include "components/browser_sync/profile_sync_service.h"
#include "components/sync/base/model_type.h"
#include "components/sync/driver/sync_driver_switches.h"

namespace {

syncer::ModelTypeSet AllowedTypesInStandaloneTransportMode() {
  // Only some special whitelisted types (and control types) are allowed in
  // standalone transport mode.
  syncer::ModelTypeSet allowed_types(syncer::USER_CONSENTS,
                                     syncer::AUTOFILL_WALLET_DATA);
  allowed_types.PutAll(syncer::ControlTypes());
  return allowed_types;
}

class SyncDisabledByUserChecker : public SingleClientStatusChangeChecker {
 public:
  explicit SyncDisabledByUserChecker(browser_sync::ProfileSyncService* service)
      : SingleClientStatusChangeChecker(service) {}

  bool IsExitConditionSatisfied() override {
    return service()->HasDisableReason(
        syncer::SyncService::DISABLE_REASON_USER_CHOICE);
  }

  std::string GetDebugMessage() const override {
    return "Sync Disabled by User";
  }
};

class SingleClientStandaloneTransportSyncTest : public SyncTest {
 public:
  SingleClientStandaloneTransportSyncTest() : SyncTest(SINGLE_CLIENT) {}
  ~SingleClientStandaloneTransportSyncTest() override {}

 private:
  DISALLOW_COPY_AND_ASSIGN(SingleClientStandaloneTransportSyncTest);
};

#if defined(OS_CHROMEOS) || defined(OS_ANDROID)
IN_PROC_BROWSER_TEST_F(SingleClientStandaloneTransportSyncTest,
                       StartsSyncFeatureOnSignin) {
  // On platforms where Sync starts automatically (in practice, Android and
  // ChromeOS), IsFirstSetupComplete gets set automatically, and so the full
  // Sync feature will start upon sign-in to a primary account.
  ASSERT_TRUE(browser_defaults::kSyncAutoStarts);

  ASSERT_TRUE(SetupClients()) << "SetupClients() failed.";

  // Signing in (without explicitly setting up Sync) should trigger starting the
  // Sync machinery. Because IsFirstSetupComplete gets set automatically, this
  // will actually start the full Sync feature, not just the transport.
  ASSERT_TRUE(GetClient(0)->SignInPrimaryAccount());
  EXPECT_EQ(syncer::SyncService::TransportState::INITIALIZING,
            GetSyncService(0)->GetTransportState());

  EXPECT_TRUE(GetClient(0)->AwaitSyncTransportActive());

  EXPECT_EQ(syncer::SyncService::TransportState::ACTIVE,
            GetSyncService(0)->GetTransportState());

  ASSERT_TRUE(GetSyncService(0)->GetUserSettings()->IsFirstSetupComplete());

  EXPECT_TRUE(GetSyncService(0)->IsSyncFeatureEnabled());
  EXPECT_TRUE(GetSyncService(0)->IsSyncFeatureActive());
}
#else
IN_PROC_BROWSER_TEST_F(SingleClientStandaloneTransportSyncTest,
                       StartsSyncTransportOnSignin) {
  ASSERT_TRUE(SetupClients()) << "SetupClients() failed.";

  // Signing in (without explicitly setting up Sync) should trigger starting the
  // Sync machinery in standalone transport mode.
  ASSERT_TRUE(GetClient(0)->SignInPrimaryAccount());
  EXPECT_NE(syncer::SyncService::TransportState::DISABLED,
            GetSyncService(0)->GetTransportState());

  EXPECT_TRUE(GetClient(0)->AwaitSyncTransportActive());

  EXPECT_EQ(syncer::SyncService::TransportState::ACTIVE,
            GetSyncService(0)->GetTransportState());

  // Both IsSyncRequested and IsFirstSetupComplete should remain false (i.e.
  // at their default values). They only get set during the Sync setup flow,
  // either by the Sync confirmation dialog or by the settings page if going
  // through the advanced settings flow.
  EXPECT_FALSE(GetSyncService(0)->GetUserSettings()->IsFirstSetupComplete());
  EXPECT_FALSE(GetSyncService(0)->GetUserSettings()->IsSyncRequested());

  EXPECT_FALSE(GetSyncService(0)->IsSyncFeatureEnabled());
  EXPECT_FALSE(GetSyncService(0)->IsSyncFeatureActive());

  // Make sure that only the allowed types got activated. Note that, depending
  // on some other feature flags, not all of the allowed types are necessarily
  // active, and that's okay.
  syncer::ModelTypeSet bad_types =
      syncer::Difference(GetSyncService(0)->GetActiveDataTypes(),
                         AllowedTypesInStandaloneTransportMode());
  EXPECT_TRUE(bad_types.Empty()) << syncer::ModelTypeSetToString(bad_types);
}
#endif  // defined(OS_CHROMEOS) || defined(OS_ANDROID)

IN_PROC_BROWSER_TEST_F(SingleClientStandaloneTransportSyncTest,
                       SwitchesBetweenTransportAndFeature) {
  ASSERT_TRUE(SetupClients()) << "SetupClients() failed.";

  // Set up Sync-the-feature.
  ASSERT_TRUE(GetClient(0)->SetupSync());
  ASSERT_EQ(syncer::SyncService::TransportState::ACTIVE,
            GetSyncService(0)->GetTransportState());
  ASSERT_TRUE(GetSyncService(0)->IsSyncFeatureEnabled());
  ASSERT_TRUE(GetSyncService(0)->IsSyncFeatureActive());

  // Make sure that some model type which is not allowed in transport-only mode
  // got activated.
  ASSERT_FALSE(AllowedTypesInStandaloneTransportMode().Has(syncer::BOOKMARKS));
  ASSERT_TRUE(GetSyncService(0)->GetUserSettings()->GetChosenDataTypes().Has(
      syncer::BOOKMARKS));
  EXPECT_TRUE(GetSyncService(0)->GetActiveDataTypes().Has(syncer::BOOKMARKS));

  // Turn off Sync-the-feature by user choice. The machinery should start up
  // again in transport-only mode.
  GetSyncService(0)->GetUserSettings()->SetSyncRequested(false);
  EXPECT_TRUE(GetClient(0)->AwaitSyncTransportActive());

  EXPECT_EQ(syncer::SyncService::TransportState::ACTIVE,
            GetSyncService(0)->GetTransportState());
  EXPECT_FALSE(GetSyncService(0)->IsSyncFeatureEnabled());
  EXPECT_FALSE(GetSyncService(0)->IsSyncFeatureActive());

  syncer::ModelTypeSet bad_types =
      syncer::Difference(GetSyncService(0)->GetActiveDataTypes(),
                         AllowedTypesInStandaloneTransportMode());
  EXPECT_TRUE(bad_types.Empty()) << syncer::ModelTypeSetToString(bad_types);

  // Finally, turn Sync-the-feature on again.
  GetSyncService(0)->GetUserSettings()->SetSyncRequested(true);
  EXPECT_TRUE(GetClient(0)->AwaitSyncSetupCompletion());
  EXPECT_EQ(syncer::SyncService::TransportState::ACTIVE,
            GetSyncService(0)->GetTransportState());
  EXPECT_TRUE(GetSyncService(0)->IsSyncFeatureEnabled());
  EXPECT_TRUE(GetSyncService(0)->IsSyncFeatureActive());
  EXPECT_TRUE(GetSyncService(0)->GetActiveDataTypes().Has(syncer::BOOKMARKS));
}

// Tests the behavior of receiving a "Reset Sync" operation from the dashboard
// while Sync-the-feature is active: On non-ChromeOS, this signs the user out,
// so Sync will be fully disabled. On ChromeOS, there is no sign-out, so
// Sync-the-transport will start.
IN_PROC_BROWSER_TEST_F(SingleClientStandaloneTransportSyncTest,
                       HandlesResetFromDashboardWhenSyncActive) {
  ASSERT_TRUE(SetupClients()) << "SetupClients() failed.";

  // Set up Sync-the-feature.
  ASSERT_TRUE(GetClient(0)->SetupSync());
  ASSERT_EQ(syncer::SyncService::TransportState::ACTIVE,
            GetSyncService(0)->GetTransportState());
  ASSERT_TRUE(GetSyncService(0)->IsSyncFeatureEnabled());
  ASSERT_TRUE(GetSyncService(0)->IsSyncFeatureActive());

  // Trigger a "Reset Sync" from the dashboard and wait for it to apply. This
  // involves clearing the server data so that the birthday gets incremented,
  // and also sending an appropriate error.
  GetFakeServer()->ClearServerData();
  ASSERT_TRUE(GetFakeServer()->TriggerActionableError(
      sync_pb::SyncEnums::NOT_MY_BIRTHDAY, "Reset Sync from Dashboard",
      "https://chrome.google.com/sync", sync_pb::SyncEnums::UNKNOWN_ACTION));
  EXPECT_TRUE(SyncDisabledByUserChecker(GetSyncService(0)).Wait());
  GetFakeServer()->ClearActionableError();

  // On all platforms, Sync-the-feature should now be disabled.
  EXPECT_FALSE(GetSyncService(0)->IsSyncFeatureEnabled());
  EXPECT_TRUE(GetSyncService(0)->HasDisableReason(
      syncer::SyncService::DISABLE_REASON_USER_CHOICE));

#if defined(OS_CHROMEOS)
  // On ChromeOS, Sync should start up again in standalone transport mode.
  EXPECT_FALSE(GetSyncService(0)->HasDisableReason(
      syncer::SyncService::DISABLE_REASON_NOT_SIGNED_IN));
  EXPECT_NE(syncer::SyncService::TransportState::DISABLED,
            GetSyncService(0)->GetTransportState());

  EXPECT_TRUE(GetClient(0)->AwaitSyncTransportActive());
  EXPECT_EQ(syncer::SyncService::TransportState::ACTIVE,
            GetSyncService(0)->GetTransportState());
  EXPECT_FALSE(GetSyncService(0)->IsSyncFeatureEnabled());
#else
  // On non-ChromeOS platforms, the "Reset Sync" operation also signs the user
  // out, so Sync should now be fully disabled. Note that this behavior may
  // change in the future, see crbug.com/246839.
  EXPECT_TRUE(GetSyncService(0)->HasDisableReason(
      syncer::SyncService::DISABLE_REASON_NOT_SIGNED_IN));
  EXPECT_EQ(syncer::SyncService::TransportState::DISABLED,
            GetSyncService(0)->GetTransportState());
#endif  // defined(OS_CHROMEOS)
}

}  // namespace
