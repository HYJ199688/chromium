// Copyright (c) 2015 The Chromium Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include "base/command_line.h"
#include "base/macros.h"
#include "base/message_loop/message_loop.h"
#include "base/strings/utf_string_conversions.h"
#include "base/test/metrics/histogram_tester.h"
#include "build/build_config.h"
#include "chrome/browser/chrome_notification_types.h"
#include "chrome/browser/sync/test/integration/profile_sync_service_harness.h"
#include "chrome/browser/sync/test/integration/secondary_account_helper.h"
#include "chrome/browser/sync/test/integration/single_client_status_change_checker.h"
#include "chrome/browser/sync/test/integration/sync_datatype_helper.h"
#include "chrome/browser/sync/test/integration/sync_test.h"
#include "chrome/browser/sync/test/integration/wallet_helper.h"
#include "chrome/browser/web_data_service_factory.h"
#include "components/autofill/core/browser/autofill_data_util.h"
#include "components/autofill/core/browser/autofill_metadata.h"
#include "components/autofill/core/browser/credit_card.h"
#include "components/autofill/core/browser/field_types.h"
#include "components/autofill/core/browser/payments/payments_customer_data.h"
#include "components/autofill/core/browser/personal_data_manager.h"
#include "components/autofill/core/browser/personal_data_manager_observer.h"
#include "components/autofill/core/browser/test_autofill_clock.h"
#include "components/autofill/core/common/autofill_features.h"
#include "components/autofill/core/common/autofill_prefs.h"
#include "components/autofill/core/common/autofill_switches.h"
#include "components/browser_sync/profile_sync_service.h"
#include "components/prefs/pref_service.h"
#include "components/sync/base/model_type.h"
#include "components/sync/driver/sync_driver_switches.h"
#include "components/sync/protocol/model_type_state.pb.h"
#include "components/sync/test/fake_server/fake_server.h"
#include "components/webdata/common/web_data_service_consumer.h"
#include "content/public/browser/notification_service.h"
#include "testing/gmock/include/gmock/gmock.h"

using autofill::AutofillMetadata;
using autofill::AutofillProfile;
using autofill::CreditCard;
using autofill::data_util::TruncateUTF8;
using base::ASCIIToUTF16;
using wallet_helper::CreateDefaultSyncPaymentsCustomerData;
using wallet_helper::CreateDefaultSyncWalletAddress;
using wallet_helper::CreateDefaultSyncWalletCard;
using wallet_helper::CreateSyncPaymentsCustomerData;
using wallet_helper::CreateSyncWalletAddress;
using wallet_helper::CreateSyncWalletCard;
using wallet_helper::ExpectDefaultCreditCardValues;
using wallet_helper::ExpectDefaultProfileValues;
using wallet_helper::GetAccountWebDataService;
using wallet_helper::GetDefaultCreditCard;
using wallet_helper::GetPersonalDataManager;
using wallet_helper::GetProfileWebDataService;
using wallet_helper::GetServerAddressesMetadata;
using wallet_helper::GetServerCardsMetadata;
using wallet_helper::GetWalletDataModelTypeState;
using wallet_helper::kDefaultBillingAddressID;
using wallet_helper::kDefaultCardID;
using wallet_helper::kDefaultCustomerID;
using wallet_helper::UnmaskServerCard;

namespace {

ACTION_P(QuitMessageLoop, loop) {
  loop->Quit();
}

const char kLocalGuidA[] = "EDC609ED-7EEE-4F27-B00C-423242A9C44A";
const char kDifferentBillingAddressId[] = "another address entity ID";
const base::Time kArbitraryDefaultTime = base::Time::FromDoubleT(25);

template <class T>
class AutofillWebDataServiceConsumer : public WebDataServiceConsumer {
 public:
  AutofillWebDataServiceConsumer() {}

  virtual ~AutofillWebDataServiceConsumer() {}

  void OnWebDataServiceRequestDone(
      WebDataServiceBase::Handle handle,
      std::unique_ptr<WDTypedResult> result) override {
    result_ = std::move(static_cast<WDResult<T>*>(result.get())->GetValue());
    run_loop_.Quit();
  }

  void Wait() { run_loop_.Run(); }

  T& result() { return result_; }

 private:
  base::RunLoop run_loop_;
  T result_;
  DISALLOW_COPY_AND_ASSIGN(AutofillWebDataServiceConsumer);
};

class PersonalDataLoadedObserverMock
    : public autofill::PersonalDataManagerObserver {
 public:
  PersonalDataLoadedObserverMock() {}
  ~PersonalDataLoadedObserverMock() override {}

  MOCK_METHOD0(OnPersonalDataChanged, void());
};

class WaitForNextWalletUpdateChecker : public StatusChangeChecker,
                                       public syncer::SyncServiceObserver {
 public:
  explicit WaitForNextWalletUpdateChecker(
      browser_sync::ProfileSyncService* service)
      : service_(service),
        initial_marker_(GetInitialMarker(service)),
        scoped_observer_(this) {
    scoped_observer_.Add(service);
  }

  bool IsExitConditionSatisfied() override {
    // GetLastCycleSnapshot() returns by value, so make sure to capture it for
    // iterator use.
    const syncer::SyncCycleSnapshot snap = service_->GetLastCycleSnapshot();
    const syncer::ProgressMarkerMap& progress_markers =
        snap.download_progress_markers();
    auto marker_it = progress_markers.find(syncer::AUTOFILL_WALLET_DATA);
    if (marker_it == progress_markers.end()) {
      return false;
    }
    return marker_it->second != initial_marker_;
  }

  std::string GetDebugMessage() const override {
    return "Waiting for an updated Wallet progress marker.";
  }

  // syncer::SyncServiceObserver implementation.
  void OnSyncCycleCompleted(syncer::SyncService* sync) override {
    CheckExitCondition();
  }

 private:
  static std::string GetInitialMarker(
      const browser_sync::ProfileSyncService* service) {
    // GetLastCycleSnapshot() returns by value, so make sure to capture it for
    // iterator use.
    const syncer::SyncCycleSnapshot snap = service->GetLastCycleSnapshot();
    const syncer::ProgressMarkerMap& progress_markers =
        snap.download_progress_markers();
    auto marker_it = progress_markers.find(syncer::AUTOFILL_WALLET_DATA);
    if (marker_it == progress_markers.end()) {
      return "N/A";
    }
    return marker_it->second;
  }

  const browser_sync::ProfileSyncService* service_;
  const std::string initial_marker_;
  ScopedObserver<browser_sync::ProfileSyncService,
                 WaitForNextWalletUpdateChecker>
      scoped_observer_;
};

std::vector<std::unique_ptr<CreditCard>> GetServerCards(
    scoped_refptr<autofill::AutofillWebDataService> service) {
  AutofillWebDataServiceConsumer<std::vector<std::unique_ptr<CreditCard>>>
      consumer;
  service->GetServerCreditCards(&consumer);
  consumer.Wait();
  return std::move(consumer.result());
}

std::vector<std::unique_ptr<AutofillProfile>> GetServerProfiles(
    scoped_refptr<autofill::AutofillWebDataService> service) {
  AutofillWebDataServiceConsumer<std::vector<std::unique_ptr<AutofillProfile>>>
      consumer;
  service->GetServerProfiles(&consumer);
  consumer.Wait();
  return std::move(consumer.result());
}

#if !defined(OS_CHROMEOS)
std::unique_ptr<autofill::PaymentsCustomerData> GetPaymentsCustomerData(
    scoped_refptr<autofill::AutofillWebDataService> service) {
  AutofillWebDataServiceConsumer<
      std::unique_ptr<autofill::PaymentsCustomerData>>
      consumer;
  service->GetPaymentsCustomerData(&consumer);
  consumer.Wait();
  return std::move(consumer.result());
}
#endif

}  // namespace

class SingleClientWalletSyncTest : public SyncTest {
 public:
  SingleClientWalletSyncTest() : SyncTest(SINGLE_CLIENT) {
    test_clock_.SetNow(kArbitraryDefaultTime);
  }

  ~SingleClientWalletSyncTest() override {}

 protected:
  void RefreshAndWaitForOnPersonalDataChanged(
      autofill::PersonalDataManager* pdm) {
    WaitForOnPersonalDataChanged(/*should_trigger_refresh=*/true, pdm);
  }

  void WaitForOnPersonalDataChanged(bool should_trigger_refresh,
                                    autofill::PersonalDataManager* pdm) {
    pdm->AddObserver(&personal_data_observer_);
    if (should_trigger_refresh) {
      pdm->Refresh();
    }
    base::RunLoop run_loop;
    EXPECT_CALL(personal_data_observer_, OnPersonalDataChanged())
        .WillRepeatedly(QuitMessageLoop(&run_loop));
    run_loop.Run();
    pdm->RemoveObserver(&personal_data_observer_);
  }

  void WaitForNumberOfCards(autofill::PersonalDataManager* pdm,
                            size_t expected_count) {
    while (pdm->GetCreditCards().size() != expected_count) {
      WaitForOnPersonalDataChanged(/*should_trigger_refresh=*/false, pdm);
    }
  }

  void ExpectAddressesDiffInHistograms(int added, int removed) {
    histogram_tester_.ExpectUniqueSample("Autofill.WalletAddresses2.Added",
                                         /*bucket=*/added,
                                         /*count=*/1);
    histogram_tester_.ExpectUniqueSample("Autofill.WalletAddresses2.Removed",
                                         /*bucket=*/removed,
                                         /*count=*/1);
    histogram_tester_.ExpectUniqueSample(
        "Autofill.WalletAddresses2.AddedOrRemoved",
        /*bucket=*/added + removed,
        /*count=*/1);
  }

  void ExpectNoHistogramsForAddressesDiff() {
    histogram_tester_.ExpectTotalCount("Autofill.WalletAddresses2.Added", 0);
    histogram_tester_.ExpectTotalCount("Autofill.WalletAddresses2.Removed", 0);
    histogram_tester_.ExpectTotalCount(
        "Autofill.WalletAddresses2.AddedOrRemoved", 0);
  }

