// Copyright (c) 2015 The Chromium Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include "base/base64.h"
#include "base/bind.h"
#include "base/json/json_reader.h"
#include "base/json/string_escape.h"
#include "base/stl_util.h"
#include "build/build_config.h"
#include "chrome/browser/browser_process.h"
#include "chrome/browser/chromeos/login/enrollment/enrollment_screen.h"
#include "chrome/browser/chromeos/login/enrollment/enterprise_enrollment_helper.h"
#include "chrome/browser/chromeos/login/enrollment/enterprise_enrollment_helper_impl.h"
#include "chrome/browser/chromeos/login/enrollment/enterprise_enrollment_helper_mock.h"
#include "chrome/browser/chromeos/login/login_manager_test.h"
#include "chrome/browser/chromeos/login/startup_utils.h"
#include "chrome/browser/chromeos/login/test/enrollment_helper_mixin.h"
#include "chrome/browser/chromeos/login/test/js_checker.h"
#include "chrome/browser/chromeos/login/test/oobe_screen_waiter.h"
#include "chrome/browser/chromeos/login/ui/login_display_host.h"
#include "chrome/browser/chromeos/login/wizard_controller.h"
#include "chrome/browser/chromeos/policy/browser_policy_connector_chromeos.h"
#include "chrome/browser/chromeos/policy/enrollment_status_chromeos.h"
#include "chromeos/dbus/auth_policy/fake_auth_policy_client.h"
#include "chromeos/dbus/constants/dbus_switches.h"
#include "chromeos/dbus/dbus_thread_manager.h"
#include "chromeos/dbus/upstart/upstart_client.h"
#include "content/public/test/browser_test_utils.h"
#include "content/public/test/test_utils.h"

using testing::_;
using testing::Invoke;
using testing::InvokeWithoutArgs;

