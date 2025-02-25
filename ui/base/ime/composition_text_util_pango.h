// Copyright 2013 The Chromium Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#ifndef UI_BASE_IME_COMPOSITION_TEXT_UTIL_PANGO_H_
#define UI_BASE_IME_COMPOSITION_TEXT_UTIL_PANGO_H_

#include "base/component_export.h"
#include "ui/base/glib/glib_integers.h"

typedef struct _PangoAttrList PangoAttrList;

namespace ui {

struct CompositionText;

// Extracts composition text information (text, underlines, selection range)
// from given Gtk preedit data (utf-8 text, pango attributes, cursor position).
COMPONENT_EXPORT(UI_BASE_IME)
void ExtractCompositionTextFromGtkPreedit(const gchar* utf8_text,
                                          PangoAttrList* attrs,
                                          int cursor_position,
                                          CompositionText* composition);

}  // namespace ui

#endif  // UI_BASE_IME_COMPOSITION_TEXT_UTIL_PANGO_H_