  void ExpectCardsDiffInHistograms(int added, int removed) {
    histogram_tester_.ExpectUniqueSample("Autofill.WalletCards2.Added",
                                         /*bucket=*/added,
                                         /*count=*/1);
    histogram_tester_.ExpectUniqueSample("Autofill.WalletCards2.Removed",
                                         /*bucket=*/removed,
                                         /*count=*/1);
    histogram_tester_.ExpectUniqueSample("Autofill.WalletCards2.AddedOrRemoved",
                                         /*bucket=*/added + removed,
                                         /*count=*/1);
  }

  void ExpectNoHistogramsForCardsDiff() {
    histogram_tester_.ExpectTotalCount("Autofill.WalletCards2.Added", 0);
    histogram_tester_.ExpectTotalCount("Autofill.WalletCards2.Removed", 0);
    histogram_tester_.ExpectTotalCount("Autofill.WalletCards2.AddedOrRemoved",
                                       0);
  }

  void AdvanceAutofillClockByOneDay() {
    test_clock_.Advance(base::TimeDelta::FromDays(1));
  }

  PersonalDataLoadedObserverMock personal_data_observer_;
  base::HistogramTester histogram_tester_;
  autofill::TestAutofillClock test_clock_;

 private:
  DISALLOW_COPY_AND_ASSIGN(SingleClientWalletSyncTest);
};

class SingleClientWalletSyncTestWithoutAccountStorage
    : public UssWalletSwitchToggler,
      public SingleClientWalletSyncTest {
 public:
  SingleClientWalletSyncTestWithoutAccountStorage() {
    InitWithFeatures(/*enabled_features=*/{},
                     /*disabled_features=*/
                     {autofill::features::kAutofillEnableAccountWalletStorage});
  }
};

IN_PROC_BROWSER_TEST_P(SingleClientWalletSyncTestWithoutAccountStorage,
                       DownloadProfileStorage) {
  GetFakeServer()->SetWalletData({CreateDefaultSyncWalletAddress(),
                                  CreateDefaultSyncWalletCard(),
                                  CreateDefaultSyncPaymentsCustomerData()});
  ASSERT_TRUE(SetupSync());

  auto profile_data = GetProfileWebDataService(0);
  ASSERT_NE(nullptr, profile_data);
  auto account_data = GetAccountWebDataService(0);
  ASSERT_EQ(nullptr, account_data);

  autofill::PersonalDataManager* pdm = GetPersonalDataManager(0);
  ASSERT_NE(nullptr, pdm);
  std::vector<CreditCard*> cards = pdm->GetCreditCards();
  ASSERT_EQ(1uL, cards.size());
  std::vector<AutofillProfile*> profiles = pdm->GetServerProfiles();
  ASSERT_EQ(1uL, profiles.size());

  // Check that the data was set correctly.
  ExpectDefaultCreditCardValues(*cards[0]);
  ExpectDefaultProfileValues(*profiles[0]);

  // Check that the data is stored in the profile storage.
  EXPECT_EQ(1U, GetServerCards(GetProfileWebDataService(0)).size());
  EXPECT_EQ(1U, GetServerProfiles(GetProfileWebDataService(0)).size());

  // No histograms for initial sync.
  ExpectNoHistogramsForCardsDiff();
  ExpectNoHistogramsForAddressesDiff();
}

class SingleClientWalletWithAccountStorageSyncTest
    : public UssWalletSwitchToggler,
      public SingleClientWalletSyncTest {
 public:
  SingleClientWalletWithAccountStorageSyncTest() {
    InitWithFeatures(
        /*enabled_features=*/{switches::kSyncUSSAutofillWalletData,
                              autofill::features::
                                  kAutofillEnableAccountWalletStorage},
        /*disabled_features=*/{});
  }
};

// ChromeOS does not support late signin after profile creation, so the test
// below does not apply, at least in the current form.
#if !defined(OS_CHROMEOS)
// The account storage requires USS, so we only test the USS implementation
// here.
IN_PROC_BROWSER_TEST_P(SingleClientWalletWithAccountStorageSyncTest,
                       DownloadAccountStorage_Card) {
  ASSERT_TRUE(SetupClients());
  autofill::PersonalDataManager* pdm = GetPersonalDataManager(0);
  pdm->OnSyncServiceInitialized(GetSyncService(0));

  GetFakeServer()->SetWalletData({CreateDefaultSyncWalletAddress(),
                                  CreateDefaultSyncWalletCard(),
                                  CreateDefaultSyncPaymentsCustomerData()});

  ASSERT_TRUE(GetClient(0)->SignInPrimaryAccount());
  ASSERT_TRUE(GetClient(0)->AwaitEngineInitialization());
  ASSERT_TRUE(AwaitQuiescence());
  ASSERT_EQ(syncer::SyncService::TransportState::ACTIVE,
            GetSyncService(0)->GetTransportState());
  ASSERT_TRUE(GetSyncService(0)->GetActiveDataTypes().Has(
      syncer::AUTOFILL_WALLET_DATA));

  auto profile_data = GetProfileWebDataService(0);
  ASSERT_NE(nullptr, profile_data);
  auto account_data = GetAccountWebDataService(0);
  ASSERT_NE(nullptr, account_data);

  // Check that no data is stored in the profile storage.
  EXPECT_EQ(0U, GetServerCards(profile_data).size());
  EXPECT_EQ(0U, GetServerProfiles(profile_data).size());

  // Check that one card is stored in the account storage.
  EXPECT_EQ(1U, GetServerCards(account_data).size());

  ASSERT_NE(nullptr, pdm);
  std::vector<CreditCard*> cards = pdm->GetCreditCards();
  ASSERT_EQ(1uL, cards.size());

  ExpectDefaultCreditCardValues(*cards[0]);

  // Now sign back out.
  GetClient(0)->SignOutPrimaryAccount();

  // Verify that sync is stopped.
  ASSERT_EQ(syncer::SyncService::TransportState::DISABLED,
            GetSyncService(0)->GetTransportState());
  ASSERT_FALSE(GetSyncService(0)->GetActiveDataTypes().Has(
      syncer::AUTOFILL_WALLET_DATA));

  // Wait for PDM to receive the data change.
  WaitForOnPersonalDataChanged(/*should_trigger_refresh=*/false, pdm);

  // Check that the account storage is now cleared.
  EXPECT_EQ(0U, GetServerCards(account_data).size());

  cards = pdm->GetCreditCards();
  EXPECT_EQ(0U, cards.size());
}
#endif  // !defined(OS_CHROMEOS)

class SingleClientWalletSyncTestWithDefaultFeatures
    : public UssWalletSwitchToggler,
      public SingleClientWalletSyncTest {
 public:
  SingleClientWalletSyncTestWithDefaultFeatures() { InitWithDefaultFeatures(); }
};

IN_PROC_BROWSER_TEST_P(SingleClientWalletSyncTestWithDefaultFeatures,
                       EnabledByDefault) {
  ASSERT_TRUE(SetupSync());
  ASSERT_TRUE(GetClient(0)->service()->GetActiveDataTypes().Has(
      syncer::AUTOFILL_WALLET_DATA));
  // TODO(pvalenzuela): Assert that the local root node for AUTOFILL_WALLET_DATA
  // exists.
  ASSERT_TRUE(GetClient(0)->service()->GetActiveDataTypes().Has(
      syncer::AUTOFILL_WALLET_METADATA));
}

// Wallet data should get cleared from the database when sync is disabled.
IN_PROC_BROWSER_TEST_P(SingleClientWalletSyncTestWithDefaultFeatures,
                       ClearOnDisableSync) {
  GetFakeServer()->SetWalletData({CreateDefaultSyncWalletAddress(),
                                  CreateDefaultSyncWalletCard(),
                                  CreateDefaultSyncPaymentsCustomerData()});
  ASSERT_TRUE(SetupSync());

  autofill::PersonalDataManager* pdm = GetPersonalDataManager(0);
  ASSERT_NE(nullptr, pdm);

  // Make sure the data & metadata is in the DB.
  ASSERT_EQ(1uL, pdm->GetServerProfiles().size());
  ASSERT_EQ(1uL, pdm->GetCreditCards().size());
  ASSERT_EQ(kDefaultCustomerID, pdm->GetPaymentsCustomerData()->customer_id);
  ASSERT_EQ(1U, GetServerCardsMetadata(0).size());
  ASSERT_EQ(1U, GetServerAddressesMetadata(0).size());

  // Disable sync, the data & metadata should be gone.
  GetSyncService(0)->StopAndClear();
  WaitForNumberOfCards(pdm, 0);

  EXPECT_EQ(0uL, pdm->GetServerProfiles().size());
  EXPECT_EQ(0uL, pdm->GetCreditCards().size());
  EXPECT_EQ(nullptr, pdm->GetPaymentsCustomerData());
  EXPECT_EQ(0U, GetServerCardsMetadata(0).size());
  EXPECT_EQ(0U, GetServerAddressesMetadata(0).size());

  // Turn sync on again, the data should come back.
  GetSyncService(0)->GetUserSettings()->SetSyncRequested(true);
  // StopAndClear() also clears the "first setup complete" flag, so set it
  // again.
  GetSyncService(0)->GetUserSettings()->SetFirstSetupComplete();
  // Wait until Sync restores the card and it arrives at PDM.
  WaitForNumberOfCards(pdm, 1);

  EXPECT_EQ(1uL, pdm->GetServerProfiles().size());
  EXPECT_EQ(1uL, pdm->GetCreditCards().size());
  EXPECT_EQ(kDefaultCustomerID, pdm->GetPaymentsCustomerData()->customer_id);
  EXPECT_EQ(1U, GetServerCardsMetadata(0).size());
  EXPECT_EQ(1U, GetServerAddressesMetadata(0).size());

  // No histograms for initial sync & for clearing.
  ExpectNoHistogramsForCardsDiff();
  ExpectNoHistogramsForAddressesDiff();
}

