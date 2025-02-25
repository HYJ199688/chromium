// Copyright 2019 The Chromium Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include <memory>
#include <string>
#include <utility>

#include "base/macros.h"
#include "base/strings/string16.h"
#include "base/strings/utf_string_conversions.h"
#include "base/test/scoped_feature_list.h"
#include "chrome/browser/chromeos/crostini/crostini_manager.h"
#include "chrome/browser/chromeos/profiles/profile_helper.h"
#include "chrome/browser/chromeos/usb/cros_usb_detector.h"
#include "chrome/browser/notifications/notification_display_service.h"
#include "chrome/browser/notifications/notification_display_service_tester.h"
#include "chrome/browser/notifications/system_notification_helper.h"
#include "chrome/browser/profiles/profile_manager.h"
#include "chrome/test/base/browser_with_test_window_test.h"
#include "chrome/test/base/testing_browser_process.h"
#include "chrome/test/base/testing_profile_manager.h"
#include "chromeos/constants/chromeos_features.h"
#include "chromeos/dbus/dbus_thread_manager.h"
#include "chromeos/dbus/fake_cicerone_client.h"
#include "chromeos/dbus/fake_concierge_client.h"
#include "device/usb/public/cpp/fake_usb_device_info.h"
#include "device/usb/public/cpp/fake_usb_device_manager.h"
#include "device/usb/public/mojom/device.mojom.h"
#include "device/usb/public/mojom/device_manager.mojom.h"
#include "testing/gtest/include/gtest/gtest.h"
#include "ui/message_center/public/cpp/notification.h"
#include "url/gurl.h"

namespace {

const char* kProfileName = "test@example.com";

// USB device product name.
const char* kProductName_1 = "Google Product A";
const char* kProductName_2 = "Google Product B";
const char* kProductName_3 = "Google Product C";
const char* kUnknownProductName = "USB device";
const char* kManufacturerName = "Google";

const int kUsbConfigWithInterfaces = 1;

struct InterfaceCodes {
  InterfaceCodes(uint8_t device_class,
                 uint8_t subclass_code,
                 uint8_t protocol_code)
      : device_class(device_class),
        subclass_code(subclass_code),
        protocol_code(protocol_code) {}
  uint8_t device_class;
  uint8_t subclass_code;
  uint8_t protocol_code;
};

scoped_refptr<device::FakeUsbDeviceInfo> CreateTestDeviceFromCodes(
    uint8_t device_class,
    const std::vector<InterfaceCodes>& interface_codes) {
  auto config = device::mojom::UsbConfigurationInfo::New();
  config->configuration_value = kUsbConfigWithInterfaces;
  // The usb_utils do not filter by device class, only by configurations, and
  // the FakeUsbDeviceInfo does not set up configurations for a fake device's
  // class code. This helper sets up a configuration to match a devices class
  // code so that USB devices can be filtered out.
  for (size_t i = 0; i < interface_codes.size(); ++i) {
    auto alternate = device::mojom::UsbAlternateInterfaceInfo::New();
    alternate->alternate_setting = 0;
    alternate->class_code = interface_codes[i].device_class;
    alternate->subclass_code = interface_codes[i].subclass_code;
    alternate->protocol_code = interface_codes[i].protocol_code;

    auto interface = device::mojom::UsbInterfaceInfo::New();
    interface->interface_number = i;
    interface->alternates.push_back(std::move(alternate));

    config->interfaces.push_back(std::move(interface));
  }

  std::vector<device::mojom::UsbConfigurationInfoPtr> configs;
  configs.push_back(std::move(config));

  scoped_refptr<device::FakeUsbDeviceInfo> device =
      new device::FakeUsbDeviceInfo(/*vendor_id*/ 0, /*product_id*/ 1,
                                    device_class, std::move(configs));
  device->SetActiveConfig(kUsbConfigWithInterfaces);
  return device;
}

scoped_refptr<device::FakeUsbDeviceInfo> CreateTestDeviceOfClass(
    uint8_t device_class) {
  return CreateTestDeviceFromCodes(device_class,
                                   {InterfaceCodes(device_class, 0xff, 0xff)});
}

}  // namespace

class CrosUsbDetectorTest : public BrowserWithTestWindowTest {
 public:
  CrosUsbDetectorTest() {
    chromeos::DBusThreadManager::Initialize();
    fake_cicerone_client_ = static_cast<chromeos::FakeCiceroneClient*>(
        chromeos::DBusThreadManager::Get()->GetCiceroneClient());
    fake_concierge_client_ = static_cast<chromeos::FakeConciergeClient*>(
        chromeos::DBusThreadManager::Get()->GetConciergeClient());
    cros_usb_detector_ = std::make_unique<chromeos::CrosUsbDetector>();
  }

