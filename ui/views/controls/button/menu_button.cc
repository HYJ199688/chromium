// Copyright (c) 2012 The Chromium Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include "ui/views/controls/button/menu_button.h"

#include "ui/events/event.h"
#include "ui/events/event_constants.h"
#include "ui/gfx/canvas.h"
#include "ui/gfx/text_constants.h"
#include "ui/views/controls/button/menu_button_listener.h"
#include "ui/views/mouse_constants.h"

namespace views {
// static
const char MenuButton::kViewClassName[] = "MenuButton";
constexpr int MenuButton::kMenuMarkerPaddingLeft = 3;
constexpr int MenuButton::kMenuMarkerPaddingRight = -1;

MenuButton::MenuButton(const base::string16& text,
                       MenuButtonListener* menu_button_listener,
                       int button_context)
    : LabelButton(nullptr, text, button_context),
      menu_button_controller_(this, menu_button_listener) {
  SetHorizontalAlignment(gfx::ALIGN_LEFT);
}

MenuButton::~MenuButton() = default;

bool MenuButton::Activate(const ui::Event* event) {
  return menu_button_controller_.Activate(event);
}

bool MenuButton::IsTriggerableEventType(const ui::Event& event) {
  return menu_button_controller_.IsTriggerableEventType(event);
}

const char* MenuButton::GetClassName() const {
  return kViewClassName;
}

bool MenuButton::OnMousePressed(const ui::MouseEvent& event) {
  return menu_button_controller_.OnMousePressed(event);
}

void MenuButton::OnMouseReleased(const ui::MouseEvent& event) {
  menu_button_controller_.OnMouseReleased(event);
}

bool MenuButton::OnKeyPressed(const ui::KeyEvent& event) {
  return menu_button_controller_.OnKeyPressed(event);
}

bool MenuButton::OnKeyReleased(const ui::KeyEvent& event) {
  return menu_button_controller_.OnKeyReleased(event);
}

void MenuButton::GetAccessibleNodeData(ui::AXNodeData* node_data) {
  menu_button_controller_.GetAccessibleNodeData(node_data);
}

void MenuButton::OnMouseEntered(const ui::MouseEvent& event) {}
void MenuButton::OnMouseExited(const ui::MouseEvent& event) {}
void MenuButton::OnMouseMoved(const ui::MouseEvent& event) {}
void MenuButton::OnGestureEvent(ui::GestureEvent* event) {}

void MenuButton::LabelButtonStateChanged(ButtonState old_state) {
  LabelButton::StateChanged(old_state);
}

bool MenuButton::IsTriggerableEvent(const ui::Event& event) {
  return menu_button_controller_.IsTriggerableEvent(event);
}

bool MenuButton::ShouldEnterPushedState(const ui::Event& event) {
  return menu_button_controller_.ShouldEnterPushedState(event);
}

void MenuButton::StateChanged(ButtonState old_state) {
  menu_button_controller_.StateChanged(old_state);
}

void MenuButton::NotifyClick(const ui::Event& event) {
  menu_button_controller_.NotifyClick(event);
}

}  // namespace views