// Wallet data should get cleared from the database when sync is (temporarily)
// stopped, e.g. due to the Sync feature toggle in Android settings.
IN_PROC_BROWSER_TEST_P(SingleClientWalletSyncTestWithDefaultFeatures,
                       ClearOnStopSync) {
  GetFakeServer()->SetWalletData({CreateDefaultSyncWalletAddress(),
                                  CreateDefaultSyncWalletCard(),
                                  CreateDefaultSyncPaymentsCustomerData()});
  ASSERT_TRUE(SetupSync());

  autofill::PersonalDataManager* pdm = GetPersonalDataManager(0);
  ASSERT_NE(nullptr, pdm);

  // Make sure the data & metadata is in the DB.
  ASSERT_EQ(1uL, pdm->GetServerProfiles().size());
  ASSERT_EQ(1uL, pdm->GetCreditCards().size());
  ASSERT_EQ(kDefaultCustomerID, pdm->GetPaymentsCustomerData()->customer_id);
  ASSERT_EQ(1U, GetServerCardsMetadata(0).size());
  ASSERT_EQ(1U, GetServerAddressesMetadata(0).size());

  // Stop sync, the data & metadata should be gone.
  GetSyncService(0)->GetUserSettings()->SetSyncRequested(false);
  WaitForNumberOfCards(pdm, 0);

  EXPECT_EQ(0uL, pdm->GetServerProfiles().size());
  EXPECT_EQ(0uL, pdm->GetCreditCards().size());
  EXPECT_EQ(nullptr, pdm->GetPaymentsCustomerData());
  EXPECT_EQ(0U, GetServerCardsMetadata(0).size());
  EXPECT_EQ(0U, GetServerAddressesMetadata(0).size());

  // Turn sync on again, the data should come back.
  GetSyncService(0)->GetUserSettings()->SetSyncRequested(true);
  // Wait until Sync restores the card and it arrives at PDM.
  WaitForNumberOfCards(pdm, 1);

  EXPECT_EQ(1uL, pdm->GetServerProfiles().size());
  EXPECT_EQ(1uL, pdm->GetCreditCards().size());
  EXPECT_EQ(kDefaultCustomerID, pdm->GetPaymentsCustomerData()->customer_id);
  EXPECT_EQ(1U, GetServerCardsMetadata(0).size());
  EXPECT_EQ(1U, GetServerAddressesMetadata(0).size());

  // No histograms for initial sync & for clearing.
  ExpectNoHistogramsForCardsDiff();
  ExpectNoHistogramsForAddressesDiff();
}

// ChromeOS does not sign out, so the test below does not apply.
#if !defined(OS_CHROMEOS)
// Wallet data should get cleared from the database when the user signs out.
IN_PROC_BROWSER_TEST_P(SingleClientWalletSyncTestWithDefaultFeatures,
                       ClearOnSignOut) {
  GetFakeServer()->SetWalletData({CreateDefaultSyncWalletAddress(),
                                  CreateDefaultSyncWalletCard(),
                                  CreateDefaultSyncPaymentsCustomerData()});
  ASSERT_TRUE(SetupSync());

  autofill::PersonalDataManager* pdm = GetPersonalDataManager(0);
  ASSERT_NE(nullptr, pdm);

  // Make sure the data & metadata is in the DB.
  ASSERT_EQ(1uL, pdm->GetServerProfiles().size());
  ASSERT_EQ(1uL, pdm->GetCreditCards().size());
  ASSERT_EQ(kDefaultCustomerID, pdm->GetPaymentsCustomerData()->customer_id);
  ASSERT_EQ(1U, GetServerCardsMetadata(0).size());
  ASSERT_EQ(1U, GetServerAddressesMetadata(0).size());

  // Signout, the data & metadata should be gone.
  GetClient(0)->SignOutPrimaryAccount();
  WaitForNumberOfCards(pdm, 0);

  EXPECT_EQ(0uL, pdm->GetServerProfiles().size());
  EXPECT_EQ(0uL, pdm->GetCreditCards().size());
  EXPECT_EQ(nullptr, pdm->GetPaymentsCustomerData());
  EXPECT_EQ(0U, GetServerCardsMetadata(0).size());
  EXPECT_EQ(0U, GetServerAddressesMetadata(0).size());

  // No histograms for initial sync & for clearing.
  ExpectNoHistogramsForCardsDiff();
  ExpectNoHistogramsForAddressesDiff();
}
#endif  // !defined(OS_CHROMEOS)

// Wallet is not using incremental updates. Make sure existing data gets
// replaced when synced down.
IN_PROC_BROWSER_TEST_P(SingleClientWalletSyncTestWithDefaultFeatures,
                       NewSyncDataShouldReplaceExistingData) {
  GetFakeServer()->SetWalletData(
      {CreateSyncWalletCard(/*name=*/"card-1", /*last_four=*/"0001",
                            kDefaultBillingAddressID),
       CreateSyncWalletAddress(/*name=*/"address-1", /*company=*/"Company-1"),
       CreateDefaultSyncPaymentsCustomerData()});
  ASSERT_TRUE(SetupSync());

  // Make sure the data is in the DB.
  autofill::PersonalDataManager* pdm = GetPersonalDataManager(0);
  ASSERT_NE(nullptr, pdm);
  std::vector<CreditCard*> cards = pdm->GetCreditCards();
  ASSERT_EQ(1uL, cards.size());
  EXPECT_EQ(ASCIIToUTF16("0001"), cards[0]->LastFourDigits());
  std::vector<AutofillProfile*> profiles = pdm->GetServerProfiles();
  ASSERT_EQ(1uL, profiles.size());
  EXPECT_EQ("Company-1", TruncateUTF8(base::UTF16ToUTF8(
                             profiles[0]->GetRawInfo(autofill::COMPANY_NAME))));
  EXPECT_EQ(kDefaultCustomerID, pdm->GetPaymentsCustomerData()->customer_id);

  // No histograms for initial sync.
  ExpectNoHistogramsForCardsDiff();
  ExpectNoHistogramsForAddressesDiff();

  // Put some completely new data in the sync server.
  GetFakeServer()->SetWalletData(
      {CreateSyncWalletCard(/*name=*/"new-card", /*last_four=*/"0002",
                            kDefaultBillingAddressID),
       CreateSyncWalletAddress(/*name=*/"new-address", /*company=*/"Company-2"),
       CreateSyncPaymentsCustomerData(/*customer_id=*/"newid")});

  // Constructing the checker captures the current progress marker. Make sure to
  // do that before triggering the fetch.
  WaitForNextWalletUpdateChecker checker(GetSyncService(0));
  // Trigger a sync and wait for the new data to arrive.
  TriggerSyncForModelTypes(0,
                           syncer::ModelTypeSet(syncer::AUTOFILL_WALLET_DATA));
  ASSERT_TRUE(checker.Wait());

  // Make sure only the new data is present.
  cards = pdm->GetCreditCards();
  ASSERT_EQ(1uL, cards.size());
  EXPECT_EQ(ASCIIToUTF16("0002"), cards[0]->LastFourDigits());
  profiles = pdm->GetServerProfiles();
  ASSERT_EQ(1uL, profiles.size());
  EXPECT_EQ("Company-2", TruncateUTF8(base::UTF16ToUTF8(
                             profiles[0]->GetRawInfo(autofill::COMPANY_NAME))));
  EXPECT_EQ("newid", pdm->GetPaymentsCustomerData()->customer_id);

  // Expect correct histograms are reported for the update.
  ExpectCardsDiffInHistograms(/*added=*/1, /*removed=*/1);
  ExpectAddressesDiffInHistograms(/*added=*/1, /*removed=*/1);
}

// Wallet is not using incremental updates. The server either sends a non-empty
// update with deletion gc directives and with the (possibly empty) full data
// set, or (more often) an empty update.
IN_PROC_BROWSER_TEST_P(SingleClientWalletSyncTestWithDefaultFeatures,
                       EmptyUpdatesAreIgnored) {
  GetFakeServer()->SetWalletData(
      {CreateSyncWalletCard(/*name=*/"card-1", /*last_four=*/"0001",
                            kDefaultBillingAddressID),
       CreateSyncWalletAddress(/*name=*/"address-1", /*company=*/"Company-1"),
       CreateDefaultSyncPaymentsCustomerData()});
  ASSERT_TRUE(SetupSync());

  // Make sure the card is in the DB.
  autofill::PersonalDataManager* pdm = GetPersonalDataManager(0);
  ASSERT_NE(nullptr, pdm);
  std::vector<CreditCard*> cards = pdm->GetCreditCards();
  ASSERT_EQ(1uL, cards.size());
  EXPECT_EQ(ASCIIToUTF16("0001"), cards[0]->LastFourDigits());
  std::vector<AutofillProfile*> profiles = pdm->GetServerProfiles();
  ASSERT_EQ(1uL, profiles.size());
  EXPECT_EQ("Company-1", TruncateUTF8(base::UTF16ToUTF8(
                             profiles[0]->GetRawInfo(autofill::COMPANY_NAME))));
  EXPECT_EQ(kDefaultCustomerID, pdm->GetPaymentsCustomerData()->customer_id);

  // Do not change anything on the server so that the update forced below is an
  // empty one.

  // Constructing the checker captures the current progress marker. Make sure to
  // do that before triggering the fetch.
  WaitForNextWalletUpdateChecker checker(GetSyncService(0));
  // Trigger a sync and wait for the new data to arrive.
  TriggerSyncForModelTypes(0,
                           syncer::ModelTypeSet(syncer::AUTOFILL_WALLET_DATA));
  ASSERT_TRUE(checker.Wait());

  // Refresh the pdm so that it gets cards from autofill table.
  RefreshAndWaitForOnPersonalDataChanged(pdm);

  // Make sure the same data is present on the client.
  cards = pdm->GetCreditCards();
  ASSERT_EQ(1uL, cards.size());
  EXPECT_EQ(ASCIIToUTF16("0001"), cards[0]->LastFourDigits());
  profiles = pdm->GetServerProfiles();
  ASSERT_EQ(1uL, profiles.size());
  EXPECT_EQ("Company-1", TruncateUTF8(base::UTF16ToUTF8(
                             profiles[0]->GetRawInfo(autofill::COMPANY_NAME))));
  EXPECT_EQ(kDefaultCustomerID, pdm->GetPaymentsCustomerData()->customer_id);

  // No histograms for initial sync, nor for an empty update.
  ExpectNoHistogramsForCardsDiff();
  ExpectNoHistogramsForAddressesDiff();
}

