// Copyright 2013 The Chromium Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

// libudev is used for monitoring device changes.

#include "media/device_monitors/device_monitor_udev.h"

#include <string>

#include "base/bind.h"
#include "base/sequence_checker.h"
#include "base/system/system_monitor.h"
#include "base/task/post_task.h"
#include "device/udev_linux/udev.h"
#include "device/udev_linux/udev_linux.h"

namespace {

struct SubsystemMap {
  base::SystemMonitor::DeviceType device_type;
  const char* subsystem;
  const char* devtype;
};

const char kAudioSubsystem[] = "sound";
const char kVideoSubsystem[] = "video4linux";

// Add more subsystems here for monitoring.
const SubsystemMap kSubsystemMap[] = {
    {base::SystemMonitor::DEVTYPE_AUDIO, kAudioSubsystem, NULL},
    {base::SystemMonitor::DEVTYPE_VIDEO_CAPTURE, kVideoSubsystem, NULL},
};

}  // namespace

namespace media {

// Wraps a device::UdevLinux with an API that makes it easier to use from
// DeviceMonitorLinux. Since it is essentially a wrapper around blocking udev
// calls, Initialize() must be called from a task runner that can block.
class DeviceMonitorLinux::BlockingTaskRunnerHelper {
 public:
  BlockingTaskRunnerHelper();
  ~BlockingTaskRunnerHelper() = default;

  void Initialize();

 private:
  void OnDevicesChanged(udev_device* device);

  std::unique_ptr<device::UdevLinux> udev_;

  SEQUENCE_CHECKER(sequence_checker_);

  DISALLOW_COPY_AND_ASSIGN(BlockingTaskRunnerHelper);
};

DeviceMonitorLinux::BlockingTaskRunnerHelper::BlockingTaskRunnerHelper() {
  // Detaches from the sequence on which this object was created. It will be
  // bound to its owning sequence when Initialize() is called.
  DETACH_FROM_SEQUENCE(sequence_checker_);
}

void DeviceMonitorLinux::BlockingTaskRunnerHelper::Initialize() {
  DCHECK_CALLED_ON_VALID_SEQUENCE(sequence_checker_);
  std::vector<device::UdevLinux::UdevMonitorFilter> filters;
  for (const SubsystemMap& entry : kSubsystemMap) {
    filters.push_back(
        device::UdevLinux::UdevMonitorFilter(entry.subsystem, entry.devtype));
  }
  udev_ = std::make_unique<device::UdevLinux>(
      filters, base::BindRepeating(&BlockingTaskRunnerHelper::OnDevicesChanged,
                                   base::Unretained(this)));
}

void DeviceMonitorLinux::BlockingTaskRunnerHelper::OnDevicesChanged(
    udev_device* device) {
  DCHECK_CALLED_ON_VALID_SEQUENCE(sequence_checker_);
  DCHECK(device);

  base::SystemMonitor::DeviceType device_type =
      base::SystemMonitor::DEVTYPE_UNKNOWN;
  const std::string subsystem(device::udev_device_get_subsystem(device));
  for (const SubsystemMap& entry : kSubsystemMap) {
    if (subsystem == entry.subsystem) {
      device_type = entry.device_type;
      break;
    }
  }
  DCHECK_NE(device_type, base::SystemMonitor::DEVTYPE_UNKNOWN);

  // base::SystemMonitor takes care of notifying each observer in their own task
  // runner via base::ObserverListThreadSafe.
  base::SystemMonitor::Get()->ProcessDevicesChanged(device_type);
}

DeviceMonitorLinux::DeviceMonitorLinux()
    : blocking_task_runner_(base::CreateSequencedTaskRunnerWithTraits(
          {base::MayBlock(), base::TaskPriority::USER_VISIBLE,
           base::TaskShutdownBehavior::CONTINUE_ON_SHUTDOWN})),
      blocking_task_helper_(new BlockingTaskRunnerHelper,
                            base::OnTaskRunnerDeleter(blocking_task_runner_)) {
  // Unretained() is safe because the deletion of |blocking_task_helper_|
  // is scheduled on |blocking_task_runner_| when DeviceMonitorLinux is
  // deleted.
  blocking_task_runner_->PostTask(
      FROM_HERE,
      base::BindOnce(&DeviceMonitorLinux::BlockingTaskRunnerHelper::Initialize,
                     base::Unretained(blocking_task_helper_.get())));
}

DeviceMonitorLinux::~DeviceMonitorLinux() = default;

}  // namespace media
