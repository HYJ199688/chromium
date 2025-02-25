// Copyright 2019 The Chromium Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include "components/send_tab_to_self/test_send_tab_to_self_model.h"

namespace send_tab_to_self {

std::vector<std::string> TestSendTabToSelfModel::GetAllGuids() const {
  return {};
}

void TestSendTabToSelfModel::DeleteAllEntries() {}

const SendTabToSelfEntry* TestSendTabToSelfModel::GetEntryByGUID(
    const std::string& guid) const {
  return nullptr;
}

const SendTabToSelfEntry* TestSendTabToSelfModel::AddEntry(
    const GURL& url,
    const std::string& title,
    base::Time navigation_time) {
  return nullptr;
}

void TestSendTabToSelfModel::DeleteEntry(const std::string& guid) {}

void TestSendTabToSelfModel::DismissEntry(const std::string& guid) {}

}  // namespace send_tab_to_self
