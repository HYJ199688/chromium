// Copyright 2013 The Chromium Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include "chrome/browser/chromeos/input_method/input_method_engine.h"

#include <map>
#include <memory>
#include <utility>

#include "ash/public/interfaces/constants.mojom.h"
#include "base/logging.h"
#include "base/metrics/histogram_macros.h"
#include "base/strings/string_number_conversions.h"
#include "base/strings/string_util.h"
#include "base/strings/utf_string_conversions.h"
#include "chrome/browser/profiles/profile_manager.h"
#include "chrome/browser/ui/ash/keyboard/chrome_keyboard_controller_client.h"
#include "content/public/common/service_manager_connection.h"
#include "mojo/public/cpp/bindings/binding.h"
#include "services/service_manager/public/cpp/connector.h"
#include "ui/aura/window.h"
#include "ui/aura/window_tree_host.h"
#include "ui/base/ime/candidate_window.h"
#include "ui/base/ime/chromeos/component_extension_ime_manager.h"
#include "ui/base/ime/chromeos/extension_ime_util.h"
#include "ui/base/ime/chromeos/ime_keymap.h"
#include "ui/base/ime/composition_text.h"
#include "ui/base/ime/constants.h"
#include "ui/base/ime/ime_bridge.h"
#include "ui/base/ime/mojo/ime.mojom.h"
#include "ui/base/ime/text_input_flags.h"
#include "ui/base/ui_base_features.h"
#include "ui/chromeos/ime/input_method_menu_item.h"
#include "ui/chromeos/ime/input_method_menu_manager.h"
#include "ui/events/event.h"
#include "ui/events/event_sink.h"
#include "ui/events/event_utils.h"
#include "ui/events/keycodes/dom/dom_code.h"
#include "ui/events/keycodes/dom/keycode_converter.h"

using input_method::InputMethodEngineBase;

