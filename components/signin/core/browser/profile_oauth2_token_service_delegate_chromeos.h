// Copyright 2018 The Chromium Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#ifndef COMPONENTS_SIGNIN_CORE_BROWSER_PROFILE_OAUTH2_TOKEN_SERVICE_DELEGATE_CHROMEOS_H_
#define COMPONENTS_SIGNIN_CORE_BROWSER_PROFILE_OAUTH2_TOKEN_SERVICE_DELEGATE_CHROMEOS_H_

#include <map>
#include <memory>
#include <set>
#include <string>
#include <vector>

#include "base/macros.h"
#include "base/memory/weak_ptr.h"
#include "base/sequence_checker.h"
#include "chromeos/components/account_manager/account_manager.h"
#include "google_apis/gaia/oauth2_token_service_delegate.h"
#include "services/network/public/cpp/network_connection_tracker.h"

class AccountTrackerService;

namespace signin {

class ProfileOAuth2TokenServiceDelegateChromeOS
    : public OAuth2TokenServiceDelegate,
      public chromeos::AccountManager::Observer,
      public network::NetworkConnectionTracker::NetworkConnectionObserver {
 public:
  // Accepts non-owning pointers to |AccountTrackerService|,
  // |NetworkConnectorTracker|, and |chromeos::AccountManager|. These objects
  // must all outlive |this| delegate.
  ProfileOAuth2TokenServiceDelegateChromeOS(
      AccountTrackerService* account_tracker_service,
      network::NetworkConnectionTracker* network_connection_tracker,
      chromeos::AccountManager* account_manager);
  ~ProfileOAuth2TokenServiceDelegateChromeOS() override;

  // OAuth2TokenServiceDelegate overrides.
  OAuth2AccessTokenFetcher* CreateAccessTokenFetcher(
      const std::string& account_id,
      scoped_refptr<network::SharedURLLoaderFactory> url_loader_factory,
      OAuth2AccessTokenConsumer* consumer) override;
  bool RefreshTokenIsAvailable(const std::string& account_id) const override;
  void UpdateAuthError(const std::string& account_id,
                       const GoogleServiceAuthError& error) override;
  GoogleServiceAuthError GetAuthError(
      const std::string& account_id) const override;
  std::vector<std::string> GetAccounts() override;
  void LoadCredentials(const std::string& primary_account_id) override;
  void UpdateCredentials(const std::string& account_id,
                         const std::string& refresh_token) override;
  scoped_refptr<network::SharedURLLoaderFactory> GetURLLoaderFactory()
      const override;
  void RevokeCredentials(const std::string& account_id) override;
  void RevokeAllCredentials() override;
  const net::BackoffEntry* BackoffEntry() const override;

  // |chromeos::AccountManager::Observer| overrides.
  void OnTokenUpserted(
      const chromeos::AccountManager::Account& account) override;
  void OnAccountRemoved(
      const chromeos::AccountManager::Account& account) override;

  // |NetworkConnectionTracker::NetworkConnectionObserver| overrides.
  void OnConnectionChanged(network::mojom::ConnectionType type) override;

 private:
  FRIEND_TEST_ALL_PREFIXES(CrOSOAuthDelegateTest,
                           BackOffIsTriggerredForTransientErrors);
  FRIEND_TEST_ALL_PREFIXES(CrOSOAuthDelegateTest,
                           BackOffIsResetOnNetworkChange);

  // A utility class to keep track of |GoogleServiceAuthError|s for an account.
  struct AccountErrorStatus {
    // The last auth error seen.
    GoogleServiceAuthError last_auth_error;
  };

  // Callback handler for |chromeos::AccountManager::GetAccounts|.
  void OnGetAccounts(
      const std::vector<chromeos::AccountManager::Account>& accounts);

  // A non-owning pointer.
  AccountTrackerService* const account_tracker_service_;

  // A non-owning pointer.
  network::NetworkConnectionTracker* const network_connection_tracker_;

  // A non-owning pointer. |chromeos::AccountManager| is available
  // throughout the lifetime of a user session.
  chromeos::AccountManager* const account_manager_;

  // A cache of AccountKeys.
  std::set<chromeos::AccountManager::AccountKey> account_keys_;

  // A map from account id to the last seen error for that account.
  std::map<std::string, AccountErrorStatus> errors_;

  // Used to rate-limit token fetch requests so as to not overload the server.
  net::BackoffEntry backoff_entry_;
  GoogleServiceAuthError backoff_error_;

  SEQUENCE_CHECKER(sequence_checker_);
  base::WeakPtrFactory<ProfileOAuth2TokenServiceDelegateChromeOS> weak_factory_;

  DISALLOW_COPY_AND_ASSIGN(ProfileOAuth2TokenServiceDelegateChromeOS);
};

}  // namespace signin

#endif  // COMPONENTS_SIGNIN_CORE_BROWSER_PROFILE_OAUTH2_TOKEN_SERVICE_DELEGATE_CHROMEOS_H_
