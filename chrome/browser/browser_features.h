// Copyright 2018 The Chromium Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

// This file defines the browser-specific base::FeatureList features that are
// not shared with other process types.

#ifndef CHROME_BROWSER_BROWSER_FEATURES_H_
#define CHROME_BROWSER_BROWSER_FEATURES_H_

#include "base/feature_list.h"
#include "build/build_config.h"

namespace features {

// All features in alphabetical order. The features should be documented
// alongside the definition of their values in the .cc file.

#if defined(OS_CHROMEOS)
extern const base::Feature kDoubleTapToZoomInTabletMode;
#endif

extern const base::Feature kExtensionsToolbarMenu;

extern const base::Feature kTabGroups;

extern const base::Feature kTabHoverCards;
extern const char kTabHoverCardsFeatureParameterName[];

extern const base::Feature kTabHoverCardImages;

#if !defined(OS_ANDROID)
extern const base::Feature kWebUIDarkMode;
#endif

}  // namespace features

#endif  // CHROME_BROWSER_BROWSER_FEATURES_H_
