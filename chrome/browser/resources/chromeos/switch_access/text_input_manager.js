// Copyright 2018 The Chromium Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

/**
 * Class to handle text input for improved accuracy and efficiency.
 */

class TextInputManager {
  /** @param {!NavigationManager} navigationManager */
  constructor(navigationManager) {
    /** @private {chrome.automation.AutomationNode} */
    this.node_;

    /** @private {!NavigationManager} */
    this.navigationManager_ = navigationManager;

    /** @private {!chrome.accessibilityPrivate.FocusRingInfo} */
    this.textInputFocusRing_ = {
      id: SAConstants.TEXT_FOCUS_ID,
      rects: [],
      type: chrome.accessibilityPrivate.FocusType.DASHED,
      color: SAConstants.PRIMARY_FOCUS_COLOR,
      secondaryColor: SAConstants.SECONDARY_FOCUS_COLOR
    };
  }

  /**
   * Enters the keyboard and draws the text input focus ring.
   * @param {!chrome.automation.AutomationNode} node
   */
  enterKeyboard(node) {
    if (!SwitchAccessPredicate.isTextInput(node))
      return false;

    this.node_ = node;
    this.drawFocusRingForTextInput_();
    return true;
  }

  /** Resets the focus ring and the focus in |navigationManager_|. */
  returnToTextFocus() {
    if (!this.node_)
      return;
    this.clearFocusRingForTextInput_();
    this.navigationManager_.exitCurrentScope(this.node_);
    this.node_ = null;
  }

  /**
   * Sends a keyEvent to click the center of the provided node.
   * @param {!chrome.automation.AutomationNode} node
   * @return {boolean} Whether a key was pressed.
   */
  pressKey(node) {
    if (node.role !== chrome.automation.RoleType.BUTTON)
      return false;
    if (!this.inVirtualKeyboard(node))
      return false;

    const x = node.location.left + Math.round(node.location.width / 2);
    const y = node.location.top + Math.round(node.location.height / 2);

    chrome.accessibilityPrivate.sendSyntheticMouseEvent({
      type: chrome.accessibilityPrivate.SyntheticMouseEventType.PRESS,
      x: x,
      y: y
    });

    setTimeout(
        () => chrome.accessibilityPrivate.sendSyntheticMouseEvent({
          type: chrome.accessibilityPrivate.SyntheticMouseEventType.RELEASE,
          x: x,
          y: y
        }),
        SAConstants.KEY_PRESS_DURATION_MS);

    return true;
  }

  /**
   * Returns the container with a role of keyboard.
   * @param {!chrome.automation.AutomationNode} desktop
   * @return {chrome.automation.AutomationNode}
   */
  getKeyboard(desktop) {
    const treeWalker = new AutomationTreeWalker(
        desktop, constants.Dir.FORWARD,
        {visit: (node) => node.role === chrome.automation.RoleType.KEYBOARD});
    treeWalker.next();
    return treeWalker.node;
  }

  /**
   * Checks if |node| is in the virtual keyboard.
   * @param {!chrome.automation.AutomationNode} node
   * @return {boolean}
   */
  inVirtualKeyboard(node) {
    if (node.role === chrome.automation.RoleType.KEYBOARD)
      return true;
    if (node.parent)
      return this.inVirtualKeyboard(node.parent);
    return false;
  }

  /**
   * Draws a dashed focus ring around the active text input, so the user can
   * easily reference where they are typing.
   * @private
   */
  drawFocusRingForTextInput_() {
    if (!this.node_)
      return;

    this.textInputFocusRing_.rects = [this.node_.location];
    chrome.accessibilityPrivate.setFocusRings([this.textInputFocusRing_]);
    return true;
  }

  /**
   * Clears the focus ring around the active text input.
   * @private
   */
  clearFocusRingForTextInput_() {
    this.textInputFocusRing_.rects = [];
    chrome.accessibilityPrivate.setFocusRings([this.textInputFocusRing_]);
  }
}
