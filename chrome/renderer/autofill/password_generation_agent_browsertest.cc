// Copyright 2013 The Chromium Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include <string.h>

#include <memory>

#include "base/bind.h"
#include "base/macros.h"
#include "base/run_loop.h"
#include "base/strings/stringprintf.h"
#include "base/strings/utf_string_conversions.h"
#include "base/test/metrics/histogram_tester.h"
#include "chrome/renderer/autofill/fake_mojo_password_manager_driver.h"
#include "chrome/renderer/autofill/fake_password_generation_driver.h"
#include "chrome/renderer/autofill/password_generation_test_utils.h"
#include "chrome/test/base/chrome_render_view_test.h"
#include "components/autofill/content/renderer/autofill_agent.h"
#include "components/autofill/content/renderer/form_autofill_util.h"
#include "components/autofill/content/renderer/test_password_autofill_agent.h"
#include "components/autofill/content/renderer/test_password_generation_agent.h"
#include "components/autofill/core/common/autofill_switches.h"
#include "components/autofill/core/common/form_data.h"
#include "components/autofill/core/common/password_generation_util.h"
#include "components/password_manager/core/common/password_manager_features.h"
#include "content/public/renderer/render_frame.h"
#include "content/public/renderer/render_view.h"
#include "services/service_manager/public/cpp/interface_provider.h"
#include "testing/gtest/include/gtest/gtest.h"
#include "third_party/blink/public/common/associated_interfaces/associated_interface_provider.h"
#include "third_party/blink/public/platform/web_string.h"
#include "third_party/blink/public/web/web_document.h"
#include "third_party/blink/public/web/web_local_frame.h"
#include "third_party/blink/public/web/web_widget.h"
#include "ui/events/keycodes/keyboard_codes.h"

using base::ASCIIToUTF16;
using blink::WebDocument;
using blink::WebElement;
using blink::WebInputElement;
using blink::WebNode;
using blink::WebString;
using testing::_;
using testing::AnyNumber;
using testing::AtMost;

namespace autofill {

constexpr char kSigninFormHTML[] =
    "<FORM name = 'blah' action = 'http://www.random.com/'> "
    "  <INPUT type = 'text' id = 'username'/> "
    "  <INPUT type = 'password' id = 'password'/> "
    "  <INPUT type = 'button' id = 'dummy'/> "
    "  <INPUT type = 'submit' value = 'LOGIN' />"
    "</FORM>";

constexpr char kAccountCreationFormHTML[] =
    "<FORM id = 'blah' action = 'http://www.random.com/pa/th?q=1&p=3#first'> "
    "  <INPUT type = 'text' id = 'username'/> "
    "  <INPUT type = 'password' id = 'first_password' size = 5/>"
    "  <INPUT type = 'password' id = 'second_password' size = 5/> "
    "  <INPUT type = 'text' id = 'address'/> "
    "  <INPUT type = 'button' id = 'dummy'/> "
    "  <INPUT type = 'submit' value = 'LOGIN' />"
    "</FORM>";

constexpr char kAccountCreationNoForm[] =
    "<INPUT type = 'text' id = 'username'/> "
    "<INPUT type = 'password' id = 'first_password' size = 5/>"
    "<INPUT type = 'password' id = 'second_password' size = 5/> "
    "<INPUT type = 'text' id = 'address'/> "
    "<INPUT type = 'button' id = 'dummy'/> "
    "<INPUT type = 'submit' value = 'LOGIN' />";

constexpr char kAccountCreationNoIds[] =
    "<FORM action = 'http://www.random.com/pa/th?q=1&p=3#first'> "
    "  <INPUT type = 'text'/> "
    "  <INPUT type = 'password' class='first_password'/>"
    "  <INPUT type = 'password' class='second_password'/> "
    "  <INPUT type = 'text'/> "
    "  <INPUT type = 'button' id = 'dummy'/> "
    "  <INPUT type = 'submit' value = 'LOGIN'/>"
    "</FORM>";

constexpr char kDisabledElementAccountCreationFormHTML[] =
    "<FORM name = 'blah' action = 'http://www.random.com/'> "
    "  <INPUT type = 'text' id = 'username'/> "
    "  <INPUT type = 'password' id = 'first_password' "
    "         autocomplete = 'off' size = 5/>"
    "  <INPUT type = 'password' id = 'second_password' size = 5/> "
    "  <INPUT type = 'text' id = 'address'/> "
    "  <INPUT type = 'text' id = 'disabled' disabled/> "
    "  <INPUT type = 'button' id = 'dummy'/> "
    "  <INPUT type = 'submit' value = 'LOGIN' />"
    "</FORM>";

constexpr char kHiddenPasswordAccountCreationFormHTML[] =
    "<FORM name = 'blah' action = 'http://www.random.com/'> "
    "  <INPUT type = 'text' id = 'username'/> "
    "  <INPUT type = 'password' id = 'first_password'/> "
    "  <INPUT type = 'password' id = 'second_password' style='display:none'/> "
    "  <INPUT type = 'button' id = 'dummy'/> "
    "  <INPUT type = 'submit' value = 'LOGIN' />"
    "</FORM>";

constexpr char kInvalidActionAccountCreationFormHTML[] =
    "<FORM name = 'blah' action = 'invalid'> "
    "  <INPUT type = 'text' id = 'username'/> "
    "  <INPUT type = 'password' id = 'first_password'/> "
    "  <INPUT type = 'password' id = 'second_password'/> "
    "  <INPUT type = 'button' id = 'dummy'/> "
    "  <INPUT type = 'submit' value = 'LOGIN' />"
    "</FORM>";

constexpr char kMultipleAccountCreationFormHTML[] =
    "<FORM name = 'login' action = 'http://www.random.com/'> "
    "  <INPUT type = 'text' id = 'random'/> "
    "  <INPUT type = 'text' id = 'username'/> "
    "  <INPUT type = 'password' id = 'password'/> "
    "  <INPUT type = 'button' id = 'dummy'/> "
    "  <INPUT type = 'submit' value = 'LOGIN' />"
    "</FORM>"
    "<FORM name = 'signup' action = 'http://www.random.com/signup'> "
    "  <INPUT type = 'text' id = 'username'/> "
    "  <INPUT type = 'password' id = 'first_password' "
    "         autocomplete = 'off' size = 5/>"
    "  <INPUT type = 'password' id = 'second_password' size = 5/> "
    "  <INPUT type = 'text' id = 'address'/> "
    "  <INPUT type = 'submit' value = 'LOGIN' />"
    "</FORM>";

constexpr char kBothAutocompleteAttributesFormHTML[] =
    "<FORM name = 'blah' action = 'http://www.random.com/'> "
    "  <INPUT type = 'text' autocomplete='username' id = 'username'/> "
    "  <INPUT type = 'password' id = 'first_password' "
    "         autocomplete = 'new-password' size = 5/>"
    "  <INPUT type = 'password' id = 'second_password' size = 5/> "
    "  <INPUT type = 'button' id = 'dummy'/> "
    "  <INPUT type = 'submit' value = 'LOGIN' />"
    "</FORM>";

constexpr char kUsernameAutocompleteAttributeFormHTML[] =
    "<FORM name = 'blah' action = 'http://www.random.com/'> "
    "  <INPUT type = 'text' autocomplete='username' id = 'username'/> "
    "  <INPUT type = 'password' id = 'first_password' size = 5/>"
    "  <INPUT type = 'password' id = 'second_password' size = 5/> "
    "  <INPUT type = 'button' id = 'dummy'/> "
    "  <INPUT type = 'submit' value = 'LOGIN' />"
    "</FORM>";

constexpr char kNewPasswordAutocompleteAttributeFormHTML[] =
    "<FORM name = 'blah' action = 'http://www.random.com/'> "
    "  <INPUT type = 'text' id = 'username'/> "
    "  <INPUT type = 'password' id = 'first_password' "
    "         autocomplete='new-password' size = 5/>"
    "  <INPUT type = 'password' id = 'second_password' size = 5/> "
    "  <INPUT type = 'button' id = 'dummy'/> "
    "  <INPUT type = 'submit' value = 'LOGIN' />"
    "</FORM>";

constexpr char kCurrentAndNewPasswordAutocompleteAttributeFormHTML[] =
    "<FORM name = 'blah' action = 'http://www.random.com/'> "
    "  <INPUT type = 'password' id = 'old_password' "
    "         autocomplete='current-password'/>"
    "  <INPUT type = 'password' id = 'new_password' "
    "         autocomplete='new-password'/>"
    "  <INPUT type = 'password' id = 'confirm_password' "
    "         autocomplete='new-password'/>"
    "  <INPUT type = 'button' id = 'dummy'/> "
    "  <INPUT type = 'submit' value = 'LOGIN' />"
    "</FORM>";

constexpr char kPasswordChangeFormHTML[] =
    "<FORM name = 'ChangeWithUsernameForm' action = 'http://www.bidule.com'> "
    "  <INPUT type = 'text' id = 'username'/> "
    "  <INPUT type = 'password' id = 'password'/> "
    "  <INPUT type = 'password' id = 'newpassword'/> "
    "  <INPUT type = 'password' id = 'confirmpassword'/> "
    "  <INPUT type = 'button' id = 'dummy'/> "
    "  <INPUT type = 'submit' value = 'Login'/> "
    "</FORM>";

constexpr char kPasswordFormAndSpanHTML[] =
    "<FORM name = 'blah' action = 'http://www.random.com/pa/th?q=1&p=3#first'>"
    "  <INPUT type = 'text' id = 'username'/> "
    "  <INPUT type = 'password' id = 'password'/> "
    "  <INPUT type = 'button' id = 'dummy'/> "
    "</FORM>"
    "<SPAN id='span'>Text to click on</SPAN>";

class PasswordGenerationAgentTest : public ChromeRenderViewTest {
 public:
  enum AutomaticGenerationStatus {
    kNotReported,
    kAvailable,
    kUnavailable,
  };