  ~CrosUsbDetectorTest() override { chromeos::DBusThreadManager::Shutdown(); }

  TestingProfile* CreateProfile() override {
    return profile_manager()->CreateTestingProfile(kProfileName);
  }

  void AttachUsbDeviceToVmSuccessCallback(base::OnceClosure closure,
                                          crostini::CrostiniResult result) {
    EXPECT_TRUE(fake_concierge_client_->attach_usb_device_called());
    std::move(closure).Run();
  }

  void DetachUsbDeviceToVmSuccessCallback(base::OnceClosure closure,
                                          crostini::CrostiniResult result) {
    EXPECT_TRUE(fake_concierge_client_->detach_usb_device_called());
    std::move(closure).Run();
  }

  void SetUp() override {
    BrowserWithTestWindowTest::SetUp();
    scoped_feature_list_.InitWithFeatures(
        {chromeos::features::kCrostiniUsbSupport,
         chromeos::features::kCrostiniUsbAllowUnsupported},
        {});
    profile_manager()->SetLoggedIn(true);
    chromeos::ProfileHelper::Get()->SetActiveUserIdForTesting(kProfileName);
    TestingBrowserProcess::GetGlobal()->SetSystemNotificationHelper(
        std::make_unique<SystemNotificationHelper>());
    display_service_ = std::make_unique<NotificationDisplayServiceTester>(
        nullptr /* profile */);

    // Set a fake USB device manager before ConnectToDeviceManager().
    device::mojom::UsbDeviceManagerPtr device_manager_ptr;
    device_manager_.AddBinding(mojo::MakeRequest(&device_manager_ptr));
    chromeos::CrosUsbDetector::Get()->SetDeviceManagerForTesting(
        std::move(device_manager_ptr));
  }

  void TearDown() override {
    BrowserWithTestWindowTest::TearDown();
  }

  void ConnectToDeviceManager() {
    chromeos::CrosUsbDetector::Get()->ConnectToDeviceManager();
  }

 protected:
  base::string16 connection_message(const char* product_name) {
    return base::ASCIIToUTF16(base::StringPrintf(
        "Open Settings to connect %s to Linux", product_name));
  }

  base::string16 expected_title() {
    return base::ASCIIToUTF16("USB device detected");
  }

  device::FakeUsbDeviceManager device_manager_;
  std::unique_ptr<NotificationDisplayServiceTester> display_service_;

  // Owned by chromeos::DBusThreadManager
  chromeos::FakeCiceroneClient* fake_cicerone_client_;
  chromeos::FakeConciergeClient* fake_concierge_client_;

  std::unique_ptr<chromeos::CrosUsbDetector> cros_usb_detector_;

  base::test::ScopedFeatureList scoped_feature_list_;

 private:
  DISALLOW_COPY_AND_ASSIGN(CrosUsbDetectorTest);
};

TEST_F(CrosUsbDetectorTest, UsbDeviceAddedAndRemoved) {
  ConnectToDeviceManager();
  base::RunLoop().RunUntilIdle();

  auto device = base::MakeRefCounted<device::FakeUsbDeviceInfo>(
      0, 1, kManufacturerName, kProductName_1, "002");
  device_manager_.AddDevice(device);
  base::RunLoop().RunUntilIdle();

  std::string notification_id =
      chromeos::CrosUsbDetector::MakeNotificationId(device->guid());

  base::Optional<message_center::Notification> notification =
      display_service_->GetNotification(notification_id);
  ASSERT_TRUE(notification);

  EXPECT_EQ(expected_title(), notification->title());
  EXPECT_EQ(connection_message(kProductName_1), notification->message());
  EXPECT_TRUE(notification->delegate());

  device_manager_.RemoveDevice(device);
  base::RunLoop().RunUntilIdle();
  // Device is removed, so notification should be removed too.
  EXPECT_FALSE(display_service_->GetNotification(notification_id));
}

