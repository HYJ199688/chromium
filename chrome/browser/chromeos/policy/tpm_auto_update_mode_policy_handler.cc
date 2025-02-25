// Copyright 2019 The Chromium Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include "chrome/browser/chromeos/policy/tpm_auto_update_mode_policy_handler.h"

#include <utility>

#include "base/bind.h"
#include "base/values.h"
#include "chrome/browser/browser_process.h"
#include "chrome/browser/chromeos/policy/browser_policy_connector_chromeos.h"
#include "chrome/browser/chromeos/tpm_firmware_update.h"
#include "chromeos/dbus/dbus_thread_manager.h"
#include "chromeos/dbus/session_manager/session_manager_client.h"
#include "chromeos/settings/cros_settings_names.h"
#include "chromeos/settings/cros_settings_provider.h"

namespace {

// Reads the value of the the device setting key
// TPMFirmwareUpdateSettings.AutoUpdateMode from a trusted store. If the value
// is temporarily untrusted |callback| will be invoked later when trusted values
// are available and AutoUpdateMode::kNever will be returned. This value is set
// via the device policy TPMFirmwareUpdateSettings.
policy::AutoUpdateMode GetTPMAutoUpdateModeSetting(
    const chromeos::CrosSettings* cros_settings,
    const base::RepeatingCallback<void()> callback) {
  if (!g_browser_process->platform_part()
           ->browser_policy_connector_chromeos()
           ->IsEnterpriseManaged()) {
    return policy::AutoUpdateMode::kNever;
  }

  chromeos::CrosSettingsProvider::TrustedStatus status =
      cros_settings->PrepareTrustedValues(base::BindRepeating(callback));
  if (status != chromeos::CrosSettingsProvider::TRUSTED)
    return policy::AutoUpdateMode::kNever;
  const base::Value* tpm_settings =
      cros_settings->GetPref(chromeos::kTPMFirmwareUpdateSettings);

  if (!tpm_settings)
    return policy::AutoUpdateMode::kNever;

  const base::Value* const auto_update_mode = tpm_settings->FindKeyOfType(
      chromeos::tpm_firmware_update::kSettingsKeyAutoUpdateMode,
      base::Value::Type::INTEGER);

  // Policy not set.
  if (!auto_update_mode || auto_update_mode->GetInt() == 0)
    return policy::AutoUpdateMode::kNever;

  // Verify that the value is within range.
  if (auto_update_mode->GetInt() <
          static_cast<int>(policy::AutoUpdateMode::kNever) ||
      auto_update_mode->GetInt() >
          static_cast<int>(policy::AutoUpdateMode::kEnrollment)) {
    NOTREACHED() << "Invalid value for device policy key "
                    "TPMFirmwareUpdateSettings.AutoUpdateMode";
    return policy::AutoUpdateMode::kNever;
  }

  return static_cast<policy::AutoUpdateMode>(auto_update_mode->GetInt());
}

}  // namespace

namespace policy {

TPMAutoUpdateModePolicyHandler::TPMAutoUpdateModePolicyHandler(
    chromeos::CrosSettings* cros_settings)
    : cros_settings_(cros_settings), weak_factory_(this) {
  policy_subscription_ = cros_settings_->AddSettingsObserver(
      chromeos::kTPMFirmwareUpdateSettings,
      base::BindRepeating(&TPMAutoUpdateModePolicyHandler::OnPolicyChanged,
                          weak_factory_.GetWeakPtr()));

  update_checker_callback_ =
      base::BindRepeating(&TPMAutoUpdateModePolicyHandler::CheckForUpdate,
                          weak_factory_.GetWeakPtr());
  // Fire it once so we're sure we get an invocation on startup.
  OnPolicyChanged();
}

TPMAutoUpdateModePolicyHandler::~TPMAutoUpdateModePolicyHandler() = default;

void TPMAutoUpdateModePolicyHandler::OnPolicyChanged() {
  AutoUpdateMode auto_update_mode = GetTPMAutoUpdateModeSetting(
      cros_settings_,
      base::BindRepeating(&TPMAutoUpdateModePolicyHandler::OnPolicyChanged,
                          weak_factory_.GetWeakPtr()));

  if (auto_update_mode == AutoUpdateMode::kNever ||
      auto_update_mode == AutoUpdateMode::kEnrollment) {
    return;
  }

  if (auto_update_mode == AutoUpdateMode::kWithoutAcknowledgment)
    update_checker_callback_.Run(base::BindOnce(&OnUpdateAvailableCheckResult));
}

void TPMAutoUpdateModePolicyHandler::CheckForUpdate(
    base::OnceCallback<void(bool)> callback) {
  chromeos::tpm_firmware_update::UpdateAvailable(
      std::move(callback), base::TimeDelta().FromSeconds(20));
}

// static
void TPMAutoUpdateModePolicyHandler::OnUpdateAvailableCheckResult(
    bool update_available) {
  if (!update_available)
    return;

  chromeos::DBusThreadManager::Get()
      ->GetSessionManagerClient()
      ->StartTPMFirmwareUpdate("preserve_stateful");
}

void TPMAutoUpdateModePolicyHandler::SetUpdateCheckerCallbackForTesting(
    const UpdateCheckerCallback& callback) {
  update_checker_callback_ = callback;
}

}  // namespace policy
