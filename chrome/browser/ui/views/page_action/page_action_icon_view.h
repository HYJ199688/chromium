// Copyright 2013 The Chromium Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#ifndef CHROME_BROWSER_UI_VIEWS_PAGE_ACTION_PAGE_ACTION_ICON_VIEW_H_
#define CHROME_BROWSER_UI_VIEWS_PAGE_ACTION_PAGE_ACTION_ICON_VIEW_H_

#include <memory>

#include "base/macros.h"
#include "base/scoped_observer.h"
#include "chrome/browser/ui/layout_constants.h"
#include "chrome/browser/ui/omnibox/omnibox_theme.h"
#include "chrome/browser/ui/views/location_bar/icon_label_bubble_view.h"
#include "third_party/skia/include/core/SkColor.h"
#include "ui/gfx/color_palette.h"
#include "ui/gfx/image/image_skia.h"
#include "ui/views/animation/ink_drop_host_view.h"
#include "ui/views/controls/image_view.h"

class CommandUpdater;

namespace content {
class WebContents;
}

namespace gfx {
struct VectorIcon;
}

namespace views {
class BubbleDialogDelegateView;
}

// Represents an inbuilt (as opposed to an extension) page action icon that
// shows a bubble when clicked.
class PageActionIconView : public IconLabelBubbleView {
 public:
  class Delegate {
   public:
    // Gets the color to use for the ink highlight.
    virtual SkColor GetPageActionInkDropColor() const = 0;

    virtual content::WebContents* GetWebContentsForPageActionIconView() = 0;
  };

  // Updates the color of the icon, this must be set before the icon is drawn.
  void SetIconColor(SkColor icon_color);

  void set_icon_size(int size) { icon_size_ = size; }

  // Returns the bubble instance for the icon.
  virtual views::BubbleDialogDelegateView* GetBubble() const = 0;

  // Updates the icon state and associated bubble when the WebContents changes.
  // Returns true if there was a change.
  virtual bool Update();

  // Retrieve the text to be used for a tooltip or accessible name.
  virtual base::string16 GetTextForTooltipAndAccessibleName() const = 0;

 protected:
  enum ExecuteSource {
    EXECUTE_SOURCE_MOUSE,
    EXECUTE_SOURCE_KEYBOARD,
    EXECUTE_SOURCE_GESTURE,
  };

  PageActionIconView(CommandUpdater* command_updater,
                     int command_id,
                     Delegate* delegate,
                     const gfx::FontList& = gfx::FontList());
  ~PageActionIconView() override;

  // Returns true if a related bubble is showing.
  bool IsBubbleShowing() const override;

  // Enables or disables the associated command.
  // Returns true if the command is enabled.
  bool SetCommandEnabled(bool enabled) const;

  // Sets the tooltip text.
  void SetTooltipText(const base::string16& tooltip);

  // Invoked prior to executing the command.
  virtual void OnExecuting(ExecuteSource execute_source) = 0;

  // Invoked after the icon is pressed.
  virtual void OnPressed(bool activated) {}

  // views::IconLabelBubbleView:
  SkColor GetTextColor() const override;
  void GetAccessibleNodeData(ui::AXNodeData* node_data) override;
  base::string16 GetTooltipText(const gfx::Point& p) const override;
  bool OnMousePressed(const ui::MouseEvent& event) override;
  void OnMouseReleased(const ui::MouseEvent& event) override;
  bool OnKeyPressed(const ui::KeyEvent& event) override;
  bool OnKeyReleased(const ui::KeyEvent& event) override;
  void ViewHierarchyChanged(
      const views::ViewHierarchyChangedDetails& details) override;
  void OnNativeThemeChanged(const ui::NativeTheme* theme) override;
  void OnThemeChanged() override;
  SkColor GetInkDropBaseColor() const override;
  bool ShouldShowSeparator() const final;

  // ui::EventHandler:
  void OnGestureEvent(ui::GestureEvent* event) override;

 protected:
  // Calls OnExecuting and runs |command_id_| with a valid |command_updater_|.
  virtual void ExecuteCommand(ExecuteSource source);

  // Gets the given vector icon.
  virtual const gfx::VectorIcon& GetVectorIcon() const = 0;

  // Provides the badge to be shown on top of the vector icon, if any.
  virtual const gfx::VectorIcon& GetVectorIconBadge() const;

  // IconLabelBubbleView:
  void OnBoundsChanged(const gfx::Rect& previous_bounds) override;
  void OnTouchUiChanged() override;
  void UpdateBorder() override;

  // Updates the icon image after some state has changed.
  virtual void UpdateIconImage();

  // Sets the active state of the icon. An active icon will be displayed in a
  // "call to action" color.
  void SetActiveInternal(bool active);

  // Returns the associated web contents from the delegate.
  content::WebContents* GetWebContents() const;

  bool active() const { return active_; }

 private:
  // The size of the icon image (excluding the ink drop).
  int icon_size_ = GetLayoutConstant(LOCATION_BAR_ICON_SIZE);

  // What color to paint the icon with.
  SkColor icon_color_ = gfx::kPlaceholderColor;

  // The CommandUpdater for the Browser object that owns the location bar.
  CommandUpdater* command_updater_;

  // Delegate for access to associated state.
  Delegate* delegate_;

  // The command ID executed when the user clicks this icon.
  const int command_id_;

  // The active node_data. The precise definition of "active" is unique to each
  // subclass, but generally indicates that the associated feature is acting on
  // the web page.
  bool active_ = false;

  // This is used to check if the bookmark bubble was showing during the mouse
  // pressed event. If this is true then the mouse released event is ignored to
  // prevent the bubble from reshowing.
  bool suppress_mouse_released_action_ = false;

  DISALLOW_COPY_AND_ASSIGN(PageActionIconView);
};

#endif  // CHROME_BROWSER_UI_VIEWS_PAGE_ACTION_PAGE_ACTION_ICON_VIEW_H_