namespace chromeos {

namespace {

const char kErrorNotActive[] = "IME is not active";
const char kErrorWrongContext[] = "Context is not active";
const char kCandidateNotFound[] = "Candidate not found";

// The default entry number of a page in CandidateWindowProperty.
const int kDefaultPageSize = 9;

}  // namespace

// The helper to make the InputMethodEngine as a mojom::ImeEngine and
// mojom::ImeEngineFactory.
// It forwards the mojom::ImeEngine method calls to InputMethodEngine's
// ui::IMEEngineHandlerInterface methods.
// TODO(crbug.com/946352): Removes this helper after the
// ui::IMEEngineHandlerInterface is deprecated. So that InputMethodEngine can
// directly inherits from mojom::ImeEngine/ImeEngineFactory.
class MojoHelper : public ime::mojom::ImeEngine,
                   public ime::mojom::ImeEngineFactory {
 public:
  MojoHelper(InputMethodEngine* engine,
             ime::mojom::ImeEngineFactoryRegistryPtr registry)
      : engine_(engine), factory_binding_(this), engine_binding_(this) {
    ime::mojom::ImeEngineFactoryPtr factory_ptr;
    factory_binding_.Bind(mojo::MakeRequest(&factory_ptr));
    if (registry) {
      registry_ = std::move(registry);
    } else {
      auto* conn = content::ServiceManagerConnection::GetForProcess();
      if (!conn)  // Could be null in tests.
        return;
      conn->GetConnector()->BindInterface(ash::mojom::kServiceName, &registry_);
    }
    registry_->ActivateFactory(std::move(factory_ptr));
  }
  ~MojoHelper() override = default;

  // ime::mojom::ImeEngineFactory overrides:
  void CreateEngine(ime::mojom::ImeEngineRequest engine_request,
                    ime::mojom::ImeEngineClientPtr client) override {
    engine_binding_.Bind(std::move(engine_request));
    engine_client_ = std::move(client);
  }

  // ime::mojom::ImeEngine overrides:
  void StartInput(ime::mojom::EditorInfoPtr info) override {
    ui::IMEEngineHandlerInterface::InputContext context(
        info->type, info->mode, info->flags, info->focus_reason,
        info->should_do_learning);
    engine_->FocusIn(context);
  }
  void FinishInput() override { engine_->FocusOut(); }
  void CancelInput() override { engine_->Reset(); }
  void ProcessKeyEvent(
      std::unique_ptr<ui::Event> key_event,
      ime::mojom::ImeEngine::ProcessKeyEventCallback cb) override {
    engine_->ProcessKeyEvent(*(key_event->AsKeyEvent()), std::move(cb));
  }
  void UpdateSurroundingInfo(const std::string& text,
                             int32_t cursor,
                             int32_t anchor,
                             int32_t offset) override {
    engine_->SetSurroundingText(text, cursor, anchor, offset);
  }
  void UpdateCompositionBounds(const std::vector<gfx::Rect>& bounds) override {
    engine_->SetCompositionBounds(bounds);
  }

  ime::mojom::ImeEngineClientPtr& engine_client() { return engine_client_; }

  void FlushForTesting() {
    if (registry_)
      registry_.FlushForTesting();
    if (engine_client_)
      engine_client_.FlushForTesting();
  }

 private:
  InputMethodEngine* engine_;
  mojo::Binding<ime::mojom::ImeEngineFactory> factory_binding_;
  mojo::Binding<ime::mojom::ImeEngine> engine_binding_;

  ime::mojom::ImeEngineClientPtr engine_client_;
  ime::mojom::ImeEngineFactoryRegistryPtr registry_;

  DISALLOW_COPY_AND_ASSIGN(MojoHelper);
};

InputMethodEngine::Candidate::Candidate() {}

InputMethodEngine::Candidate::Candidate(const Candidate& other) = default;

InputMethodEngine::Candidate::~Candidate() {}

// When the default values are changed, please modify
// CandidateWindow::CandidateWindowProperty defined in chromeos/ime/ too.
InputMethodEngine::CandidateWindowProperty::CandidateWindowProperty()
    : page_size(kDefaultPageSize),
      is_cursor_visible(true),
      is_vertical(false),
      show_window_at_composition(false) {}

InputMethodEngine::CandidateWindowProperty::~CandidateWindowProperty() {}

InputMethodEngine::InputMethodEngine()
    : candidate_window_(new ui::CandidateWindow()),
      window_visible_(false),
      is_mirroring_(false),
      is_casting_(false) {}

InputMethodEngine::~InputMethodEngine() {}

void InputMethodEngine::Enable(const std::string& component_id) {
  InputMethodEngineBase::Enable(component_id);
  EnableInputView();
  mojo_helper_ = std::make_unique<MojoHelper>(
      this, std::move(ime_engine_factory_registry_));
}

void InputMethodEngine::Disable() {
  InputMethodEngineBase::Disable();
  mojo_helper_.reset();
}

bool InputMethodEngine::IsActive() const {
  return !active_component_id_.empty();
}

void InputMethodEngine::PropertyActivate(const std::string& property_name) {
  observer_->OnMenuItemActivated(active_component_id_, property_name);
}

void InputMethodEngine::CandidateClicked(uint32_t index) {
  if (index > candidate_ids_.size()) {
    return;
  }

  // Only left button click is supported at this moment.
  observer_->OnCandidateClicked(active_component_id_, candidate_ids_.at(index),
                                InputMethodEngineBase::MOUSE_BUTTON_LEFT);
}

void InputMethodEngine::SetMirroringEnabled(bool mirroring_enabled) {
  if (mirroring_enabled != is_mirroring_) {
    is_mirroring_ = mirroring_enabled;
    observer_->OnScreenProjectionChanged(is_mirroring_ || is_casting_);
  }
}

void InputMethodEngine::SetCastingEnabled(bool casting_enabled) {
  if (casting_enabled != is_casting_) {
    is_casting_ = casting_enabled;
    observer_->OnScreenProjectionChanged(is_mirroring_ || is_casting_);
  }
}

void InputMethodEngine::FlushForTesting() {
  mojo_helper_->FlushForTesting();
}

const InputMethodEngine::CandidateWindowProperty&
InputMethodEngine::GetCandidateWindowProperty() const {
  return candidate_window_property_;
}

void InputMethodEngine::SetCandidateWindowProperty(
    const CandidateWindowProperty& property) {
  // Type conversion from InputMethodEngine::CandidateWindowProperty to
  // CandidateWindow::CandidateWindowProperty defined in chromeos/ime/.
  ui::CandidateWindow::CandidateWindowProperty dest_property;
  dest_property.page_size = property.page_size;
  dest_property.is_cursor_visible = property.is_cursor_visible;
  dest_property.is_vertical = property.is_vertical;
  dest_property.show_window_at_composition =
      property.show_window_at_composition;
  dest_property.cursor_position =
      candidate_window_->GetProperty().cursor_position;
  dest_property.auxiliary_text = property.auxiliary_text;
  dest_property.is_auxiliary_text_visible = property.is_auxiliary_text_visible;

  candidate_window_->SetProperty(dest_property);
  candidate_window_property_ = property;

  if (IsActive()) {
    IMECandidateWindowHandlerInterface* cw_handler =
        ui::IMEBridge::Get()->GetCandidateWindowHandler();
    if (cw_handler)
      cw_handler->UpdateLookupTable(*candidate_window_, window_visible_);
  }
}

bool InputMethodEngine::SetCandidateWindowVisible(bool visible,
                                                  std::string* error) {
  if (!IsActive()) {
    *error = kErrorNotActive;
    return false;
  }

  window_visible_ = visible;
  IMECandidateWindowHandlerInterface* cw_handler =
      ui::IMEBridge::Get()->GetCandidateWindowHandler();
  if (cw_handler)
    cw_handler->UpdateLookupTable(*candidate_window_, window_visible_);
  return true;
}

bool InputMethodEngine::SetCandidates(
    int context_id,
    const std::vector<Candidate>& candidates,
    std::string* error) {
  if (!IsActive()) {
    *error = kErrorNotActive;
    return false;
  }
  if (context_id != context_id_ || context_id_ == -1) {
    *error = kErrorWrongContext;
    return false;
  }

  // TODO: Nested candidates
  candidate_ids_.clear();
  candidate_indexes_.clear();
  candidate_window_->mutable_candidates()->clear();
  for (std::vector<Candidate>::const_iterator ix = candidates.begin();
       ix != candidates.end(); ++ix) {
    ui::CandidateWindow::Entry entry;
    entry.value = base::UTF8ToUTF16(ix->value);
    entry.label = base::UTF8ToUTF16(ix->label);
    entry.annotation = base::UTF8ToUTF16(ix->annotation);
    entry.description_title = base::UTF8ToUTF16(ix->usage.title);
    entry.description_body = base::UTF8ToUTF16(ix->usage.body);

    // Store a mapping from the user defined ID to the candidate index.
    candidate_indexes_[ix->id] = candidate_ids_.size();
    candidate_ids_.push_back(ix->id);

    candidate_window_->mutable_candidates()->push_back(entry);
  }
  if (IsActive()) {
    IMECandidateWindowHandlerInterface* cw_handler =
        ui::IMEBridge::Get()->GetCandidateWindowHandler();
    if (cw_handler)
      cw_handler->UpdateLookupTable(*candidate_window_, window_visible_);
  }
  return true;
}

bool InputMethodEngine::SetCursorPosition(int context_id,
                                          int candidate_id,
                                          std::string* error) {
  if (!IsActive()) {
    *error = kErrorNotActive;
    return false;
  }
  if (context_id != context_id_ || context_id_ == -1) {
    *error = kErrorWrongContext;
    return false;
  }

  std::map<int, int>::const_iterator position =
      candidate_indexes_.find(candidate_id);
  if (position == candidate_indexes_.end()) {
    *error = kCandidateNotFound;
    return false;
  }

  candidate_window_->set_cursor_position(position->second);
  IMECandidateWindowHandlerInterface* cw_handler =
      ui::IMEBridge::Get()->GetCandidateWindowHandler();
  if (cw_handler)
    cw_handler->UpdateLookupTable(*candidate_window_, window_visible_);
  return true;
}

bool InputMethodEngine::SetMenuItems(
    const std::vector<input_method::InputMethodManager::MenuItem>& items) {
  return UpdateMenuItems(items);
}

bool InputMethodEngine::UpdateMenuItems(
    const std::vector<input_method::InputMethodManager::MenuItem>& items) {
  if (!IsActive())
    return false;

  ui::ime::InputMethodMenuItemList menu_item_list;
  for (std::vector<input_method::InputMethodManager::MenuItem>::const_iterator
           item = items.begin();
       item != items.end(); ++item) {
    ui::ime::InputMethodMenuItem property;
    MenuItemToProperty(*item, &property);
    menu_item_list.push_back(property);
  }

  ui::ime::InputMethodMenuManager::GetInstance()
      ->SetCurrentInputMethodMenuItemList(menu_item_list);

  input_method::InputMethodManager::Get()->NotifyImeMenuItemsChanged(
      active_component_id_, items);
  return true;
}

void InputMethodEngine::HideInputView() {
  auto* keyboard_client = ChromeKeyboardControllerClient::Get();
  if (keyboard_client->is_keyboard_enabled())
    keyboard_client->HideKeyboard(ash::mojom::HideReason::kUser);
}

void InputMethodEngine::UpdateComposition(
    const ui::CompositionText& composition_text,
    uint32_t cursor_pos,
    bool is_visible) {
  ui::IMEInputContextHandlerInterface* input_context =
      ui::IMEBridge::Get()->GetInputContextHandler();
  if (input_context)
    input_context->UpdateCompositionText(composition_text, cursor_pos,
                                         is_visible);
}

void InputMethodEngine::CommitTextToInputContext(int context_id,
                                                 const std::string& text) {
  ui::IMEBridge::Get()->GetInputContextHandler()->CommitText(text);

  // Records histograms for committed characters.
  if (!composition_text_->text.empty()) {
    base::string16 wtext = base::UTF8ToUTF16(text);
    UMA_HISTOGRAM_CUSTOM_COUNTS("InputMethod.CommitLength", wtext.length(), 1,
                                25, 25);
    composition_text_.reset(new ui::CompositionText());
  }
}

bool InputMethodEngine::SendKeyEvent(ui::KeyEvent* event,
                                     const std::string& code) {
  DCHECK(event);
  if (event->key_code() == ui::VKEY_UNKNOWN)
    event->set_key_code(ui::DomKeycodeToKeyboardCode(code));

  ui::IMEInputContextHandlerInterface* input_context =
      ui::IMEBridge::Get()->GetInputContextHandler();
  if (!input_context)
    return false;

  // Marks the simulated key event is from the Virtual Keyboard.
  ui::Event::Properties properties;
  properties[ui::kPropertyFromVK] = std::vector<uint8_t>();
  event->SetProperties(properties);

  input_context->SendKeyEvent(event);
  return true;
}

void InputMethodEngine::EnableInputView() {
  input_method::InputMethodManager::Get()
      ->GetActiveIMEState()
      ->EnableInputView();
  auto* keyboard_client = ChromeKeyboardControllerClient::Get();
  if (keyboard_client->is_keyboard_enabled())
    keyboard_client->ReloadKeyboardIfNeeded();
}


// TODO(uekawa): rename this method to a more reasonable name.
void InputMethodEngine::MenuItemToProperty(
    const input_method::InputMethodManager::MenuItem& item,
    ui::ime::InputMethodMenuItem* property) {
  property->key = item.id;

  if (item.modified & MENU_ITEM_MODIFIED_LABEL) {
    property->label = item.label;
  }
  if (item.modified & MENU_ITEM_MODIFIED_VISIBLE) {
    // TODO(nona): Implement it.
  }
  if (item.modified & MENU_ITEM_MODIFIED_CHECKED) {
    property->is_selection_item_checked = item.checked;
  }
  if (item.modified & MENU_ITEM_MODIFIED_ENABLED) {
    // TODO(nona): implement sensitive entry(crbug.com/140192).
  }
  if (item.modified & MENU_ITEM_MODIFIED_STYLE) {
    if (!item.children.empty()) {
      // TODO(nona): Implement it.
    } else {
      switch (item.style) {
        case input_method::InputMethodManager::MENU_ITEM_STYLE_NONE:
          NOTREACHED();
          break;
        case input_method::InputMethodManager::MENU_ITEM_STYLE_CHECK:
          // TODO(nona): Implement it.
          break;
        case input_method::InputMethodManager::MENU_ITEM_STYLE_RADIO:
          property->is_selection_item = true;
          break;
        case input_method::InputMethodManager::MENU_ITEM_STYLE_SEPARATOR:
          // TODO(nona): Implement it.
          break;
      }
    }
  }

  // TODO(nona): Support item.children.
}

}  // namespace chromeos