TEST_F(CrosUsbDetectorTest, UsbDeviceClassBlockedAdded) {
  ConnectToDeviceManager();
  base::RunLoop().RunUntilIdle();

  scoped_refptr<device::FakeUsbDeviceInfo> device =
      CreateTestDeviceOfClass(/* USB_CLASS_HID */ 0x03);

  device_manager_.AddDevice(device);
  base::RunLoop().RunUntilIdle();

  std::string notification_id =
      chromeos::CrosUsbDetector::MakeNotificationId(device->guid());
  ASSERT_FALSE(display_service_->GetNotification(notification_id));

  // TODO(jopra): Check that the device is not available for sharing.
}

TEST_F(CrosUsbDetectorTest, UsbDeviceClassAdbAdded) {
  ConnectToDeviceManager();
  base::RunLoop().RunUntilIdle();

  const int kAdbClass = 0xff;
  const int kAdbSubclass = 0x42;
  const int kAdbProtocol = 0x1;
  // Adb interface as well as a forbidden interface
  scoped_refptr<device::FakeUsbDeviceInfo> device = CreateTestDeviceFromCodes(
      /* USB_CLASS_HID */ 0x03,
      {InterfaceCodes(kAdbClass, kAdbSubclass, kAdbProtocol),
       InterfaceCodes(0x03, 0xff, 0xff)});

  device_manager_.AddDevice(device);
  base::RunLoop().RunUntilIdle();

  std::string notification_id =
      chromeos::CrosUsbDetector::MakeNotificationId(device->guid());
  ASSERT_TRUE(display_service_->GetNotification(notification_id));
}

TEST_F(CrosUsbDetectorTest, UsbDeviceClassWithoutNotificationAdded) {
  ConnectToDeviceManager();
  base::RunLoop().RunUntilIdle();

  scoped_refptr<device::FakeUsbDeviceInfo> device =
      CreateTestDeviceOfClass(/* USB_CLASS_AUDIO */ 0x01);

  device_manager_.AddDevice(device);
  base::RunLoop().RunUntilIdle();

  std::string notification_id =
      chromeos::CrosUsbDetector::MakeNotificationId(device->guid());
  ASSERT_FALSE(display_service_->GetNotification(notification_id));

  // TODO(jopra): Check that the device is available for sharing.
}

TEST_F(CrosUsbDetectorTest, UsbDeviceWithoutProductNameAddedAndRemoved) {
  std::string product_name = "";
  ConnectToDeviceManager();
  base::RunLoop().RunUntilIdle();

  auto device = base::MakeRefCounted<device::FakeUsbDeviceInfo>(
      0, 1, kManufacturerName, product_name, "002");
  device_manager_.AddDevice(device);
  base::RunLoop().RunUntilIdle();

  std::string notification_id =
      chromeos::CrosUsbDetector::MakeNotificationId(device->guid());

  base::Optional<message_center::Notification> notification =
      display_service_->GetNotification(notification_id);
  ASSERT_TRUE(notification);

  EXPECT_EQ(expected_title(), notification->title());
  EXPECT_EQ(connection_message("USB device from Google"),
            notification->message());
  EXPECT_TRUE(notification->delegate());

  device_manager_.RemoveDevice(device);
  base::RunLoop().RunUntilIdle();
  // Device is removed, so notification should be removed too.
  EXPECT_FALSE(display_service_->GetNotification(notification_id));
}

TEST_F(CrosUsbDetectorTest,
       UsbDeviceWithoutProductNameOrManufacturerNameAddedAndRemoved) {
  std::string product_name = "";
  std::string manufacturer_name = "";
  ConnectToDeviceManager();
  base::RunLoop().RunUntilIdle();

  auto device = base::MakeRefCounted<device::FakeUsbDeviceInfo>(
      0, 1, manufacturer_name, product_name, "002");
  device_manager_.AddDevice(device);
  base::RunLoop().RunUntilIdle();

  std::string notification_id =
      chromeos::CrosUsbDetector::MakeNotificationId(device->guid());

  base::Optional<message_center::Notification> notification =
      display_service_->GetNotification(notification_id);
  ASSERT_TRUE(notification);
  EXPECT_EQ(expected_title(), notification->title());
  EXPECT_EQ(connection_message(kUnknownProductName), notification->message());
  EXPECT_TRUE(notification->delegate());

  device_manager_.RemoveDevice(device);
  base::RunLoop().RunUntilIdle();
  // Device is removed, so notification should be removed too.
  EXPECT_FALSE(display_service_->GetNotification(notification_id));
}

