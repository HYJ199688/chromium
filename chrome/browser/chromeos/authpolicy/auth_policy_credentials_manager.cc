// Copyright 2017 The Chromium Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include "chrome/browser/chromeos/authpolicy/auth_policy_credentials_manager.h"

#include <memory>
#include <utility>

#include "ash/public/cpp/notification_utils.h"
#include "ash/public/cpp/vector_icons/vector_icons.h"
#include "base/bind.h"
#include "base/location.h"
#include "base/memory/singleton.h"
#include "base/strings/string_util.h"
#include "base/strings/utf_string_conversions.h"
#include "chrome/browser/browser_process.h"
#include "chrome/browser/chromeos/profiles/profile_helper.h"
#include "chrome/browser/lifetime/application_lifetime.h"
#include "chrome/browser/notifications/notification_common.h"
#include "chrome/browser/notifications/notification_display_service.h"
#include "chrome/browser/notifications/notification_display_service_factory.h"
#include "chrome/browser/profiles/profile.h"
#include "chrome/grit/chromium_strings.h"
#include "chrome/grit/generated_resources.h"
#include "chrome/grit/theme_resources.h"
#include "chromeos/components/account_manager/account_manager.h"
#include "chromeos/components/account_manager/account_manager_factory.h"
#include "chromeos/constants/chromeos_switches.h"
#include "chromeos/dbus/auth_policy/auth_policy_client.h"
#include "chromeos/network/network_handler.h"
#include "chromeos/network/network_state.h"
#include "chromeos/network/network_state_handler.h"
#include "components/keyed_service/content/browser_context_dependency_manager.h"
#include "dbus/message.h"
#include "ui/base/l10n/l10n_util.h"
#include "ui/base/resource/resource_bundle.h"
#include "ui/message_center/public/cpp/notification.h"
#include "ui/message_center/public/cpp/notification_delegate.h"

