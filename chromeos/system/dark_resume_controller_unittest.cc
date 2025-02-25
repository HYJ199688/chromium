// Copyright 2018 The Chromium Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include "chromeos/system/dark_resume_controller.h"

#include <memory>
#include <utility>

#include "base/run_loop.h"
#include "base/test/scoped_task_environment.h"
#include "chromeos/dbus/power/fake_power_manager_client.h"
#include "services/device/public/cpp/test/test_wake_lock_provider.h"
#include "services/device/public/mojom/constants.mojom.h"
#include "services/service_manager/public/cpp/test/test_connector_factory.h"
#include "testing/gtest/include/gtest/gtest.h"

namespace chromeos {
namespace system {

namespace {

constexpr char kWakeLockDescription[] = "DarkResumeTest";

}

using device::mojom::WakeLockType;

class DarkResumeControllerTest : public testing::Test {
 public:
  DarkResumeControllerTest()
      : scoped_task_environment_(
            base::test::ScopedTaskEnvironment::MainThreadType::MOCK_TIME),
        wake_lock_provider_(
            connector_factory_.RegisterInstance(device::mojom::kServiceName)) {}

  ~DarkResumeControllerTest() override = default;

  void SetUp() override {
    // Create wake lock that will be acquired and released in tests.
    wake_lock_provider_.GetWakeLockWithoutContext(
        WakeLockType::kPreventAppSuspension,
        device::mojom::WakeLockReason::kOther, kWakeLockDescription,
        mojo::MakeRequest(&wake_lock_));

    PowerManagerClient::InitializeFake();

    dark_resume_controller_ = std::make_unique<DarkResumeController>(
        connector_factory_.GetDefaultConnector());
  }

  void TearDown() override {
    dark_resume_controller_.reset();
    PowerManagerClient::Shutdown();
  }

 protected:
  // Returns the number of active wake locks of type |type|.
  int32_t GetActiveWakeLocks(WakeLockType type) {
    base::RunLoop run_loop;
    int32_t result_count = -1;
    wake_lock_provider_.GetActiveWakeLocksForTests(
        type,
        base::BindOnce(
            [](base::RunLoop* run_loop, int32_t* result_count, int32_t count) {
              *result_count = count;
              run_loop->Quit();
            },
            &run_loop, &result_count));
    run_loop.Run();
    return result_count;
  }

  FakePowerManagerClient* fake_power_manager_client() {
    return FakePowerManagerClient::Get();
  }

  base::test::ScopedTaskEnvironment scoped_task_environment_;
  service_manager::TestConnectorFactory connector_factory_;
  device::mojom::WakeLockPtr wake_lock_;
  std::unique_ptr<DarkResumeController> dark_resume_controller_;

 private:
  device::TestWakeLockProvider wake_lock_provider_;

