// Copyright 2018 The Chromium Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include "ash/shelf/shelf_tooltip_preview_bubble.h"

#include "ash/shelf/shelf_widget.h"
#include "ash/wm/pip/pip_positioner.h"
#include "base/strings/utf_string_conversions.h"
#include "ui/views/bubble/bubble_frame_view.h"

namespace ash {


// The padding inside the tooltip.
constexpr int kTooltipPaddingTop = 8;
constexpr int kTooltipPaddingBottom = 16;
constexpr int kTooltipPaddingLeftRight = 16;

// The padding between individual previews.
constexpr int kPreviewPadding = 10;

// The border radius of the whole bubble
constexpr int kPreviewBorderRadius = 16;

ShelfTooltipPreviewBubble::ShelfTooltipPreviewBubble(
    views::View* anchor,
    const std::vector<aura::Window*>& windows,
    ShelfTooltipManager* manager,
    ShelfAlignment alignment,
    SkColor background_color)
    : ShelfBubble(anchor, alignment, background_color), manager_(manager) {
  set_border_radius(kPreviewBorderRadius);
  set_margins(gfx::Insets(kTooltipPaddingTop, kTooltipPaddingLeftRight,
                          kTooltipPaddingBottom, kTooltipPaddingLeftRight));
  const ui::NativeTheme* theme = anchor_widget()->GetNativeTheme();

  for (auto* window : windows) {
    WindowPreview* preview = new WindowPreview(window, this, theme);
    AddChildView(preview);
    previews_.push_back(preview);
  }

  CreateBubble();
  PipPositioner::MarkWindowAsIgnoredForCollisionDetection(
      GetWidget()->GetNativeWindow());
}

ShelfTooltipPreviewBubble::~ShelfTooltipPreviewBubble() = default;

void ShelfTooltipPreviewBubble::Layout() {
  width_ = 0;
  height_ = 0;
  for (WindowPreview* preview : previews_) {
    preview->Layout();
    gfx::Size size = preview->CalculatePreferredSize();
    preview->SetBoundsRect(gfx::Rect(width_, 0, size.width(), size.height()));
    height_ = std::max(height_, size.height());
    width_ += size.width() + kPreviewPadding;
  }

  // We've counted one too many paddings at the end.
  width_ -= kPreviewPadding;
}

void ShelfTooltipPreviewBubble::RemovePreview(WindowPreview* to_remove) {
  base::Erase(previews_, to_remove);
  RemoveChildView(to_remove);
  // If we don't have any previews left, close the tooltip.
  if (previews_.empty()) {
    manager_->Close();
  }
}

gfx::Size ShelfTooltipPreviewBubble::CalculatePreferredSize() const {
  if (previews_.empty()) {
    return BubbleDialogDelegateView::CalculatePreferredSize();
  }
  return gfx::Size(width_, height_);
}

bool ShelfTooltipPreviewBubble::ShouldCloseOnPressDown() {
  return false;
}

bool ShelfTooltipPreviewBubble::ShouldCloseOnMouseExit() {
  return false;
}

void ShelfTooltipPreviewBubble::OnPreviewDismissed(WindowPreview* preview) {
  RemovePreview(preview);
}

void ShelfTooltipPreviewBubble::OnPreviewActivated(WindowPreview* preview) {
  // Always close the tooltip when a window has been focused.
  manager_->Close();
}

}  // namespace ash
