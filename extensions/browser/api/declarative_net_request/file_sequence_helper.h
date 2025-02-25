// Copyright 2019 The Chromium Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#ifndef EXTENSIONS_BROWSER_API_DECLARATIVE_NET_REQUEST_FILE_SEQUENCE_HELPER_H_
#define EXTENSIONS_BROWSER_API_DECLARATIVE_NET_REQUEST_FILE_SEQUENCE_HELPER_H_

#include <memory>
#include <vector>

#include "base/callback_forward.h"
#include "base/macros.h"
#include "base/memory/weak_ptr.h"
#include "base/optional.h"
#include "extensions/browser/api/declarative_net_request/ruleset_matcher.h"
#include "extensions/browser/api/declarative_net_request/ruleset_source.h"
#include "extensions/common/extension_id.h"

namespace service_manager {
class Connector;
}  // namespace service_manager

namespace extensions {
namespace declarative_net_request {

// Holds the data relating to the loading of a single ruleset.
class RulesetInfo {
 public:
  explicit RulesetInfo(RulesetSource source);
  ~RulesetInfo();
  RulesetInfo(RulesetInfo&&);
  RulesetInfo& operator=(RulesetInfo&&);

  const RulesetSource& source() const { return source_; }

  // Returns the ownership of the ruleset matcher to the caller. Must only be
  // called for a successful load.
  std::unique_ptr<RulesetMatcher> TakeMatcher();

  // Clients should set a new checksum if the checksum stored in prefs should
  // be updated.
  void set_new_checksum(int new_checksum) { new_checksum_ = new_checksum; }
  base::Optional<int> new_checksum() const { return new_checksum_; }

  // The expected checksum for the indexed ruleset.
  void set_expected_checksum(int checksum) { expected_checksum_ = checksum; }
  base::Optional<int> expected_checksum() const { return expected_checksum_; }

  // Whether re-indexing of the ruleset was successful.
  void set_reindexing_successful(bool val) { reindexing_successful_ = val; }
  base::Optional<bool> reindexing_successful() const {
    return reindexing_successful_;
  }

  // Must be called after CreateVerifiedMatcher.
  RulesetMatcher::LoadRulesetResult load_ruleset_result() const;

  // Must be called after CreateVerifiedMatcher.
  bool did_load_successfully() const {
    return load_ruleset_result() == RulesetMatcher::kLoadSuccess;
  }

  // Must be invoked on the extension file task runner. Must only be called
  // after the expected checksum is set.
  void CreateVerifiedMatcher();

 private:
  RulesetSource source_;

  // The expected checksum of the indexed ruleset.
  base::Optional<int> expected_checksum_;

  // Stores the result of creating a verified matcher from the |source_|.
  std::unique_ptr<RulesetMatcher> matcher_;
  base::Optional<RulesetMatcher::LoadRulesetResult> load_ruleset_result_;

  // The new checksum to be persisted to prefs. A new checksum should only be
  // set in case of flatbuffer version mismatch.
  base::Optional<int> new_checksum_;

  // Whether the reindexing of this ruleset was successful.
  base::Optional<bool> reindexing_successful_;

  DISALLOW_COPY_AND_ASSIGN(RulesetInfo);
};

// Helper to pass information related to the ruleset being loaded.
struct LoadRequestData {
  explicit LoadRequestData(ExtensionId extension_id);
  ~LoadRequestData();
  LoadRequestData(LoadRequestData&&);
  LoadRequestData& operator=(LoadRequestData&&);

  ExtensionId extension_id;
  std::vector<RulesetInfo> rulesets;

 private:
  DISALLOW_COPY_AND_ASSIGN(LoadRequestData);
};

//  Helper class to load indexed rulesets. Can be created on any sequence but
//  must be used on the extension file task runner. Also tries to reindex the
//  rulesets on failure.
class FileSequenceHelper {
 public:
  FileSequenceHelper();
  ~FileSequenceHelper();

  // Loads rulesets for |load_data|. Invokes |ui_callback| on the UI thread once
  // loading is done.
  using LoadRulesetsUICallback = base::OnceCallback<void(LoadRequestData)>;
  void LoadRulesets(LoadRequestData load_data,
                    LoadRulesetsUICallback ui_callback) const;

 private:
  // Callback invoked when the JSON rulesets are reindexed.
  void OnRulesetsReindexed(LoadRulesetsUICallback ui_callback,
                           LoadRequestData load_data) const;

  const std::unique_ptr<service_manager::Connector> connector_;

  // Must be the last member variable. See WeakPtrFactory documentation for
  // details. Mutable to allow GetWeakPtr() usage from const methods.
  mutable base::WeakPtrFactory<FileSequenceHelper> weak_factory_;

  DISALLOW_COPY_AND_ASSIGN(FileSequenceHelper);
};

}  // namespace declarative_net_request
}  // namespace extensions

#endif  // EXTENSIONS_BROWSER_API_DECLARATIVE_NET_REQUEST_FILE_SEQUENCE_HELPER_H_