namespace chromeos {

namespace {

constexpr base::TimeDelta kGetUserStatusCallsInterval =
    base::TimeDelta::FromHours(1);
constexpr char kProfileSigninNotificationId[] = "chrome://settings/signin/";

// Sets up Chrome OS Account Manager.
// |profile| is a non-owning pointer to |Profile|.
// |account_id| is the |AccountId| for the Device Account.
void SetupAccountManager(Profile* profile, const AccountId& account_id) {
  if (!switches::IsAccountManagerEnabled())
    return;

  AccountManagerFactory* factory =
      g_browser_process->platform_part()->GetAccountManagerFactory();
  DCHECK(factory);
  AccountManager* account_manager =
      factory->GetAccountManager(profile->GetPath().value());
  DCHECK(account_manager);
  // |AccountManager::UpsertAccount| is idempotent and safe to call multiple
  // times.
  account_manager->UpsertAccount(
      AccountManager::AccountKey{
          account_id.GetObjGuid(),
          account_manager::AccountType::ACCOUNT_TYPE_ACTIVE_DIRECTORY},
      account_id.GetUserEmail(), AccountManager::kActiveDirectoryDummyToken);
}

}  // namespace

AuthPolicyCredentialsManager::AuthPolicyCredentialsManager(Profile* profile)
    : profile_(profile),
      kerberos_files_handler_(base::BindRepeating(
          &AuthPolicyCredentialsManager::GetUserKerberosFiles,
          base::Unretained(this))) {
  const user_manager::User* user =
      ProfileHelper::Get()->GetUserByProfile(profile);
  CHECK(user && user->IsActiveDirectoryUser());
  StartObserveNetwork();
  account_id_ = user->GetAccountId();
  GetUserStatus();
  GetUserKerberosFiles();

  // Connecting to the signal sent by authpolicyd notifying that Kerberos files
  // have changed.
  AuthPolicyClient::Get()->ConnectToSignal(
      authpolicy::kUserKerberosFilesChangedSignal,
      base::Bind(
          &AuthPolicyCredentialsManager::OnUserKerberosFilesChangedCallback,
          weak_factory_.GetWeakPtr()),
      base::Bind(&AuthPolicyCredentialsManager::OnSignalConnectedCallback,
                 weak_factory_.GetWeakPtr()));

  SetupAccountManager(profile, user->GetAccountId());
}

AuthPolicyCredentialsManager::~AuthPolicyCredentialsManager() {}

void AuthPolicyCredentialsManager::Shutdown() {
  StopObserveNetwork();
}

void AuthPolicyCredentialsManager::DefaultNetworkChanged(
    const NetworkState* network) {
  GetUserStatusIfConnected(network);
}

void AuthPolicyCredentialsManager::NetworkConnectionStateChanged(
    const NetworkState* network) {
  GetUserStatusIfConnected(network);
}

void AuthPolicyCredentialsManager::OnShuttingDown() {
  StopObserveNetwork();
}

void AuthPolicyCredentialsManager::GetUserStatus() {
  DCHECK(!is_get_status_in_progress_);
  is_get_status_in_progress_ = true;
  rerun_get_status_on_error_ = false;
  scheduled_get_user_status_call_.Cancel();
  authpolicy::GetUserStatusRequest request;
  request.set_user_principal_name(account_id_.GetUserEmail());
  request.set_account_id(account_id_.GetObjGuid());
  AuthPolicyClient::Get()->GetUserStatus(
      request,
      base::BindOnce(&AuthPolicyCredentialsManager::OnGetUserStatusCallback,
                     weak_factory_.GetWeakPtr()));
}

void AuthPolicyCredentialsManager::OnGetUserStatusCallback(
    authpolicy::ErrorType error,
    const authpolicy::ActiveDirectoryUserStatus& user_status) {
  DCHECK(is_get_status_in_progress_);
  is_get_status_in_progress_ = false;
  ScheduleGetUserStatus();
  last_error_ = error;
  if (error != authpolicy::ERROR_NONE) {
    DLOG(ERROR) << "GetUserStatus failed with " << error;
    if (rerun_get_status_on_error_) {
      rerun_get_status_on_error_ = false;
      GetUserStatus();
    }
    return;
  }
  rerun_get_status_on_error_ = false;

  // user_status.account_info() is missing if the TGT is invalid.
  if (user_status.has_account_info()) {
    CHECK(user_status.account_info().account_id() == account_id_.GetObjGuid());
    UpdateDisplayAndGivenName(user_status.account_info());
  }

  // user_status.password_status() is missing if the TGT is invalid or device is
  // offline.
  bool force_online_signin = false;
  if (user_status.has_password_status()) {
    switch (user_status.password_status()) {
      case authpolicy::ActiveDirectoryUserStatus::PASSWORD_VALID:
        break;
      case authpolicy::ActiveDirectoryUserStatus::PASSWORD_EXPIRED:
        ShowNotification(IDS_ACTIVE_DIRECTORY_PASSWORD_EXPIRED);
        force_online_signin = true;
        break;
      case authpolicy::ActiveDirectoryUserStatus::PASSWORD_CHANGED:
        ShowNotification(IDS_ACTIVE_DIRECTORY_PASSWORD_CHANGED);
        force_online_signin = true;
        break;
    }
  }

  // user_status.tgt_status() is always present.
  DCHECK(user_status.has_tgt_status());
  switch (user_status.tgt_status()) {
    case authpolicy::ActiveDirectoryUserStatus::TGT_VALID:
      break;
    case authpolicy::ActiveDirectoryUserStatus::TGT_EXPIRED:
    case authpolicy::ActiveDirectoryUserStatus::TGT_NOT_FOUND:
      ShowNotification(IDS_ACTIVE_DIRECTORY_REFRESH_AUTH_TOKEN);
      break;
  }

  user_manager::UserManager::Get()->SaveForceOnlineSignin(account_id_,
                                                          force_online_signin);
}

void AuthPolicyCredentialsManager::GetUserKerberosFiles() {
  AuthPolicyClient::Get()->GetUserKerberosFiles(
      account_id_.GetObjGuid(),
      base::BindOnce(
          &AuthPolicyCredentialsManager::OnGetUserKerberosFilesCallback,
          weak_factory_.GetWeakPtr()));
}

void AuthPolicyCredentialsManager::OnGetUserKerberosFilesCallback(
    authpolicy::ErrorType error,
    const authpolicy::KerberosFiles& kerberos_files) {
  auto nullstr = base::Optional<std::string>();
  kerberos_files_handler_.SetFiles(
      kerberos_files.has_krb5cc() ? kerberos_files.krb5cc() : nullstr,
      kerberos_files.has_krb5conf() ? kerberos_files.krb5conf() : nullstr);
}

void AuthPolicyCredentialsManager::ScheduleGetUserStatus() {
  // Unretained is safe here because it is a CancelableClosure and owned by this
  // object.
  scheduled_get_user_status_call_.Reset(base::Bind(
      &AuthPolicyCredentialsManager::GetUserStatus, base::Unretained(this)));
  // TODO(rsorokin): This does not re-schedule after wake from sleep
  // (and thus the maximal interval between two calls can be (sleep time +
  // kGetUserStatusCallsInterval)) (see crbug.com/726672).
  base::ThreadTaskRunnerHandle::Get()->PostDelayedTask(
      FROM_HERE, scheduled_get_user_status_call_.callback(),
      kGetUserStatusCallsInterval);
}

void AuthPolicyCredentialsManager::StartObserveNetwork() {
  DCHECK(NetworkHandler::IsInitialized());
  if (is_observing_network_)
    return;
  is_observing_network_ = true;
  NetworkHandler::Get()->network_state_handler()->AddObserver(this, FROM_HERE);
}

void AuthPolicyCredentialsManager::StopObserveNetwork() {
  if (!is_observing_network_)
    return;
  DCHECK(NetworkHandler::IsInitialized());
  is_observing_network_ = false;
  NetworkHandler::Get()->network_state_handler()->RemoveObserver(this,
                                                                 FROM_HERE);
}

void AuthPolicyCredentialsManager::UpdateDisplayAndGivenName(
    const authpolicy::ActiveDirectoryAccountInfo& account_info) {
  if (display_name_ == account_info.display_name() &&
      given_name_ == account_info.given_name()) {
    return;
  }
  display_name_ = account_info.display_name();
  given_name_ = account_info.given_name();
  user_manager::UserManager::Get()->UpdateUserAccountData(
      account_id_,
      user_manager::UserManager::UserAccountData(
          base::UTF8ToUTF16(display_name_), base::UTF8ToUTF16(given_name_),
          std::string() /* locale */));
}

void AuthPolicyCredentialsManager::ShowNotification(int message_id) {
  if (shown_notifications_.count(message_id) > 0)
    return;

  message_center::RichNotificationData data;
  data.buttons.push_back(message_center::ButtonInfo(
      l10n_util::GetStringUTF16(IDS_SYNC_RELOGIN_LINK_LABEL)));

  const std::string notification_id = kProfileSigninNotificationId +
                                      profile_->GetProfileUserName() +
                                      std::to_string(message_id);
  message_center::NotifierId notifier_id(
      message_center::NotifierType::SYSTEM_COMPONENT,
      kProfileSigninNotificationId);

  // Set |profile_id| for multi-user notification blocker.
  notifier_id.profile_id = profile_->GetProfileUserName();

  auto delegate =
      base::MakeRefCounted<message_center::HandleNotificationClickDelegate>(
          base::BindRepeating([](base::Optional<int> button_index) {
            chrome::AttemptUserExit();
          }));

  std::unique_ptr<message_center::Notification> notification =
      ash::CreateSystemNotification(
          message_center::NOTIFICATION_TYPE_SIMPLE, notification_id,
          l10n_util::GetStringUTF16(IDS_SIGNIN_ERROR_BUBBLE_VIEW_TITLE),
          l10n_util::GetStringUTF16(message_id),
          l10n_util::GetStringUTF16(IDS_SIGNIN_ERROR_DISPLAY_SOURCE),
          GURL(notification_id), notifier_id, data, std::move(delegate),
          ash::kNotificationWarningIcon,
          message_center::SystemNotificationWarningLevel::WARNING);
  notification->SetSystemPriority();

  // Add the notification.
  NotificationDisplayServiceFactory::GetForProfile(profile_)->Display(
      NotificationHandler::Type::TRANSIENT, *notification,
      /*metadata=*/nullptr);
  shown_notifications_.insert(message_id);
}

void AuthPolicyCredentialsManager::GetUserStatusIfConnected(
    const NetworkState* network) {
  if (!network || !network->IsConnectedState())
    return;
  if (is_get_status_in_progress_) {
    rerun_get_status_on_error_ = true;
    return;
  }
  if (last_error_ != authpolicy::ERROR_NONE)
    GetUserStatus();
}

void AuthPolicyCredentialsManager::OnUserKerberosFilesChangedCallback(
    dbus::Signal* signal) {
  DCHECK_EQ(signal->GetInterface(), authpolicy::kAuthPolicyInterface);
  DCHECK_EQ(signal->GetMember(), authpolicy::kUserKerberosFilesChangedSignal);
  GetUserKerberosFiles();
}

void AuthPolicyCredentialsManager::OnSignalConnectedCallback(
    const std::string& interface_name,
    const std::string& signal_name,
    bool success) {
  DCHECK_EQ(interface_name, authpolicy::kAuthPolicyInterface);
  DCHECK_EQ(signal_name, authpolicy::kUserKerberosFilesChangedSignal);
  DCHECK(success);
}

// static
AuthPolicyCredentialsManagerFactory*
AuthPolicyCredentialsManagerFactory::GetInstance() {
  return base::Singleton<AuthPolicyCredentialsManagerFactory>::get();
}

AuthPolicyCredentialsManagerFactory::AuthPolicyCredentialsManagerFactory()
    : BrowserContextKeyedServiceFactory(
          "AuthPolicyCredentialsManager",
          BrowserContextDependencyManager::GetInstance()) {}

AuthPolicyCredentialsManagerFactory::~AuthPolicyCredentialsManagerFactory() {}

bool AuthPolicyCredentialsManagerFactory::ServiceIsCreatedWithBrowserContext()
    const {
  return true;
}

KeyedService* AuthPolicyCredentialsManagerFactory::BuildServiceInstanceFor(
    content::BrowserContext* context) const {
  // UserManager is usually not initialized in tests.
  if (!user_manager::UserManager::IsInitialized())
    return nullptr;
  Profile* profile = Profile::FromBrowserContext(context);
  const user_manager::User* user =
      ProfileHelper::Get()->GetUserByProfile(profile);
  if (!user || !user->IsActiveDirectoryUser())
    return nullptr;
  return new AuthPolicyCredentialsManager(profile);
}

}  // namespace chromeos