// Check on top of EmptyUpdatesAreIgnored that the new progress marker is stored
// for empty updates. This is a regression test for crbug.com/924447.
IN_PROC_BROWSER_TEST_P(SingleClientWalletSyncTestWithDefaultFeatures,
                       EmptyUpdatesUpdateProgressMarker) {
  GetFakeServer()->SetWalletData(
      {CreateSyncWalletCard(/*name=*/"card-1", /*last_four=*/"0001",
                            kDefaultBillingAddressID),
       CreateSyncWalletAddress(/*name=*/"address-1", /*company=*/"Company-1"),
       CreateDefaultSyncPaymentsCustomerData()});
  ASSERT_TRUE(SetupSync());

  sync_pb::ModelTypeState state_before = GetWalletDataModelTypeState(0);

  // Do not change anything on the server so that the update forced below is an
  // empty one.

  // Constructing the checker captures the current progress marker. Make sure to
  // do that before triggering the fetch.
  WaitForNextWalletUpdateChecker checker(GetSyncService(0));
  // Trigger a sync and wait for the new data to arrive.
  TriggerSyncForModelTypes(0,
                           syncer::ModelTypeSet(syncer::AUTOFILL_WALLET_DATA));
  ASSERT_TRUE(checker.Wait());

  sync_pb::ModelTypeState state_after = GetWalletDataModelTypeState(0);
  EXPECT_NE(state_before.progress_marker().token(),
            state_after.progress_marker().token());
}

// If the server sends the same cards and addresses again, they should not
// change on the client. We should also not overwrite existing metadata.
IN_PROC_BROWSER_TEST_P(SingleClientWalletSyncTestWithDefaultFeatures,
                       SameUpdatesAreIgnored) {
  GetFakeServer()->SetWalletData(
      {CreateSyncWalletCard(/*name=*/"card-1", /*last_four=*/"0001",
                            kDefaultBillingAddressID),
       CreateSyncWalletAddress(/*name=*/"address-1", /*company=*/"Company-1"),
       CreateDefaultSyncPaymentsCustomerData()});
  ASSERT_TRUE(SetupSync());

  // No histograms for initial sync.
  ExpectNoHistogramsForCardsDiff();
  ExpectNoHistogramsForAddressesDiff();

  // Refresh the pdm so that it gets data from autofill table.
  autofill::PersonalDataManager* pdm = GetPersonalDataManager(0);
  ASSERT_NE(nullptr, pdm);
  RefreshAndWaitForOnPersonalDataChanged(pdm);

  // Record use of to get non-default metadata values.
  std::vector<CreditCard*> cards = pdm->GetCreditCards();
  ASSERT_EQ(1uL, cards.size());
  pdm->RecordUseOf(*cards[0]);
  std::vector<AutofillProfile*> profiles = pdm->GetServerProfiles();
  ASSERT_EQ(1uL, profiles.size());
  pdm->RecordUseOf(*profiles[0]);

  // Keep the same data (only change the customer data to force the FakeServer
  // to send the full update).
  GetFakeServer()->SetWalletData(
      {CreateSyncWalletCard(/*name=*/"card-1", /*last_four=*/"0001",
                            kDefaultBillingAddressID),
       CreateSyncWalletAddress(/*name=*/"address-1", /*company=*/"Company-1"),
       CreateSyncPaymentsCustomerData("different")});

  // Constructing the checker captures the current progress marker. Make sure to
  // do that before triggering the fetch.
  WaitForNextWalletUpdateChecker checker(GetSyncService(0));
  // Trigger a sync and wait for the new data to arrive.
  TriggerSyncForModelTypes(0,
                           syncer::ModelTypeSet(syncer::AUTOFILL_WALLET_DATA));
  ASSERT_TRUE(checker.Wait());

  // Refresh the pdm so that it gets cards from autofill table.
  RefreshAndWaitForOnPersonalDataChanged(pdm);

  // Make sure the data is present on the client.
  cards = pdm->GetCreditCards();
  ASSERT_EQ(1uL, cards.size());
  EXPECT_EQ(ASCIIToUTF16("0001"), cards[0]->LastFourDigits());
  profiles = pdm->GetServerProfiles();
  ASSERT_EQ(1uL, profiles.size());
  EXPECT_EQ("Company-1", TruncateUTF8(base::UTF16ToUTF8(
                             profiles[0]->GetRawInfo(autofill::COMPANY_NAME))));
  EXPECT_EQ("different", pdm->GetPaymentsCustomerData()->customer_id);

  // Expect correct histograms for the (no) update.
  ExpectCardsDiffInHistograms(/*added=*/0, /*removed=*/0);
  ExpectAddressesDiffInHistograms(/*added=*/0, /*removed=*/0);

  // Test that the non-default metadata values stayed around.
  std::map<std::string, AutofillMetadata> cards_metadata =
      GetServerCardsMetadata(0);
  EXPECT_EQ(1U, cards_metadata.size());
  EXPECT_EQ(2U, cards_metadata.begin()->second.use_count);
  std::map<std::string, AutofillMetadata> addresses_metadata =
      GetServerAddressesMetadata(0);
  EXPECT_EQ(1U, addresses_metadata.size());
  // TODO(crbug.com/941498): Once RecordUseOf() is fixed for server addresses,
  // change this expectation back to 2.
  EXPECT_EQ(1U, addresses_metadata.begin()->second.use_count);
}

// If the server sends the same cards and addresses with changed data, they
// should change on the client.
IN_PROC_BROWSER_TEST_P(SingleClientWalletSyncTestWithDefaultFeatures,
                       ChangedEntityGetsUpdated) {
  GetFakeServer()->SetWalletData(
      {CreateSyncWalletCard(/*name=*/"card-1", /*last_four=*/"0002",
                            kDefaultBillingAddressID),
       CreateSyncWalletAddress(/*name=*/"address-1", /*company=*/"Company-2"),
       CreateDefaultSyncPaymentsCustomerData()});
  ASSERT_TRUE(SetupSync());

  // Refresh the pdm so that it gets data from autofill table.
  autofill::PersonalDataManager* pdm = GetPersonalDataManager(0);
  ASSERT_NE(nullptr, pdm);
  RefreshAndWaitForOnPersonalDataChanged(pdm);

  // Record use of to get non-default metadata values.
  std::vector<CreditCard*> cards = pdm->GetCreditCards();
  ASSERT_EQ(1uL, cards.size());
  pdm->RecordUseOf(*cards[0]);
  std::vector<AutofillProfile*> profiles = pdm->GetServerProfiles();
  ASSERT_EQ(1uL, profiles.size());
  pdm->RecordUseOf(*profiles[0]);

  // Update the data (also change the customer data to force the full update as
  // FakeServer computes the hash for progress markers only based on ids).
  GetFakeServer()->SetWalletData(
      {CreateSyncWalletCard(/*name=*/"card-1", /*last_four=*/"0001",
                            kDefaultBillingAddressID),
       CreateSyncWalletAddress(/*name=*/"address-1", /*company=*/"Company-1"),
       CreateSyncPaymentsCustomerData("different")});

  // Constructing the checker captures the current progress marker. Make sure to
  // do that before triggering the fetch.
  WaitForNextWalletUpdateChecker checker(GetSyncService(0));
  // Trigger a sync and wait for the new data to arrive.
  TriggerSyncForModelTypes(0,
                           syncer::ModelTypeSet(syncer::AUTOFILL_WALLET_DATA));
  ASSERT_TRUE(checker.Wait());

  // Refresh the pdm so that it gets cards from autofill table.
  RefreshAndWaitForOnPersonalDataChanged(pdm);

  // Make sure the data is present on the client.
  cards = pdm->GetCreditCards();
  ASSERT_EQ(1uL, cards.size());
  EXPECT_EQ(ASCIIToUTF16("0001"), cards[0]->LastFourDigits());
  profiles = pdm->GetServerProfiles();
  ASSERT_EQ(1uL, profiles.size());
  EXPECT_EQ("Company-1", TruncateUTF8(base::UTF16ToUTF8(
                             profiles[0]->GetRawInfo(autofill::COMPANY_NAME))));

  // Expect correct histograms for the (remove&add) update.
  ExpectCardsDiffInHistograms(/*added=*/1, /*removed=*/1);
  ExpectAddressesDiffInHistograms(/*added=*/1, /*removed=*/1);

  // Test that the non-default metadata values stayed around.
  std::map<std::string, AutofillMetadata> cards_metadata =
      GetServerCardsMetadata(0);
  EXPECT_EQ(1U, cards_metadata.size());
  EXPECT_EQ(2U, cards_metadata.begin()->second.use_count);
  std::map<std::string, AutofillMetadata> addresses_metadata =
      GetServerAddressesMetadata(0);
  EXPECT_EQ(1U, addresses_metadata.size());
  // Metadata for server addresses cannot be preserved because the id changes
  // since it is generated on the client as a hash of all the data fields. As a
  // result, the metadata entry gets deleted and recreated under a different id
  // and has a default use count.
  EXPECT_EQ(1U, addresses_metadata.begin()->second.use_count);
}

