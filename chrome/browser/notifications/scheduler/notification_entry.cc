// Copyright 2019 The Chromium Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include "chrome/browser/notifications/scheduler/notification_entry.h"

#include <utility>

namespace notifications {

NotificationEntry::NotificationEntry(SchedulerClientType type,
                                     const std::string& guid)
    : type(type), guid(guid) {}

NotificationEntry::~NotificationEntry() = default;

}  // namespace notifications
