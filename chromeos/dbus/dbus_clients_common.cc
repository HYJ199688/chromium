// Copyright 2014 The Chromium Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include "chromeos/dbus/dbus_clients_common.h"

#include "base/command_line.h"
#include "chromeos/dbus/constants/dbus_switches.h"
#include "chromeos/dbus/cras_audio_client.h"
#include "chromeos/dbus/dbus_client_implementation_type.h"
#include "chromeos/dbus/dbus_thread_manager.h"
#include "chromeos/dbus/fake_cras_audio_client.h"
#include "chromeos/dbus/fake_gsm_sms_client.h"
#include "chromeos/dbus/fake_modem_messaging_client.h"
#include "chromeos/dbus/fake_shill_device_client.h"
#include "chromeos/dbus/fake_shill_ipconfig_client.h"
#include "chromeos/dbus/fake_shill_manager_client.h"
#include "chromeos/dbus/fake_shill_profile_client.h"
#include "chromeos/dbus/fake_shill_service_client.h"
#include "chromeos/dbus/fake_shill_third_party_vpn_driver_client.h"
#include "chromeos/dbus/fake_sms_client.h"
#include "chromeos/dbus/gsm_sms_client.h"
#include "chromeos/dbus/modem_messaging_client.h"
#include "chromeos/dbus/session_manager/session_manager_client.h"
#include "chromeos/dbus/shill_device_client.h"
#include "chromeos/dbus/shill_ipconfig_client.h"
#include "chromeos/dbus/shill_manager_client.h"
#include "chromeos/dbus/shill_profile_client.h"
#include "chromeos/dbus/shill_service_client.h"
#include "chromeos/dbus/shill_third_party_vpn_driver_client.h"
#include "chromeos/dbus/sms_client.h"

namespace chromeos {

DBusClientsCommon::DBusClientsCommon(bool use_real_clients) {
  const DBusClientImplementationType client_impl_type =
      use_real_clients ? REAL_DBUS_CLIENT_IMPLEMENTATION
                       : FAKE_DBUS_CLIENT_IMPLEMENTATION;

  if (use_real_clients)
    cras_audio_client_.reset(CrasAudioClient::Create());
  else
    cras_audio_client_.reset(new FakeCrasAudioClient);

  if (use_real_clients) {
    shill_manager_client_.reset(ShillManagerClient::Create());
    shill_device_client_.reset(ShillDeviceClient::Create());
    shill_ipconfig_client_.reset(ShillIPConfigClient::Create());
    shill_service_client_.reset(ShillServiceClient::Create());
    shill_profile_client_.reset(ShillProfileClient::Create());
    shill_third_party_vpn_driver_client_.reset(
        ShillThirdPartyVpnDriverClient::Create());
  } else {
    shill_manager_client_.reset(new FakeShillManagerClient);
    shill_device_client_.reset(new FakeShillDeviceClient);
    shill_ipconfig_client_.reset(new FakeShillIPConfigClient);
    shill_service_client_.reset(new FakeShillServiceClient);
    shill_profile_client_.reset(new FakeShillProfileClient);
    shill_third_party_vpn_driver_client_.reset(
        new FakeShillThirdPartyVpnDriverClient);
  }

  if (use_real_clients) {
    gsm_sms_client_.reset(GsmSMSClient::Create());
  } else {
    FakeGsmSMSClient* gsm_sms_client = new FakeGsmSMSClient();
    gsm_sms_client->set_sms_test_message_switch_present(
        base::CommandLine::ForCurrentProcess()->HasSwitch(
            chromeos::switches::kSmsTestMessages));
    gsm_sms_client_.reset(gsm_sms_client);
  }

  if (use_real_clients)
    modem_messaging_client_.reset(ModemMessagingClient::Create());
  else
    modem_messaging_client_.reset(new FakeModemMessagingClient);

  session_manager_client_.reset(SessionManagerClient::Create(client_impl_type));

  if (use_real_clients)
    sms_client_.reset(SMSClient::Create());
  else
    sms_client_.reset(new FakeSMSClient);
}

DBusClientsCommon::~DBusClientsCommon() = default;

void DBusClientsCommon::Initialize(dbus::Bus* system_bus) {
  DCHECK(DBusThreadManager::IsInitialized());

  cras_audio_client_->Init(system_bus);
  gsm_sms_client_->Init(system_bus);
  modem_messaging_client_->Init(system_bus);
  session_manager_client_->Init(system_bus);
  shill_device_client_->Init(system_bus);
  shill_ipconfig_client_->Init(system_bus);
  shill_manager_client_->Init(system_bus);
  shill_service_client_->Init(system_bus);
  shill_profile_client_->Init(system_bus);
  shill_third_party_vpn_driver_client_->Init(system_bus);
  sms_client_->Init(system_bus);

  ShillManagerClient::TestInterface* manager =
      shill_manager_client_->GetTestInterface();
  if (manager)
    manager->SetupDefaultEnvironment();
}

}  // namespace chromeos
