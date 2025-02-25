// Copyright 2019 The Chromium Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include "chrome/browser/notifications/scheduler/scheduler_config.h"

namespace notifications {

// The impression history is hold for 4 weeks.
constexpr base::TimeDelta kDefaultImpressionExpiration =
    base::TimeDelta::FromDays(28);

// The suppression lasts 8 weeks.
constexpr base::TimeDelta kDefaultSuppressionDuration =
    base::TimeDelta::FromDays(56);

// Check consecutive notification dismisses in this duration to generate a
// dismiss event.
constexpr base::TimeDelta kDefaultDimissDuration = base::TimeDelta::FromDays(7);

// static
std::unique_ptr<SchedulerConfig> SchedulerConfig::Create() {
  return std::make_unique<SchedulerConfig>();
}

SchedulerConfig::SchedulerConfig()
    : max_daily_shown_all_type(3),
      max_daily_shown_per_type(10),
      impression_expiration(kDefaultImpressionExpiration),
      suppression_duration(kDefaultSuppressionDuration),
      dismiss_count(3),
      dismiss_duration(kDefaultDimissDuration) {
  // TODO(xingliu): Add constructor using finch data.
}

SchedulerConfig::~SchedulerConfig() = default;

}  // namespace notifications