  PasswordGenerationAgentTest() = default;

  void RegisterMainFrameRemoteInterfaces() override {
    // Because the test cases only involve the main frame in this test,
    // the fake password client is only used for the main frame.
    blink::AssociatedInterfaceProvider* remote_associated_interfaces =
        view_->GetMainRenderFrame()->GetRemoteAssociatedInterfaces();
    remote_associated_interfaces->OverrideBinderForTesting(
        mojom::PasswordGenerationDriver::Name_,
        base::BindRepeating(
            &PasswordGenerationAgentTest::BindPasswordManagerClient,
            base::Unretained(this)));
    remote_associated_interfaces->OverrideBinderForTesting(
        mojom::PasswordManagerDriver::Name_,
        base::BindRepeating(
            &PasswordGenerationAgentTest::BindPasswordManagerDriver,
            base::Unretained(this)));
  }

  void SetUp() override {
    ChromeRenderViewTest::SetUp();

    // TODO(crbug/862989): Remove workaround preventing non-test classes to bind
    // fake_driver_ or fake_pw_client_.
    password_autofill_agent_->GetPasswordManagerDriver();
    password_generation_->RequestPasswordManagerClientForTesting();
    base::RunLoop().RunUntilIdle();  // Executes binding the interfaces.
    // Reject all requests to bind driver/client to anything but the test class:
    view_->GetMainRenderFrame()
        ->GetRemoteAssociatedInterfaces()
        ->OverrideBinderForTesting(
            mojom::PasswordGenerationDriver::Name_,
            base::BindRepeating([](mojo::ScopedInterfaceEndpointHandle handle) {
              handle.reset();
            }));
    view_->GetMainRenderFrame()
        ->GetRemoteAssociatedInterfaces()
        ->OverrideBinderForTesting(
            mojom::PasswordManagerDriver::Name_,
            base::BindRepeating([](mojo::ScopedInterfaceEndpointHandle handle) {
              handle.reset();
            }));

    // Necessary for focus changes to work correctly and dispatch blur events
    // when a field was previously focused.
    GetWebWidget()->SetFocus(true);
  }

  void TearDown() override {
    // Unloading the document may trigger the event.
    EXPECT_CALL(fake_pw_client_, AutomaticGenerationStatusChanged(false, _))
        .Times(AtMost(1));
    ChromeRenderViewTest::TearDown();
  }

  void LoadHTMLWithUserGesture(const char* html) {
    LoadHTML(html);

    // Enable show-ime event when element is focused by indicating that a user
    // gesture has been processed since load.
    EXPECT_TRUE(SimulateElementClick("dummy"));
  }

  void FocusField(const char* element_id) {
    WebDocument document = GetMainFrame()->GetDocument();
    blink::WebElement element =
        document.GetElementById(blink::WebString::FromUTF8(element_id));
    ASSERT_FALSE(element.IsNull());
    ExecuteJavaScriptForTests(
        base::StringPrintf("document.getElementById('%s').focus();",
                           element_id).c_str());
  }

  void ExpectAutomaticGenerationAvailable(const char* element_id,
                                          AutomaticGenerationStatus available) {
    SCOPED_TRACE(testing::Message() << "element_id = " << element_id
                                    << "available = " << available);
    if (available == kNotReported) {
      EXPECT_CALL(fake_pw_client_, AutomaticGenerationStatusChanged(_, _))
          .Times(0);
    } else {
      EXPECT_CALL(fake_pw_client_,
                  AutomaticGenerationStatusChanged(available == kAvailable, _));
    }
    FocusField(element_id);
    base::RunLoop().RunUntilIdle();
    testing::Mock::VerifyAndClearExpectations(&fake_pw_client_);

    // Check that aria-autocomplete attribute is set correctly.
    if (available == kAvailable) {
      WebDocument doc = GetMainFrame()->GetDocument();
      WebElement element = doc.GetElementById(WebString::FromUTF8(element_id));
      EXPECT_EQ("list", element.GetAttribute("aria-autocomplete"));
    }
  }

  void ExpectFormClassifierVoteReceived(
      bool received,
      const base::string16& expected_generation_element) {
    base::RunLoop().RunUntilIdle();
    if (received) {
      ASSERT_TRUE(fake_driver_.called_save_generation_field());
      EXPECT_EQ(expected_generation_element,
                fake_driver_.save_generation_field());
    } else {
      ASSERT_FALSE(fake_driver_.called_save_generation_field());
    }

    fake_driver_.reset_save_generation_field();
  }

  void SelectGenerationFallbackAndExpect(bool available) {
    if (available) {
      EXPECT_CALL(*this, UserTriggeredGeneratePasswordReply(
                             testing::Ne(base::nullopt)));
    } else {
      EXPECT_CALL(*this, UserTriggeredGeneratePasswordReply(
                             testing::Eq(base::nullopt)));
    }
    password_generation_->UserTriggeredGeneratePassword(base::BindOnce(
        &PasswordGenerationAgentTest::UserTriggeredGeneratePasswordReply,
        base::Unretained(this)));
    testing::Mock::VerifyAndClearExpectations(this);
  }

  void BindPasswordManagerDriver(mojo::ScopedInterfaceEndpointHandle handle) {
    fake_driver_.BindRequest(
        mojom::PasswordManagerDriverAssociatedRequest(std::move(handle)));
  }

  void BindPasswordManagerClient(mojo::ScopedInterfaceEndpointHandle handle) {
    fake_pw_client_.BindRequest(
        mojom::PasswordGenerationDriverAssociatedRequest(std::move(handle)));
  }

  // Callback for UserTriggeredGeneratePassword.
  MOCK_METHOD1(UserTriggeredGeneratePasswordReply,
               void(const base::Optional<
                    autofill::password_generation::PasswordGenerationUIData>&));

  FakeMojoPasswordManagerDriver fake_driver_;
  testing::StrictMock<FakePasswordGenerationDriver> fake_pw_client_;