namespace chromeos {

namespace {

constexpr char kAdDialog[] = "oauth-enroll-ad-join-ui";
constexpr char kAdErrorCard[] = "oauth-enroll-active-directory-join-error-card";

constexpr char kAdUnlockConfigurationStep[] = "unlockStep";
constexpr char kAdUnlockPasswordInput[] = "unlockPasswordInput";
constexpr char kAdUnlockButton[] = "unlockButton";
constexpr char kSkipButton[] = "skipButton";

constexpr char kAdCredentialsStep[] = "credsStep";
constexpr char kAdJoinConfigurationForm[] = "joinConfig";
constexpr char kAdBackToUnlockButton[] = "backToUnlockButton";
constexpr char kAdMachineNameInput[] = "machineNameInput";
constexpr char kAdUsernameInput[] = "userInput";
constexpr char kAdPasswordInput[] = "passwordInput";
constexpr char kAdConfigurationSelect[] = "joinConfigSelect";
constexpr char kSubmitButton[] = "submitButton";
constexpr char kNextButton[] = "nextButton";

constexpr char kAdEncryptionTypesSelect[] = "encryptionList";
constexpr char kAdMachineOrgUnitInput[] = "orgUnitInput";
constexpr char kAdMoreOptionsSaveButton[] = "moreOptionsSave";

constexpr char kAdUserDomain[] = "user.domain.com";
constexpr char kAdMachineDomain[] = "machine.domain.com";
constexpr char kAdMachineDomainDN[] =
    "OU=leaf,OU=root,DC=machine,DC=domain,DC=com";
constexpr const char* kAdOrganizationlUnit[] = {"leaf", "root"};
constexpr char kAdTestUser[] = "test_user@user.domain.com";
constexpr char kDMToken[] = "dm_token";
constexpr char kAdDomainJoinEncryptedConfig[] =
    "W/x3ToZtYrHTzD21dlx2MrMhs2bDFTNmvew/toQhO+RdBV8XmQfJqVMaRtIO+Uji6zBueUyxcl"
    "MtiFJnimvYh0DUFQ5PJ3PY49BPACPnrGws51or1pEZJkXiKOEapRwNfqHz5tOnvFS1VqSvcv6Z"
    "JQqFQHKfvodGiEZv52+iViQTCSup8VJWCtfJxy/LxqHly/4xaUDNn8Sbbv8V/j8HUxc7/rwmnm"
    "R5B6qxIYDfYOpZWQXnVunlB2bBkcCEgXdS9YN+opsftlkNPsVrcdHUWwCmqxxAsuVZaTfxu+7C"
    "ZhSG72VH3BtQUsyGoh9evSIhAcid1CGbSx16sJVZyhZVXMF9D80AEl6aWDyxh43iJy0AgLpfkP"
    "mfkpZ3+iv0EJocFUhFINrq0fble+wE8KsOtlUBne4jFA/fifOrRBoIdXdLLz3+FbL4A7zY9qbd"
    "PbDw5J5W3nnaJWhTd5R765LbPp7wNAzdPh4a++E0dUUSVXO2K5HkAopV9RkeDea2kaxOLi1ioj"
    "H8fxubSHp4e8ZYSAX4N9JkJWiDurp8yEpUno2aw2Y7HafkMs0GMnO0sdkJfLZrnDq9wkZh7bMD"
    "6sp5tiOqVbTG6QH1BdlJBryTAjlrMFL6y7zFvfMZSJhbI6VwQyskGX/TOYEnsXuWEpRBxtDVV/"
    "QLUWM0orFELZPoPdeuH3dACsEv4mMBo8hWlKu/S3SHXt2hrvI1PXDO10AOHy8CPNPs7p/LeuJq"
    "XHRYOKsuNZnYbFJR1r+rZhkvYFpn6dHOLbe7RScqkq9cUYVvxK84COIdbEay9w1Son4sFJZszi"
    "Ve+uc/oFWcVp6GZPzvWSfjrTXYqIFDw/WsC8mYMgqOvTZCKj6M3pUyvc7bT3hIPqGXZyp5Pmzb"
    "jpCn95i8tlnjfmiZaDjl3HxrY15zvw==";
constexpr char kAdDomainJoinUnlockedConfig[] = R"!!!(
[
  {
    "name": "Sales",
    "ad_username": "domain_join_account@example.com",
    "ad_password": "test123",
    "computer_ou": "OU=sales,DC=example,DC=com",
    "encryption_types": "all"
  },
  {
    "name": "Marketing",
    "ad_username": "domain_join_account@example.com",
    "ad_password": "test123",
    "computer_ou": "OU=marketing,DC=example,DC=com"
  },
  {
    "name": "Engineering",
    "ad_username": "other_domain_join_account@example.com",
    "ad_password": "test345",
    "computer_ou": "OU=engineering,DC=example,DC=com",
    "computer_name_validation_regex": "^DEVICE_\\d+$"
  }
]
)!!!";

class MockAuthPolicyClient : public FakeAuthPolicyClient {
 public:
  MockAuthPolicyClient() = default;
  ~MockAuthPolicyClient() override = default;
  void JoinAdDomain(const authpolicy::JoinDomainRequest& request,
                    int password_fd,
                    JoinCallback callback) override {
    if (expected_request_) {
      ASSERT_EQ(expected_request_->SerializeAsString(),
                request.SerializeAsString());
      expected_request_.reset();
    }
    FakeAuthPolicyClient::JoinAdDomain(request, password_fd,
                                       std::move(callback));
  }

  void set_expected_request(
      std::unique_ptr<authpolicy::JoinDomainRequest> expected_request) {
    expected_request_ = std::move(expected_request);
  }

 private:
  std::unique_ptr<authpolicy::JoinDomainRequest> expected_request_;
};

}  // namespace

class EnterpriseEnrollmentTestBase : public LoginManagerTest {
 public:
  explicit EnterpriseEnrollmentTestBase(bool should_initialize_webui)
      : LoginManagerTest(true /*should_launch_browser*/,
                         should_initialize_webui) {
  }


  // Submits regular enrollment credentials.
  void SubmitEnrollmentCredentials() {
    enrollment_screen()->OnLoginDone(
        "testuser@test.com", test::EnrollmentHelperMixin::kTestAuthCode);
  }

  // Fills out the UI with device attribute information and submits it.
  void SubmitAttributePromptUpdate() {
    // Fill out the attribute prompt info and submit it.
    test::OobeJS().TypeIntoPath("asset_id", {"oauth-enroll-asset-id"});
    test::OobeJS().TypeIntoPath("location", {"oauth-enroll-location"});
    test::OobeJS().TapOn("enroll-attributes-submit-button");
  }

