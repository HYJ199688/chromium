// Copyright 2018 The Chromium Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#ifndef UI_BASE_IME_INPUT_METHOD_WIN_BASE_H_
#define UI_BASE_IME_INPUT_METHOD_WIN_BASE_H_

#include <windows.h>

#include <string>

#include "base/compiler_specific.h"
#include "base/component_export.h"
#include "base/macros.h"
#include "ui/base/ime/input_method_base.h"
#include "ui/base/ime/win/imm32_manager.h"

namespace ui {

// A common InputMethod base implementation for Windows.
class COMPONENT_EXPORT(UI_BASE_IME) InputMethodWinBase
    : public InputMethodBase {
 public:
  InputMethodWinBase(internal::InputMethodDelegate* delegate,
                     HWND toplevel_window_handle);
  ~InputMethodWinBase() override;

 protected:
  void OnDidChangeFocusedClient(TextInputClient* focused_before,
                                TextInputClient* focused) override;
  ui::EventDispatchDetails DispatchKeyEvent(ui::KeyEvent* event) override;

  // Returns true if the Win32 native window bound to |client| is considered
  // to be ready for receiving keyboard input.
  bool IsWindowFocused(const TextInputClient* client) const;

  // For both WM_CHAR and WM_SYSCHAR
  LRESULT OnChar(HWND window_handle,
                 UINT message,
                 WPARAM wparam,
                 LPARAM lparam,
                 const MSG& event,
                 BOOL* handled);

  // Some IMEs rely on WM_IME_REQUEST message even when TSF is enabled. So
  // OnImeRequest (and its actual implementations as OnDocumentFeed,
  // OnReconvertString, and OnQueryCharPosition) are placed in this base class.
  LRESULT OnImeRequest(UINT message,
                       WPARAM wparam,
                       LPARAM lparam,
                       BOOL* handled);
  LRESULT OnDocumentFeed(RECONVERTSTRING* reconv);
  LRESULT OnReconvertString(RECONVERTSTRING* reconv);
  LRESULT OnQueryCharPosition(IMECHARPOSITION* char_positon);

  // Callback function for IMEEngineHandlerInterface::ProcessKeyEvent.
  void ProcessKeyEventDone(ui::KeyEvent* event,
                           const std::vector<MSG>* char_msgs,
                           bool is_handled);

  ui::EventDispatchDetails ProcessUnhandledKeyEvent(
      ui::KeyEvent* event,
      const std::vector<MSG>* char_msgs);

  // Update composition bounds for Chromium IME extension.
  void UpdateCompositionBoundsForEngine(const TextInputClient* client);

  // Reset composition status for Chromium IME extension.
  void ResetEngine();

  // Cancel composition for Chromium IME extension.
  void CancelCompositionForEngine();

  // Update focus state for Chromium IME extension and update input context in
  // ui::IMEBridge.
  void UpdateEngineFocusAndInputContext();

  // The toplevel window handle.
  const HWND toplevel_window_handle_;

  // Represents if WM_CHAR[wparam=='\r'] should be dispatched to the focused
  // text input client or ignored silently. This flag is introduced as a quick
  // workaround against https://crbug.com/319100
  // TODO(yukawa, IME): Figure out long-term solution.
  bool accept_carriage_return_ = false;

  // The new text direction and layout alignment requested by the user by
  // pressing ctrl-shift. It'll be sent to the text input client when the key
  // is released.
  base::i18n::TextDirection pending_requested_direction_;

  // Used for making callbacks.
  base::WeakPtrFactory<InputMethodWinBase> weak_ptr_factory_;

  DISALLOW_COPY_AND_ASSIGN(InputMethodWinBase);
};

}  // namespace ui

#endif  // UI_BASE_IME_INPUT_METHOD_WIN_BASE_H_