TEST_F(CrosUsbDetectorTest, UsbDeviceWasThereBeforeAndThenRemoved) {
  // USB device was added before cros_usb_detector was created.
  auto device = base::MakeRefCounted<device::FakeUsbDeviceInfo>(
      0, 1, kManufacturerName, kProductName_1, "002");
  device_manager_.AddDevice(device);
  base::RunLoop().RunUntilIdle();

  std::string notification_id =
      chromeos::CrosUsbDetector::MakeNotificationId(device->guid());

  EXPECT_FALSE(display_service_->GetNotification(notification_id));

  ConnectToDeviceManager();
  base::RunLoop().RunUntilIdle();

  device_manager_.RemoveDevice(device);
  base::RunLoop().RunUntilIdle();
  EXPECT_FALSE(display_service_->GetNotification(notification_id));
}

TEST_F(
    CrosUsbDetectorTest,
    ThreeUsbDevicesWereThereBeforeAndThenRemovedBeforeUsbDetectorWasCreated) {
  auto device_1 = base::MakeRefCounted<device::FakeUsbDeviceInfo>(
      0, 1, kManufacturerName, kProductName_1, "002");
  std::string notification_id_1 =
      chromeos::CrosUsbDetector::MakeNotificationId(device_1->guid());

  auto device_2 = base::MakeRefCounted<device::FakeUsbDeviceInfo>(
      3, 4, kManufacturerName, kProductName_2, "005");
  std::string notification_id_2 =
      chromeos::CrosUsbDetector::MakeNotificationId(device_2->guid());

  auto device_3 = base::MakeRefCounted<device::FakeUsbDeviceInfo>(
      6, 7, kManufacturerName, kProductName_3, "008");
  std::string notification_id_3 =
      chromeos::CrosUsbDetector::MakeNotificationId(device_3->guid());

  // Three usb devices were added and removed before cros_usb_detector was
  // created.
  device_manager_.AddDevice(device_1);
  base::RunLoop().RunUntilIdle();
  EXPECT_FALSE(display_service_->GetNotification(notification_id_1));

  device_manager_.AddDevice(device_2);
  base::RunLoop().RunUntilIdle();
  EXPECT_FALSE(display_service_->GetNotification(notification_id_2));

  device_manager_.AddDevice(device_3);
  base::RunLoop().RunUntilIdle();
  EXPECT_FALSE(display_service_->GetNotification(notification_id_3));

  device_manager_.RemoveDevice(device_1);
  base::RunLoop().RunUntilIdle();
  EXPECT_FALSE(display_service_->GetNotification(notification_id_1));

  device_manager_.RemoveDevice(device_2);
  base::RunLoop().RunUntilIdle();
  EXPECT_FALSE(display_service_->GetNotification(notification_id_2));

  device_manager_.RemoveDevice(device_3);
  base::RunLoop().RunUntilIdle();
  EXPECT_FALSE(display_service_->GetNotification(notification_id_3));

  ConnectToDeviceManager();
  base::RunLoop().RunUntilIdle();

  EXPECT_FALSE(display_service_->GetNotification(notification_id_1));
  EXPECT_FALSE(display_service_->GetNotification(notification_id_2));
  EXPECT_FALSE(display_service_->GetNotification(notification_id_3));
}

TEST_F(CrosUsbDetectorTest,
       ThreeUsbDevicesWereThereBeforeAndThenRemovedAfterUsbDetectorWasCreated) {
  auto device_1 = base::MakeRefCounted<device::FakeUsbDeviceInfo>(
      0, 1, kManufacturerName, kProductName_1, "002");
  std::string notification_id_1 =
      chromeos::CrosUsbDetector::MakeNotificationId(device_1->guid());

  auto device_2 = base::MakeRefCounted<device::FakeUsbDeviceInfo>(
      3, 4, kManufacturerName, kProductName_2, "005");
  std::string notification_id_2 =
      chromeos::CrosUsbDetector::MakeNotificationId(device_2->guid());

  auto device_3 = base::MakeRefCounted<device::FakeUsbDeviceInfo>(
      6, 7, kManufacturerName, kProductName_3, "008");
  std::string notification_id_3 =
      chromeos::CrosUsbDetector::MakeNotificationId(device_3->guid());

  // Three usb devices were added before cros_usb_detector was created.
  device_manager_.AddDevice(device_1);
  base::RunLoop().RunUntilIdle();
  EXPECT_FALSE(display_service_->GetNotification(notification_id_1));

  device_manager_.AddDevice(device_2);
  base::RunLoop().RunUntilIdle();
  EXPECT_FALSE(display_service_->GetNotification(notification_id_2));

  device_manager_.AddDevice(device_3);
  base::RunLoop().RunUntilIdle();
  EXPECT_FALSE(display_service_->GetNotification(notification_id_3));

  ConnectToDeviceManager();
  base::RunLoop().RunUntilIdle();

  EXPECT_FALSE(display_service_->GetNotification(notification_id_1));
  EXPECT_FALSE(display_service_->GetNotification(notification_id_2));
  EXPECT_FALSE(display_service_->GetNotification(notification_id_3));

  device_manager_.RemoveDevice(device_1);
  base::RunLoop().RunUntilIdle();
  EXPECT_FALSE(display_service_->GetNotification(notification_id_1));

  device_manager_.RemoveDevice(device_2);
  base::RunLoop().RunUntilIdle();
  EXPECT_FALSE(display_service_->GetNotification(notification_id_2));

  device_manager_.RemoveDevice(device_3);
  base::RunLoop().RunUntilIdle();
  EXPECT_FALSE(display_service_->GetNotification(notification_id_3));
}