  // Completes the enrollment process.
  void CompleteEnrollment() {
    enrollment_screen()->OnDeviceEnrolled();

    // Make sure all other pending JS calls have complete.
    ExecutePendingJavaScript();
  }

  // Makes sure that all pending JS calls have been executed. It is important
  // to make this a separate call from the DOM checks because JSChecker uses
  // a different IPC message for JS communication than the login code. This
  // means that the JS script ordering is not preserved between the login code
  // and the test code.
  void ExecutePendingJavaScript() { test::OobeJS().Evaluate(";"); }

  // Returns true if there are any DOM elements with the given class.
  bool IsStepDisplayed(const std::string& step) {
    const std::string js =
        "document.getElementsByClassName('oauth-enroll-state-" + step +
        "').length";
    int count = test::OobeJS().GetInt(js);
    return count > 0;
  }

  // Setup the enrollment screen.
  void ShowEnrollmentScreen() {
    LoginDisplayHost* host = LoginDisplayHost::default_host();
    ASSERT_TRUE(host != nullptr);
    host->StartWizard(OobeScreen::SCREEN_OOBE_ENROLLMENT);
    OobeScreenWaiter(OobeScreen::SCREEN_OOBE_ENROLLMENT).Wait();
    ASSERT_TRUE(enrollment_screen() != nullptr);
    ASSERT_TRUE(WizardController::default_controller() != nullptr);
    ASSERT_FALSE(StartupUtils::IsOobeCompleted());
  }

  // Helper method to return the current EnrollmentScreen instance.
  EnrollmentScreen* enrollment_screen() {
    return EnrollmentScreen::Get(
        WizardController::default_controller()->screen_manager());
  }

 protected:
  test::EnrollmentHelperMixin enrollment_helper_{&mixin_host_};

 private:
  DISALLOW_COPY_AND_ASSIGN(EnterpriseEnrollmentTestBase);
};

class EnterpriseEnrollmentTest : public EnterpriseEnrollmentTestBase {
 public:
  EnterpriseEnrollmentTest()
      : EnterpriseEnrollmentTestBase(true /* should_initialize_webui */) {}

 private:
  DISALLOW_COPY_AND_ASSIGN(EnterpriseEnrollmentTest);
};

class ActiveDirectoryJoinTest : public EnterpriseEnrollmentTest {
 public:
  ActiveDirectoryJoinTest() = default;

  void SetUp() override {
    mock_auth_policy_client_ = new MockAuthPolicyClient();
    mock_auth_policy_client()->DisableOperationDelayForTesting();

    EnterpriseEnrollmentTestBase::SetUp();
  }

  std::string AdElement(const std::string& inner_id) {
    return test::GetOobeElementPath({kAdDialog, inner_id});
  }

  void ExpectElementValid(const std::string& inner_id, bool is_valid) {
    test::OobeJS().ExpectNE(AdElement(inner_id) + ".invalid", is_valid);
  }

  void CheckActiveDirectoryCredentialsShown() {
    EXPECT_TRUE(IsStepDisplayed("ad-join"));
    test::OobeJS().ExpectVisiblePath({kAdDialog, kAdCredentialsStep});
    test::OobeJS().ExpectHiddenPath({kAdDialog, kAdUnlockConfigurationStep});
  }

  void CheckConfigurationSelectionVisible(bool visible) {
    if (visible)
      test::OobeJS().ExpectVisiblePath({kAdDialog, kAdJoinConfigurationForm});
    else
      test::OobeJS().ExpectHiddenPath({kAdDialog, kAdJoinConfigurationForm});
  }

  void CheckActiveDirectoryUnlockConfigurationShown() {
    EXPECT_TRUE(IsStepDisplayed("ad-join"));
    test::OobeJS().ExpectHiddenPath({kAdDialog, kAdCredentialsStep});
    test::OobeJS().ExpectVisiblePath({kAdDialog, kAdUnlockConfigurationStep});
  }

