// Copyright 2014 The Chromium Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include "chromeos/tpm/tpm_token_loader.h"

#include <algorithm>

#include "base/bind.h"
#include "base/location.h"
#include "base/sequenced_task_runner.h"
#include "base/single_thread_task_runner.h"
#include "base/system/sys_info.h"
#include "base/task_runner_util.h"
#include "base/threading/thread_task_runner_handle.h"
#include "chromeos/dbus/dbus_thread_manager.h"
#include "chromeos/tpm/tpm_token_info_getter.h"
#include "crypto/nss_util.h"

namespace chromeos {

namespace {

void PostResultToTaskRunner(scoped_refptr<base::SequencedTaskRunner> runner,
                            const base::Callback<void(bool)>& callback,
                            bool success) {
  runner->PostTask(FROM_HERE, base::BindOnce(callback, success));
}

}  // namespace

static TPMTokenLoader* g_tpm_token_loader = NULL;

// static
void TPMTokenLoader::Initialize() {
  CHECK(!g_tpm_token_loader);
  g_tpm_token_loader = new TPMTokenLoader(false /*for_test*/);
}

// static
void TPMTokenLoader::InitializeForTest() {
  CHECK(!g_tpm_token_loader);
  g_tpm_token_loader = new TPMTokenLoader(true /*for_test*/);
}

// static
void TPMTokenLoader::Shutdown() {
  CHECK(g_tpm_token_loader);
  delete g_tpm_token_loader;
  g_tpm_token_loader = NULL;
}

// static
TPMTokenLoader* TPMTokenLoader::Get() {
  CHECK(g_tpm_token_loader)
      << "TPMTokenLoader::Get() called before Initialize()";
  return g_tpm_token_loader;
}

// static
bool TPMTokenLoader::IsInitialized() {
  return g_tpm_token_loader;
}

TPMTokenLoader::TPMTokenLoader(bool for_test)
    : initialized_for_test_(for_test),
      tpm_token_state_(TPM_STATE_UNKNOWN),
      tpm_token_info_getter_(TPMTokenInfoGetter::CreateForSystemToken(
          CryptohomeClient::Get(),
          base::ThreadTaskRunnerHandle::Get())),
      tpm_token_slot_id_(-1),
      can_start_before_login_(false),
      weak_factory_(this) {
  if (!initialized_for_test_ && LoginState::IsInitialized())
    LoginState::Get()->AddObserver(this);

  if (initialized_for_test_) {
    tpm_token_state_ = TPM_TOKEN_INITIALIZED;
    tpm_user_pin_ = "111111";
  }
}

void TPMTokenLoader::SetCryptoTaskRunner(
    const scoped_refptr<base::SequencedTaskRunner>& crypto_task_runner) {
  crypto_task_runner_ = crypto_task_runner;
  MaybeStartTokenInitialization();
}

void TPMTokenLoader::EnsureStarted() {
  if (can_start_before_login_)
    return;
  can_start_before_login_ = true;
  MaybeStartTokenInitialization();
}

TPMTokenLoader::~TPMTokenLoader() {
  if (!initialized_for_test_ && LoginState::IsInitialized())
    LoginState::Get()->RemoveObserver(this);
}

TPMTokenLoader::TPMTokenStatus TPMTokenLoader::IsTPMTokenEnabled(
    const TPMReadyCallback& callback) {
  if (tpm_token_state_ == TPM_TOKEN_INITIALIZED)
    return TPM_TOKEN_STATUS_ENABLED;
  if (!IsTPMLoadingEnabled() || tpm_token_state_ == TPM_DISABLED)
    return TPM_TOKEN_STATUS_DISABLED;
  // Status is not known yet.
  if (!callback.is_null())
    tpm_ready_callback_list_.push_back(callback);
  return TPM_TOKEN_STATUS_UNDETERMINED;
}

bool TPMTokenLoader::IsTPMLoadingEnabled() const {
  // TPM loading is enabled on non-ChromeOS environments, e.g. when running
  // tests on Linux.
  // Treat TPM as disabled for guest users since they do not store certs.
  return initialized_for_test_ || (base::SysInfo::IsRunningOnChromeOS() &&
                                   !LoginState::Get()->IsGuestSessionUser());
}

void TPMTokenLoader::MaybeStartTokenInitialization() {
  CHECK(thread_checker_.CalledOnValidThread());

  // This is the entry point to the TPM token initialization process,
  // which we should do at most once.
  if (tpm_token_state_ != TPM_STATE_UNKNOWN || !crypto_task_runner_.get())
    return;

  bool start_initialization =
      (LoginState::IsInitialized() && LoginState::Get()->IsUserLoggedIn()) ||
      can_start_before_login_;

  VLOG(1) << "StartTokenInitialization: " << start_initialization;
  if (!start_initialization)
    return;

  if (!IsTPMLoadingEnabled())
    tpm_token_state_ = TPM_DISABLED;

  ContinueTokenInitialization();

  DCHECK_NE(tpm_token_state_, TPM_STATE_UNKNOWN);
}

void TPMTokenLoader::ContinueTokenInitialization() {
  CHECK(thread_checker_.CalledOnValidThread());
  VLOG(1) << "ContinueTokenInitialization: " << tpm_token_state_;

  switch (tpm_token_state_) {
    case TPM_STATE_UNKNOWN: {
      crypto_task_runner_->PostTaskAndReply(
          FROM_HERE, base::BindOnce(&crypto::EnableTPMTokenForNSS),
          base::BindOnce(&TPMTokenLoader::OnTPMTokenEnabledForNSS,
                         weak_factory_.GetWeakPtr()));
      tpm_token_state_ = TPM_INITIALIZATION_STARTED;
      return;
    }
    case TPM_INITIALIZATION_STARTED: {
      NOTREACHED();
      return;
    }
    case TPM_TOKEN_ENABLED_FOR_NSS: {
      tpm_token_info_getter_->Start(base::BindOnce(
          &TPMTokenLoader::OnGotTpmTokenInfo, weak_factory_.GetWeakPtr()));
      return;
    }
    case TPM_DISABLED: {
      // TPM is disabled, so proceed with empty tpm token name.
      NotifyTPMTokenReady();
      return;
    }
    case TPM_TOKEN_INFO_RECEIVED: {
      crypto_task_runner_->PostTask(
          FROM_HERE,
          base::BindOnce(
              &crypto::InitializeTPMTokenAndSystemSlot, tpm_token_slot_id_,
              base::Bind(&PostResultToTaskRunner,
                         base::ThreadTaskRunnerHandle::Get(),
                         base::Bind(&TPMTokenLoader::OnTPMTokenInitialized,
                                    weak_factory_.GetWeakPtr()))));
      return;
    }
    case TPM_TOKEN_INITIALIZED: {
      NotifyTPMTokenReady();
      return;
    }
  }
}

void TPMTokenLoader::OnTPMTokenEnabledForNSS() {
  VLOG(1) << "TPMTokenEnabledForNSS";
  tpm_token_state_ = TPM_TOKEN_ENABLED_FOR_NSS;
  ContinueTokenInitialization();
}

void TPMTokenLoader::OnGotTpmTokenInfo(
    base::Optional<CryptohomeClient::TpmTokenInfo> token_info) {
  if (!token_info.has_value()) {
    tpm_token_state_ = TPM_DISABLED;
    ContinueTokenInitialization();
    return;
  }

  tpm_token_slot_id_ = token_info->slot;
  tpm_user_pin_ = token_info->user_pin;
  tpm_token_state_ = TPM_TOKEN_INFO_RECEIVED;

  ContinueTokenInitialization();
}

void TPMTokenLoader::OnTPMTokenInitialized(bool success) {
  VLOG(1) << "OnTPMTokenInitialized: " << success;

  tpm_token_state_ = success ? TPM_TOKEN_INITIALIZED : TPM_DISABLED;
  ContinueTokenInitialization();
}

void TPMTokenLoader::NotifyTPMTokenReady() {
  DCHECK(tpm_token_state_ == TPM_DISABLED ||
         tpm_token_state_ == TPM_TOKEN_INITIALIZED);
  bool tpm_status = tpm_token_state_ == TPM_TOKEN_INITIALIZED;
  for (TPMReadyCallbackList::iterator i = tpm_ready_callback_list_.begin();
       i != tpm_ready_callback_list_.end();
       ++i) {
    i->Run(tpm_status);
  }
  tpm_ready_callback_list_.clear();
}

void TPMTokenLoader::LoggedInStateChanged() {
  VLOG(1) << "LoggedInStateChanged";
  MaybeStartTokenInitialization();
}

}  // namespace chromeos