// If the server sends the same cards again, they should not change on the
// client even if the cards on the client are unmasked.
IN_PROC_BROWSER_TEST_P(SingleClientWalletSyncTestWithDefaultFeatures,
                       SameUpdatesAreIgnoredWhenLocalCardsUnmasked) {
// We need to allow storing full server cards for this test to work properly.
#if defined(OS_LINUX) && !defined(OS_CHROMEOS)
  base::CommandLine::ForCurrentProcess()->AppendSwitch(
      autofill::switches::kEnableOfferStoreUnmaskedWalletCards);
#endif
  GetFakeServer()->SetWalletData(
      {CreateSyncWalletCard(/*name=*/"card-1", /*last_four=*/"0001",
                            kDefaultBillingAddressID),
       CreateDefaultSyncPaymentsCustomerData()});
  ASSERT_TRUE(SetupSync());

  // No histograms for initial sync.
  ExpectNoHistogramsForCardsDiff();

  // Check we have the card locally.
  autofill::PersonalDataManager* pdm = GetPersonalDataManager(0);
  ASSERT_NE(nullptr, pdm);
  std::vector<CreditCard*> cards = pdm->GetCreditCards();
  ASSERT_EQ(1uL, cards.size());
  EXPECT_EQ(ASCIIToUTF16("0001"), cards[0]->LastFourDigits());
  EXPECT_EQ(CreditCard::MASKED_SERVER_CARD, cards[0]->record_type());

  // Unmask the card (the full card number has to start with "34" to match the
  // type of the masked card which is by default AMEX in the tests).
  UnmaskServerCard(0, *cards[0], base::UTF8ToUTF16("3404000300020001"));

  // Keep the same data (only change the customer data to force the FakeServer
  // to send the full update).
  GetFakeServer()->SetWalletData(
      {CreateSyncWalletCard(/*name=*/"card-1", /*last_four=*/"0001",
                            kDefaultBillingAddressID),
       CreateSyncPaymentsCustomerData("different")});

  // Constructing the checker captures the current progress marker. Make sure to
  // do that before triggering the fetch.
  WaitForNextWalletUpdateChecker checker(GetSyncService(0));
  // Trigger a sync and wait for the new data to arrive.
  TriggerSyncForModelTypes(0,
                           syncer::ModelTypeSet(syncer::AUTOFILL_WALLET_DATA));
  ASSERT_TRUE(checker.Wait());

  // Refresh the pdm so that it gets cards from autofill table.
  RefreshAndWaitForOnPersonalDataChanged(pdm);

  // Make sure the data is present on the client.
  cards = pdm->GetCreditCards();
  ASSERT_EQ(1uL, cards.size());
  EXPECT_EQ(ASCIIToUTF16("0001"), cards[0]->LastFourDigits());
  EXPECT_EQ(CreditCard::FULL_SERVER_CARD, cards[0]->record_type());
  EXPECT_EQ("different", pdm->GetPaymentsCustomerData()->customer_id);

  // Expect correct histograms for the (no) update.
  ExpectCardsDiffInHistograms(/*added=*/0, /*removed=*/0);
}

// Tests that we do not report any diff metric on startup without getting a new
// full update from the server. The test makes the initial sync in the first run
// and then performs only an empty update after restart (see the test below).
IN_PROC_BROWSER_TEST_P(SingleClientWalletSyncTestWithDefaultFeatures,
                       PRE_NoMetricReportedOnStartup) {
  GetFakeServer()->SetWalletData(
      {CreateSyncWalletCard(/*name=*/"card-1", /*last_four=*/"0001",
                            kDefaultBillingAddressID),
       CreateSyncWalletAddress(/*name=*/"address-1", /*company=*/"Company-1"),
       CreateDefaultSyncPaymentsCustomerData()});
  ASSERT_TRUE(SetupSync());

  // Make sure the data is present on the client.
  autofill::PersonalDataManager* pdm = GetPersonalDataManager(0);
  ASSERT_EQ(1uL, pdm->GetCreditCards().size());
  ASSERT_EQ(1uL, pdm->GetServerProfiles().size());
  ASSERT_EQ(kDefaultCustomerID, pdm->GetPaymentsCustomerData()->customer_id);

  // No histograms for initial sync.
  ExpectNoHistogramsForCardsDiff();
  ExpectNoHistogramsForAddressesDiff();
}

IN_PROC_BROWSER_TEST_P(SingleClientWalletSyncTestWithDefaultFeatures,
                       NoMetricReportedOnStartup) {
  // Set the same data on the server so that we get an empty update (this is
  // based on a hash of the data).
  GetFakeServer()->SetWalletData(
      {CreateSyncWalletCard(/*name=*/"card-1", /*last_four=*/"0001",
                            kDefaultBillingAddressID),
       CreateSyncWalletAddress(/*name=*/"address-1", /*company=*/"Company-1"),
       CreateDefaultSyncPaymentsCustomerData()});
  ASSERT_TRUE(SetupSync());

  // Make sure the data is still present on the client.
  autofill::PersonalDataManager* pdm = GetPersonalDataManager(0);
  ASSERT_EQ(1uL, pdm->GetCreditCards().size());
  ASSERT_EQ(1uL, pdm->GetServerProfiles().size());
  ASSERT_EQ(kDefaultCustomerID, pdm->GetPaymentsCustomerData()->customer_id);

  // No histograms for the startup, neither for the empty update.
  ExpectNoHistogramsForCardsDiff();
  ExpectNoHistogramsForAddressesDiff();
}

// Tests that we _do_ report one diff metric on startup when we get an update
// from the server. The test makes the initial sync in the first run and then
// performs one full update after restart (see the test below). This in
// particular tests that the Directory implementation handles well the case that
// the update is received before MergeDataAndStartSyncing.
IN_PROC_BROWSER_TEST_P(SingleClientWalletSyncTestWithDefaultFeatures,
                       PRE_OneMetricReportedOnStartup) {
  GetFakeServer()->SetWalletData(
      {CreateSyncWalletCard(/*name=*/"card-1", /*last_four=*/"0001",
                            kDefaultBillingAddressID),
       CreateSyncWalletAddress(/*name=*/"address-1", /*company=*/"Company-1"),
       CreateDefaultSyncPaymentsCustomerData()});
  ASSERT_TRUE(SetupSync());

  // Make sure the data is present on the client.
  autofill::PersonalDataManager* pdm = GetPersonalDataManager(0);
  ASSERT_EQ(1uL, pdm->GetCreditCards().size());
  ASSERT_EQ(1uL, pdm->GetServerProfiles().size());
  ASSERT_EQ(kDefaultCustomerID, pdm->GetPaymentsCustomerData()->customer_id);

  // No histograms for initial sync.
  ExpectNoHistogramsForCardsDiff();
  ExpectNoHistogramsForAddressesDiff();
}

IN_PROC_BROWSER_TEST_P(SingleClientWalletSyncTestWithDefaultFeatures,
                       OneMetricReportedOnStartup) {
  // Set different data so that we get a full update.
  GetFakeServer()->SetWalletData(
      {CreateSyncWalletCard(/*name=*/"card-1", /*last_four=*/"0001",
                            kDefaultBillingAddressID),
       CreateSyncWalletCard(/*name=*/"card-2", /*last_four=*/"0002",
                            kDefaultBillingAddressID),
       CreateSyncWalletAddress(/*name=*/"address-1", /*company=*/"Company-1"),
       CreateDefaultSyncPaymentsCustomerData()});
  ASSERT_TRUE(SetupSync());

  // Make sure the data is still present on the client.
  autofill::PersonalDataManager* pdm = GetPersonalDataManager(0);
  ASSERT_EQ(2uL, pdm->GetCreditCards().size());
  ASSERT_EQ(1uL, pdm->GetServerProfiles().size());
  ASSERT_EQ(kDefaultCustomerID, pdm->GetPaymentsCustomerData()->customer_id);

  // One histogram for startup / full update.
  ExpectCardsDiffInHistograms(/*added=*/1, /*removed=*/0);
  ExpectAddressesDiffInHistograms(/*added=*/0, /*removed=*/0);
}

// Tests that we do report age metric on startup.
IN_PROC_BROWSER_TEST_P(SingleClientWalletSyncTestWithDefaultFeatures,
                       PRE_UseDateMetricReportedOnStartup) {
  GetFakeServer()->SetWalletData(
      {CreateSyncWalletCard(/*name=*/"card-1", /*last_four=*/"0001",
                            kDefaultBillingAddressID),
       CreateSyncWalletAddress(/*name=*/"address-1", /*company=*/"Company-1"),
       CreateDefaultSyncPaymentsCustomerData()});
  ASSERT_TRUE(SetupSync());

  // Make sure the data is present on the client.
  autofill::PersonalDataManager* pdm = GetPersonalDataManager(0);
  ASSERT_EQ(1uL, pdm->GetCreditCards().size());
  ASSERT_EQ(1uL, pdm->GetServerProfiles().size());
  ASSERT_EQ(kDefaultCustomerID, pdm->GetPaymentsCustomerData()->customer_id);

  // Here, we would ideally test that no metrics get recorded during initial
  // sync. Due to design differences between USS&Directory, we cannot make it
  // work (the metadata syncable service has no reliable way to tell it is
  // initial sync).
}

IN_PROC_BROWSER_TEST_P(SingleClientWalletSyncTestWithDefaultFeatures,
                       UseDateMetricReportedOnStartup) {
  // Advance the clock to get a reasonable value.
  AdvanceAutofillClockByOneDay();

  // Set the same data on the server so that we get an empty update (this is
  // based on a hash of the data).
  GetFakeServer()->SetWalletData(
      {CreateSyncWalletCard(/*name=*/"card-1", /*last_four=*/"0001",
                            kDefaultBillingAddressID),
       CreateSyncWalletAddress(/*name=*/"address-1", /*company=*/"Company-1"),
       CreateDefaultSyncPaymentsCustomerData()});
  ASSERT_TRUE(SetupSync());

  // Make sure the data is still present on the client.
  autofill::PersonalDataManager* pdm = GetPersonalDataManager(0);
  ASSERT_EQ(1uL, pdm->GetCreditCards().size());
  ASSERT_EQ(1uL, pdm->GetServerProfiles().size());
  ASSERT_EQ(kDefaultCustomerID, pdm->GetPaymentsCustomerData()->customer_id);

  // The metric gets recorded.
  histogram_tester_.ExpectTotalCount("Autofill.WalletUseDate.Card", 1);
  histogram_tester_.ExpectTimeBucketCount(
      "Autofill.WalletUseDate.Card",
      /*sample=*/base::TimeDelta::FromDays(1),
      /*count=*/1);
  histogram_tester_.ExpectTotalCount("Autofill.WalletUseDate.Address", 1);
  histogram_tester_.ExpectTimeBucketCount(
      "Autofill.WalletUseDate.Address",
      /*sample=*/base::TimeDelta::FromDays(1),
      /*count=*/1);
}

