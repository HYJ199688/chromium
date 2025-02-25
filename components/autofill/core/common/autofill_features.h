// Copyright 2017 The Chromium Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#ifndef COMPONENTS_AUTOFILL_CORE_COMMON_AUTOFILL_FEATURES_H_
#define COMPONENTS_AUTOFILL_CORE_COMMON_AUTOFILL_FEATURES_H_

#include <string>

#include "base/feature_list.h"
#include "base/strings/string16.h"
#include "build/build_config.h"

class PrefService;

namespace base {
struct Feature;
}

namespace autofill {
namespace features {

// All features in alphabetical order.
extern const base::Feature kAutocompleteRetentionPolicyEnabled;
extern const base::Feature kAutofillAllowNonHttpActivation;
extern const base::Feature kAutofillAddressNormalizer;
extern const base::Feature kAutofillAlwaysFillAddresses;
extern const base::Feature kAutofillAlwaysShowServerCardsInSyncTransport;
extern const base::Feature kAutofillCacheQueryResponses;
extern const base::Feature kAutofillCreateDataForTest;
extern const base::Feature kAutofillCreditCardAssist;
extern const base::Feature kAutofillEnableAccountWalletStorage;
extern const base::Feature kAutofillEnableAccountWalletStorageUpload;
extern const base::Feature kAutofillEnableCompanyName;
extern const base::Feature kAutofillEnforceMinRequiredFieldsForHeuristics;
extern const base::Feature kAutofillEnforceMinRequiredFieldsForQuery;
extern const base::Feature kAutofillEnforceMinRequiredFieldsForUpload;
extern const base::Feature kAutofillGetPaymentsIdentityFromSync;
extern const base::Feature kAutofillKeyboardAccessory;
extern const base::Feature kAutofillManualFallback;
extern const base::Feature kAutofillManualFallbackPhaseTwo;
extern const base::Feature kAutofillMetadataUploads;
extern const base::Feature kAutofillOverrideWithRaterConsensus;
extern const base::Feature kAutofillPreferServerNamePredictions;
extern const base::Feature kAutofillPrefilledFields;
extern const base::Feature kAutofillProfileServerValidation;
extern const base::Feature kAutofillRestrictUnownedFieldsToFormlessCheckout;
extern const base::Feature kAutofillRichMetadataQueries;
extern const base::Feature kAutofillSaveCardDialogUnlabeledExpirationDate;
extern const base::Feature kAutofillSaveOnProbablySubmitted;
extern const base::Feature kAutofillServerCommunication;
extern const base::Feature kAutofillSettingsCardTypeSplit;
extern const base::Feature kAutofillShowAllSuggestionsOnPrefilledForms;
extern const base::Feature kAutofillShowAutocompleteConsoleWarnings;
extern const base::Feature kAutofillUseImprovedLabelDisambiguation;
extern const base::Feature kAutofillShowTypePredictions;
extern const base::Feature kAutofillSkipComparingInferredLabels;
extern const base::Feature kAutofillTokenPrefixMatching;
extern const base::Feature kAutofillUploadThrottling;
extern const base::Feature kAutofillUseApi;
extern const base::Feature kAutofillProfileClientValidation;
extern const base::Feature kAutomaticPasswordGeneration;

#if defined(OS_ANDROID)
extern const base::Feature kAutofillManualFallbackAndroid;
extern const base::Feature kAutofillRefreshStyleAndroid;
#endif  // OS_ANDROID

// Returns whether the Autofill credit card assist infobar should be shown.
bool IsAutofillCreditCardAssistEnabled();

#if defined(OS_MACOSX)
// Returns true if whether the views autofill popup feature is enabled or the
// we're using the views browser.
bool IsMacViewsAutofillPopupExperimentEnabled();
#endif  // defined(OS_MACOSX)

// Returns whether the UI for passwords in manual fallback is enabled.
bool IsPasswordManualFallbackEnabled();

// Returns whether the UI for addresses and credit cards in manual fallback is
// enabled.
bool IsAutofillManualFallbackEnabled();

// Returns true if expiration dates on the save card dialog should be
// unlabeled, i.e. not preceded by "Exp."
bool IsAutofillSaveCardDialogUnlabeledExpirationDateEnabled();

}  // namespace features
}  // namespace autofill

#endif  // COMPONENTS_AUTOFILL_CORE_COMMON_AUTOFILL_FEATURES_H_