  void CheckAttributeValue(const base::Value* config_value,
                           const std::string& default_value,
                           const std::string& js_element) {
    std::string expected_value(default_value);
    if (config_value)
      expected_value = config_value->GetString();
    test::OobeJS().ExpectTrue(js_element + " === '" + expected_value + "'");
  }

  void CheckAttributeValueAndDisabled(const base::Value* config_value,
                                      const std::string& default_value,
                                      const std::string& js_element) {
    CheckAttributeValue(config_value, default_value, js_element + ".value");
    const bool is_disabled = bool(config_value);
    test::OobeJS().ExpectEQ(js_element + ".disabled", is_disabled);
  }

  // Checks pattern attribute on the machine name input field. If |config_value|
  // is nullptr the attribute should be undefined.
  void CheckPatternAttribute(const base::Value* config_value) {
    if (config_value) {
      std::string escaped_pattern;
      // Escape regex pattern.
      EXPECT_TRUE(base::EscapeJSONString(config_value->GetString(),
                                         false /* put_in_quotes */,
                                         &escaped_pattern));
      test::OobeJS().ExpectTrue(AdElement(kAdMachineNameInput) +
                                ".pattern  === '" + escaped_pattern + "'");
    } else {
      test::OobeJS().ExpectTrue("typeof " + AdElement(kAdMachineNameInput) +
                                ".pattern === 'undefined'");
    }
  }

  // Goes through |configuration| which is JSON (see
  // kAdDomainJoinUnlockedConfig). Selects each of them and checks that all the
  // input fields are set correctly. Also checks if there is a "Custom" option
  // which does not set any fields.
  void CheckPossibleConfiguration(const std::string& configuration) {
    std::unique_ptr<base::ListValue> options =
        base::ListValue::From(base::JSONReader::ReadDeprecated(
            configuration,
            base::JSONParserOptions::JSON_ALLOW_TRAILING_COMMAS));
    base::DictionaryValue custom_option;
    custom_option.SetKey("name", base::Value("Custom"));
    options->GetList().emplace_back(std::move(custom_option));
    for (size_t i = 0; i < options->GetList().size(); ++i) {
      const base::Value& option = options->GetList()[i];
      // Select configuration value.
      test::OobeJS().SelectElementInPath(std::to_string(i),
                                         {kAdDialog, kAdConfigurationSelect});

      CheckAttributeValue(
          option.FindKeyOfType("name", base::Value::Type::STRING), "",
          AdElement(kAdConfigurationSelect) + ".selectedOptions[0].label");

      CheckAttributeValueAndDisabled(
          option.FindKeyOfType("ad_username", base::Value::Type::STRING), "",
          AdElement(kAdUsernameInput));

      CheckAttributeValueAndDisabled(
          option.FindKeyOfType("ad_password", base::Value::Type::STRING), "",
          AdElement(kAdPasswordInput));

      CheckAttributeValueAndDisabled(
          option.FindKeyOfType("computer_ou", base::Value::Type::STRING), "",
          AdElement(kAdMachineOrgUnitInput));

      CheckAttributeValueAndDisabled(
          option.FindKeyOfType("encryption_types", base::Value::Type::STRING),
          "strong", AdElement(kAdEncryptionTypesSelect));

      CheckPatternAttribute(option.FindKeyOfType(
          "computer_name_validation_regex", base::Value::Type::STRING));
    }
  }

  // Submits Active Directory domain join credentials.
  void SubmitActiveDirectoryCredentials(const std::string& machine_name,
                                        const std::string& machine_dn,
                                        const std::string& encryption_types,
                                        const std::string& username,
                                        const std::string& password) {
    CheckActiveDirectoryCredentialsShown();

    test::OobeJS().TypeIntoPath(machine_name, {kAdDialog, kAdMachineNameInput});
    test::OobeJS().TypeIntoPath(username, {kAdDialog, kAdUsernameInput});
    test::OobeJS().TypeIntoPath(password, {kAdDialog, kAdPasswordInput});
    test::OobeJS().TypeIntoPath(machine_dn,
                                {kAdDialog, kAdMachineOrgUnitInput});

    if (!encryption_types.empty()) {
      test::OobeJS().SelectElementInPath(encryption_types,
                                         {kAdDialog, kAdEncryptionTypesSelect});
    }
    test::OobeJS().TapOnPath({kAdDialog, kAdMoreOptionsSaveButton});
    test::OobeJS().TapOnPath({kAdDialog, kNextButton});
  }