TEST_F(CrosUsbDetectorTest,
       TwoUsbDevicesWereThereBeforeAndThenRemovedAndNewUsbDeviceAdded) {
  auto device_1 = base::MakeRefCounted<device::FakeUsbDeviceInfo>(
      0, 1, kManufacturerName, kProductName_1, "002");
  std::string notification_id_1 =
      chromeos::CrosUsbDetector::MakeNotificationId(device_1->guid());

  auto device_2 = base::MakeRefCounted<device::FakeUsbDeviceInfo>(
      3, 4, kManufacturerName, kProductName_2, "005");
  std::string notification_id_2 =
      chromeos::CrosUsbDetector::MakeNotificationId(device_2->guid());

  // Two usb devices were added before cros_usb_detector was created.
  device_manager_.AddDevice(device_1);
  device_manager_.AddDevice(device_2);
  base::RunLoop().RunUntilIdle();
  EXPECT_FALSE(display_service_->GetNotification(notification_id_1));
  EXPECT_FALSE(display_service_->GetNotification(notification_id_2));

  ConnectToDeviceManager();
  base::RunLoop().RunUntilIdle();
  EXPECT_FALSE(display_service_->GetNotification(notification_id_1));
  EXPECT_FALSE(display_service_->GetNotification(notification_id_2));

  device_manager_.RemoveDevice(device_1);
  device_manager_.RemoveDevice(device_2);
  base::RunLoop().RunUntilIdle();

  EXPECT_FALSE(display_service_->GetNotification(notification_id_1));
  EXPECT_FALSE(display_service_->GetNotification(notification_id_2));

  device_manager_.AddDevice(device_2);
  base::RunLoop().RunUntilIdle();
  base::Optional<message_center::Notification> notification =
      display_service_->GetNotification(notification_id_2);
  ASSERT_TRUE(notification);

  EXPECT_EQ(expected_title(), notification->title());
  EXPECT_EQ(connection_message(kProductName_2), notification->message());
  EXPECT_TRUE(notification->delegate());

  device_manager_.RemoveDevice(device_2);
  base::RunLoop().RunUntilIdle();
  EXPECT_FALSE(display_service_->GetNotification(notification_id_2));
}

