// Copyright 2016 The Chromium Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#ifndef CHROMEOS_DBUS_UPSTART_FAKE_UPSTART_CLIENT_H_
#define CHROMEOS_DBUS_UPSTART_FAKE_UPSTART_CLIENT_H_

#include "base/macros.h"

#include "chromeos/dbus/upstart/upstart_client.h"

namespace chromeos {

class COMPONENT_EXPORT(CHROMEOS_DBUS) FakeUpstartClient : public UpstartClient {
 public:
  FakeUpstartClient();
  ~FakeUpstartClient() override;

  // Returns the fake global instance if initialized. May return null.
  static FakeUpstartClient* Get();

  // UpstartClient overrides:
  void StartJob(const std::string& job,
                const std::vector<std::string>& upstart_env,
                VoidDBusMethodCallback callback) override;
  void StopJob(const std::string& job,
               VoidDBusMethodCallback callback) override;
  void StartAuthPolicyService() override;
  void RestartAuthPolicyService() override;
  void StartKerberosService(VoidDBusMethodCallback callback) override;
  void StartMediaAnalytics(const std::vector<std::string>& upstart_env,
                           VoidDBusMethodCallback callback) override;
  void RestartMediaAnalytics(VoidDBusMethodCallback callback) override;
  void StopMediaAnalytics() override;
  void StopMediaAnalytics(VoidDBusMethodCallback callback) override;
  void StartWilcoDtcService(VoidDBusMethodCallback callback) override;
  void StopWilcoDtcService(VoidDBusMethodCallback callback) override;

 private:
  DISALLOW_COPY_AND_ASSIGN(FakeUpstartClient);
};

}  // namespace chromeos

#endif  // CHROMEOS_DBUS_UPSTART_FAKE_UPSTART_CLIENT_H_
