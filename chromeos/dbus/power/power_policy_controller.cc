// Copyright (c) 2013 The Chromium Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include "chromeos/dbus/power/power_policy_controller.h"

#include <stdint.h>

#include <memory>
#include <utility>

#include "base/format_macros.h"
#include "base/logging.h"
#include "base/strings/string_util.h"
#include "base/strings/stringprintf.h"
#include "chromeos/dbus/power_manager/backlight.pb.h"

// Avoid some ugly line-wrapping later.
using base::StringAppendF;

namespace chromeos {

namespace {

PowerPolicyController* g_power_policy_controller = nullptr;

// Appends a description of |field|, a field within |delays|, a
// power_manager::PowerManagementPolicy::Delays object, to |str|, an
// std::string, if the field is set.  |name| is a char* describing the
// field.
#define APPEND_DELAY(str, delays, field, name)                   \
  {                                                              \
    if (delays.has_##field())                                    \
      StringAppendF(&str, name "=%" PRId64 " ", delays.field()); \
  }

// Appends descriptions of all of the set delays in |delays|, a
// power_manager::PowerManagementPolicy::Delays object, to |str|, an
// std::string.  |prefix| should be a char* containing either "ac" or
// "battery".
#define APPEND_DELAYS(str, delays, prefix)                                 \
  {                                                                        \
    APPEND_DELAY(str, delays, screen_dim_ms, prefix "_screen_dim_ms");     \
    APPEND_DELAY(str, delays, screen_off_ms, prefix "_screen_off_ms");     \
    APPEND_DELAY(str, delays, screen_lock_ms, prefix "_screen_lock_ms");   \
    APPEND_DELAY(str, delays, idle_warning_ms, prefix "_idle_warning_ms"); \
    APPEND_DELAY(str, delays, idle_ms, prefix "_idle_ms");                 \
  }

// Returns the power_manager::PowerManagementPolicy_Action value
// corresponding to |action|.
power_manager::PowerManagementPolicy_Action GetProtoAction(
    PowerPolicyController::Action action) {
  switch (action) {
    case PowerPolicyController::ACTION_SUSPEND:
      return power_manager::PowerManagementPolicy_Action_SUSPEND;
    case PowerPolicyController::ACTION_STOP_SESSION:
      return power_manager::PowerManagementPolicy_Action_STOP_SESSION;
    case PowerPolicyController::ACTION_SHUT_DOWN:
      return power_manager::PowerManagementPolicy_Action_SHUT_DOWN;
    case PowerPolicyController::ACTION_DO_NOTHING:
      return power_manager::PowerManagementPolicy_Action_DO_NOTHING;
    default:
      NOTREACHED() << "Unhandled action " << action;
      return power_manager::PowerManagementPolicy_Action_DO_NOTHING;
  }
}

// Returns false if |use_audio_activity| and |use_audio_activity| prevent wake
// locks created for |reason| from being honored or true otherwise.
bool IsWakeLockReasonHonored(PowerPolicyController::WakeLockReason reason,
                             bool use_audio_activity,
                             bool use_video_activity) {
  if (reason == PowerPolicyController::REASON_AUDIO_PLAYBACK &&
      !use_audio_activity)
    return false;
  if (reason == PowerPolicyController::REASON_VIDEO_PLAYBACK &&
      !use_video_activity)
    return false;
  return true;
}

power_manager::PowerManagementPolicy::PeakShiftDayConfig::WeekDay
GetProtoWeekDay(PowerPolicyController::WeekDay week_day) {
  switch (week_day) {
    case PowerPolicyController::WeekDay::WEEK_DAY_MONDAY:
      return power_manager::PowerManagementPolicy::PeakShiftDayConfig::MONDAY;
    case PowerPolicyController::WeekDay::WEEK_DAY_TUESDAY:
      return power_manager::PowerManagementPolicy::PeakShiftDayConfig::TUESDAY;
    case PowerPolicyController::WeekDay::WEEK_DAY_WEDNESDAY:
      return power_manager::PowerManagementPolicy::PeakShiftDayConfig::
          WEDNESDAY;
    case PowerPolicyController::WeekDay::WEEK_DAY_THURSDAY:
      return power_manager::PowerManagementPolicy::PeakShiftDayConfig::THURSDAY;
    case PowerPolicyController::WeekDay::WEEK_DAY_FRIDAY:
      return power_manager::PowerManagementPolicy::PeakShiftDayConfig::FRIDAY;
    case PowerPolicyController::WeekDay::WEEK_DAY_SATURDAY:
      return power_manager::PowerManagementPolicy::PeakShiftDayConfig::SATURDAY;
    case PowerPolicyController::WeekDay::WEEK_DAY_SUNDAY:
      return power_manager::PowerManagementPolicy::PeakShiftDayConfig::SUNDAY;
  }
}

std::unique_ptr<
    power_manager::PowerManagementPolicy::PeakShiftDayConfig::DayTime>
GetProtoDayTime(const PowerPolicyController::DayTime& day_time) {
  auto proto = std::make_unique<
      power_manager::PowerManagementPolicy::PeakShiftDayConfig::DayTime>();
  proto->set_hour(day_time.hour);
  proto->set_minute(day_time.minute);
  return proto;
}

}  // namespace

const int PowerPolicyController::kScreenLockAfterOffDelayMs = 10000;  // 10 sec.
const char PowerPolicyController::kPrefsReason[] = "Prefs";

// -1 is interpreted as "unset" by powerd, resulting in powerd's default
// delays being used instead.  There are no similarly-interpreted values
// for the other fields, unfortunately (but the constructor-assigned values
// will only reach powerd if Chrome messes up and forgets to override them
// with the pref-assigned values).
PowerPolicyController::PrefValues::PrefValues()
    : ac_screen_dim_delay_ms(-1),
      ac_screen_off_delay_ms(-1),
      ac_screen_lock_delay_ms(-1),
      ac_idle_warning_delay_ms(-1),
      ac_idle_delay_ms(-1),
      battery_screen_dim_delay_ms(-1),
      battery_screen_off_delay_ms(-1),
      battery_screen_lock_delay_ms(-1),
      battery_idle_warning_delay_ms(-1),
      battery_idle_delay_ms(-1),
      ac_idle_action(ACTION_SUSPEND),
      battery_idle_action(ACTION_SUSPEND),
      lid_closed_action(ACTION_SUSPEND),
      use_audio_activity(true),
      use_video_activity(true),
      ac_brightness_percent(-1.0),
      battery_brightness_percent(-1.0),
      allow_wake_locks(true),
      allow_screen_wake_locks(true),
      enable_auto_screen_lock(false),
      presentation_screen_dim_delay_factor(1.0),
      user_activity_screen_dim_delay_factor(1.0),
      wait_for_initial_user_activity(false),
      force_nonzero_brightness_for_user_activity(true),
      peak_shift_enabled(false),
      peak_shift_battery_threshold(-1) {}

PowerPolicyController::PrefValues::~PrefValues() = default;

// static
std::string PowerPolicyController::GetPolicyDebugString(
    const power_manager::PowerManagementPolicy& policy) {
  std::string str;
  if (policy.has_ac_delays())
    APPEND_DELAYS(str, policy.ac_delays(), "ac");
  if (policy.has_battery_delays())
    APPEND_DELAYS(str, policy.battery_delays(), "battery");
  if (policy.has_ac_idle_action())
    StringAppendF(&str, "ac_idle=%d ", policy.ac_idle_action());
  if (policy.has_battery_idle_action())
    StringAppendF(&str, "battery_idle=%d ", policy.battery_idle_action());
  if (policy.has_lid_closed_action())
    StringAppendF(&str, "lid_closed=%d ", policy.lid_closed_action());
  if (policy.has_screen_wake_lock())
    StringAppendF(&str, "screen_wake_lock=%d ", policy.screen_wake_lock());
  if (policy.has_dim_wake_lock())
    StringAppendF(&str, "dim_wake_lock=%d ", policy.dim_wake_lock());
  if (policy.has_system_wake_lock())
    StringAppendF(&str, "system_wake_lock=%d ", policy.system_wake_lock());
  if (policy.has_use_audio_activity())
    StringAppendF(&str, "use_audio=%d ", policy.use_audio_activity());
  if (policy.has_use_video_activity())
    StringAppendF(&str, "use_video=%d ", policy.use_audio_activity());
  if (policy.has_ac_brightness_percent())
    StringAppendF(&str, "ac_brightness_percent=%f ",
                  policy.ac_brightness_percent());
  if (policy.has_battery_brightness_percent()) {
    StringAppendF(&str, "battery_brightness_percent=%f ",
                  policy.battery_brightness_percent());
  }
  if (policy.has_presentation_screen_dim_delay_factor()) {
    StringAppendF(&str, "presentation_screen_dim_delay_factor=%f ",
                  policy.presentation_screen_dim_delay_factor());
  }
  if (policy.has_user_activity_screen_dim_delay_factor()) {
    StringAppendF(&str, "user_activity_screen_dim_delay_factor=%f ",
                  policy.user_activity_screen_dim_delay_factor());
  }
  if (policy.has_wait_for_initial_user_activity()) {
    StringAppendF(&str, "wait_for_initial_user_activity=%d ",
                  policy.wait_for_initial_user_activity());
  }
  if (policy.has_force_nonzero_brightness_for_user_activity()) {
    StringAppendF(&str, "force_nonzero_brightness_for_user_activity=%d ",
                  policy.force_nonzero_brightness_for_user_activity());
  }

  str += GetPeakShiftPolicyDebugString(policy);

  if (policy.has_reason())
    StringAppendF(&str, "reason=\"%s\" ", policy.reason().c_str());
  base::TrimWhitespaceASCII(str, base::TRIM_TRAILING, &str);
  return str;
}

// static
std::string PowerPolicyController::GetPeakShiftPolicyDebugString(
    const power_manager::PowerManagementPolicy& policy) {
  std::string str;
  if (policy.has_peak_shift_battery_percent_threshold()) {
    StringAppendF(&str, "peak_shift_battery_threshold=%d ",
                  policy.peak_shift_battery_percent_threshold());
  }
  if (policy.peak_shift_day_configs_size()) {
    StringAppendF(&str, "peak_shift_day_configuration=[");
    for (auto config : policy.peak_shift_day_configs()) {
      StringAppendF(&str,
                    "{day=%d start_time=%d:%02d end_time=%d:%02d "
                    "charge_start_time=%d:%02d} ",
                    config.day(), config.start_time().hour(),
                    config.start_time().minute(), config.end_time().hour(),
                    config.end_time().minute(),
                    config.charge_start_time().hour(),
                    config.charge_start_time().minute());
    }
    StringAppendF(&str, "]");
  }
  return str;
}

// static
void PowerPolicyController::Initialize(PowerManagerClient* client) {
  DCHECK(!IsInitialized());
  g_power_policy_controller = new PowerPolicyController(client);
}

// static
bool PowerPolicyController::IsInitialized() {
  return g_power_policy_controller;
}

// static
void PowerPolicyController::Shutdown() {
  DCHECK(IsInitialized());
  delete g_power_policy_controller;
  g_power_policy_controller = nullptr;
}

// static
PowerPolicyController* PowerPolicyController::Get() {
  DCHECK(IsInitialized());
  return g_power_policy_controller;
}

void PowerPolicyController::ApplyPrefs(const PrefValues& values) {
  prefs_policy_.Clear();

  power_manager::PowerManagementPolicy::Delays* delays =
      prefs_policy_.mutable_ac_delays();
  delays->set_screen_dim_ms(values.ac_screen_dim_delay_ms);
  delays->set_screen_off_ms(values.ac_screen_off_delay_ms);
  delays->set_screen_lock_ms(values.ac_screen_lock_delay_ms);
  delays->set_idle_warning_ms(values.ac_idle_warning_delay_ms);
  delays->set_idle_ms(values.ac_idle_delay_ms);

  // If auto screen-locking is enabled, ensure that the screen is locked soon
  // after it's turned off due to user inactivity.
  int64_t lock_ms = delays->screen_off_ms() + kScreenLockAfterOffDelayMs;
  if (values.enable_auto_screen_lock && delays->screen_off_ms() > 0 &&
      (delays->screen_lock_ms() <= 0 || lock_ms < delays->screen_lock_ms()) &&
      lock_ms < delays->idle_ms()) {
    delays->set_screen_lock_ms(lock_ms);
  }

  delays = prefs_policy_.mutable_battery_delays();
  delays->set_screen_dim_ms(values.battery_screen_dim_delay_ms);
  delays->set_screen_off_ms(values.battery_screen_off_delay_ms);
  delays->set_screen_lock_ms(values.battery_screen_lock_delay_ms);
  delays->set_idle_warning_ms(values.battery_idle_warning_delay_ms);
  delays->set_idle_ms(values.battery_idle_delay_ms);

  lock_ms = delays->screen_off_ms() + kScreenLockAfterOffDelayMs;
  if (values.enable_auto_screen_lock && delays->screen_off_ms() > 0 &&
      (delays->screen_lock_ms() <= 0 || lock_ms < delays->screen_lock_ms()) &&
      lock_ms < delays->idle_ms()) {
    delays->set_screen_lock_ms(lock_ms);
  }
  auto_screen_lock_enabled_ = values.enable_auto_screen_lock;

  prefs_policy_.set_ac_idle_action(GetProtoAction(values.ac_idle_action));
  prefs_policy_.set_battery_idle_action(
      GetProtoAction(values.battery_idle_action));
  prefs_policy_.set_lid_closed_action(GetProtoAction(values.lid_closed_action));
  prefs_policy_.set_use_audio_activity(values.use_audio_activity);
  prefs_policy_.set_use_video_activity(values.use_video_activity);

  if (!per_session_brightness_override_) {
    if (values.ac_brightness_percent >= 0.0)
      prefs_policy_.set_ac_brightness_percent(values.ac_brightness_percent);
    if (values.battery_brightness_percent >= 0.0) {
      prefs_policy_.set_battery_brightness_percent(
          values.battery_brightness_percent);
    }
  }

  prefs_policy_.set_presentation_screen_dim_delay_factor(
      values.presentation_screen_dim_delay_factor);
  prefs_policy_.set_user_activity_screen_dim_delay_factor(
      values.user_activity_screen_dim_delay_factor);

  prefs_policy_.set_wait_for_initial_user_activity(
      values.wait_for_initial_user_activity);
  prefs_policy_.set_force_nonzero_brightness_for_user_activity(
      values.force_nonzero_brightness_for_user_activity);

  honor_wake_locks_ = values.allow_wake_locks;
  honor_screen_wake_locks_ =
      honor_wake_locks_ && values.allow_screen_wake_locks;

  if (values.peak_shift_enabled) {
    prefs_policy_.set_peak_shift_battery_percent_threshold(
        values.peak_shift_battery_threshold);
    for (const PeakShiftDayConfiguration& values_config :
         values.peak_shift_day_configurations) {
      power_manager::PowerManagementPolicy::PeakShiftDayConfig* proto_config =
          prefs_policy_.add_peak_shift_day_configs();
      proto_config->set_day(GetProtoWeekDay(values_config.day));
      proto_config->set_allocated_start_time(
          GetProtoDayTime(values_config.start_time).release());
      proto_config->set_allocated_end_time(
          GetProtoDayTime(values_config.end_time).release());
      proto_config->set_allocated_charge_start_time(
          GetProtoDayTime(values_config.charge_start_time).release());
    }
  }

  prefs_were_set_ = true;
  SendCurrentPolicy();
}

base::TimeDelta PowerPolicyController::GetMaxPolicyAutoScreenLockDelay() {
  if (!prefs_were_set_ || !auto_screen_lock_enabled_) {
    return base::TimeDelta();
  }
  int ac_delay = prefs_policy_.ac_delays().screen_lock_ms();
  int battery_delay = prefs_policy_.battery_delays().screen_lock_ms();
  return base::TimeDelta::FromMilliseconds(std::max(ac_delay, battery_delay));
}

int PowerPolicyController::AddScreenWakeLock(WakeLockReason reason,
                                             const std::string& description) {
  return AddWakeLockInternal(WakeLock::TYPE_SCREEN, reason, description);
}

int PowerPolicyController::AddDimWakeLock(WakeLockReason reason,
                                          const std::string& description) {
  return AddWakeLockInternal(WakeLock::TYPE_DIM, reason, description);
}

int PowerPolicyController::AddSystemWakeLock(WakeLockReason reason,
                                             const std::string& description) {
  return AddWakeLockInternal(WakeLock::TYPE_SYSTEM, reason, description);
}

void PowerPolicyController::RemoveWakeLock(int id) {
  if (!wake_locks_.erase(id))
    LOG(WARNING) << "Ignoring request to remove nonexistent wake lock " << id;
  else
    SendCurrentPolicy();
}

void PowerPolicyController::PowerManagerRestarted() {
  SendCurrentPolicy();
}

void PowerPolicyController::ScreenBrightnessChanged(
    const power_manager::BacklightBrightnessChange& change) {
  if (prefs_were_set_ &&
      (prefs_policy_.has_ac_brightness_percent() ||
       prefs_policy_.has_battery_brightness_percent()) &&
      change.cause() ==
          power_manager::BacklightBrightnessChange_Cause_USER_REQUEST) {
    per_session_brightness_override_ = true;
  }
}

void PowerPolicyController::NotifyChromeIsExiting() {
  if (chrome_is_exiting_)
    return;
  chrome_is_exiting_ = true;
  SendCurrentPolicy();
}

void PowerPolicyController::SetEncryptionMigrationActive(bool active) {
  if (encryption_migration_active_ == active)
    return;

  encryption_migration_active_ = active;
  SendCurrentPolicy();
}

PowerPolicyController::PowerPolicyController(PowerManagerClient* client)
    : client_(client) {
  DCHECK(client_);
  client_->AddObserver(this);
}

PowerPolicyController::~PowerPolicyController() {
  client_->RemoveObserver(this);
}

PowerPolicyController::WakeLock::WakeLock(Type type,
                                          WakeLockReason reason,
                                          const std::string& description)
    : type(type), reason(reason), description(description) {}

PowerPolicyController::WakeLock::~WakeLock() = default;

int PowerPolicyController::AddWakeLockInternal(WakeLock::Type type,
                                               WakeLockReason reason,
                                               const std::string& description) {
  const int id = next_wake_lock_id_++;
  wake_locks_.insert(std::make_pair(id, WakeLock(type, reason, description)));
  SendCurrentPolicy();
  return id;
}

void PowerPolicyController::SendCurrentPolicy() {
  std::string causes;

  power_manager::PowerManagementPolicy policy = prefs_policy_;
  if (prefs_were_set_)
    causes = kPrefsReason;

  if (honor_wake_locks_) {
    bool have_screen_wake_locks = false;
    bool have_dim_wake_locks = false;
    bool have_system_wake_locks = false;
    for (const auto& it : wake_locks_) {
      // Skip audio and video locks that should be ignored due to policy.
      if (!IsWakeLockReasonHonored(it.second.reason,
                                   policy.use_audio_activity(),
                                   policy.use_video_activity()))
        continue;

      switch (it.second.type) {
        case WakeLock::TYPE_SCREEN:
          have_screen_wake_locks = true;
          break;
        case WakeLock::TYPE_DIM:
          have_dim_wake_locks = true;
          break;
        case WakeLock::TYPE_SYSTEM:
          have_system_wake_locks = true;
          break;
      }
      causes += (causes.empty() ? "" : ", ") + it.second.description;
    }

    // Downgrade full-brightness and dimmed-brightness locks to system locks if
    // wake locks aren't allowed to keep the screen on.
    if (!honor_screen_wake_locks_ &&
        (have_screen_wake_locks || have_dim_wake_locks)) {
      have_system_wake_locks = true;
      have_screen_wake_locks = false;
      have_dim_wake_locks = false;
    }

    if (have_screen_wake_locks)
      policy.set_screen_wake_lock(true);
    if (have_dim_wake_locks)
      policy.set_dim_wake_lock(true);
    if (have_system_wake_locks)
      policy.set_system_wake_lock(true);
  }

  if (encryption_migration_active_ &&
      policy.lid_closed_action() !=
          power_manager::PowerManagementPolicy_Action_DO_NOTHING) {
    policy.set_lid_closed_action(
        power_manager::PowerManagementPolicy_Action_SUSPEND);
    causes +=
        std::string((causes.empty() ? "" : ", ")) + "encryption migration";
  }

  // To avoid a race in the case where the user asks Chrome to sign out
  // and then immediately closes the lid, override the lid-closed action
  // so the system will stay awake while Chrome is exiting. When Chrome
  // restarts to display the login screen, it will send an updated
  // policy that powerd can act on.
  if (chrome_is_exiting_ &&
      (!policy.has_lid_closed_action() ||
       policy.lid_closed_action() ==
           power_manager::PowerManagementPolicy_Action_SUSPEND ||
       policy.lid_closed_action() ==
           power_manager::PowerManagementPolicy_Action_SHUT_DOWN)) {
    policy.set_lid_closed_action(
        power_manager::PowerManagementPolicy_Action_DO_NOTHING);
  }

  if (!causes.empty())
    policy.set_reason(causes);
  client_->SetPolicy(policy);
}

}  // namespace chromeos