  void SetExpectedJoinRequest(
      const std::string& machine_name,
      const std::string& machine_domain,
      authpolicy::KerberosEncryptionTypes encryption_types,
      std::vector<std::string> organizational_unit,
      const std::string& username,
      const std::string& dm_token) {
    auto request = std::make_unique<authpolicy::JoinDomainRequest>();
    if (!machine_name.empty())
      request->set_machine_name(machine_name);
    if (!machine_domain.empty())
      request->set_machine_domain(machine_domain);
    for (std::string& it : organizational_unit)
      request->add_machine_ou()->swap(it);
    if (!username.empty())
      request->set_user_principal_name(username);
    if (!dm_token.empty())
      request->set_dm_token(dm_token);
    request->set_kerberos_encryption_types(encryption_types);
    mock_auth_policy_client()->set_expected_request(std::move(request));
  }


  MockAuthPolicyClient* mock_auth_policy_client() {
    return mock_auth_policy_client_;
  }

  void SetupActiveDirectoryJSNotifications() {
    test::OobeJS().ExecuteAsync(
        "var originalShowStep = login.OAuthEnrollmentScreen.showStep;\n"
        "login.OAuthEnrollmentScreen.showStep = function(step) {\n"
        "  originalShowStep(step);\n"
        "  if (step == 'working') {\n"
        "    window.domAutomationController.send('ShowSpinnerScreen');\n"
        "  }"
        "}\n"
        "var originalShowError = login.OAuthEnrollmentScreen.showError;\n"
        "login.OAuthEnrollmentScreen.showError = function(message, retry) {\n"
        "  originalShowError(message, retry);\n"
        "  window.domAutomationController.send('ShowADJoinError');\n"
        "}\n");
    test::OobeJS().ExecuteAsync(
        "var originalSetAdJoinParams ="
        "    login.OAuthEnrollmentScreen.setAdJoinParams;"
        "login.OAuthEnrollmentScreen.setAdJoinParams = function("
        "    machineName, user, errorState, showUnlockConfig) {"
        "  originalSetAdJoinParams("
        "      machineName, user, errorState, showUnlockConfig);"
        "  window.domAutomationController.send('ShowJoinDomainError');"
        "}");
    test::OobeJS().ExecuteAsync(
        "var originalSetAdJoinConfiguration ="
        "    login.OAuthEnrollmentScreen.setAdJoinConfiguration;"
        "login.OAuthEnrollmentScreen.setAdJoinConfiguration = function("
        "    options) {"
        "  originalSetAdJoinConfiguration(options);"
        "  window.domAutomationController.send('SetAdJoinConfiguration');"
        "}");
  }

  void WaitForMessage(content::DOMMessageQueue* message_queue,
                      const std::string& expected_message) {
    std::string message;
    do {
      ASSERT_TRUE(message_queue->WaitForMessage(&message));
    } while (message != expected_message);
  }

 private:
  // Owned by the AuthPolicyClient global instance.
  MockAuthPolicyClient* mock_auth_policy_client_ = nullptr;

  DISALLOW_COPY_AND_ASSIGN(ActiveDirectoryJoinTest);
};