// Wallet data should get cleared from the database when the wallet sync type
// flag is disabled.
IN_PROC_BROWSER_TEST_P(SingleClientWalletSyncTestWithDefaultFeatures,
                       ClearOnDisableWalletSync) {
  GetFakeServer()->SetWalletData({CreateDefaultSyncWalletAddress(),
                                  CreateDefaultSyncWalletCard(),
                                  CreateDefaultSyncPaymentsCustomerData()});
  ASSERT_TRUE(SetupSync());

  autofill::PersonalDataManager* pdm = GetPersonalDataManager(0);
  ASSERT_NE(nullptr, pdm);

  // Make sure the data & metadata is in the DB.
  ASSERT_EQ(1uL, pdm->GetServerProfiles().size());
  ASSERT_EQ(1uL, pdm->GetCreditCards().size());
  ASSERT_EQ(kDefaultCustomerID, pdm->GetPaymentsCustomerData()->customer_id);
  ASSERT_EQ(1U, GetServerCardsMetadata(0).size());
  ASSERT_EQ(1U, GetServerAddressesMetadata(0).size());

  // Turn off autofill sync, the data & metadata should be gone.
  ASSERT_TRUE(GetClient(0)->DisableSyncForDatatype(syncer::AUTOFILL));
  WaitForOnPersonalDataChanged(/*should_trigger_refresh=*/false, pdm);

  EXPECT_EQ(0uL, pdm->GetServerProfiles().size());
  EXPECT_EQ(0uL, pdm->GetCreditCards().size());
  EXPECT_EQ(nullptr, pdm->GetPaymentsCustomerData());
  EXPECT_EQ(0U, GetServerCardsMetadata(0).size());
  EXPECT_EQ(0U, GetServerAddressesMetadata(0).size());

  // No histograms for initial sync & for clearing.
  ExpectNoHistogramsForCardsDiff();
  ExpectNoHistogramsForAddressesDiff();
}

// Wallet data should get cleared from the database when the wallet autofill
// integration flag is disabled.
IN_PROC_BROWSER_TEST_P(SingleClientWalletSyncTestWithDefaultFeatures,
                       ClearOnDisableWalletAutofill) {
  GetFakeServer()->SetWalletData({CreateDefaultSyncWalletAddress(),
                                  CreateDefaultSyncWalletCard(),
                                  CreateDefaultSyncPaymentsCustomerData()});
  ASSERT_TRUE(SetupSync());

  autofill::PersonalDataManager* pdm = GetPersonalDataManager(0);
  ASSERT_NE(nullptr, pdm);

  // Make sure the data & metadata is in the DB.
  ASSERT_EQ(1uL, pdm->GetServerProfiles().size());
  ASSERT_EQ(1uL, pdm->GetCreditCards().size());
  ASSERT_EQ(kDefaultCustomerID, pdm->GetPaymentsCustomerData()->customer_id);
  ASSERT_EQ(1U, GetServerCardsMetadata(0).size());
  ASSERT_EQ(1U, GetServerAddressesMetadata(0).size());

  // Turn off the wallet autofill pref, the data & metadata should be gone as a
  // side effect of the wallet model type controller noticing.
  autofill::prefs::SetPaymentsIntegrationEnabled(GetProfile(0)->GetPrefs(),
                                                 false);
  WaitForOnPersonalDataChanged(/*should_trigger_refresh=*/false, pdm);

  EXPECT_EQ(0uL, pdm->GetServerProfiles().size());
  EXPECT_EQ(0uL, pdm->GetCreditCards().size());
  EXPECT_EQ(nullptr, pdm->GetPaymentsCustomerData());
  EXPECT_EQ(0U, GetServerCardsMetadata(0).size());
  EXPECT_EQ(0U, GetServerAddressesMetadata(0).size());

  // No histograms for initial sync & for clearing.
  ExpectNoHistogramsForCardsDiff();
  ExpectNoHistogramsForAddressesDiff();
}

// Wallet data present on the client should be cleared in favor of the new data
// synced down form the server.
IN_PROC_BROWSER_TEST_P(SingleClientWalletSyncTestWithDefaultFeatures,
                       NewWalletCardRemovesExistingCardAndProfile) {
  ASSERT_TRUE(SetupClients());
  autofill::PersonalDataManager* pdm = GetPersonalDataManager(0);
  ASSERT_NE(nullptr, pdm);

  // Add a server credit card on the client.
  CreditCard credit_card(CreditCard::MASKED_SERVER_CARD, "a123");
  std::vector<CreditCard> credit_cards = {credit_card};
  wallet_helper::SetServerCreditCards(0, credit_cards);

  // Add a server profile on the client.
  AutofillProfile profile(AutofillProfile::SERVER_PROFILE, "a123");
  profile.SetRawInfo(autofill::COMPANY_NAME, ASCIIToUTF16("JustATest"));
  std::vector<AutofillProfile> client_profiles = {profile};
  wallet_helper::SetServerProfiles(0, client_profiles);

  // Add PaymentsCustomerData on the client.
  wallet_helper::SetPaymentsCustomerData(
      0, autofill::PaymentsCustomerData(/*customer_id=*/kDefaultCustomerID));

  // Refresh the pdm so that it gets data from autofill table.
  RefreshAndWaitForOnPersonalDataChanged(pdm);

  // Make sure the card was added correctly.
  std::vector<CreditCard*> cards = pdm->GetCreditCards();
  ASSERT_EQ(1uL, cards.size());
  EXPECT_EQ("a123", cards[0]->server_id());

  // Make sure the profile was added correctly.
  std::vector<AutofillProfile*> profiles = pdm->GetServerProfiles();
  ASSERT_EQ(1uL, profiles.size());
  EXPECT_EQ("JustATest", TruncateUTF8(base::UTF16ToUTF8(
                             profiles[0]->GetRawInfo(autofill::COMPANY_NAME))));

  // Make sure the customer data was added correctly.
  EXPECT_EQ(kDefaultCustomerID, pdm->GetPaymentsCustomerData()->customer_id);

  // Add a new card from the server and sync it down.
  GetFakeServer()->SetWalletData(
      {CreateDefaultSyncWalletCard(), CreateDefaultSyncPaymentsCustomerData()});
  ASSERT_TRUE(SetupSync());

  // The only card present on the client should be the one from the server.
  cards = pdm->GetCreditCards();
  EXPECT_EQ(1uL, cards.size());
  EXPECT_EQ(kDefaultCardID, cards[0]->server_id());

  // There should be no profile present.
  profiles = pdm->GetServerProfiles();
  EXPECT_EQ(0uL, profiles.size());

  // The PaymentsCustomerData should still be there.
  EXPECT_EQ(kDefaultCustomerID, pdm->GetPaymentsCustomerData()->customer_id);

  // Expect correct histograms are reported for the update.
  ExpectCardsDiffInHistograms(/*added=*/1, /*removed=*/1);
  ExpectAddressesDiffInHistograms(/*added=*/0, /*removed=*/1);
}

// Wallet data present on the client should be cleared in favor of the new data
// synced down form the server.
IN_PROC_BROWSER_TEST_P(SingleClientWalletSyncTestWithDefaultFeatures,
                       NewWalletDataRemovesExistingData) {
  ASSERT_TRUE(SetupClients());
  autofill::PersonalDataManager* pdm = GetPersonalDataManager(0);
  ASSERT_NE(nullptr, pdm);

  // Add a server profile on the client.
  AutofillProfile profile(AutofillProfile::SERVER_PROFILE, "a123");
  profile.SetRawInfo(autofill::COMPANY_NAME, ASCIIToUTF16("JustATest"));
  std::vector<AutofillProfile> client_profiles = {profile};
  wallet_helper::SetServerProfiles(0, client_profiles);

  // Add a server credit card on the client.
  CreditCard credit_card(CreditCard::MASKED_SERVER_CARD, "a123");
  std::vector<CreditCard> credit_cards = {credit_card};
  wallet_helper::SetServerCreditCards(0, credit_cards);

  // Add PaymentsCustomerData on the client.
  wallet_helper::SetPaymentsCustomerData(
      0, autofill::PaymentsCustomerData(/*customer_id=*/kDefaultCustomerID));

  // Refresh the pdm so that it gets cards from autofill table.
  RefreshAndWaitForOnPersonalDataChanged(pdm);

  // Make sure the profile was added correctly.
  std::vector<AutofillProfile*> profiles = pdm->GetServerProfiles();
  EXPECT_EQ(1uL, profiles.size());
  EXPECT_EQ("JustATest", TruncateUTF8(base::UTF16ToUTF8(
                             profiles[0]->GetRawInfo(autofill::COMPANY_NAME))));

  // Make sure the card was added correctly.
  std::vector<CreditCard*> cards = pdm->GetCreditCards();
  EXPECT_EQ(1uL, cards.size());
  EXPECT_EQ("a123", cards[0]->server_id());

  // Make sure the customer data was added correctly.
  EXPECT_EQ(kDefaultCustomerID, pdm->GetPaymentsCustomerData()->customer_id);

  // Add a new profile and new customer data from the server and sync them down.
  GetFakeServer()->SetWalletData(
      {CreateDefaultSyncWalletAddress(),
       CreateSyncPaymentsCustomerData(/*customer_id=*/"newid")});
  ASSERT_TRUE(SetupSync());

  // The only profile present on the client should be the one from the server.
  profiles = pdm->GetServerProfiles();
  EXPECT_EQ(1uL, profiles.size());
  EXPECT_NE("JustATest", TruncateUTF8(base::UTF16ToUTF8(
                             profiles[0]->GetRawInfo(autofill::COMPANY_NAME))));

  // There should be no cards present.
  cards = pdm->GetCreditCards();
  EXPECT_EQ(0uL, cards.size());

  // Payments customer data should be updated.
  EXPECT_EQ("newid", pdm->GetPaymentsCustomerData()->customer_id);
}