  DISALLOW_COPY_AND_ASSIGN(DarkResumeControllerTest);
};

TEST_F(DarkResumeControllerTest, CheckSuspendAfterDarkResumeNoWakeLocksHeld) {
  // Trigger a dark resume event, move time forward to trigger a wake lock check
  // and check if a re-suspend happened if no wake locks were acquired.
  fake_power_manager_client()->SendDarkSuspendImminent();
  scoped_task_environment_.FastForwardBy(
      DarkResumeController::kDarkResumeWakeLockCheckTimeout);
  base::RunLoop run_loop;
  run_loop.RunUntilIdle();
  EXPECT_TRUE(dark_resume_controller_->IsDarkResumeStateClearedForTesting());
  EXPECT_EQ(
      0,
      fake_power_manager_client()->num_pending_suspend_readiness_callbacks());

  // Trigger a dark resume event, acquire and release a wake lock and move time
  // forward to trigger a wake lock check. The device should re-suspend in this
  // case since no wake locks were held at the time of the wake lock check.
  fake_power_manager_client()->SendDarkSuspendImminent();
  wake_lock_->RequestWakeLock();
  wake_lock_->CancelWakeLock();
  scoped_task_environment_.FastForwardBy(
      DarkResumeController::kDarkResumeWakeLockCheckTimeout);
  base::RunLoop run_loop2;
  run_loop2.RunUntilIdle();
  EXPECT_TRUE(dark_resume_controller_->IsDarkResumeStateClearedForTesting());
  EXPECT_EQ(
      0,
      fake_power_manager_client()->num_pending_suspend_readiness_callbacks());
}

TEST_F(DarkResumeControllerTest, CheckSuspendAfterDarkResumeWakeLocksHeld) {
  // Trigger a dark resume event, acquire a wake lock and move time forward to a
  // wake lock check. At this point the system shouldn't re-suspend i.e. the
  // suspend readiness callback should be set and wake lock release should have
  // observers.
  fake_power_manager_client()->SendDarkSuspendImminent();
  wake_lock_->RequestWakeLock();
  scoped_task_environment_.FastForwardBy(
      DarkResumeController::kDarkResumeWakeLockCheckTimeout);
  base::RunLoop run_loop;
  run_loop.RunUntilIdle();
  EXPECT_TRUE(dark_resume_controller_->IsDarkResumeStateSetForTesting());

  // Move time forward by < |kDarkResumeHardTimeout| and release the
  // partial wake lock. This should instantaneously re-suspend the device.
  scoped_task_environment_.FastForwardBy(
      DarkResumeController::kDarkResumeHardTimeout -
      base::TimeDelta::FromSeconds(1));
  wake_lock_->CancelWakeLock();
  base::RunLoop run_loop2;
  run_loop2.RunUntilIdle();
  EXPECT_TRUE(dark_resume_controller_->IsDarkResumeStateClearedForTesting());
  EXPECT_EQ(
      0,
      fake_power_manager_client()->num_pending_suspend_readiness_callbacks());
}

TEST_F(DarkResumeControllerTest, CheckSuspendAfterDarkResumeHardTimeout) {
  // Trigger a dark resume event, acquire a wake lock and move time forward to a
  // wake lock check. At this point the system shouldn't re-suspend i.e. the
  // suspend readiness callback should be set and wake lock release should have
  // observers.
  fake_power_manager_client()->SendDarkSuspendImminent();
  wake_lock_->RequestWakeLock();
  scoped_task_environment_.FastForwardBy(
      DarkResumeController::kDarkResumeWakeLockCheckTimeout);
  base::RunLoop run_loop;
  run_loop.RunUntilIdle();
  EXPECT_TRUE(dark_resume_controller_->IsDarkResumeStateSetForTesting());

  // Move time forward by |kDarkResumeHardTimeout|. At this point the
  // device should re-suspend even though the wake lock is acquired.
  scoped_task_environment_.FastForwardBy(
      DarkResumeController::kDarkResumeHardTimeout);
  EXPECT_EQ(1, GetActiveWakeLocks(WakeLockType::kPreventAppSuspension));
  base::RunLoop run_loop2;
  run_loop2.RunUntilIdle();
  EXPECT_TRUE(dark_resume_controller_->IsDarkResumeStateClearedForTesting());
  EXPECT_EQ(
      0,
      fake_power_manager_client()->num_pending_suspend_readiness_callbacks());
}

TEST_F(DarkResumeControllerTest, CheckStateResetAfterSuspendDone) {
  // Trigger a dark resume event, acquire a wake lock and move time forward to a
  // wake lock check. At this point the system shouldn't re-suspend i.e. the
  // suspend readiness callback should be set and wake lock release should have
  // observers.
  fake_power_manager_client()->SendDarkSuspendImminent();
  wake_lock_->RequestWakeLock();
  scoped_task_environment_.FastForwardBy(
      DarkResumeController::kDarkResumeWakeLockCheckTimeout);
  base::RunLoop run_loop;
  run_loop.RunUntilIdle();
  EXPECT_TRUE(dark_resume_controller_->IsDarkResumeStateSetForTesting());

  // Trigger suspend done event. Check if state is reset as dark resume would be
  // exited.
  fake_power_manager_client()->SendSuspendDone();
  EXPECT_TRUE(dark_resume_controller_->IsDarkResumeStateClearedForTesting());
}

}  // namespace system
}  // namespace chromeos