#if defined(MEMORY_SANITIZER)
#define TEST_DISABLED_ON_MSAN(test_fixture, test_name) \
  IN_PROC_BROWSER_TEST_F(test_fixture, DISABLED_##test_name)
#else
#define TEST_DISABLED_ON_MSAN(test_fixture, test_name) \
  IN_PROC_BROWSER_TEST_F(test_fixture, test_name)
#endif

// Shows the enrollment screen and simulates an enrollment complete event. We
// verify that the enrollmenth helper receives the correct auth code.
// Flaky on MSAN. https://crbug.com/876362
TEST_DISABLED_ON_MSAN(EnterpriseEnrollmentTest,
                      TestAuthCodeGetsProperlyReceivedFromGaia) {
  ShowEnrollmentScreen();
  enrollment_helper_.ExpectEnrollmentMode(
      policy::EnrollmentConfig::MODE_MANUAL);
  enrollment_helper_.ExpectEnrollmentCredentials();
  enrollment_helper_.SetupClearAuth();

  SubmitEnrollmentCredentials();
}

// Shows the enrollment screen and simulates an enrollment failure. Verifies
// that the error screen is displayed.
// TODO(crbug.com/690634): Disabled due to timeout flakiness.
IN_PROC_BROWSER_TEST_F(EnterpriseEnrollmentTest,
                       DISABLED_TestProperPageGetsLoadedOnEnrollmentFailure) {
  ShowEnrollmentScreen();

  enrollment_screen()->OnEnrollmentError(policy::EnrollmentStatus::ForStatus(
      policy::EnrollmentStatus::REGISTRATION_FAILED));
  ExecutePendingJavaScript();

  // Verify that the error page is displayed.
  EXPECT_TRUE(IsStepDisplayed("error"));
  EXPECT_FALSE(IsStepDisplayed("success"));
}

// Shows the enrollment screen and simulates a successful enrollment. Verifies
// that the success screen is then displayed.
// Flaky on MSAN. https://crbug.com/876362
TEST_DISABLED_ON_MSAN(EnterpriseEnrollmentTest,
                      TestProperPageGetsLoadedOnEnrollmentSuccess) {
  ShowEnrollmentScreen();
  enrollment_helper_.ExpectEnrollmentMode(
      policy::EnrollmentConfig::MODE_MANUAL);
  enrollment_helper_.DisableAttributePromptUpdate();
  SubmitEnrollmentCredentials();
  CompleteEnrollment();

  // Verify that the success page is displayed.
  EXPECT_TRUE(IsStepDisplayed("success"));
  EXPECT_FALSE(IsStepDisplayed("error"));
}

// Shows the enrollment screen and mocks the enrollment helper to request an
// attribute prompt screen. Verifies the attribute prompt screen is displayed.
// Verifies that the data the user enters into the attribute prompt screen is
// received by the enrollment helper.
// Flaky on MSAN. https://crbug.com/876362
TEST_DISABLED_ON_MSAN(EnterpriseEnrollmentTest,
                      TestAttributePromptPageGetsLoaded) {
  ShowEnrollmentScreen();
  enrollment_helper_.ExpectEnrollmentMode(
      policy::EnrollmentConfig::MODE_MANUAL);
  enrollment_helper_.ExpectAttributePromptUpdate("asset_id", "location");
  SubmitEnrollmentCredentials();
  CompleteEnrollment();

  // Make sure the attribute-prompt view is open.
  EXPECT_TRUE(IsStepDisplayed("attribute-prompt"));
  EXPECT_FALSE(IsStepDisplayed("success"));
  EXPECT_FALSE(IsStepDisplayed("error"));

  SubmitAttributePromptUpdate();
}

// Shows the enrollment screen and mocks the enrollment helper to show Active
// Directory domain join screen. Verifies the domain join screen is displayed.
// Submits Active Directory credentials. Verifies that the AuthpolicyClient
// calls us back with the correct realm.
// Timeouts on MSAN with polymer2. https://crbug.com/887577
TEST_DISABLED_ON_MSAN(ActiveDirectoryJoinTest,
                      TestActiveDirectoryEnrollment_Success) {
  ShowEnrollmentScreen();
  enrollment_helper_.DisableAttributePromptUpdate();
  enrollment_helper_.SetupActiveDirectoryJoin(
      enrollment_screen(), kAdUserDomain, std::string(), kDMToken);
  SubmitEnrollmentCredentials();

  chromeos::UpstartClient::Get()->StartAuthPolicyService();

  CheckActiveDirectoryCredentialsShown();
  CheckConfigurationSelectionVisible(false);
  content::DOMMessageQueue message_queue;
  SetupActiveDirectoryJSNotifications();
  SetExpectedJoinRequest("machine_name", "" /* machine_domain */,
                         authpolicy::KerberosEncryptionTypes::ENC_TYPES_ALL,
                         {} /* machine_ou */, kAdTestUser, kDMToken);
  SubmitActiveDirectoryCredentials("machine_name", "" /* machine_dn */, "all",
                                   kAdTestUser, "password");
  WaitForMessage(&message_queue, "\"ShowSpinnerScreen\"");
  EXPECT_FALSE(IsStepDisplayed("ad-join"));

  CompleteEnrollment();
  // Verify that the success page is displayed.
  EXPECT_TRUE(IsStepDisplayed("success"));
  EXPECT_FALSE(IsStepDisplayed("error"));
}

// Verifies that the distinguished name specified on the Active Directory join
// domain screen correctly parsed and passed into AuthPolicyClient.
// Timeouts on MSAN with polymer2. https://crbug.com/887577
TEST_DISABLED_ON_MSAN(ActiveDirectoryJoinTest,
                      TestActiveDirectoryEnrollment_DistinguishedName) {
  ShowEnrollmentScreen();
  enrollment_helper_.DisableAttributePromptUpdate();
  enrollment_helper_.SetupActiveDirectoryJoin(
      enrollment_screen(), kAdMachineDomain, std::string(), kDMToken);

  SubmitEnrollmentCredentials();

  chromeos::UpstartClient::Get()->StartAuthPolicyService();

  content::DOMMessageQueue message_queue;
  SetupActiveDirectoryJSNotifications();
  SetExpectedJoinRequest(
      "machine_name", kAdMachineDomain,
      authpolicy::KerberosEncryptionTypes::ENC_TYPES_STRONG,
      std::vector<std::string>(
          kAdOrganizationlUnit,
          kAdOrganizationlUnit + base::size(kAdOrganizationlUnit)),
      kAdTestUser, kDMToken);
  SubmitActiveDirectoryCredentials("machine_name", kAdMachineDomainDN,
                                   "" /* encryption_types */, kAdTestUser,
                                   "password");
  WaitForMessage(&message_queue, "\"ShowSpinnerScreen\"");
  EXPECT_FALSE(IsStepDisplayed("ad-join"));

  CompleteEnrollment();
  // Verify that the success page is displayed.
  EXPECT_TRUE(IsStepDisplayed("success"));
  EXPECT_FALSE(IsStepDisplayed("error"));
}

// Shows the enrollment screen and mocks the enrollment helper to show Active
// Directory domain join screen. Verifies the domain join screen is displayed.
// Submits Active Directory different incorrect credentials. Verifies that the
// correct error is displayed.
// Timeouts on MSAN with polymer2. https://crbug.com/887577
TEST_DISABLED_ON_MSAN(ActiveDirectoryJoinTest,
                      TestActiveDirectoryEnrollment_UIErrors) {
  ShowEnrollmentScreen();
  enrollment_helper_.SetupActiveDirectoryJoin(
      enrollment_screen(), kAdUserDomain, std::string(), kDMToken);
  SubmitEnrollmentCredentials();

  chromeos::UpstartClient::Get()->StartAuthPolicyService();

  content::DOMMessageQueue message_queue;
  // Checking error in case of empty password. Whether password is not empty
  // being checked in the UI. Machine name length is checked after that in the
  // authpolicyd.
  SetupActiveDirectoryJSNotifications();
  SubmitActiveDirectoryCredentials("too_long_machine_name", "" /* machine_dn */,
                                   "" /* encryption_types */, kAdTestUser,
                                   "" /* password */);
  EXPECT_TRUE(IsStepDisplayed("ad-join"));
  ExpectElementValid(kAdMachineNameInput, true);
  ExpectElementValid(kAdUsernameInput, true);
  ExpectElementValid(kAdPasswordInput, false);

  // Checking error in case of too long machine name.
  SubmitActiveDirectoryCredentials("too_long_machine_name", "" /* machine_dn */,
                                   "" /* encryption_types */, kAdTestUser,
                                   "password");
  WaitForMessage(&message_queue, "\"ShowJoinDomainError\"");
  EXPECT_TRUE(IsStepDisplayed("ad-join"));
  ExpectElementValid(kAdMachineNameInput, false);
  ExpectElementValid(kAdUsernameInput, true);
  ExpectElementValid(kAdPasswordInput, true);

  // Checking error in case of bad username (without realm).
  SubmitActiveDirectoryCredentials("machine_name", "" /* machine_dn */,
                                   "" /* encryption_types */, "test_user",
                                   "password");
  WaitForMessage(&message_queue, "\"ShowJoinDomainError\"");
  EXPECT_TRUE(IsStepDisplayed("ad-join"));
  ExpectElementValid(kAdMachineNameInput, true);
  ExpectElementValid(kAdUsernameInput, false);
  ExpectElementValid(kAdPasswordInput, true);
}

// Check that correct error card is shown (Active Directory one). Also checks
// that hitting retry shows Active Directory screen again.
// Timeouts on MSAN with polymer2. https://crbug.com/887577
TEST_DISABLED_ON_MSAN(ActiveDirectoryJoinTest,
                      TestActiveDirectoryEnrollment_ErrorCard) {
  ShowEnrollmentScreen();
  enrollment_helper_.SetupActiveDirectoryJoin(
      enrollment_screen(), kAdUserDomain, std::string(), kDMToken);
  SubmitEnrollmentCredentials();

  chromeos::UpstartClient::Get()->StartAuthPolicyService();

  content::DOMMessageQueue message_queue;
  SetupActiveDirectoryJSNotifications();
  // Legacy type triggers error card.
  SubmitActiveDirectoryCredentials("machine_name", "" /* machine_dn */,
                                   "legacy", kAdTestUser, "password");
  WaitForMessage(&message_queue, "\"ShowADJoinError\"");
  EXPECT_TRUE(IsStepDisplayed("active-directory-join-error"));
  test::OobeJS().TapOnPath({kAdErrorCard, kSubmitButton});
  EXPECT_TRUE(IsStepDisplayed("ad-join"));
}

// Check that configuration for the streamline Active Directory domain join
// propagates correctly to the Domain Join UI.
// Timeouts on MSAN with polymer2. https://crbug.com/887577
TEST_DISABLED_ON_MSAN(ActiveDirectoryJoinTest,
                      TestActiveDirectoryEnrollment_Streamline) {
  ShowEnrollmentScreen();
  std::string binary_config;
  EXPECT_TRUE(base::Base64Decode(kAdDomainJoinEncryptedConfig, &binary_config));
  enrollment_helper_.SetupActiveDirectoryJoin(
      enrollment_screen(), kAdUserDomain, binary_config, kDMToken);
  SubmitEnrollmentCredentials();

  chromeos::UpstartClient::Get()->StartAuthPolicyService();

  ExecutePendingJavaScript();
  content::DOMMessageQueue message_queue;
  SetupActiveDirectoryJSNotifications();

  // Unlock password step should we shown.
  CheckActiveDirectoryUnlockConfigurationShown();
  ExpectElementValid(kAdUnlockPasswordInput, true);

  // Test skipping the password step and getting back.
  test::OobeJS().TapOnPath({kAdDialog, kSkipButton});
  CheckActiveDirectoryCredentialsShown();
  CheckConfigurationSelectionVisible(false);
  test::OobeJS().TapOnPath({kAdDialog, kAdBackToUnlockButton});
  CheckActiveDirectoryUnlockConfigurationShown();

  // Enter wrong unlock password.
  test::OobeJS().TypeIntoPath("wrong_password",
                              {kAdDialog, kAdUnlockPasswordInput});
  test::OobeJS().TapOnPath({kAdDialog, kAdUnlockButton});
  WaitForMessage(&message_queue, "\"ShowJoinDomainError\"");
  ExpectElementValid(kAdUnlockPasswordInput, false);

  // Enter right unlock password.
  test::OobeJS().TypeIntoPath("test765!", {kAdDialog, kAdUnlockPasswordInput});
  test::OobeJS().TapOnPath({kAdDialog, kAdUnlockButton});
  WaitForMessage(&message_queue, "\"SetAdJoinConfiguration\"");
  CheckActiveDirectoryCredentialsShown();
  // Configuration selector should be visible.
  CheckConfigurationSelectionVisible(true);

  // Go through configuration.
  CheckPossibleConfiguration(kAdDomainJoinUnlockedConfig);
}

}  // namespace chromeos
