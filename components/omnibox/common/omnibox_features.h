// Copyright 2019 The Chromium Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#ifndef COMPONENTS_OMNIBOX_COMMON_OMNIBOX_FEATURES_H_
#define COMPONENTS_OMNIBOX_COMMON_OMNIBOX_FEATURES_H_

#include "base/feature_list.h"

namespace omnibox {

extern const base::Feature kHideFileUrlScheme;
extern const base::Feature kHideSteadyStateUrlScheme;
extern const base::Feature kHideSteadyStateUrlTrivialSubdomains;
extern const base::Feature kHideSteadyStateUrlPathQueryAndRef;
extern const base::Feature kOneClickUnelide;
extern const base::Feature kSimplifyHttpsIndicator;
extern const base::Feature kOmniboxRichEntitySuggestions;
extern const base::Feature kOmniboxNewAnswerLayout;
extern const base::Feature kOmniboxReverseAnswers;
extern const base::Feature kOmniboxTailSuggestions;
extern const base::Feature kOmniboxTabSwitchSuggestions;
extern const base::Feature kOmniboxReverseTabSwitchLogic;
extern const base::Feature kExperimentalKeywordMode;
extern const base::Feature kOmniboxPedalSuggestions;
extern const base::Feature kOmniboxContextMenuForSuggestions;
extern const base::Feature kEnableClipboardProviderTextSuggestions;
extern const base::Feature kEnableClipboardProviderImageSuggestions;
extern const base::Feature kSearchProviderWarmUpOnFocus;
extern const base::Feature kZeroSuggestRedirectToChrome;
extern const base::Feature kDisplayTitleForCurrentUrl;
extern const base::Feature kQueryInOmnibox;
extern const base::Feature kUIExperimentMaxAutocompleteMatches;
extern const base::Feature kUIExperimentShowSuggestionFavicons;
extern const base::Feature kUIExperimentSwapTitleAndUrl;
extern const base::Feature kUIExperimentVerticalMargin;
extern const base::Feature kUIExperimentBlueSearchLoopAndSearchQuery;
extern const base::Feature kUIExperimentBlueTitlesAndGrayUrlsOnPageSuggestions;
extern const base::Feature kUIExperimentBlueTitlesOnPageSuggestions;
extern const base::Feature kUIExperimentShowSuffixOnAllSearchSuggestions;
extern const base::Feature kUIExperimentBoldUserTextOnSearchSuggestions;
extern const base::Feature kUIExperimentWhiteBackgroundOnBlur;
extern const base::Feature kUIExperimentUseGenericSearchEngineIcon;
extern const base::Feature kUIExperimentUnboldSuggestionText;
extern const base::Feature kSpeculativeServiceWorkerStartOnQueryInput;
extern const base::Feature kDocumentProvider;
extern const base::Feature kDedupeGoogleDriveURLs;
extern const base::Feature kOmniboxPopupShortcutIconsInZeroState;
extern const base::Feature kOmniboxMaterialDesignWeatherIcons;
extern const base::Feature kZeroSuggestionsOnNTP;

}  // namespace omnibox

#endif  // COMPONENTS_OMNIBOX_COMMON_OMNIBOX_FEATURES_H_