TEST_F(CrosUsbDetectorTest, ThreeUsbDevicesAddedAndRemoved) {
  auto device_1 = base::MakeRefCounted<device::FakeUsbDeviceInfo>(
      0, 1, kManufacturerName, kProductName_1, "002");
  std::string notification_id_1 =
      chromeos::CrosUsbDetector::MakeNotificationId(device_1->guid());

  auto device_2 = base::MakeRefCounted<device::FakeUsbDeviceInfo>(
      3, 4, kManufacturerName, kProductName_2, "005");
  std::string notification_id_2 =
      chromeos::CrosUsbDetector::MakeNotificationId(device_2->guid());

  auto device_3 = base::MakeRefCounted<device::FakeUsbDeviceInfo>(
      6, 7, kManufacturerName, kProductName_3, "008");
  std::string notification_id_3 =
      chromeos::CrosUsbDetector::MakeNotificationId(device_3->guid());

  ConnectToDeviceManager();
  base::RunLoop().RunUntilIdle();

  device_manager_.AddDevice(device_1);
  base::RunLoop().RunUntilIdle();
  base::Optional<message_center::Notification> notification_1 =
      display_service_->GetNotification(notification_id_1);
  ASSERT_TRUE(notification_1);

  EXPECT_EQ(expected_title(), notification_1->title());
  EXPECT_EQ(connection_message(kProductName_1), notification_1->message());
  EXPECT_TRUE(notification_1->delegate());

  device_manager_.RemoveDevice(device_1);
  base::RunLoop().RunUntilIdle();
  EXPECT_FALSE(display_service_->GetNotification(notification_id_1));

  device_manager_.AddDevice(device_2);
  base::RunLoop().RunUntilIdle();
  base::Optional<message_center::Notification> notification_2 =
      display_service_->GetNotification(notification_id_2);
  ASSERT_TRUE(notification_2);

  EXPECT_EQ(expected_title(), notification_2->title());
  EXPECT_EQ(connection_message(kProductName_2), notification_2->message());
  EXPECT_TRUE(notification_2->delegate());

  device_manager_.RemoveDevice(device_2);
  base::RunLoop().RunUntilIdle();
  EXPECT_FALSE(display_service_->GetNotification(notification_id_2));

  device_manager_.AddDevice(device_3);
  base::RunLoop().RunUntilIdle();
  base::Optional<message_center::Notification> notification_3 =
      display_service_->GetNotification(notification_id_3);
  ASSERT_TRUE(notification_3);

  EXPECT_EQ(expected_title(), notification_3->title());
  EXPECT_EQ(connection_message(kProductName_3), notification_3->message());
  EXPECT_TRUE(notification_3->delegate());

  device_manager_.RemoveDevice(device_3);
  base::RunLoop().RunUntilIdle();
  EXPECT_FALSE(display_service_->GetNotification(notification_id_3));
}

TEST_F(CrosUsbDetectorTest, ThreeUsbDeviceAddedAndRemovedDifferentOrder) {
  auto device_1 = base::MakeRefCounted<device::FakeUsbDeviceInfo>(
      0, 1, kManufacturerName, kProductName_1, "002");
  std::string notification_id_1 =
      chromeos::CrosUsbDetector::MakeNotificationId(device_1->guid());

  auto device_2 = base::MakeRefCounted<device::FakeUsbDeviceInfo>(
      3, 4, kManufacturerName, kProductName_2, "005");
  std::string notification_id_2 =
      chromeos::CrosUsbDetector::MakeNotificationId(device_2->guid());

  auto device_3 = base::MakeRefCounted<device::FakeUsbDeviceInfo>(
      6, 7, kManufacturerName, kProductName_3, "008");
  std::string notification_id_3 =
      chromeos::CrosUsbDetector::MakeNotificationId(device_3->guid());

  ConnectToDeviceManager();
  base::RunLoop().RunUntilIdle();

  device_manager_.AddDevice(device_1);
  base::RunLoop().RunUntilIdle();
  base::Optional<message_center::Notification> notification_1 =
      display_service_->GetNotification(notification_id_1);
  ASSERT_TRUE(notification_1);

  EXPECT_EQ(expected_title(), notification_1->title());
  EXPECT_EQ(connection_message(kProductName_1), notification_1->message());
  EXPECT_TRUE(notification_1->delegate());

  device_manager_.AddDevice(device_2);
  base::RunLoop().RunUntilIdle();
  base::Optional<message_center::Notification> notification_2 =
      display_service_->GetNotification(notification_id_2);
  ASSERT_TRUE(notification_2);

  EXPECT_EQ(expected_title(), notification_2->title());
  EXPECT_EQ(connection_message(kProductName_2), notification_2->message());
  EXPECT_TRUE(notification_2->delegate());

  device_manager_.RemoveDevice(device_2);
  base::RunLoop().RunUntilIdle();
  EXPECT_FALSE(display_service_->GetNotification(notification_id_2));

  device_manager_.AddDevice(device_3);
  base::RunLoop().RunUntilIdle();
  base::Optional<message_center::Notification> notification_3 =
      display_service_->GetNotification(notification_id_3);
  ASSERT_TRUE(notification_3);

  EXPECT_EQ(expected_title(), notification_3->title());
  EXPECT_EQ(connection_message(kProductName_3), notification_3->message());
  EXPECT_TRUE(notification_3->delegate());

  device_manager_.RemoveDevice(device_1);
  base::RunLoop().RunUntilIdle();
  EXPECT_FALSE(display_service_->GetNotification(notification_id_1));

  device_manager_.RemoveDevice(device_3);
  base::RunLoop().RunUntilIdle();
  EXPECT_FALSE(display_service_->GetNotification(notification_id_3));
}
