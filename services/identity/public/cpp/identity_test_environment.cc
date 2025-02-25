// Copyright 2017 The Chromium Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include "services/identity/public/cpp/identity_test_environment.h"

#include "base/bind.h"
#include "build/build_config.h"

#include "base/run_loop.h"
#include "base/threading/thread_task_runner_handle.h"
#include "components/image_fetcher/core/fake_image_decoder.h"
#include "components/signin/core/browser/account_tracker_service.h"
#include "components/signin/core/browser/fake_profile_oauth2_token_service.h"
#include "components/signin/core/browser/gaia_cookie_manager_service.h"
#include "components/signin/core/browser/signin_manager.h"
#include "components/signin/core/browser/test_signin_client.h"
#include "components/sync_preferences/testing_pref_service_syncable.h"
#include "google_apis/gaia/oauth2_access_token_consumer.h"
#include "services/identity/public/cpp/accounts_cookie_mutator.h"
#include "services/identity/public/cpp/accounts_cookie_mutator_impl.h"
#include "services/identity/public/cpp/accounts_mutator.h"
#include "services/identity/public/cpp/diagnostics_provider_impl.h"
#include "services/identity/public/cpp/identity_test_utils.h"
#include "services/identity/public/cpp/primary_account_mutator.h"
#include "services/identity/public/cpp/test_identity_manager_observer.h"

#if !defined(OS_CHROMEOS)
#include "services/identity/public/cpp/primary_account_mutator_impl.h"
#endif

#if !defined(OS_ANDROID) && !defined(OS_IOS)
#include "services/identity/public/cpp/accounts_mutator_impl.h"
#endif

#if defined(OS_ANDROID)
#include "components/signin/core/browser/child_account_info_fetcher_android.h"
#endif

namespace identity {

class IdentityManagerDependenciesOwner {
 public:
  IdentityManagerDependenciesOwner(
      sync_preferences::TestingPrefServiceSyncable* pref_service,
      TestSigninClient* test_signin_client);
  ~IdentityManagerDependenciesOwner();

  sync_preferences::TestingPrefServiceSyncable* pref_service();

  TestSigninClient* signin_client();

 private:
  // Depending on whether a |pref_service| instance is passed in
  // the constructor, exactly one of these will be non-null.
  std::unique_ptr<sync_preferences::TestingPrefServiceSyncable>
      owned_pref_service_;
  sync_preferences::TestingPrefServiceSyncable* raw_pref_service_ = nullptr;

  std::unique_ptr<TestSigninClient> owned_signin_client_;
  TestSigninClient* raw_signin_client_ = nullptr;


