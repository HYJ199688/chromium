// Copyright 2018 The Chromium Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#ifndef ASH_ASSISTANT_UI_ASSISTANT_CONTAINER_VIEW_H_
#define ASH_ASSISTANT_UI_ASSISTANT_CONTAINER_VIEW_H_

#include <memory>

#include "ash/assistant/model/assistant_ui_model_observer.h"
#include "ash/assistant/ui/assistant_container_view_focus_traversable.h"
#include "base/component_export.h"
#include "base/macros.h"
#include "ui/views/bubble/bubble_dialog_delegate_view.h"

namespace ash {

class AssistantContainerViewAnimator;
class AssistantMainView;
class AssistantMiniView;
class AssistantViewDelegate;
class AssistantWebView;

class COMPONENT_EXPORT(ASSISTANT_UI) AssistantContainerView
    : public views::BubbleDialogDelegateView,
      public AssistantUiModelObserver {
 public:
  explicit AssistantContainerView(AssistantViewDelegate* delegate);
  ~AssistantContainerView() override;

  // views::BubbleDialogDelegateView:
  const char* GetClassName() const override;
  void AddedToWidget() override;
  ax::mojom::Role GetAccessibleWindowRole() const override;
  base::string16 GetAccessibleWindowTitle() const override;
  int GetDialogButtons() const override;
  views::FocusTraversable* GetFocusTraversable() override;
  void ChildPreferredSizeChanged(views::View* child) override;
  void ViewHierarchyChanged(
      const views::ViewHierarchyChangedDetails& details) override;
  void SizeToContents() override;
  void OnBeforeBubbleWidgetInit(views::Widget::InitParams* params,
                                views::Widget* widget) const override;
  views::ClientView* CreateClientView(views::Widget* widget) override;
  void Init() override;
  void RequestFocus() override;

  // AssistantUiModelObserver:
  void OnUiModeChanged(AssistantUiMode ui_mode) override;
  void OnUsableWorkAreaChanged(const gfx::Rect& usable_work_area) override;

  // Returns the first focusable view or nullptr to defer to views::FocusSearch.
  views::View* FindFirstFocusableView();

  // Returns background color.
  SkColor GetBackgroundColor() const;

  // Returns/sets corner radius.
  int GetCornerRadius() const;
  void SetCornerRadius(int corner_radius);

  // Returns the layer for the non-client view.
  ui::Layer* GetNonClientViewLayer();

 private:
  // Update anchor rect with respect to the current usable work area.
  void UpdateAnchor();

  AssistantViewDelegate* const delegate_;

  AssistantMainView* assistant_main_view_;  // Owned by view hierarchy.
  AssistantMiniView* assistant_mini_view_;  // Owned by view hierarchy.
  AssistantWebView* assistant_web_view_;    // Owned by view hierarchy.

  std::unique_ptr<AssistantContainerViewAnimator> animator_;
  AssistantContainerViewFocusTraversable focus_traversable_;

  DISALLOW_COPY_AND_ASSIGN(AssistantContainerView);
};

}  // namespace ash

#endif  // ASH_ASSISTANT_UI_ASSISTANT_CONTAINER_VIEW_H_