 private:
  DISALLOW_COPY_AND_ASSIGN(PasswordGenerationAgentTest);
};

class PasswordGenerationAgentTestForHtmlAnnotation
    : public PasswordGenerationAgentTest {
 public:
  PasswordGenerationAgentTestForHtmlAnnotation() = default;

  void SetUp() override {
    base::CommandLine::ForCurrentProcess()->AppendSwitch(
        switches::kShowAutofillSignatures);
    PasswordGenerationAgentTest::SetUp();
  }

  void TestAnnotateForm(bool has_form_tag);

 private:
  DISALLOW_COPY_AND_ASSIGN(PasswordGenerationAgentTestForHtmlAnnotation);
};

void PasswordGenerationAgentTestForHtmlAnnotation::TestAnnotateForm(
    bool has_form_tag) {
  SCOPED_TRACE(testing::Message() << "has_form_tag = " << has_form_tag);
  const char* kHtmlForm =
      has_form_tag ? kAccountCreationFormHTML : kAccountCreationNoForm;
  LoadHTMLWithUserGesture(kHtmlForm);
  SetNotBlacklistedMessage(password_generation_, kHtmlForm);
  if (has_form_tag) {
    SetAccountCreationFormsDetectedMessage(password_generation_,
                                           GetMainFrame()->GetDocument(), 0, 1);
  } else {
    SetAccountCreationFormsDetectedMessageForUnownedInputs(
        password_generation_, GetMainFrame()->GetDocument());
  }
  ExpectAutomaticGenerationAvailable("first_password", kAvailable);
  WebDocument document = GetMainFrame()->GetDocument();

  const char* kFormSignature =
      has_form_tag ? "3524919054660658462" : "7671707438749847833";
  if (has_form_tag) {
    // Check the form signature is set in the <form> tag.
    blink::WebElement form_element =
        document.GetElementById(blink::WebString::FromUTF8("blah"));
    ASSERT_FALSE(form_element.IsNull());
    blink::WebString form_signature =
        form_element.GetAttribute(blink::WebString::FromUTF8("form_signature"));
    ASSERT_FALSE(form_signature.IsNull());
    EXPECT_EQ(kFormSignature, form_signature.Ascii());
  }

  // Check field signatures and form signature are set in the <input>s.
  blink::WebElement username_element =
      document.GetElementById(blink::WebString::FromUTF8("username"));
  ASSERT_FALSE(username_element.IsNull());
  blink::WebString username_signature = username_element.GetAttribute(
      blink::WebString::FromUTF8("field_signature"));
  ASSERT_FALSE(username_signature.IsNull());
  EXPECT_EQ("239111655", username_signature.Ascii());
  blink::WebString form_signature_in_username = username_element.GetAttribute(
      blink::WebString::FromUTF8("form_signature"));
  EXPECT_EQ(kFormSignature, form_signature_in_username.Ascii());
  EXPECT_EQ(
      "username_element",
      username_element
          .GetAttribute(blink::WebString::FromUTF8("pm_parser_annotation"))
          .Ascii());

  blink::WebElement password_element =
      document.GetElementById(blink::WebString::FromUTF8("first_password"));
  ASSERT_FALSE(password_element.IsNull());
  blink::WebString password_signature = password_element.GetAttribute(
      blink::WebString::FromUTF8("field_signature"));
  ASSERT_FALSE(password_signature.IsNull());
  EXPECT_EQ("3933215845", password_signature.Ascii());
  blink::WebString form_signature_in_password = password_element.GetAttribute(
      blink::WebString::FromUTF8("form_signature"));
  EXPECT_EQ(kFormSignature, form_signature_in_password.Ascii());
  // The parser annotation is based on local heuristics, but not server side
  // prediction. So, the new password element is classified as the current
  // password.
  EXPECT_EQ(
      "password_element",
      password_element
          .GetAttribute(blink::WebString::FromUTF8("pm_parser_annotation"))
          .Ascii());

  // Check the generation element is marked.
  blink::WebString generation_mark = password_element.GetAttribute(
      blink::WebString::FromUTF8("password_creation_field"));
  ASSERT_FALSE(generation_mark.IsNull());
  EXPECT_EQ("1", generation_mark.Utf8());

  blink::WebElement confirmation_password_element =
      document.GetElementById(blink::WebString::FromUTF8("second_password"));
  // The parser annotation is based on local heuristics, but not server side
  // prediction. So, the confirmation password element is classified as the
  // new password.
  EXPECT_EQ(
      "new_password_element",
      confirmation_password_element
          .GetAttribute(blink::WebString::FromUTF8("pm_parser_annotation"))
          .Ascii());
}

TEST_F(PasswordGenerationAgentTest, HiddenSecondPasswordDetectionTest) {
  // Hidden fields are not treated differently.
  LoadHTMLWithUserGesture(kHiddenPasswordAccountCreationFormHTML);
  SetNotBlacklistedMessage(password_generation_,
                           kHiddenPasswordAccountCreationFormHTML);
  SetAccountCreationFormsDetectedMessage(password_generation_,
                                         GetMainFrame()->GetDocument(), 0, 1);
  ExpectAutomaticGenerationAvailable("first_password", kAvailable);
}

TEST_F(PasswordGenerationAgentTest, InvalidActionDetectionTest) {
  // This doesn't trigger because the form action is invalid.
  LoadHTMLWithUserGesture(kInvalidActionAccountCreationFormHTML);
  SetNotBlacklistedMessage(password_generation_,
                           kInvalidActionAccountCreationFormHTML);
  SetAccountCreationFormsDetectedMessage(password_generation_,
                                         GetMainFrame()->GetDocument(), 0, 1);
  ExpectAutomaticGenerationAvailable("first_password", kNotReported);
}

TEST_F(PasswordGenerationAgentTest, DetectionTestNoForm) {
  LoadHTMLWithUserGesture(kAccountCreationNoForm);
  SetNotBlacklistedMessage(password_generation_, kAccountCreationNoForm);
  SetAccountCreationFormsDetectedMessageForUnownedInputs(
      password_generation_, GetMainFrame()->GetDocument());

  ExpectAutomaticGenerationAvailable("first_password", kAvailable);
  ExpectAutomaticGenerationAvailable("second_password", kUnavailable);
}

TEST_F(PasswordGenerationAgentTest, FillTest) {
  // Add event listeners for password fields.
  std::vector<base::string16> variables_to_check;
  std::string events_registration_script =
      CreateScriptToRegisterListeners("first_password", &variables_to_check) +
      CreateScriptToRegisterListeners("second_password", &variables_to_check);

  // Make sure that we are enabled before loading HTML.
  std::string html =
      std::string(kAccountCreationFormHTML) + events_registration_script;
  // Begin with no gesture and therefore no focused element.
  LoadHTML(html.c_str());
  WebDocument document = GetMainFrame()->GetDocument();
  ASSERT_TRUE(document.FocusedElement().IsNull());
  SetNotBlacklistedMessage(password_generation_, html.c_str());
  SetAccountCreationFormsDetectedMessage(password_generation_, document, 0, 1);

  WebElement element =
      document.GetElementById(WebString::FromUTF8("first_password"));
  ASSERT_FALSE(element.IsNull());
  WebInputElement first_password_element = element.To<WebInputElement>();
  element = document.GetElementById(WebString::FromUTF8("second_password"));
  ASSERT_FALSE(element.IsNull());
  WebInputElement second_password_element = element.To<WebInputElement>();

  // Both password fields should be empty.
  EXPECT_TRUE(first_password_element.Value().IsNull());
  EXPECT_TRUE(second_password_element.Value().IsNull());

  base::string16 password = base::ASCIIToUTF16("random_password");
  EXPECT_CALL(fake_pw_client_,
              PresaveGeneratedPassword(testing::Field(
                  &autofill::PasswordForm::password_value, password)));
  password_generation_->GeneratedPasswordAccepted(password);

  // Password fields are filled out and set as being autofilled.
  EXPECT_EQ(password, first_password_element.Value().Utf16());
  EXPECT_EQ(password, second_password_element.Value().Utf16());
  EXPECT_TRUE(first_password_element.IsAutofilled());
  EXPECT_TRUE(second_password_element.IsAutofilled());

  // Make sure all events are called.
  for (const base::string16& variable : variables_to_check) {
    int value;
    EXPECT_TRUE(ExecuteJavaScriptAndReturnIntValue(variable, &value));
    EXPECT_EQ(1, value) << variable;
  }

  // Focus moved to the next input field.
  // TODO(zysxqn): Change this back to the address element once Bug 90224
  // https://bugs.webkit.org/show_bug.cgi?id=90224 has been fixed.
  element = document.GetElementById(WebString::FromUTF8("first_password"));
  ASSERT_FALSE(element.IsNull());
  EXPECT_EQ(element, document.FocusedElement());
}

TEST_F(PasswordGenerationAgentTest, EditingTest) {
  LoadHTMLWithUserGesture(kAccountCreationFormHTML);
  SetNotBlacklistedMessage(password_generation_, kAccountCreationFormHTML);
  SetAccountCreationFormsDetectedMessage(password_generation_,
                                         GetMainFrame()->GetDocument(), 0, 1);

  WebDocument document = GetMainFrame()->GetDocument();
  WebElement element =
      document.GetElementById(WebString::FromUTF8("first_password"));
  ASSERT_FALSE(element.IsNull());
  WebInputElement first_password_element = element.To<WebInputElement>();
  element = document.GetElementById(WebString::FromUTF8("second_password"));
  ASSERT_FALSE(element.IsNull());
  WebInputElement second_password_element = element.To<WebInputElement>();

  base::string16 password = base::ASCIIToUTF16("random_password");
  EXPECT_CALL(fake_pw_client_,
              PresaveGeneratedPassword(testing::Field(
                  &autofill::PasswordForm::password_value, password)));
  password_generation_->GeneratedPasswordAccepted(password);

  // Passwords start out the same.
  EXPECT_EQ(password, first_password_element.Value().Utf16());
  EXPECT_EQ(password, second_password_element.Value().Utf16());

  // After editing the first field they are still the same.
  std::string edited_password_ascii = "edited_password";
  base::string16 edited_password = base::ASCIIToUTF16(edited_password_ascii);
  EXPECT_CALL(fake_pw_client_,
              PresaveGeneratedPassword(testing::Field(
                  &autofill::PasswordForm::password_value, edited_password)));
  EXPECT_CALL(fake_pw_client_, ShowPasswordEditingPopup(_, _));
  SimulateUserInputChangeForElement(&first_password_element,
                                    edited_password_ascii);
  EXPECT_EQ(edited_password, first_password_element.Value().Utf16());
  EXPECT_EQ(edited_password, second_password_element.Value().Utf16());
  EXPECT_TRUE(first_password_element.IsAutofilled());
  EXPECT_TRUE(second_password_element.IsAutofilled());

  // Verify that password mirroring works correctly even when the password
  // is deleted.
  EXPECT_CALL(fake_pw_client_, PasswordNoLongerGenerated(_));
  EXPECT_CALL(fake_pw_client_, AutomaticGenerationStatusChanged(true, _));
  SimulateUserInputChangeForElement(&first_password_element, std::string());
  EXPECT_EQ(base::string16(), first_password_element.Value().Utf16());
  EXPECT_EQ(base::string16(), second_password_element.Value().Utf16());
  EXPECT_FALSE(first_password_element.IsAutofilled());
  EXPECT_FALSE(second_password_element.IsAutofilled());
}

TEST_F(PasswordGenerationAgentTest, EditingEventsTest) {
  LoadHTMLWithUserGesture(kAccountCreationFormHTML);
  SetNotBlacklistedMessage(password_generation_, kAccountCreationFormHTML);
  SetAccountCreationFormsDetectedMessage(password_generation_,
                                         GetMainFrame()->GetDocument(), 0, 1);

  // Generate password.
  ExpectAutomaticGenerationAvailable("first_password", kAvailable);
  base::string16 password = base::ASCIIToUTF16("random_password");
  EXPECT_CALL(fake_pw_client_,
              PresaveGeneratedPassword(testing::Field(
                  &autofill::PasswordForm::password_value, password)));
  password_generation_->GeneratedPasswordAccepted(password);
  fake_pw_client_.Flush();
  testing::Mock::VerifyAndClearExpectations(&fake_pw_client_);

  // Start removing characters one by one and observe the events sent to the
  // browser.
  EXPECT_CALL(fake_pw_client_, ShowPasswordEditingPopup(_, _));
  FocusField("first_password");
  fake_pw_client_.Flush();
  testing::Mock::VerifyAndClearExpectations(&fake_pw_client_);
  size_t max_chars_to_delete_before_editing =
      password.length() -
      PasswordGenerationAgent::kMinimumLengthForEditedPassword;
  for (size_t i = 0; i < max_chars_to_delete_before_editing; ++i) {
    password.erase(password.end() - 1);
    EXPECT_CALL(fake_pw_client_,
                PresaveGeneratedPassword(testing::Field(
                    &autofill::PasswordForm::password_value, password)));
    SimulateUserTypingASCIICharacter(ui::VKEY_BACK, true);
    fake_pw_client_.Flush();
    fake_driver_.Flush();
    EXPECT_TRUE(fake_driver_.last_focused_element_was_fillable());
    EXPECT_TRUE(fake_driver_.last_focused_input_was_password());
    testing::Mock::VerifyAndClearExpectations(&fake_pw_client_);
  }

  // Delete one more character and move back to the generation state.
  EXPECT_CALL(fake_pw_client_, PasswordNoLongerGenerated(_));
  EXPECT_CALL(fake_pw_client_, AutomaticGenerationStatusChanged(true, _));
  SimulateUserTypingASCIICharacter(ui::VKEY_BACK, true);
  fake_pw_client_.Flush();
  // Last focused element shouldn't change while editing.
  fake_driver_.Flush();
  EXPECT_TRUE(fake_driver_.last_focused_element_was_fillable());
  EXPECT_TRUE(fake_driver_.last_focused_input_was_password());
}

TEST_F(PasswordGenerationAgentTest, BlacklistedTest) {
  // Did not receive not blacklisted message. Don't show password generation
  // icon.
  LoadHTMLWithUserGesture(kAccountCreationFormHTML);
  SetAccountCreationFormsDetectedMessage(password_generation_,
                                         GetMainFrame()->GetDocument(), 0, 1);
  ExpectAutomaticGenerationAvailable("first_password", kNotReported);
}

TEST_F(PasswordGenerationAgentTest, UnblacklistedAnotherFormTest) {
  // Receive one not blacklisted message for non account creation form. Don't
  // show password generation icon.
  LoadHTMLWithUserGesture(kAccountCreationFormHTML);
  SetNotBlacklistedMessage(password_generation_, kSigninFormHTML);
  SetAccountCreationFormsDetectedMessage(password_generation_,
                                         GetMainFrame()->GetDocument(), 0, 1);
  ExpectAutomaticGenerationAvailable("first_password", kNotReported);
}

TEST_F(PasswordGenerationAgentTest, UnblacklistedTest) {
  // Receive one not blacklisted message for account creation form. Show
  // password generation icon.
  LoadHTMLWithUserGesture(kAccountCreationFormHTML);
  SetNotBlacklistedMessage(password_generation_, kAccountCreationFormHTML);
  SetAccountCreationFormsDetectedMessage(password_generation_,
                                         GetMainFrame()->GetDocument(), 0, 1);
  ExpectAutomaticGenerationAvailable("first_password", kAvailable);
}

TEST_F(PasswordGenerationAgentTest, UnblacklistedMultipleTest) {
  // Receive two not blacklisted messages, one is for account creation form and
  // the other is not. Show password generation icon.
  LoadHTMLWithUserGesture(kAccountCreationFormHTML);
  SetNotBlacklistedMessage(password_generation_, kAccountCreationFormHTML);
  SetNotBlacklistedMessage(password_generation_, kSigninFormHTML);
  SetAccountCreationFormsDetectedMessage(password_generation_,
                                         GetMainFrame()->GetDocument(), 0, 1);
  ExpectAutomaticGenerationAvailable("first_password", kAvailable);
}

TEST_F(PasswordGenerationAgentTest, AccountCreationFormsDetectedTest) {
  // Did not receive account creation forms detected message. Don't show
  // password generation icon.
  LoadHTMLWithUserGesture(kAccountCreationFormHTML);
  SetNotBlacklistedMessage(password_generation_, kAccountCreationFormHTML);

  // Receive the account creation forms detected message. Show password
  // generation icon.
  LoadHTMLWithUserGesture(kAccountCreationFormHTML);
  SetNotBlacklistedMessage(password_generation_, kAccountCreationFormHTML);
  SetAccountCreationFormsDetectedMessage(password_generation_,
                                         GetMainFrame()->GetDocument(), 0, 1);
  ExpectAutomaticGenerationAvailable("first_password", kAvailable);
}

TEST_F(PasswordGenerationAgentTest, MaximumCharsForGenerationOffer) {
  base::HistogramTester histogram_tester;

  LoadHTMLWithUserGesture(kAccountCreationFormHTML);
  SetNotBlacklistedMessage(password_generation_, kAccountCreationFormHTML);
  SetAccountCreationFormsDetectedMessage(password_generation_,
                                         GetMainFrame()->GetDocument(), 0, 1);
  // There should now be a message to show the UI.
  ExpectAutomaticGenerationAvailable("first_password", kAvailable);

  WebDocument document = GetMainFrame()->GetDocument();
  WebElement element =
      document.GetElementById(WebString::FromUTF8("first_password"));
  ASSERT_FALSE(element.IsNull());
  WebInputElement first_password_element = element.To<WebInputElement>();

  // Make a password just under maximum offer size.
  // Due to implementation details it's OK to get one more trigger for the
  // automatic generation.
  EXPECT_CALL(fake_pw_client_, AutomaticGenerationStatusChanged(true, _))
      .Times(AtMost(1));
  SimulateUserInputChangeForElement(
      &first_password_element,
      std::string(PasswordGenerationAgent::kMaximumCharsForGenerationOffer,
                  'a'));
  testing::Mock::VerifyAndClearExpectations(&fake_pw_client_);

  fake_pw_client_.reset_called_password_generation_rejected_by_typing();

  // Simulate a user typing a password just over maximum offer size.
  SimulateUserTypingASCIICharacter('a', true);
  // There should now be a message that generation was rejected.
  fake_pw_client_.Flush();
  EXPECT_TRUE(fake_pw_client_.called_password_generation_rejected_by_typing());
  fake_pw_client_.reset_called_password_generation_rejected_by_typing();

  // Simulate the user deleting characters. The generation popup should be
  // shown again.
  EXPECT_CALL(fake_pw_client_, AutomaticGenerationStatusChanged(true, _));
  SimulateUserTypingASCIICharacter(ui::VKEY_BACK, true);
  fake_pw_client_.Flush();
  testing::Mock::VerifyAndClearExpectations(&fake_pw_client_);

  // Change focus. Bubble should be hidden.
  EXPECT_CALL(fake_pw_client_, AutomaticGenerationStatusChanged(false, _));
  ExecuteJavaScriptForTests("document.getElementById('username').focus();");
  fake_pw_client_.Flush();

  // Focusing the password field will bring up the generation UI again.
  EXPECT_CALL(fake_pw_client_, AutomaticGenerationStatusChanged(true, _));
  ExecuteJavaScriptForTests(
      "document.getElementById('first_password').focus();");
  fake_pw_client_.Flush();
  testing::Mock::VerifyAndClearExpectations(&fake_pw_client_);

  // Loading a different page triggers UMA stat upload. Verify that only one
  // display event is sent.
  EXPECT_CALL(fake_pw_client_, AutomaticGenerationStatusChanged(false, _));
  LoadHTMLWithUserGesture(kSigninFormHTML);

  histogram_tester.ExpectBucketCount(
      "PasswordGeneration.Event",
      autofill::password_generation::GENERATION_POPUP_SHOWN, 1);
}

TEST_F(PasswordGenerationAgentTest, MinimumLengthForEditedPassword) {
  LoadHTMLWithUserGesture(kAccountCreationFormHTML);
  SetNotBlacklistedMessage(password_generation_, kAccountCreationFormHTML);
  SetAccountCreationFormsDetectedMessage(password_generation_,
                                         GetMainFrame()->GetDocument(), 0, 1);

  // Generate a new password.
  base::string16 password = base::ASCIIToUTF16("random_password");
  EXPECT_CALL(fake_pw_client_,
              PresaveGeneratedPassword(testing::Field(
                  &autofill::PasswordForm::password_value, password)));
  password_generation_->GeneratedPasswordAccepted(password);
  fake_pw_client_.Flush();
  testing::Mock::VerifyAndClearExpectations(&fake_pw_client_);

  // Delete most of the password.
  EXPECT_CALL(fake_pw_client_, ShowPasswordEditingPopup(_, _));
  EXPECT_CALL(fake_pw_client_, AutomaticGenerationStatusChanged(_, _)).Times(0);
  FocusField("first_password");
  size_t max_chars_to_delete =
      password.length() -
      PasswordGenerationAgent::kMinimumLengthForEditedPassword;
  EXPECT_CALL(fake_pw_client_, PresaveGeneratedPassword(testing::_))
      .Times(testing::AtLeast(1));
  for (size_t i = 0; i < max_chars_to_delete; ++i)
    SimulateUserTypingASCIICharacter(ui::VKEY_BACK, false);
  fake_pw_client_.Flush();
  testing::Mock::VerifyAndClearExpectations(&fake_pw_client_);

  // Delete one more character. The state should move to offering generation.
  EXPECT_CALL(fake_pw_client_, AutomaticGenerationStatusChanged(true, _));
  EXPECT_CALL(fake_pw_client_, PasswordNoLongerGenerated(testing::_));
  SimulateUserTypingASCIICharacter(ui::VKEY_BACK, true);
  fake_pw_client_.Flush();
  testing::Mock::VerifyAndClearExpectations(&fake_pw_client_);

  // The first password field is still non empty. The second one should be
  // cleared.
  WebDocument document = GetMainFrame()->GetDocument();
  WebElement element =
      document.GetElementById(WebString::FromUTF8("first_password"));
  ASSERT_FALSE(element.IsNull());
  WebInputElement first_password_element = element.To<WebInputElement>();
  element = document.GetElementById(WebString::FromUTF8("second_password"));
  ASSERT_FALSE(element.IsNull());
  WebInputElement second_password_element = element.To<WebInputElement>();
  EXPECT_NE(base::string16(), first_password_element.Value().Utf16());
  EXPECT_EQ(base::string16(), second_password_element.Value().Utf16());
}

TEST_F(PasswordGenerationAgentTest, DynamicFormTest) {
  LoadHTMLWithUserGesture(kSigninFormHTML);
  SetNotBlacklistedMessage(password_generation_, kSigninFormHTML);

  ExecuteJavaScriptForTests(
      "var form = document.createElement('form');"
      "form.action='http://www.random.com';"
      "var username = document.createElement('input');"
      "username.type = 'text';"
      "username.id = 'dynamic_username';"
      "var first_password = document.createElement('input');"
      "first_password.type = 'password';"
      "first_password.id = 'first_password';"
      "first_password.name = 'first_password';"
      "var second_password = document.createElement('input');"
      "second_password.type = 'password';"
      "second_password.id = 'second_password';"
      "second_password.name = 'second_password';"
      "form.appendChild(username);"
      "form.appendChild(first_password);"
      "form.appendChild(second_password);"
      "document.body.appendChild(form);");
  WaitForAutofillDidAssociateFormControl();

  // This needs to come after the DOM has been modified.
  SetAccountCreationFormsDetectedMessage(password_generation_,
                                         GetMainFrame()->GetDocument(), 1, 1);

  // TODO(gcasto): I'm slightly worried about flakes in this test where
  // didAssociateFormControls() isn't called. If this turns out to be a problem
  // adding a call to OnDynamicFormsSeen(GetMainFrame()) will fix it, though
  // it will weaken the test.
  ExpectAutomaticGenerationAvailable("first_password", kAvailable);
}

TEST_F(PasswordGenerationAgentTest, MultiplePasswordFormsTest) {
  // If two forms on the page looks like possible account creation forms, make
  // sure to trigger on the one that is specified from Autofill.
  LoadHTMLWithUserGesture(kMultipleAccountCreationFormHTML);
  SetNotBlacklistedMessage(password_generation_,
                           kMultipleAccountCreationFormHTML);

  // Should trigger on the second form.
  SetAccountCreationFormsDetectedMessage(password_generation_,
                                         GetMainFrame()->GetDocument(), 1, 1);

  ExpectAutomaticGenerationAvailable("password", kNotReported);
  ExpectAutomaticGenerationAvailable("first_password", kAvailable);
}

TEST_F(PasswordGenerationAgentTest, MessagesAfterAccountSignupFormFound) {
  LoadHTMLWithUserGesture(kAccountCreationFormHTML);
  SetNotBlacklistedMessage(password_generation_, kAccountCreationFormHTML);
  SetAccountCreationFormsDetectedMessage(password_generation_,
                                         GetMainFrame()->GetDocument(), 0, 1);

  // Generation should be enabled.
  ExpectAutomaticGenerationAvailable("first_password", kAvailable);

  // Extra not blacklisted messages can be sent. Make sure that they are handled
  // correctly (generation should still be available).
  SetNotBlacklistedMessage(password_generation_, kAccountCreationFormHTML);

  // Need to focus another field first for verification to work.
  ExpectAutomaticGenerationAvailable("second_password", kUnavailable);
  ExpectAutomaticGenerationAvailable("first_password", kAvailable);
}

// Losing focus should not trigger a password generation popup.
TEST_F(PasswordGenerationAgentTest, BlurTest) {
  LoadHTMLWithUserGesture(kDisabledElementAccountCreationFormHTML);
  SetNotBlacklistedMessage(password_generation_,
                           kDisabledElementAccountCreationFormHTML);
  SetAccountCreationFormsDetectedMessage(password_generation_,
                                         GetMainFrame()->GetDocument(), 0, 1);

  // Focus on the first password field: password generation popup should show
  // up.
  ExpectAutomaticGenerationAvailable("first_password", kAvailable);

  fake_pw_client_.reset_called_generation_available_for_form();
  // Remove focus from everywhere by clicking an unfocusable element: password
  // generation popup should not show up.
  EXPECT_CALL(fake_pw_client_, AutomaticGenerationStatusChanged(false, _));
  EXPECT_TRUE(SimulateElementClick("disabled"));
  fake_pw_client_.Flush();
  EXPECT_FALSE(fake_pw_client_.called_generation_available_for_form());
}

TEST_F(PasswordGenerationAgentTest, BothAutocompleteAttributesTest) {
  // Verify that autocomplete attributes can override Autofill to enable
  // generation.
  LoadHTMLWithUserGesture(kBothAutocompleteAttributesFormHTML);
  SetNotBlacklistedMessage(password_generation_,
                           kBothAutocompleteAttributesFormHTML);
  ExpectAutomaticGenerationAvailable("first_password", kAvailable);
}

TEST_F(PasswordGenerationAgentTest, UsernameAutocompleteAttributesTest) {
  // Only username autocomplete attribute enabled doesn't trigger generation.
  LoadHTMLWithUserGesture(kUsernameAutocompleteAttributeFormHTML);
  SetNotBlacklistedMessage(password_generation_,
                           kUsernameAutocompleteAttributeFormHTML);
  ExpectAutomaticGenerationAvailable("first_password", kNotReported);
}

TEST_F(PasswordGenerationAgentTest, NewPasswordAutocompleteAttributesTest) {
  // Only new-password autocomplete attribute enabled does trigger generation.
  LoadHTMLWithUserGesture(kNewPasswordAutocompleteAttributeFormHTML);
  SetNotBlacklistedMessage(password_generation_,
                           kNewPasswordAutocompleteAttributeFormHTML);
  ExpectAutomaticGenerationAvailable("first_password", kAvailable);
}

TEST_F(PasswordGenerationAgentTest,
       CurrentAndNewPasswordAutocompleteAttributesTest) {
  // Generation is triggered if the form has only password fields.
  LoadHTMLWithUserGesture(kCurrentAndNewPasswordAutocompleteAttributeFormHTML);
  SetNotBlacklistedMessage(password_generation_,
                           kCurrentAndNewPasswordAutocompleteAttributeFormHTML);
  ExpectAutomaticGenerationAvailable("old_password", kNotReported);
  ExpectAutomaticGenerationAvailable("new_password", kAvailable);
  ExpectAutomaticGenerationAvailable("confirm_password", kUnavailable);
}

TEST_F(PasswordGenerationAgentTest, ChangePasswordFormDetectionTest) {
  // Verify that generation is shown on correct field after message receiving.
  LoadHTMLWithUserGesture(kPasswordChangeFormHTML);
  SetNotBlacklistedMessage(password_generation_, kPasswordChangeFormHTML);
  ExpectAutomaticGenerationAvailable("password", kNotReported);
  ExpectAutomaticGenerationAvailable("newpassword", kNotReported);
  ExpectAutomaticGenerationAvailable("confirmpassword", kNotReported);

  SetAccountCreationFormsDetectedMessage(password_generation_,
                                         GetMainFrame()->GetDocument(), 0, 2);
  ExpectAutomaticGenerationAvailable("password", kNotReported);
  ExpectAutomaticGenerationAvailable("newpassword", kAvailable);
  ExpectAutomaticGenerationAvailable("confirmpassword", kUnavailable);
}

TEST_F(PasswordGenerationAgentTest, ManualGenerationInFormTest) {
  LoadHTMLWithUserGesture(kSigninFormHTML);
  SimulateElementRightClick("password");
  SelectGenerationFallbackAndExpect(true);
  // Re-focusing a password field for which manual generation was requested
  // should not re-trigger generation.
  ExpectAutomaticGenerationAvailable("password", kNotReported);
}

TEST_F(PasswordGenerationAgentTest, ManualGenerationNoFormTest) {
  LoadHTMLWithUserGesture(kAccountCreationNoForm);
  SimulateElementRightClick("first_password");
  SelectGenerationFallbackAndExpect(true);
}

TEST_F(PasswordGenerationAgentTest, ManualGenerationDoesntSuppressAutomatic) {
  LoadHTMLWithUserGesture(kAccountCreationFormHTML);
  SetNotBlacklistedMessage(password_generation_, kAccountCreationFormHTML);
  SetAccountCreationFormsDetectedMessage(password_generation_,
                                         GetMainFrame()->GetDocument(), 0, 1);
  ExpectAutomaticGenerationAvailable("first_password", kAvailable);
  // The browser may show a standard password dropdown with the "Generate"
  // option. In this case manual generation is triggered.
  SelectGenerationFallbackAndExpect(true);

  // Move the focus away to somewhere.
  ExpectAutomaticGenerationAvailable("address", kUnavailable);

  // Moving the focus back should trigger the automatic generation again.
  ExpectAutomaticGenerationAvailable("first_password", kAvailable);
}

TEST_F(PasswordGenerationAgentTest, ManualGenerationNoIds) {
  LoadHTMLWithUserGesture(kAccountCreationNoIds);
  WebDocument document = GetMainFrame()->GetDocument();

  ExecuteJavaScriptForTests(
      "document.getElementsByClassName('first_password')[0].focus();");
  WebInputElement first_password_element =
      document.FocusedElement().To<WebInputElement>();
  ASSERT_FALSE(first_password_element.IsNull());
  SelectGenerationFallbackAndExpect(true);

  // Simulate that the user accepts a generated password.
  base::string16 password = ASCIIToUTF16("random_password");
  EXPECT_CALL(fake_pw_client_,
              PresaveGeneratedPassword(testing::Field(
                  &autofill::PasswordForm::password_value, password)));
  password_generation_->GeneratedPasswordAccepted(password);

  // Check that the first password field is autofilled with the generated
  // password.
  EXPECT_EQ(password, first_password_element.Value().Utf16());
  EXPECT_TRUE(first_password_element.IsAutofilled());

  // Check that the second password field is autofilled with the generated
  // password (since it is chosen as a confirmation password field).
  ExecuteJavaScriptForTests(
      "document.getElementsByClassName('second_password')[0].focus();");
  WebInputElement second_password_element =
      document.FocusedElement().To<WebInputElement>();
  ASSERT_FALSE(second_password_element.IsNull());
  EXPECT_EQ(password, second_password_element.Value().Utf16());
  EXPECT_TRUE(second_password_element.IsAutofilled());
}

TEST_F(PasswordGenerationAgentTest, PresavingGeneratedPassword) {
  const struct {
    const char* form;
    const char* generation_element;
  } kTestCases[] = {{kAccountCreationFormHTML, "first_password"},
                    {kAccountCreationNoForm, "first_password"},
                    {kPasswordChangeFormHTML, "newpassword"}};
  for (auto& test_case : kTestCases) {
    SCOPED_TRACE(testing::Message("form: ") << test_case.form);
    LoadHTMLWithUserGesture(test_case.form);
    // To be able to work with input elements outside <form>'s, use manual
    // generation.
    SimulateElementRightClick(test_case.generation_element);
    SelectGenerationFallbackAndExpect(true);

    base::string16 password = base::ASCIIToUTF16("random_password");
    EXPECT_CALL(fake_pw_client_,
                PresaveGeneratedPassword(testing::Field(
                    &autofill::PasswordForm::password_value, password)));
    password_generation_->GeneratedPasswordAccepted(password);
    base::RunLoop().RunUntilIdle();

    EXPECT_CALL(fake_pw_client_, ShowPasswordEditingPopup(_, _));
    FocusField(test_case.generation_element);
    EXPECT_CALL(fake_pw_client_, PresaveGeneratedPassword(testing::_));
    SimulateUserTypingASCIICharacter('a', true);
    base::RunLoop().RunUntilIdle();

    // The current implementation may notify about unavailable generation.
    EXPECT_CALL(fake_pw_client_, AutomaticGenerationStatusChanged(false, _))
        .Times(AtMost(1));
    FocusField("username");
    EXPECT_CALL(fake_pw_client_, PresaveGeneratedPassword(testing::_));
    SimulateUserTypingASCIICharacter('X', true);
    base::RunLoop().RunUntilIdle();
    testing::Mock::VerifyAndClearExpectations(&fake_pw_client_);

    EXPECT_CALL(fake_pw_client_, ShowPasswordEditingPopup(_, _));
    FocusField(test_case.generation_element);
    EXPECT_CALL(fake_pw_client_, PasswordNoLongerGenerated(testing::_));
    for (size_t i = 0; i < password.length(); ++i)
      SimulateUserTypingASCIICharacter(ui::VKEY_BACK, false);
    SimulateUserTypingASCIICharacter(ui::VKEY_BACK, true);
    base::RunLoop().RunUntilIdle();

    EXPECT_CALL(fake_pw_client_, PresaveGeneratedPassword(testing::_)).Times(0);
    EXPECT_CALL(fake_pw_client_, AutomaticGenerationStatusChanged(false, _))
        .Times(AtMost(1));
    FocusField("username");
    SimulateUserTypingASCIICharacter('Y', true);
    base::RunLoop().RunUntilIdle();
    testing::Mock::VerifyAndClearExpectations(&fake_pw_client_);
  }
}

TEST_F(PasswordGenerationAgentTest, FallbackForSaving) {
  LoadHTMLWithUserGesture(kAccountCreationFormHTML);
  SimulateElementRightClick("first_password");
  SelectGenerationFallbackAndExpect(true);
  EXPECT_EQ(0, fake_driver_.called_show_manual_fallback_for_saving_count());
  base::string16 password = base::ASCIIToUTF16("random_password");
  EXPECT_CALL(fake_pw_client_,
              PresaveGeneratedPassword(testing::Field(
                  &autofill::PasswordForm::password_value, password)))
      .WillOnce(testing::InvokeWithoutArgs([this]() {
        // Make sure that generation event was propagated to the browser before
        // the fallback showing. Otherwise, the fallback for saving provides a
        // save bubble instead of a confirmation bubble.
        EXPECT_EQ(0,
                  fake_driver_.called_show_manual_fallback_for_saving_count());
      }));
  password_generation_->GeneratedPasswordAccepted(password);
  fake_driver_.Flush();
  // Two fallback requests are expected because generation changes either new
  // password and confirmation fields.
  EXPECT_EQ(2, fake_driver_.called_show_manual_fallback_for_saving_count());
}

TEST_F(PasswordGenerationAgentTest, FormClassifierDisabled) {
  LoadHTMLWithUserGesture(kSigninFormHTML);
  ExpectFormClassifierVoteReceived(false /* vote is not expected */,
                                   base::string16());
}

TEST_F(PasswordGenerationAgentTest, ConfirmationFieldVoteFromServer) {
  LoadHTMLWithUserGesture(kPasswordChangeFormHTML);
  SetNotBlacklistedMessage(password_generation_, kPasswordChangeFormHTML);

  WebDocument document = GetMainFrame()->GetDocument();
  blink::WebVector<blink::WebFormElement> web_forms;
  document.Forms(web_forms);
  autofill::FormData form_data;
  WebFormElementToFormData(web_forms[0], blink::WebFormControlElement(),
                           nullptr, form_util::EXTRACT_NONE, &form_data,
                           nullptr /* FormFieldData */);

  std::vector<autofill::PasswordFormGenerationData> forms;
  autofill::PasswordFormGenerationData generation_data(
      CalculateFormSignature(form_data),
      CalculateFieldSignatureForField(form_data.fields[1]));
  generation_data.confirmation_field_signature.emplace(
      CalculateFieldSignatureForField(form_data.fields[3]));
  forms.push_back(generation_data);
  password_generation_->FoundFormsEligibleForGeneration(forms);

  WebElement element =
      document.GetElementById(WebString::FromUTF16(form_data.fields[1].name));
  ASSERT_FALSE(element.IsNull());
  WebInputElement generation_element = element.To<WebInputElement>();
  element =
      document.GetElementById(WebString::FromUTF16(form_data.fields[2].name));
  ASSERT_FALSE(element.IsNull());
  WebInputElement ignored_password_element = element.To<WebInputElement>();
  element =
      document.GetElementById(WebString::FromUTF16(form_data.fields[3].name));
  ASSERT_FALSE(element.IsNull());
  WebInputElement confirmation_password_element = element.To<WebInputElement>();

  base::string16 password = base::ASCIIToUTF16("random_password");
  EXPECT_CALL(fake_pw_client_,
              PresaveGeneratedPassword(testing::Field(
                  &autofill::PasswordForm::password_value, password)));
  password_generation_->GeneratedPasswordAccepted(password);
  EXPECT_EQ(password, generation_element.Value().Utf16());
  // Check that the generated password was copied according to the server's
  // response.
  EXPECT_EQ(base::string16(), ignored_password_element.Value().Utf16());
  EXPECT_EQ(password, confirmation_password_element.Value().Utf16());
}

TEST_F(PasswordGenerationAgentTest, RevealPassword) {
  // Checks that revealed password is masked when the field lost focus.
  // Test cases: user click on another input field and on non-focusable element.
  LoadHTMLWithUserGesture(kPasswordFormAndSpanHTML);
  SetNotBlacklistedMessage(password_generation_, kPasswordFormAndSpanHTML);
  SetAccountCreationFormsDetectedMessage(password_generation_,
                                         GetMainFrame()->GetDocument(), 0, 1);
  const char* kGenerationElementId = "password";
  const char* kSpanId = "span";
  const char* kTextFieldId = "username";

  ExpectAutomaticGenerationAvailable(kGenerationElementId, kAvailable);
  base::string16 password = base::ASCIIToUTF16("long_pwd");
  EXPECT_CALL(fake_pw_client_,
              PresaveGeneratedPassword(testing::Field(
                  &autofill::PasswordForm::password_value, password)));
  password_generation_->GeneratedPasswordAccepted(password);

  const bool kFalseTrue[] = {false, true};
  for (bool clickOnInputField : kFalseTrue) {
    SCOPED_TRACE(testing::Message("clickOnInputField = ") << clickOnInputField);
    // Click on the generation field to reveal the password value.
    EXPECT_CALL(fake_pw_client_, ShowPasswordEditingPopup(_, _));
    FocusField(kGenerationElementId);
    fake_pw_client_.Flush();

    WebDocument document = GetMainFrame()->GetDocument();
    blink::WebElement element = document.GetElementById(
        blink::WebString::FromUTF8(kGenerationElementId));
    ASSERT_FALSE(element.IsNull());
    blink::WebInputElement input = element.To<WebInputElement>();
    EXPECT_TRUE(input.ShouldRevealPassword());

    // Click on another HTML element.
    const char* const click_target_name =
        clickOnInputField ? kTextFieldId : kSpanId;
    EXPECT_TRUE(SimulateElementClick(click_target_name));
    EXPECT_FALSE(input.ShouldRevealPassword());
  }
}

TEST_F(PasswordGenerationAgentTest, JavascriptClearedTheField) {
  LoadHTMLWithUserGesture(kAccountCreationFormHTML);
  SetNotBlacklistedMessage(password_generation_, kAccountCreationFormHTML);
  SetAccountCreationFormsDetectedMessage(password_generation_,
                                         GetMainFrame()->GetDocument(), 0, 1);

  const char kGenerationElementId[] = "first_password";
  ExpectAutomaticGenerationAvailable(kGenerationElementId, kAvailable);
  base::string16 password = base::ASCIIToUTF16("long_pwd");
  EXPECT_CALL(fake_pw_client_,
              PresaveGeneratedPassword(testing::Field(
                  &autofill::PasswordForm::password_value, password)));
  password_generation_->GeneratedPasswordAccepted(password);

  EXPECT_CALL(fake_pw_client_, PasswordNoLongerGenerated(testing::_));
  EXPECT_CALL(fake_pw_client_, AutomaticGenerationStatusChanged(true, _));
  ExecuteJavaScriptForTests(
      "document.getElementById('first_password').value = '';");
  FocusField(kGenerationElementId);
  base::RunLoop().RunUntilIdle();
}

TEST_F(PasswordGenerationAgentTest, GenerationFallbackTest) {
  LoadHTMLWithUserGesture(kAccountCreationFormHTML);
  WebDocument document = GetMainFrame()->GetDocument();
  WebElement element =
      document.GetElementById(WebString::FromUTF8("first_password"));
  ASSERT_FALSE(element.IsNull());
  WebInputElement first_password_element = element.To<WebInputElement>();
  EXPECT_TRUE(first_password_element.Value().IsNull());
  SimulateElementRightClick("first_password");
  SelectGenerationFallbackAndExpect(true);
  EXPECT_TRUE(first_password_element.Value().IsNull());
}

TEST_F(PasswordGenerationAgentTest, GenerationFallback_NoFocusedElement) {
  // Checks the fallback doesn't cause a crash just in case no password element
  // had focus so far.
  LoadHTMLWithUserGesture(kAccountCreationFormHTML);
  SelectGenerationFallbackAndExpect(false);
}

TEST_F(PasswordGenerationAgentTest, AutofillToGenerationField) {
  LoadHTMLWithUserGesture(kAccountCreationFormHTML);
  SetNotBlacklistedMessage(password_generation_, kAccountCreationFormHTML);
  SetAccountCreationFormsDetectedMessage(password_generation_,
                                         GetMainFrame()->GetDocument(), 0, 1);
  ExpectAutomaticGenerationAvailable("first_password", kAvailable);

  WebDocument document = GetMainFrame()->GetDocument();
  WebElement element =
      document.GetElementById(WebString::FromUTF8("first_password"));
  ASSERT_FALSE(element.IsNull());
  const WebInputElement input_element = element.To<WebInputElement>();
  // Since password isn't generated (just suitable field was detected),
  // |OnFieldAutofilled| wouldn't trigger any actions.
  EXPECT_CALL(fake_pw_client_, PasswordNoLongerGenerated(testing::_)).Times(0);
  password_generation_->OnFieldAutofilled(input_element);
}

TEST_F(PasswordGenerationAgentTestForHtmlAnnotation, AnnotateForm) {
  TestAnnotateForm(true);
}

TEST_F(PasswordGenerationAgentTestForHtmlAnnotation, AnnotateNoForm) {
  TestAnnotateForm(false);
}

TEST_F(PasswordGenerationAgentTest, PasswordUnmaskedUntilCompleteDeletion) {
  LoadHTMLWithUserGesture(kAccountCreationFormHTML);
  SetNotBlacklistedMessage(password_generation_, kAccountCreationFormHTML);
  SetAccountCreationFormsDetectedMessage(password_generation_,
                                         GetMainFrame()->GetDocument(), 0, 1);

  constexpr char kGenerationElementId[] = "first_password";

  // Generate a new password.
  ExpectAutomaticGenerationAvailable(kGenerationElementId, kAvailable);
  base::string16 password = base::ASCIIToUTF16("random_password");
  EXPECT_CALL(fake_pw_client_,
              PresaveGeneratedPassword(testing::Field(
                  &autofill::PasswordForm::password_value, password)));
  password_generation_->GeneratedPasswordAccepted(password);
  fake_pw_client_.Flush();
  testing::Mock::VerifyAndClearExpectations(&fake_pw_client_);

  // Delete characters of the generated password until only
  // |kMinimumLengthForEditedPassword| - 1 chars remain.
  EXPECT_CALL(fake_pw_client_, ShowPasswordEditingPopup(_, _));
  FocusField(kGenerationElementId);
  EXPECT_CALL(fake_pw_client_, PasswordNoLongerGenerated(testing::_));
  EXPECT_CALL(fake_pw_client_, AutomaticGenerationStatusChanged(true, _));
  size_t max_chars_to_delete =
      password.length() -
      PasswordGenerationAgent::kMinimumLengthForEditedPassword + 1;
  for (size_t i = 0; i < max_chars_to_delete; ++i)
    SimulateUserTypingASCIICharacter(ui::VKEY_BACK, false);
  base::RunLoop().RunUntilIdle();
  fake_pw_client_.Flush();
  // The remaining characters no longer count as a generated password, so
  // generation should be offered again.

  // Check that the characters remain unmasked.
  WebDocument document = GetMainFrame()->GetDocument();
  blink::WebElement element =
      document.GetElementById(blink::WebString::FromUTF8(kGenerationElementId));
  ASSERT_FALSE(element.IsNull());
  blink::WebInputElement input = element.To<WebInputElement>();
  EXPECT_TRUE(input.ShouldRevealPassword());

  // Delete the rest of the characters. The field should now mask new
  // characters. Due to implementation details it's possible to get pings about
  // password generation available.
  EXPECT_CALL(fake_pw_client_, AutomaticGenerationStatusChanged(true, _))
      .Times(AnyNumber());
  for (size_t i = 0;
       i < PasswordGenerationAgent::kMinimumLengthForEditedPassword; ++i)
    SimulateUserTypingASCIICharacter(ui::VKEY_BACK, false);
  base::RunLoop().RunUntilIdle();
  EXPECT_FALSE(input.ShouldRevealPassword());
}

TEST_F(PasswordGenerationAgentTest, ShortPasswordMaskedAfterChangingFocus) {
  LoadHTMLWithUserGesture(kPasswordFormAndSpanHTML);
  SetNotBlacklistedMessage(password_generation_, kPasswordFormAndSpanHTML);
  SetAccountCreationFormsDetectedMessage(password_generation_,
                                         GetMainFrame()->GetDocument(), 0, 1);

  constexpr char kGenerationElementId[] = "password";

  // Generate a new password.
  ExpectAutomaticGenerationAvailable(kGenerationElementId, kAvailable);
  base::string16 password = base::ASCIIToUTF16("random_password");
  EXPECT_CALL(fake_pw_client_,
              PresaveGeneratedPassword(testing::Field(
                  &autofill::PasswordForm::password_value, password)));
  password_generation_->GeneratedPasswordAccepted(password);
  fake_pw_client_.Flush();
  testing::Mock::VerifyAndClearExpectations(&fake_pw_client_);

  // Delete characters of the generated password until only
  // |kMinimumLengthForEditedPassword| - 1 chars remain.
  EXPECT_CALL(fake_pw_client_, ShowPasswordEditingPopup(_, _));
  FocusField(kGenerationElementId);
  EXPECT_CALL(fake_pw_client_, PasswordNoLongerGenerated(testing::_));
  size_t max_chars_to_delete =
      password.length() -
      PasswordGenerationAgent::kMinimumLengthForEditedPassword + 1;
  EXPECT_CALL(fake_pw_client_, AutomaticGenerationStatusChanged(true, _));
  for (size_t i = 0; i < max_chars_to_delete; ++i)
    SimulateUserTypingASCIICharacter(ui::VKEY_BACK, false);
  // The remaining characters no longer count as a generated password, so
  // generation should be offered again.
  base::RunLoop().RunUntilIdle();
  fake_pw_client_.Flush();
  testing::Mock::VerifyAndClearExpectations(&fake_pw_client_);

  // Check that the characters remain unmasked.
  WebDocument document = GetMainFrame()->GetDocument();
  blink::WebElement element =
      document.GetElementById(blink::WebString::FromUTF8(kGenerationElementId));
  ASSERT_FALSE(element.IsNull());
  blink::WebInputElement input = element.To<WebInputElement>();
  EXPECT_TRUE(input.ShouldRevealPassword());

  // Focus another element on the page. The password should be masked.
  EXPECT_CALL(fake_pw_client_, AutomaticGenerationStatusChanged(false, _));
  ASSERT_TRUE(SimulateElementClick("span"));
  EXPECT_FALSE(input.ShouldRevealPassword());

  // Focus the password field again. As the remaining characters are not
  // a generated password, they should remain masked.
  ExpectAutomaticGenerationAvailable(kGenerationElementId, kAvailable);
  EXPECT_FALSE(input.ShouldRevealPassword());
}

TEST_F(PasswordGenerationAgentTest, GenerationAvailableByRendererIds) {
  LoadHTMLWithUserGesture(kMultipleAccountCreationFormHTML);

  constexpr const char* kPasswordElementsIds[] = {"password", "first_password",
                                                  "second_password"};

  WebDocument document = GetMainFrame()->GetDocument();
  std::vector<WebInputElement> password_elements;
  for (const char* id : kPasswordElementsIds) {
    WebElement element = document.GetElementById(WebString::FromUTF8(id));
    WebInputElement* input = ToWebInputElement(&element);
    ASSERT_TRUE(input);
    password_elements.push_back(*input);
  }

  // Simulate that the browser informs about eligible for generation form.
  // Check that generation is available only on new password field of this form.
  NewPasswordFormGenerationData generation_data = {
      .new_password_renderer_id =
          password_elements[0].UniqueRendererFormControlId()};

  password_generation_->FoundFormEligibleForGeneration(generation_data);
  ExpectAutomaticGenerationAvailable(kPasswordElementsIds[0], kAvailable);
  ExpectAutomaticGenerationAvailable(kPasswordElementsIds[1], kUnavailable);
  ExpectAutomaticGenerationAvailable(kPasswordElementsIds[2], kNotReported);

  // Simulate that the browser informs about the second eligible for generation
  // form. Check that generation is available on both forms.
  generation_data.new_password_renderer_id =
      password_elements[2].UniqueRendererFormControlId();
  password_generation_->FoundFormEligibleForGeneration(generation_data);
  ExpectAutomaticGenerationAvailable(kPasswordElementsIds[0], kAvailable);
  ExpectAutomaticGenerationAvailable(kPasswordElementsIds[1], kUnavailable);
  ExpectAutomaticGenerationAvailable(kPasswordElementsIds[2], kAvailable);
}

}  // namespace autofill