  DISALLOW_COPY_AND_ASSIGN(IdentityManagerDependenciesOwner);
};

IdentityManagerDependenciesOwner::IdentityManagerDependenciesOwner(
    sync_preferences::TestingPrefServiceSyncable* pref_service_param,
    TestSigninClient* signin_client_param)
    : owned_pref_service_(
          pref_service_param
              ? nullptr
              : std::make_unique<
                    sync_preferences::TestingPrefServiceSyncable>()),
      raw_pref_service_(pref_service_param),
      owned_signin_client_(
          signin_client_param
              ? nullptr
              : std::make_unique<TestSigninClient>(pref_service())),
      raw_signin_client_(signin_client_param) {}

IdentityManagerDependenciesOwner::~IdentityManagerDependenciesOwner() = default;

sync_preferences::TestingPrefServiceSyncable*
IdentityManagerDependenciesOwner::pref_service() {
  DCHECK(raw_pref_service_ || owned_pref_service_);
  DCHECK(!(raw_pref_service_ && owned_pref_service_));

  return raw_pref_service_ ? raw_pref_service_ : owned_pref_service_.get();
}

TestSigninClient* IdentityManagerDependenciesOwner::signin_client() {
  DCHECK(raw_signin_client_ || owned_signin_client_);
  DCHECK(!(raw_signin_client_ && owned_signin_client_));

  return raw_signin_client_ ? raw_signin_client_ : owned_signin_client_.get();
}

IdentityTestEnvironment::IdentityTestEnvironment(
    network::TestURLLoaderFactory* test_url_loader_factory,
    sync_preferences::TestingPrefServiceSyncable* pref_service,
    signin::AccountConsistencyMethod account_consistency,
    TestSigninClient* test_signin_client)
    : IdentityTestEnvironment(
          /*pref_service=*/nullptr,
          /*account_tracker_service=*/nullptr,
          /*account_fetcher_service=*/nullptr,
          /*token_service=*/nullptr,
          test_url_loader_factory,
          account_consistency,
          std::make_unique<IdentityManagerDependenciesOwner>(
              pref_service,
              test_signin_client),
          /*identity_manager=*/nullptr) {}

IdentityTestEnvironment::IdentityTestEnvironment(
    PrefService* pref_service,
    AccountTrackerService* account_tracker_service,
    AccountFetcherService* account_fetcher_service,
    FakeProfileOAuth2TokenService* token_service,
    IdentityManager* identity_manager,
    network::TestURLLoaderFactory* test_url_loader_factory,
    signin::AccountConsistencyMethod account_consistency)
    : IdentityTestEnvironment(pref_service,
                              account_tracker_service,
                              account_fetcher_service,
                              token_service,
                              test_url_loader_factory,
                              account_consistency,
                              /*dependency_owner=*/nullptr,
                              identity_manager) {}

IdentityTestEnvironment::IdentityTestEnvironment(
    PrefService* pref_service,
    AccountTrackerService* account_tracker_service,
    AccountFetcherService* account_fetcher_service,
    FakeProfileOAuth2TokenService* token_service,
    network::TestURLLoaderFactory* test_url_loader_factory,
    signin::AccountConsistencyMethod account_consistency,
    std::unique_ptr<IdentityManagerDependenciesOwner> dependencies_owner,
    IdentityManager* identity_manager)
    : test_url_loader_factory_(test_url_loader_factory),
      weak_ptr_factory_(this) {
  DCHECK(base::ThreadTaskRunnerHandle::Get())
      << "IdentityTestEnvironment requires a properly set up task environment. "
         "If your test has an existing one, move it to be initialized before "
         "IdentityTestEnvironment. Otherwise, use "
         "base::test::ScopedTaskEnvironment.";

  TestSigninClient* test_signin_client = nullptr;
  if (dependencies_owner) {
    DCHECK(!(account_tracker_service || account_fetcher_service ||
             token_service || identity_manager));

    dependencies_owner_ = std::move(dependencies_owner);
    test_signin_client = dependencies_owner_->signin_client();
    sync_preferences::TestingPrefServiceSyncable* test_pref_service =
        dependencies_owner_->pref_service();
    pref_service = test_pref_service;

    AccountTrackerService::RegisterPrefs(test_pref_service->registry());
    AccountFetcherService::RegisterPrefs(test_pref_service->registry());
    ProfileOAuth2TokenService::RegisterProfilePrefs(
        test_pref_service->registry());
    SigninManagerBase::RegisterProfilePrefs(test_pref_service->registry());
    SigninManagerBase::RegisterPrefs(test_pref_service->registry());

    owned_token_service_ =
        std::make_unique<FakeProfileOAuth2TokenService>(test_pref_service);
    token_service = owned_token_service_.get();

    owned_account_tracker_service_ = std::make_unique<AccountTrackerService>();
    owned_account_tracker_service_->Initialize(test_pref_service,
                                               base::FilePath());
    account_tracker_service = owned_account_tracker_service_.get();

    owned_account_fetcher_service_ = std::make_unique<AccountFetcherService>();
    owned_account_fetcher_service_->Initialize(
        test_signin_client, token_service, account_tracker_service,
        std::make_unique<image_fetcher::FakeImageDecoder>());
    account_fetcher_service = owned_account_fetcher_service_.get();

  } else {
    owned_signin_client_ = std::make_unique<TestSigninClient>(pref_service);
    test_signin_client = owned_signin_client_.get();
  }

  // TODO(sdefresne): services should be initialized when this version of
  // the constructor is used. However, this break a large number of tests
  // (all those that use an IdentityTestEnvironment and its dependencies
  // as member fields; they should be changed to before the check can be
  // enabled).
  // DCHECK(account_tracker_service_->account_fetcher_service())
  //     << "IdentityTestEnvironment requires its services to be initialized "
  //     << "before passing them to the constructor.";

  if (identity_manager) {
    raw_identity_manager_ = identity_manager;
  } else {
    DCHECK(pref_service && account_tracker_service && account_fetcher_service &&
           token_service);
#if defined(OS_CHROMEOS)
    std::unique_ptr<SigninManagerBase> signin_manager =
        std::make_unique<SigninManagerBase>(test_signin_client, token_service,
                                            account_tracker_service);
#else
    std::unique_ptr<SigninManagerBase> signin_manager =
        std::make_unique<SigninManager>(test_signin_client, token_service,
                                        account_tracker_service, nullptr,
                                        account_consistency);
#endif
    signin_manager->Initialize(pref_service);

    std::unique_ptr<GaiaCookieManagerService> gaia_cookie_manager_service;
    if (test_url_loader_factory != nullptr) {
      gaia_cookie_manager_service = std::make_unique<GaiaCookieManagerService>(
          token_service, test_signin_client,
          base::BindRepeating(
              [](network::TestURLLoaderFactory* test_url_loader_factory)
                  -> scoped_refptr<network::SharedURLLoaderFactory> {
                return test_url_loader_factory->GetSafeWeakWrapper();
              },
              test_url_loader_factory));
    } else {
      gaia_cookie_manager_service = std::make_unique<GaiaCookieManagerService>(
          token_service, test_signin_client);
    }

    std::unique_ptr<PrimaryAccountMutator> primary_account_mutator;
    std::unique_ptr<AccountsMutator> accounts_mutator;

#if !defined(OS_CHROMEOS)
    primary_account_mutator = std::make_unique<PrimaryAccountMutatorImpl>(
        account_tracker_service,
        static_cast<SigninManager*>(signin_manager.get()));
#endif

#if !defined(OS_ANDROID) && !defined(OS_IOS)
    accounts_mutator = std::make_unique<AccountsMutatorImpl>(
        token_service, account_tracker_service, signin_manager.get(),
        pref_service);
#endif

    std::unique_ptr<DiagnosticsProvider> diagnostics_provider =
        std::make_unique<DiagnosticsProviderImpl>(
            token_service, gaia_cookie_manager_service.get());

    std::unique_ptr<AccountsCookieMutator> accounts_cookie_mutator =
        std::make_unique<AccountsCookieMutatorImpl>(
            gaia_cookie_manager_service.get());

    owned_identity_manager_ = std::make_unique<IdentityManager>(
        std::move(gaia_cookie_manager_service), std::move(signin_manager),
        token_service, account_fetcher_service, account_tracker_service,
        std::move(primary_account_mutator), std::move(accounts_mutator),
        std::move(accounts_cookie_mutator), std::move(diagnostics_provider));
  }

  test_identity_manager_observer_ =
      std::make_unique<TestIdentityManagerObserver>(this->identity_manager());

  this->identity_manager()->AddDiagnosticsObserver(this);
}

IdentityTestEnvironment::~IdentityTestEnvironment() {
  if (owned_account_fetcher_service_) {
    owned_account_fetcher_service_->Shutdown();
  }

  if (owned_account_tracker_service_) {
    owned_account_tracker_service_->Shutdown();
  }

  identity_manager()->RemoveDiagnosticsObserver(this);
  identity_manager()->Shutdown();
}

IdentityManager* IdentityTestEnvironment::identity_manager() {
  DCHECK(raw_identity_manager_ || owned_identity_manager_);
  DCHECK(!(raw_identity_manager_ && owned_identity_manager_));

  return raw_identity_manager_ ? raw_identity_manager_
                               : owned_identity_manager_.get();
}

FakeProfileOAuth2TokenService* IdentityTestEnvironment::fake_token_service() {
  // We can't absolutely guarantee that IdentityTestEnvironment was not given an
  // IdentityManager that uses a non-fake FakeProfileOAuth2TokenService. If that
  // ever happens, this will blow up. There doesn't seem to be a better option.
  return static_cast<FakeProfileOAuth2TokenService*>(
      identity_manager()->GetTokenService());
}

TestIdentityManagerObserver*
IdentityTestEnvironment::identity_manager_observer() {
  return test_identity_manager_observer_.get();
}

CoreAccountInfo IdentityTestEnvironment::SetPrimaryAccount(
    const std::string& email) {
  return identity::SetPrimaryAccount(identity_manager(), email);
}

void IdentityTestEnvironment::SetRefreshTokenForPrimaryAccount() {
  identity::SetRefreshTokenForPrimaryAccount(identity_manager());
}

void IdentityTestEnvironment::SetInvalidRefreshTokenForPrimaryAccount() {
  identity::SetInvalidRefreshTokenForPrimaryAccount(identity_manager());
}

void IdentityTestEnvironment::RemoveRefreshTokenForPrimaryAccount() {
  identity::RemoveRefreshTokenForPrimaryAccount(identity_manager());
}

AccountInfo IdentityTestEnvironment::MakePrimaryAccountAvailable(
    const std::string& email) {
  return identity::MakePrimaryAccountAvailable(identity_manager(), email);
}

void IdentityTestEnvironment::ClearPrimaryAccount(
    ClearPrimaryAccountPolicy policy) {
  identity::ClearPrimaryAccount(identity_manager(), policy);
}

AccountInfo IdentityTestEnvironment::MakeAccountAvailable(
    const std::string& email) {
  return identity::MakeAccountAvailable(identity_manager(), email);
}

void IdentityTestEnvironment::SetRefreshTokenForAccount(
    const std::string& account_id) {
  return identity::SetRefreshTokenForAccount(identity_manager(), account_id);
}

void IdentityTestEnvironment::SetInvalidRefreshTokenForAccount(
    const std::string& account_id) {
  return identity::SetInvalidRefreshTokenForAccount(identity_manager(),
                                                    account_id);
}

void IdentityTestEnvironment::RemoveRefreshTokenForAccount(
    const std::string& account_id) {
  return identity::RemoveRefreshTokenForAccount(identity_manager(), account_id);
}

void IdentityTestEnvironment::UpdatePersistentErrorOfRefreshTokenForAccount(
    const std::string& account_id,
    const GoogleServiceAuthError& auth_error) {
  return identity::UpdatePersistentErrorOfRefreshTokenForAccount(
      identity_manager(), account_id, auth_error);
}

void IdentityTestEnvironment::SetCookieAccounts(
    const std::vector<CookieParams>& cookie_accounts) {
  DCHECK(test_url_loader_factory_)
      << "IdentityTestEnvironment constructor must have been passed a "
         "test_url_loader_factory in order to use this method.";
  identity::SetCookieAccounts(identity_manager(), test_url_loader_factory_,
                              cookie_accounts);
}

void IdentityTestEnvironment::SetAutomaticIssueOfAccessTokens(bool grant) {
  fake_token_service()->set_auto_post_fetch_response_on_message_loop(grant);
}

void IdentityTestEnvironment::
    WaitForAccessTokenRequestIfNecessaryAndRespondWithToken(
        const std::string& token,
        const base::Time& expiration,
        const std::string& id_token) {
  WaitForAccessTokenRequestIfNecessary(base::nullopt);
  fake_token_service()->IssueTokenForAllPendingRequests(
      OAuth2AccessTokenConsumer::TokenResponse(token, expiration, id_token));
}

void IdentityTestEnvironment::
    WaitForAccessTokenRequestIfNecessaryAndRespondWithToken(
        const std::string& account_id,
        const std::string& token,
        const base::Time& expiration,
        const std::string& id_token) {
  WaitForAccessTokenRequestIfNecessary(account_id);
  fake_token_service()->IssueAllTokensForAccount(
      account_id,
      OAuth2AccessTokenConsumer::TokenResponse(token, expiration, id_token));
}

void IdentityTestEnvironment::
    WaitForAccessTokenRequestIfNecessaryAndRespondWithTokenForScopes(
        const std::string& token,
        const base::Time& expiration,
        const std::string& id_token,
        const identity::ScopeSet& scopes) {
  WaitForAccessTokenRequestIfNecessary(base::nullopt);
  fake_token_service()->IssueTokenForScope(
      scopes,
      OAuth2AccessTokenConsumer::TokenResponse(token, expiration, id_token));
}

void IdentityTestEnvironment::
    WaitForAccessTokenRequestIfNecessaryAndRespondWithError(
        const GoogleServiceAuthError& error) {
  WaitForAccessTokenRequestIfNecessary(base::nullopt);
  fake_token_service()->IssueErrorForAllPendingRequests(error);
}

void IdentityTestEnvironment::
    WaitForAccessTokenRequestIfNecessaryAndRespondWithError(
        const std::string& account_id,
        const GoogleServiceAuthError& error) {
  WaitForAccessTokenRequestIfNecessary(account_id);
  fake_token_service()->IssueErrorForAllPendingRequestsForAccount(account_id,
                                                                  error);
}

void IdentityTestEnvironment::SetCallbackForNextAccessTokenRequest(
    base::OnceClosure callback) {
  on_access_token_requested_callback_ = std::move(callback);
}

IdentityTestEnvironment::AccessTokenRequestState::AccessTokenRequestState() =
    default;
IdentityTestEnvironment::AccessTokenRequestState::~AccessTokenRequestState() =
    default;
IdentityTestEnvironment::AccessTokenRequestState::AccessTokenRequestState(
    AccessTokenRequestState&& other) = default;
IdentityTestEnvironment::AccessTokenRequestState&
IdentityTestEnvironment::AccessTokenRequestState::operator=(
    AccessTokenRequestState&& other) = default;

void IdentityTestEnvironment::OnAccessTokenRequested(
    const std::string& account_id,
    const std::string& consumer_id,
    const identity::ScopeSet& scopes) {
  // Post a task to handle this access token request in order to support the
  // case where the access token request is handled synchronously in the
  // production code, in which case this callback could be coming in ahead
  // of an invocation of WaitForAccessTokenRequestIfNecessary() that will be
  // made in this same iteration of the run loop.
  base::SequencedTaskRunnerHandle::Get()->PostTask(
      FROM_HERE,
      base::BindOnce(&IdentityTestEnvironment::HandleOnAccessTokenRequested,
                     weak_ptr_factory_.GetWeakPtr(), account_id));
}

void IdentityTestEnvironment::HandleOnAccessTokenRequested(
    std::string account_id) {
  if (on_access_token_requested_callback_) {
    std::move(on_access_token_requested_callback_).Run();
    return;
  }

  for (auto it = requesters_.begin(); it != requesters_.end(); ++it) {
    if (!it->account_id || (it->account_id.value() == account_id)) {
      if (it->state == AccessTokenRequestState::kAvailable)
        return;
      if (it->on_available)
        std::move(it->on_available).Run();
      requesters_.erase(it);
      return;
    }
  }

  // A requests came in for a request for which we are not waiting. Record
  // that it's available.
  requesters_.emplace_back();
  requesters_.back().state = AccessTokenRequestState::kAvailable;
  requesters_.back().account_id = account_id;
}

void IdentityTestEnvironment::WaitForAccessTokenRequestIfNecessary(
    base::Optional<std::string> account_id) {
  // Handle HandleOnAccessTokenRequested getting called before
  // WaitForAccessTokenRequestIfNecessary.
  if (account_id) {
    for (auto it = requesters_.begin(); it != requesters_.end(); ++it) {
      if (it->account_id && it->account_id.value() == account_id.value()) {
        // Can't wait twice for same thing.
        DCHECK_EQ(AccessTokenRequestState::kAvailable, it->state);
        requesters_.erase(it);
        return;
      }
    }
  } else {
    for (auto it = requesters_.begin(); it != requesters_.end(); ++it) {
      if (it->state == AccessTokenRequestState::kAvailable) {
        requesters_.erase(it);
        return;
      }
    }
  }

  base::RunLoop run_loop;
  requesters_.emplace_back();
  requesters_.back().state = AccessTokenRequestState::kPending;
  requesters_.back().account_id = std::move(account_id);
  requesters_.back().on_available = run_loop.QuitClosure();
  run_loop.Run();
}

void IdentityTestEnvironment::UpdateAccountInfoForAccount(
    AccountInfo account_info) {
  identity::UpdateAccountInfoForAccount(identity_manager(), account_info);
}

void IdentityTestEnvironment::ResetToAccountsNotYetLoadedFromDiskState() {
  fake_token_service()->set_all_credentials_loaded_for_testing(false);
}

void IdentityTestEnvironment::ReloadAccountsFromDisk() {
  fake_token_service()->LoadCredentials("");
}

bool IdentityTestEnvironment::IsAccessTokenRequestPending() {
  return fake_token_service()->GetPendingRequests().size();
}

void IdentityTestEnvironment::SetFreshnessOfAccountsInGaiaCookie(
    bool accounts_are_fresh) {
  identity::SetFreshnessOfAccountsInGaiaCookie(identity_manager(),
                                               accounts_are_fresh);
}

void IdentityTestEnvironment::EnableRemovalOfExtendedAccountInfo() {
  identity_manager()->GetAccountFetcherService()->EnableAccountRemovalForTest();
}

void IdentityTestEnvironment::SimulateSuccessfulFetchOfAccountInfo(
    const std::string& account_id,
    const std::string& email,
    const std::string& gaia,
    const std::string& hosted_domain,
    const std::string& full_name,
    const std::string& given_name,
    const std::string& locale,
    const std::string& picture_url) {
  identity::SimulateSuccessfulFetchOfAccountInfo(
      identity_manager(), account_id, email, gaia, hosted_domain, full_name,
      given_name, locale, picture_url);
}

void IdentityTestEnvironment::SimulateMergeSessionFailure(
    const GoogleServiceAuthError& auth_error) {
  // GaiaCookieManagerService changes the visibility of inherited method
  // OnMergeSessionFailure from public to private. Cast to a base class
  // pointer to call the method.
  static_cast<GaiaAuthConsumer*>(
      identity_manager()->GetGaiaCookieManagerService())
      ->OnMergeSessionFailure(auth_error);
}

}  // namespace identity