// Tests that a local billing address id set on a card on the client should not
// be overwritten when that same card is synced again.
IN_PROC_BROWSER_TEST_P(SingleClientWalletSyncTestWithDefaultFeatures,
                       SameWalletCard_PreservesLocalBillingAddressId) {
  ASSERT_TRUE(SetupClients());
  autofill::PersonalDataManager* pdm = GetPersonalDataManager(0);
  ASSERT_NE(nullptr, pdm);

  // Add a server credit card on the client but with the billing address id of a
  // local profile.
  CreditCard credit_card = GetDefaultCreditCard();
  credit_card.set_billing_address_id(kLocalGuidA);
  std::vector<CreditCard> credit_cards = {credit_card};
  wallet_helper::SetServerCreditCards(0, credit_cards);

  // Refresh the pdm so that it gets cards from autofill table.
  RefreshAndWaitForOnPersonalDataChanged(pdm);

  // Make sure the card was added correctly.
  std::vector<CreditCard*> cards = pdm->GetCreditCards();
  ASSERT_EQ(1uL, cards.size());
  EXPECT_EQ(kDefaultCardID, cards[0]->server_id());

  // Sync the same card from the server, except with a default billing address
  // id.
  GetFakeServer()->SetWalletData({CreateDefaultSyncWalletCard()});
  ASSERT_TRUE(SetupSync());

  // The billing address is should still refer to the local profile.
  cards = pdm->GetCreditCards();
  ASSERT_EQ(1uL, cards.size());
  EXPECT_EQ(kDefaultCardID, cards[0]->server_id());
  EXPECT_EQ(kLocalGuidA, cards[0]->billing_address_id());
}

// Tests that a server billing address id set on a card on the client is
// overwritten when that same card is synced again.
IN_PROC_BROWSER_TEST_P(SingleClientWalletSyncTestWithDefaultFeatures,
                       SameWalletCard_DiscardsOldServerBillingAddressId) {
  ASSERT_TRUE(SetupClients());
  autofill::PersonalDataManager* pdm = GetPersonalDataManager(0);
  ASSERT_NE(nullptr, pdm);

  // Add a server credit card on the client but with the billing address id of a
  // server profile.
  CreditCard credit_card = GetDefaultCreditCard();
  credit_card.set_billing_address_id(kDifferentBillingAddressId);
  std::vector<CreditCard> credit_cards = {credit_card};
  wallet_helper::SetServerCreditCards(0, credit_cards);

  // Refresh the pdm so that it gets cards from autofill table.
  RefreshAndWaitForOnPersonalDataChanged(pdm);

  // Make sure the card was added correctly.
  std::vector<CreditCard*> cards = pdm->GetCreditCards();
  ASSERT_EQ(1uL, cards.size());
  EXPECT_EQ(kDefaultCardID, cards[0]->server_id());

  // Sync the same card from the server, except with a default billing address
  // id.
  GetFakeServer()->SetWalletData({CreateDefaultSyncWalletCard()});
  ASSERT_TRUE(SetupSync());

  // The billing address should be the one from the server card.
  cards = pdm->GetCreditCards();
  ASSERT_EQ(1uL, cards.size());
  EXPECT_EQ(kDefaultCardID, cards[0]->server_id());
  EXPECT_EQ(kDefaultBillingAddressID, cards[0]->billing_address_id());
}

class SingleClientWalletSecondaryAccountSyncTest
    : public UssWalletSwitchToggler,
      public SingleClientWalletSyncTest {
 public:
  SingleClientWalletSecondaryAccountSyncTest() {
    InitWithFeatures(
        /*enabled_features=*/{switches::kSyncSupportSecondaryAccount,
                              switches::kSyncUSSAutofillWalletData,
                              autofill::features::
                                  kAutofillEnableAccountWalletStorage},
        /*disabled_features=*/{});
  }
  ~SingleClientWalletSecondaryAccountSyncTest() override {}

  void SetUpInProcessBrowserTestFixture() override {
    test_signin_client_factory_ =
        secondary_account_helper::SetUpSigninClient(&test_url_loader_factory_);
  }

  void SetUpOnMainThread() override {
#if defined(OS_CHROMEOS)
    secondary_account_helper::InitNetwork();
#endif  // defined(OS_CHROMEOS)
    SyncTest::SetUpOnMainThread();
  }

  Profile* profile() { return GetProfile(0); }

 private:
  secondary_account_helper::ScopedSigninClientFactory
      test_signin_client_factory_;

  DISALLOW_COPY_AND_ASSIGN(SingleClientWalletSecondaryAccountSyncTest);
};

// ChromeOS doesn't support changes to the primary account after startup, so
// these secondary-account-related tests don't apply.
#if !defined(OS_CHROMEOS)
IN_PROC_BROWSER_TEST_P(SingleClientWalletSecondaryAccountSyncTest,
                       SwitchesFromAccountToProfileStorageOnSyncOptIn) {
  ASSERT_TRUE(SetupClients()) << "SetupClients() failed.";
  GetPersonalDataManager(0)->OnSyncServiceInitialized(GetSyncService(0));

  GetFakeServer()->SetWalletData(
      {CreateDefaultSyncWalletCard(), CreateDefaultSyncPaymentsCustomerData()});

  // Set up Sync in transport mode for a non-primary account.
  secondary_account_helper::SignInSecondaryAccount(
      profile(), &test_url_loader_factory_, "user@email.com");
  ASSERT_TRUE(GetClient(0)->AwaitSyncTransportActive());
  ASSERT_EQ(syncer::SyncService::TransportState::ACTIVE,
            GetSyncService(0)->GetTransportState());
  ASSERT_FALSE(GetSyncService(0)->IsSyncFeatureEnabled());
  ASSERT_FALSE(GetSyncService(0)->IsSyncFeatureActive());
  ASSERT_TRUE(GetSyncService(0)->GetActiveDataTypes().Has(
      syncer::AUTOFILL_WALLET_DATA));

  // PersonalDataManager should use (ephemeral) account storage.
  EXPECT_FALSE(GetPersonalDataManager(0)->IsSyncFeatureEnabled());
  EXPECT_TRUE(
      GetPersonalDataManager(0)->IsUsingAccountStorageForServerDataForTest());

  auto account_data = GetAccountWebDataService(0);
  ASSERT_NE(nullptr, account_data);
  auto profile_data = GetProfileWebDataService(0);
  ASSERT_NE(nullptr, profile_data);

  // Check that the data is stored in the account storage (ephemeral), but not
  // in the profile storage (persisted).
  EXPECT_EQ(1U, GetServerCards(account_data).size());
  EXPECT_EQ(0U, GetServerCards(profile_data).size());
  EXPECT_NE(nullptr, GetPaymentsCustomerData(account_data).get());
  EXPECT_EQ(nullptr, GetPaymentsCustomerData(profile_data).get());

  // Simulate the user opting in to full Sync: Make the account primary, and
  // set first-time setup to complete.
  secondary_account_helper::MakeAccountPrimary(profile(), "user@email.com");
  GetSyncService(0)->GetUserSettings()->SetSyncRequested(true);
  GetSyncService(0)->GetUserSettings()->SetFirstSetupComplete();

  // Wait for Sync to get reconfigured into feature mode.
  ASSERT_TRUE(GetClient(0)->AwaitSyncSetupCompletion());
  ASSERT_EQ(syncer::SyncService::TransportState::ACTIVE,
            GetSyncService(0)->GetTransportState());
  ASSERT_TRUE(GetSyncService(0)->IsSyncFeatureEnabled());
  ASSERT_TRUE(GetSyncService(0)->IsSyncFeatureActive());
  ASSERT_TRUE(GetSyncService(0)->GetActiveDataTypes().Has(
      syncer::AUTOFILL_WALLET_DATA));

  // PersonalDataManager should have switched to persistent storage.
  EXPECT_TRUE(GetPersonalDataManager(0)->IsSyncFeatureEnabled());
  EXPECT_FALSE(
      GetPersonalDataManager(0)->IsUsingAccountStorageForServerDataForTest());

  // The data should now be in the profile storage (persisted).
  EXPECT_EQ(0U, GetServerCards(account_data).size());
  EXPECT_EQ(1U, GetServerCards(profile_data).size());
  EXPECT_EQ(nullptr, GetPaymentsCustomerData(account_data).get());
  EXPECT_NE(nullptr, GetPaymentsCustomerData(profile_data).get());
}

IN_PROC_BROWSER_TEST_P(
    SingleClientWalletSecondaryAccountSyncTest,
    SwitchesFromAccountToProfileStorageOnSyncOptInWithAdvancedSetup) {
  ASSERT_TRUE(SetupClients()) << "SetupClients() failed.";
  GetPersonalDataManager(0)->OnSyncServiceInitialized(GetSyncService(0));

  GetFakeServer()->SetWalletData({CreateDefaultSyncWalletCard()});

  // Set up Sync in transport mode for a non-primary account.
  secondary_account_helper::SignInSecondaryAccount(
      profile(), &test_url_loader_factory_, "user@email.com");
  ASSERT_TRUE(GetClient(0)->AwaitSyncTransportActive());
  ASSERT_EQ(syncer::SyncService::TransportState::ACTIVE,
            GetSyncService(0)->GetTransportState());
  ASSERT_FALSE(GetSyncService(0)->IsSyncFeatureEnabled());
  ASSERT_FALSE(GetSyncService(0)->IsSyncFeatureActive());
  ASSERT_TRUE(GetSyncService(0)->GetActiveDataTypes().Has(
      syncer::AUTOFILL_WALLET_DATA));

  // PersonalDataManager should use (ephemeral) account storage.
  EXPECT_FALSE(GetPersonalDataManager(0)->IsSyncFeatureEnabled());
  EXPECT_TRUE(
      GetPersonalDataManager(0)->IsUsingAccountStorageForServerDataForTest());

  auto account_data = GetAccountWebDataService(0);
  ASSERT_NE(nullptr, account_data);
  auto profile_data = GetProfileWebDataService(0);
  ASSERT_NE(nullptr, profile_data);

  // Check that the card is stored in the account storage (ephemeral), but not
  // in the profile storage (persisted).
  EXPECT_EQ(1U, GetServerCards(account_data).size());
  EXPECT_EQ(0U, GetServerCards(profile_data).size());

  // Simulate the user opting in to full Sync: First, make the account primary.
  secondary_account_helper::MakeAccountPrimary(profile(), "user@email.com");

  // Now start actually configuring Sync.
  GetSyncService(0)->GetUserSettings()->SetSyncRequested(true);
  auto setup_handle = GetSyncService(0)->GetSetupInProgressHandle();

  // Adding a primary account triggers a restart of the Sync engine, so it
  // should now be initializing again.
  ASSERT_EQ(syncer::SyncService::TransportState::INITIALIZING,
            GetSyncService(0)->GetTransportState());

  ASSERT_TRUE(GetClient(0)->AwaitEngineInitialization());

  // Since we're still holding on to the setup-in-progress handle, the data
  // types can't be configured yet.
  ASSERT_EQ(syncer::SyncService::TransportState::PENDING_DESIRED_CONFIGURATION,
            GetSyncService(0)->GetTransportState());

  GetSyncService(0)->GetUserSettings()->SetChosenDataTypes(
      /*sync_everything=*/false, syncer::ModelTypeSet(syncer::AUTOFILL));

  // Once the user finishes the setup, we can actually configure.
  setup_handle.reset();
  ASSERT_EQ(syncer::SyncService::TransportState::CONFIGURING,
            GetSyncService(0)->GetTransportState());

  GetSyncService(0)->GetUserSettings()->SetFirstSetupComplete();

  // Wait for Sync to get reconfigured into feature mode.
  ASSERT_TRUE(GetClient(0)->AwaitSyncSetupCompletion());
  ASSERT_EQ(syncer::SyncService::TransportState::ACTIVE,
            GetSyncService(0)->GetTransportState());
  ASSERT_TRUE(GetSyncService(0)->IsSyncFeatureEnabled());
  ASSERT_TRUE(GetSyncService(0)->IsSyncFeatureActive());
  ASSERT_TRUE(GetSyncService(0)->GetActiveDataTypes().Has(
      syncer::AUTOFILL_WALLET_DATA));

  // PersonalDataManager should have switched to persistent storage.
  EXPECT_TRUE(GetPersonalDataManager(0)->IsSyncFeatureEnabled());
  EXPECT_FALSE(
      GetPersonalDataManager(0)->IsUsingAccountStorageForServerDataForTest());

  // The card should now be in the profile storage (persisted).
  EXPECT_EQ(0U, GetServerCards(account_data).size());
  EXPECT_EQ(1U, GetServerCards(profile_data).size());
}
#endif  // !defined(OS_CHROMEOS)

