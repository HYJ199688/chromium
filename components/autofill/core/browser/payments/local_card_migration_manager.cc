// Copyright 2018 The Chromium Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include "components/autofill/core/browser/payments/local_card_migration_manager.h"

#include <stddef.h>

#include <algorithm>
#include <vector>

#include "base/bind.h"
#include "base/metrics/histogram_functions.h"
#include "base/stl_util.h"
#include "base/strings/utf_string_conversions.h"
#include "components/autofill/core/browser/autofill_client.h"
#include "components/autofill/core/browser/autofill_experiments.h"
#include "components/autofill/core/browser/autofill_metrics.h"
#include "components/autofill/core/browser/credit_card.h"
#include "components/autofill/core/browser/form_data_importer.h"
#include "components/autofill/core/browser/payments/payments_client.h"
#include "components/autofill/core/browser/payments/payments_util.h"
#include "components/autofill/core/browser/personal_data_manager.h"
#include "components/autofill/core/common/autofill_features.h"
#include "components/autofill/core/common/autofill_payments_features.h"
#include "components/autofill/core/common/autofill_prefs.h"
#include "services/identity/public/cpp/identity_manager.h"

namespace autofill {

MigratableCreditCard::MigratableCreditCard(const CreditCard& credit_card)
    : credit_card_(credit_card) {}

MigratableCreditCard::~MigratableCreditCard() {}

LocalCardMigrationManager::LocalCardMigrationManager(
    AutofillClient* client,
    payments::PaymentsClient* payments_client,
    const std::string& app_locale,
    PersonalDataManager* personal_data_manager)
    : client_(client),
      payments_client_(payments_client),
      app_locale_(app_locale),
      personal_data_manager_(personal_data_manager),
      weak_ptr_factory_(this) {
  // This is to initialize StrikeDatabase is if it hasn't been already, so that
  // its cache would be loaded and ready to use when the first LCMM is created.
  if (base::FeatureList::IsEnabled(
          features::kAutofillSaveCreditCardUsesStrikeSystemV2) ||
      base::FeatureList::IsEnabled(
          features::kAutofillLocalCardMigrationUsesStrikeSystemV2)) {
    // Only init when |kAutofillSaveCreditCardUsesStrikeSystemV2| is enabled. If
    // flag is off and LegacyStrikeDatabase instead of StrikeDatabase is used,
    // this init will cause failure on GetStrikes().
    client_->GetStrikeDatabase();
  }
}

LocalCardMigrationManager::~LocalCardMigrationManager() {}

bool LocalCardMigrationManager::ShouldOfferLocalCardMigration(
    int imported_credit_card_record_type) {
  // Must be an existing card. New cards always get Upstream or local save.
  switch (imported_credit_card_record_type) {
    case FormDataImporter::ImportedCreditCardRecordType::LOCAL_CARD:
      local_card_migration_origin_ =
          AutofillMetrics::LocalCardMigrationOrigin::UseOfLocalCard;
      break;
    case FormDataImporter::ImportedCreditCardRecordType::SERVER_CARD:
      local_card_migration_origin_ =
          AutofillMetrics::LocalCardMigrationOrigin::UseOfServerCard;
      break;
    default:
      return false;
  }

  if (!IsCreditCardMigrationEnabled())
    return false;

  // Don't show the prompt if max strike count was reached.
  if (base::FeatureList::IsEnabled(
          features::kAutofillLocalCardMigrationUsesStrikeSystemV2) &&
      GetLocalCardMigrationStrikeDatabase()->IsMaxStrikesLimitReached()) {
    switch (imported_credit_card_record_type) {
      case FormDataImporter::ImportedCreditCardRecordType::LOCAL_CARD:
        AutofillMetrics::LogLocalCardMigrationNotOfferedDueToMaxStrikesMetric(
            AutofillMetrics::SaveTypeMetric::LOCAL);
        break;
      case FormDataImporter::ImportedCreditCardRecordType::SERVER_CARD:
        AutofillMetrics::LogLocalCardMigrationNotOfferedDueToMaxStrikesMetric(
            AutofillMetrics::SaveTypeMetric::SERVER);
        break;
    }
    return false;
  } else if (prefs::IsLocalCardMigrationPromptPreviouslyCancelled(
                 client_->GetPrefs())) {
    // Don't show the the prompt if user cancelled/rejected previously.
    return false;
  }

  // Fetch all migratable credit cards and store in |migratable_credit_cards_|.
  GetMigratableCreditCards();

  // If the form was submitted with a local card, only offer migration instead
  // of Upstream if there are other local cards to migrate as well. If the form
  // was submitted with a server card, offer migration if ANY local cards can be
  // migrated.
  return (imported_credit_card_record_type ==
              FormDataImporter::ImportedCreditCardRecordType::LOCAL_CARD &&
          migratable_credit_cards_.size() > 1) ||
         (imported_credit_card_record_type ==
              FormDataImporter::ImportedCreditCardRecordType::SERVER_CARD &&
          !migratable_credit_cards_.empty());
}

void LocalCardMigrationManager::AttemptToOfferLocalCardMigration(
    bool is_from_settings_page) {
  // Abort the migration if |payments_client_| is nullptr.
  if (!payments_client_)
    return;
  migration_request_ = payments::PaymentsClient::MigrationRequestDetails();

  if (observer_for_testing_)
    observer_for_testing_->OnDecideToRequestLocalCardMigration();

  payments_client_->GetUploadDetails(
      std::vector<AutofillProfile>(), GetDetectedValues(),
      /*active_experiments=*/std::vector<const char*>(), app_locale_,
      base::BindOnce(&LocalCardMigrationManager::OnDidGetUploadDetails,
                     weak_ptr_factory_.GetWeakPtr(), is_from_settings_page),
      payments::kMigrateCardsBillableServiceNumber,
      is_from_settings_page ? payments::PaymentsClient::UploadCardSource::
                                  LOCAL_CARD_MIGRATION_SETTINGS_PAGE
                            : payments::PaymentsClient::UploadCardSource::
                                  LOCAL_CARD_MIGRATION_CHECKOUT_FLOW);
}

// Callback function when user agrees to migration on the intermediate dialog.
// Call ShowMainMigrationDialog() to pop up a larger, modal dialog showing the
// local cards to be uploaded.
void LocalCardMigrationManager::OnUserAcceptedIntermediateMigrationDialog() {
  AutofillMetrics::LogLocalCardMigrationPromptMetric(
      local_card_migration_origin_,
      AutofillMetrics::INTERMEDIATE_BUBBLE_ACCEPTED);
  ShowMainMigrationDialog();
}

// Send the migration request once risk data is available.
void LocalCardMigrationManager::OnUserAcceptedMainMigrationDialog(
    const std::vector<std::string>& selected_card_guids) {
  user_accepted_main_migration_dialog_ = true;
  AutofillMetrics::LogLocalCardMigrationPromptMetric(
      local_card_migration_origin_, AutofillMetrics::MAIN_DIALOG_ACCEPTED);

  // Log number of LocalCardMigration strikes when migration was accepted.
  if (base::FeatureList::IsEnabled(
          features::kAutofillLocalCardMigrationUsesStrikeSystemV2)) {
    base::UmaHistogramCounts1000(
        "Autofill.StrikeDatabase.StrikesPresentWhenLocalCardMigrationAccepted",
        GetLocalCardMigrationStrikeDatabase()->GetStrikes());
  }

  // If there are cards which aren't selected, add 3 strikes to
  // LocalCardMigrationStrikeDatabase.
  if (base::FeatureList::IsEnabled(
          features::kAutofillLocalCardMigrationUsesStrikeSystemV2) &&
      (selected_card_guids.size() < migratable_credit_cards_.size())) {
    GetLocalCardMigrationStrikeDatabase()->AddStrikes(
        LocalCardMigrationStrikeDatabase::
            kStrikesToAddWhenCardsDeselectedAtMigration);
  }

  // Update the |migratable_credit_cards_| with the |selected_card_guids|. This
  // will remove any card from |migratable_credit_cards_| of which the GUID is
  // not in |selected_card_guids|.
  auto card_is_selected = [&selected_card_guids](MigratableCreditCard& card) {
    return !base::ContainsValue(selected_card_guids, card.credit_card().guid());
  };
  base::EraseIf(migratable_credit_cards_, card_is_selected);
  // Populating risk data and offering migration two-round pop-ups occur
  // asynchronously. If |migration_risk_data_| has already been loaded, send the
  // migrate local cards request. Otherwise, continue to wait and let
  // OnDidGetUploadRiskData handle it.
  if (!migration_request_.risk_data.empty())
    SendMigrateLocalCardsRequest();
}

void LocalCardMigrationManager::OnUserDeletedLocalCardViaMigrationDialog(
    const std::string& deleted_card_guid) {
  personal_data_manager_->RemoveByGUID(deleted_card_guid);
}

bool LocalCardMigrationManager::IsCreditCardMigrationEnabled() {
  // Confirm that the user is signed in, syncing, and the proper experiment
  // flags are enabled.
  bool migration_experiment_enabled =
      features::GetLocalCardMigrationExperimentalFlag() !=
      features::LocalCardMigrationExperimentalFlag::kMigrationDisabled;

  // If |observer_for_testing_| is set, assume we are in a browsertest and
  // credit card upload should be enabled by default. Cannot get around this as
  // Chrome OS testing requires an unsupported email domain (i.e.
  // stub-user@example.com).
  bool credit_card_upload_enabled =
      observer_for_testing_ ||
      ::autofill::IsCreditCardUploadEnabled(
          client_->GetPrefs(), client_->GetSyncService(),
          personal_data_manager_->GetAccountInfoForPaymentsServer().email);

  bool has_google_payments_account =
      (payments::GetBillingCustomerId(personal_data_manager_) != 0);

  AutofillSyncSigninState sync_state =
      personal_data_manager_->GetSyncSigninState();

  return migration_experiment_enabled && credit_card_upload_enabled &&
         has_google_payments_account &&
         // User signed-in and turned sync on.
         (sync_state == AutofillSyncSigninState::kSignedInAndSyncFeature ||
          // User signed-in but not turned on sync.
          (sync_state == AutofillSyncSigninState::
                             kSignedInAndWalletSyncTransportEnabled &&
           base::FeatureList::IsEnabled(
               features::kAutofillEnableLocalCardMigrationForNonSyncUser)));
}

void LocalCardMigrationManager::OnDidGetUploadDetails(
    bool is_from_settings_page,
    AutofillClient::PaymentsRpcResult result,
    const base::string16& context_token,
    std::unique_ptr<base::Value> legal_message,
    std::vector<std::pair<int, int>> supported_card_bin_ranges) {
  if (observer_for_testing_)
    observer_for_testing_->OnReceivedGetUploadDetailsResponse();

  if (result == AutofillClient::SUCCESS) {
    migration_request_.context_token = context_token;
    legal_message_ = base::DictionaryValue::From(std::move(legal_message));
    migration_request_.risk_data.clear();
    // If we successfully received the legal docs, trigger the offer-to-migrate
    // dialog. If triggered from settings page, we pop-up the main prompt
    // directly. If not, we pop up the intermediate bubble.
    if (is_from_settings_page) {
      // Set the origin to SettingsPage.
      local_card_migration_origin_ =
          AutofillMetrics::LocalCardMigrationOrigin::SettingsPage;
      // Pops up a larger, modal dialog showing the local cards to be uploaded.
      ShowMainMigrationDialog();
    } else {
      client_->ShowLocalCardMigrationDialog(base::BindOnce(
          &LocalCardMigrationManager::OnUserAcceptedIntermediateMigrationDialog,
          weak_ptr_factory_.GetWeakPtr()));
      AutofillMetrics::LogLocalCardMigrationPromptMetric(
          local_card_migration_origin_,
          AutofillMetrics::INTERMEDIATE_BUBBLE_SHOWN);
    }
    // TODO(crbug.com/876895): Clean up the LoadRiskData Bind/BindRepeating
    // usages
    client_->LoadRiskData(base::BindRepeating(
        &LocalCardMigrationManager::OnDidGetMigrationRiskData,
        weak_ptr_factory_.GetWeakPtr()));
  }
}

void LocalCardMigrationManager::OnDidMigrateLocalCards(
    AutofillClient::PaymentsRpcResult result,
    std::unique_ptr<std::unordered_map<std::string, std::string>> save_result,
    const std::string& display_text) {
  if (observer_for_testing_)
    observer_for_testing_->OnReceivedMigrateCardsResponse();

  if (!save_result)
    return;

  if (result == AutofillClient::PaymentsRpcResult::SUCCESS) {
    std::vector<CreditCard> migrated_cards;
    // Traverse the migratable credit cards to update each migrated card status.
    for (MigratableCreditCard& card : migratable_credit_cards_) {
      // If it is run in a test, count all cards as successfully migrated.
      if (observer_for_testing_) {
        migrated_cards.push_back(card.credit_card());
        continue;
      }

      // Not every card exists in the |save_result| since some cards are
      // unchecked by the user and not migrated.
      auto it = save_result->find(card.credit_card().guid());
      // If current card does not exist in the |save_result|, skip it.
      if (it == save_result->end())
        continue;

      // Otherwise update its migration status. Server-side response can return
      // SUCCESS, TEMPORARY_FAILURE, or PERMANENT_FAILURE (see SaveResult
      // enum). Branch here depending on which is received.
      if (it->second == kMigrationResultPermanentFailure ||
          it->second == kMigrationResultTemporaryFailure) {
        card.set_migration_status(
            autofill::MigratableCreditCard::MigrationStatus::FAILURE_ON_UPLOAD);
      } else if (it->second == kMigrationResultSuccess) {
        card.set_migration_status(
            autofill::MigratableCreditCard::MigrationStatus::SUCCESS_ON_UPLOAD);
        migrated_cards.push_back(card.credit_card());
      } else {
        NOTREACHED();
      }
    }
    // Remove cards that were successfully migrated from local storage.
    personal_data_manager_->DeleteLocalCreditCards(migrated_cards);
  }

  if (base::FeatureList::IsEnabled(
          features::kAutofillLocalCardMigrationShowFeedback)) {
    client_->ShowLocalCardMigrationResults(
        result != AutofillClient::PaymentsRpcResult::SUCCESS,
        base::UTF8ToUTF16(display_text), migratable_credit_cards_,
        base::BindRepeating(&LocalCardMigrationManager::
                                OnUserDeletedLocalCardViaMigrationDialog,
                            weak_ptr_factory_.GetWeakPtr()));
  }
}

void LocalCardMigrationManager::OnDidGetMigrationRiskData(
    const std::string& risk_data) {
  migration_request_.risk_data = risk_data;
  // Populating risk data and offering migration two-round pop-ups occur
  // asynchronously. If the main migration dialog has already been accepted,
  // send the migrate local cards request. Otherwise, continue to wait for the
  // user to accept the two round dialog.
  if (user_accepted_main_migration_dialog_)
    SendMigrateLocalCardsRequest();
}

// Send the migration request. Will call payments_client to create a new
// PaymentsRequest. Also create a new callback function OnDidMigrateLocalCards.
void LocalCardMigrationManager::SendMigrateLocalCardsRequest() {
  if (observer_for_testing_)
    observer_for_testing_->OnSentMigrateCardsRequest();

  migration_request_.app_locale = app_locale_;
  migration_request_.billing_customer_number =
      payments::GetBillingCustomerId(personal_data_manager_);
  payments_client_->MigrateCards(
      migration_request_, migratable_credit_cards_,
      base::BindOnce(&LocalCardMigrationManager::OnDidMigrateLocalCards,
                     weak_ptr_factory_.GetWeakPtr()));
  user_accepted_main_migration_dialog_ = false;
}

LocalCardMigrationStrikeDatabase*
LocalCardMigrationManager::GetLocalCardMigrationStrikeDatabase() {
  if (local_card_migration_strike_database_.get() == nullptr) {
    local_card_migration_strike_database_ =
        std::make_unique<LocalCardMigrationStrikeDatabase>(
            LocalCardMigrationStrikeDatabase(client_->GetStrikeDatabase()));
  }
  return local_card_migration_strike_database_.get();
}

// Pops up a larger, modal dialog showing the local cards to be uploaded. Pass
// the reference of vector<MigratableCreditCard> and the callback function is
// OnUserAcceptedMainMigrationDialog(). Can be called when user agrees to
// migration on the intermediate dialog or directly from settings page.
void LocalCardMigrationManager::ShowMainMigrationDialog() {
  AutofillMetrics::LogLocalCardMigrationPromptMetric(
      local_card_migration_origin_, AutofillMetrics::MAIN_DIALOG_SHOWN);
  // Pops up a larger, modal dialog showing the local cards to be uploaded.
  client_->ConfirmMigrateLocalCardToCloud(
      std::move(legal_message_),
      client_->GetIdentityManager()->GetPrimaryAccountInfo().email,
      migratable_credit_cards_,
      base::BindOnce(
          &LocalCardMigrationManager::OnUserAcceptedMainMigrationDialog,
          weak_ptr_factory_.GetWeakPtr()));
}

int LocalCardMigrationManager::GetDetectedValues() const {
  int detected_values = 0;

  // If all cards to be migrated have a cardholder name, include it in the
  // detected values.
  bool all_cards_have_cardholder_name = true;
  for (MigratableCreditCard migratable_credit_card : migratable_credit_cards_) {
    all_cards_have_cardholder_name &=
        !migratable_credit_card.credit_card()
             .GetInfo(AutofillType(CREDIT_CARD_NAME_FULL), app_locale_)
             .empty();
  }
  if (all_cards_have_cardholder_name)
    detected_values |= CreditCardSaveManager::DetectedValue::CARDHOLDER_NAME;

  // Local card migration should ONLY be offered when the user already has a
  // Google Payments account.
  DCHECK_NE(0, payments::GetBillingCustomerId(personal_data_manager_));
  detected_values |=
      CreditCardSaveManager::DetectedValue::HAS_GOOGLE_PAYMENTS_ACCOUNT;

  return detected_values;
}

void LocalCardMigrationManager::GetMigratableCreditCards() {
  std::vector<CreditCard*> local_credit_cards =
      personal_data_manager_->GetLocalCreditCards();

  // Empty previous state.
  migratable_credit_cards_.clear();

  // Initialize the local credit card list and queue for showing and uploading.
  for (CreditCard* credit_card : local_credit_cards) {
    // If the card is valid (has a valid card number, expiration date, and is
    // not expired) and is not a server card, add it to the list of migratable
    // cards.
    if (credit_card->IsValid() &&
        !personal_data_manager_->IsServerCard(credit_card)) {
      migratable_credit_cards_.push_back(MigratableCreditCard(*credit_card));
    }
  }
}

}  // namespace autofill
