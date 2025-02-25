// Copyright (c) 2019 The Chromium Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include "third_party/blink/renderer/core/html/forms/internal_popup_menu.h"

#include "build/build_config.h"
#include "testing/gtest/include/gtest/gtest.h"
#include "third_party/blink/renderer/core/dom/document.h"
#include "third_party/blink/renderer/core/dom/element.h"
#include "third_party/blink/renderer/core/html/forms/html_select_element.h"
#include "third_party/blink/renderer/core/loader/empty_clients.h"
#include "third_party/blink/renderer/core/testing/dummy_page_holder.h"
#include "third_party/blink/renderer/platform/shared_buffer.h"

namespace blink {

// InternalPopupMenuTest is not used on Android, and its Platform implementation
// does not provide the resources (as in GetDataResource) needed by
// InternalPopupMenu::WriteDocument.
#if !defined(OS_ANDROID)

TEST(InternalPopupMenuTest, WriteDocumentInStyleDirtyTree) {
  auto dummy_page_holder_ =
      std::make_unique<DummyPageHolder>(IntSize(800, 600));
  Document& document = dummy_page_holder_->GetDocument();
  document.body()->SetInnerHTMLFromString(R"HTML(
    <select id="select">
        <option value="foo">Foo</option>
        <option value="bar" style="display:none">Bar</option>
    </select>
  )HTML");
  document.View()->UpdateAllLifecyclePhases(
      DocumentLifecycle::LifecycleUpdateReason::kTest);
  HTMLSelectElement* select =
      ToHTMLSelectElement(document.getElementById("select"));
  ASSERT_TRUE(select);
  InternalPopupMenu* menu =
      InternalPopupMenu::Create(EmptyChromeClient::Create(), *select);

  document.body()->SetInlineStyleProperty(CSSPropertyID::kColor, "blue");

  scoped_refptr<SharedBuffer> buffer = SharedBuffer::Create();

  // Don't DCHECK in Element::EnsureComputedStyle.
  static_cast<PagePopupClient*>(menu)->WriteDocument(buffer.get());
}

#endif  // defined(OS_ANDROID)

}  // namespace blink