// This tests that switching between Sync-the-feature and
// Sync-standalone-transport properly migrates server credit cards between the
// profile (i.e. persisted) and account (i.e. ephemeral) storage.
// Sync can either be turned off temporarily via
// SyncUserSettings::SetSyncRequested(false), or permanently via
// SyncService::StopAndClear. For full coverage, we test all transitions, and
// each time verify that the card is in the correct storage:
// 1. Start out in Sync-the-feature mode -> profile storage.
// 2. SetSyncRequested(false) -> account storage.
// 3. Enable Sync-the-feature again -> profile storage.
// 4. StopAndClear() -> account storage.
// 5. Enable Sync-the-feature again -> profile storage.
IN_PROC_BROWSER_TEST_P(SingleClientWalletWithAccountStorageSyncTest,
                       SwitchesBetweenAccountAndProfileStorageOnTogglingSync) {
  ASSERT_TRUE(SetupClients()) << "SetupClients() failed.";
  GetPersonalDataManager(0)->OnSyncServiceInitialized(GetSyncService(0));

  GetFakeServer()->SetWalletData({CreateDefaultSyncWalletCard()});

  // STEP 1. Set up Sync in full feature mode.
  ASSERT_TRUE(GetClient(0)->SetupSync());
  ASSERT_EQ(syncer::SyncService::TransportState::ACTIVE,
            GetSyncService(0)->GetTransportState());
  ASSERT_TRUE(GetSyncService(0)->IsSyncFeatureEnabled());
  ASSERT_TRUE(GetSyncService(0)->IsSyncFeatureActive());
  ASSERT_TRUE(GetSyncService(0)->GetActiveDataTypes().Has(
      syncer::AUTOFILL_WALLET_DATA));

  // PersonalDataManager should use the regular persisted (non-account) storage.
  EXPECT_TRUE(GetPersonalDataManager(0)->IsSyncFeatureEnabled());
  EXPECT_FALSE(
      GetPersonalDataManager(0)->IsUsingAccountStorageForServerDataForTest());

  auto account_data = GetAccountWebDataService(0);
  ASSERT_NE(nullptr, account_data);
  auto profile_data = GetProfileWebDataService(0);
  ASSERT_NE(nullptr, profile_data);

  // Check that the card is stored in the profile storage (persisted), but not
  // in the account storage (ephemeral).
  EXPECT_EQ(0U, GetServerCards(account_data).size());
  EXPECT_EQ(1U, GetServerCards(profile_data).size());

  // STEP 2. Turn off Sync-the-feature temporarily (e.g. the Sync feature toggle
  // on Android), i.e. leave the Sync data around.
  GetSyncService(0)->GetUserSettings()->SetSyncRequested(false);

  // Wait for Sync to get reconfigured into transport mode.
  ASSERT_TRUE(GetClient(0)->AwaitSyncTransportActive());
  ASSERT_EQ(syncer::SyncService::TransportState::ACTIVE,
            GetSyncService(0)->GetTransportState());
  ASSERT_FALSE(GetSyncService(0)->IsSyncFeatureEnabled());
  ASSERT_FALSE(GetSyncService(0)->IsSyncFeatureActive());
  ASSERT_TRUE(GetSyncService(0)->GetActiveDataTypes().Has(
      syncer::AUTOFILL_WALLET_DATA));

  // PersonalDataManager should have switched to ephemeral storage.
  EXPECT_FALSE(GetPersonalDataManager(0)->IsSyncFeatureEnabled());
  EXPECT_TRUE(
      GetPersonalDataManager(0)->IsUsingAccountStorageForServerDataForTest());

  // The card should now be in the account storage (ephemeral). Note that even
  // though we specified KEEP_DATA above, the card is *not* in the profile
  // storage (persisted) anymore, because AUTOFILL_WALLET_DATA is special-cased
  // to always clear its data when Sync is turned off.
  EXPECT_EQ(1U, GetServerCards(account_data).size());
  EXPECT_EQ(0U, GetServerCards(profile_data).size());

  // STEP 3. Turn Sync-the-feature on again.
  GetSyncService(0)->GetUserSettings()->SetSyncRequested(true);

  // Wait for Sync to get reconfigured into full feature mode again.
  ASSERT_TRUE(GetClient(0)->AwaitSyncSetupCompletion());
  ASSERT_EQ(syncer::SyncService::TransportState::ACTIVE,
            GetSyncService(0)->GetTransportState());
  ASSERT_TRUE(GetSyncService(0)->IsSyncFeatureEnabled());
  ASSERT_TRUE(GetSyncService(0)->IsSyncFeatureActive());
  ASSERT_TRUE(GetSyncService(0)->GetActiveDataTypes().Has(
      syncer::AUTOFILL_WALLET_DATA));

  // PersonalDataManager should have switched back to persistent storage.
  EXPECT_TRUE(GetPersonalDataManager(0)->IsSyncFeatureEnabled());
  EXPECT_FALSE(
      GetPersonalDataManager(0)->IsUsingAccountStorageForServerDataForTest());

  // And the card should be in the profile i.e. persistent storage again.
  EXPECT_EQ(0U, GetServerCards(account_data).size());
  EXPECT_EQ(1U, GetServerCards(profile_data).size());

  // STEP 4. Turn off Sync-the-feature again, but this time clear data.
  GetSyncService(0)->StopAndClear();

  // Wait for Sync to get reconfigured into transport mode.
  ASSERT_TRUE(GetClient(0)->AwaitSyncTransportActive());
  ASSERT_EQ(syncer::SyncService::TransportState::ACTIVE,
            GetSyncService(0)->GetTransportState());
  ASSERT_FALSE(GetSyncService(0)->IsSyncFeatureEnabled());
  ASSERT_FALSE(GetSyncService(0)->IsSyncFeatureActive());
  ASSERT_TRUE(GetSyncService(0)->GetActiveDataTypes().Has(
      syncer::AUTOFILL_WALLET_DATA));

  // PersonalDataManager should have switched to ephemeral storage.
  EXPECT_FALSE(GetPersonalDataManager(0)->IsSyncFeatureEnabled());
  EXPECT_TRUE(
      GetPersonalDataManager(0)->IsUsingAccountStorageForServerDataForTest());

  // The card should now be in the account storage (ephemeral).
  EXPECT_EQ(1U, GetServerCards(account_data).size());
  EXPECT_EQ(0U, GetServerCards(profile_data).size());

  // STEP 5. Turn Sync-the-feature on again.
  GetSyncService(0)->GetUserSettings()->SetSyncRequested(true);
  GetSyncService(0)->GetUserSettings()->SetFirstSetupComplete();

  // Wait for Sync to get reconfigured into full feature mode again.
  ASSERT_TRUE(GetClient(0)->AwaitSyncSetupCompletion());
  ASSERT_EQ(syncer::SyncService::TransportState::ACTIVE,
            GetSyncService(0)->GetTransportState());
  ASSERT_TRUE(GetSyncService(0)->IsSyncFeatureEnabled());
  ASSERT_TRUE(GetSyncService(0)->IsSyncFeatureActive());
  ASSERT_TRUE(GetSyncService(0)->GetActiveDataTypes().Has(
      syncer::AUTOFILL_WALLET_DATA));

  // PersonalDataManager should have switched back to persistent storage.
  EXPECT_TRUE(GetPersonalDataManager(0)->IsSyncFeatureEnabled());
  EXPECT_FALSE(
      GetPersonalDataManager(0)->IsUsingAccountStorageForServerDataForTest());

  // And the card should be in the profile i.e. persistent storage again.
  EXPECT_EQ(0U, GetServerCards(account_data).size());
  EXPECT_EQ(1U, GetServerCards(profile_data).size());
}

INSTANTIATE_TEST_SUITE_P(USS,
                         SingleClientWalletSyncTestWithoutAccountStorage,
                         ::testing::Values(false, true));

INSTANTIATE_TEST_SUITE_P(USS,
                         SingleClientWalletWithAccountStorageSyncTest,
                         ::testing::Values(false, true));

INSTANTIATE_TEST_SUITE_P(USS,
                         SingleClientWalletSyncTestWithDefaultFeatures,
                         ::testing::Values(false, true));

INSTANTIATE_TEST_SUITE_P(USS,
                         SingleClientWalletSecondaryAccountSyncTest,
                         ::testing::Values(false, true));
