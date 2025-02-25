// Copyright (c) 2012 The Chromium Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

cr.define('cr.ui.dialogs', function() {
  /**
   * @constructor
   */
  function BaseDialog(parentNode) {
    this.parentNode_ = parentNode;
    this.document_ = parentNode.ownerDocument;

    // The DOM element from the dialog which should receive focus when the
    // dialog is first displayed.
    this.initialFocusElement_ = null;

    // The DOM element from the parent which had focus before we were displayed,
    // so we can restore it when we're hidden.
    this.previousActiveElement_ = null;

    this.initDom_();

    /** @private{boolean} */
    this.showing_ = false;
  }

  /**
   * Default text for Ok and Cancel buttons.
   *
   * Clients should override these with localized labels.
   */
  BaseDialog.OK_LABEL = '[LOCALIZE ME] Ok';
  BaseDialog.CANCEL_LABEL = '[LOCALIZE ME] Cancel';

  /**
   * Number of miliseconds animation is expected to take, plus some margin for
   * error.
   */
  BaseDialog.ANIMATE_STABLE_DURATION = 500;

  /** @protected */
  BaseDialog.prototype.initDom_ = function() {
    const doc = this.document_;
    this.container_ = doc.createElement('div');
    this.container_.className = 'cr-dialog-container';
    this.container_.addEventListener(
        'keydown', this.onContainerKeyDown_.bind(this));
    this.shield_ = doc.createElement('div');
    this.shield_.className = 'cr-dialog-shield';
    this.container_.appendChild(this.shield_);
    this.container_.addEventListener(
        'mousedown', this.onContainerMouseDown_.bind(this));

    this.frame_ = doc.createElement('div');
    this.frame_.className = 'cr-dialog-frame';
    this.frame_.setAttribute('role', 'dialog');
    // Elements that have negative tabIndex can be focused but are not traversed
    // by Tab key.
    this.frame_.tabIndex = -1;
    this.container_.appendChild(this.frame_);

    this.title_ = doc.createElement('div');
    this.title_.className = 'cr-dialog-title';
    this.frame_.appendChild(this.title_);

    this.closeButton_ = doc.createElement('div');
    this.closeButton_.className = 'cr-dialog-close';
    this.closeButton_.addEventListener('click', this.onCancelClick_.bind(this));
    this.frame_.appendChild(this.closeButton_);

    this.text_ = doc.createElement('div');
    this.text_.className = 'cr-dialog-text';
    this.frame_.appendChild(this.text_);

    this.buttons = doc.createElement('div');
    this.buttons.className = 'cr-dialog-buttons';
    this.frame_.appendChild(this.buttons);

    this.okButton_ = doc.createElement('button');
    this.okButton_.className = 'cr-dialog-ok';
    this.okButton_.textContent = BaseDialog.OK_LABEL;
    this.okButton_.addEventListener('click', this.onOkClick_.bind(this));
    this.buttons.appendChild(this.okButton_);

    this.cancelButton_ = doc.createElement('button');
    this.cancelButton_.className = 'cr-dialog-cancel';
    this.cancelButton_.textContent = BaseDialog.CANCEL_LABEL;
    this.cancelButton_.addEventListener(
        'click', this.onCancelClick_.bind(this));
    this.buttons.appendChild(this.cancelButton_);

    this.initialFocusElement_ = this.okButton_;
  };

  /** @private {Function|undefined} */
  BaseDialog.prototype.onOk_ = null;

  /** @private {Function|undefined} */
  BaseDialog.prototype.onCancel_ = null;

  /** @protected */
  BaseDialog.prototype.onContainerKeyDown_ = function(event) {
    // Handle Escape.
    if (event.keyCode == 27 && !this.cancelButton_.disabled) {
      this.onCancelClick_(event);
      event.stopPropagation();
      // Prevent the event from being handled by the container of the dialog.
      // e.g. Prevent the parent container from closing at the same time.
      event.preventDefault();
    }
  };

  /** @private */
  BaseDialog.prototype.onContainerMouseDown_ = function(event) {
    if (event.target == this.container_) {
      const classList = this.container_.classList;
      // Start 'pulse' animation.
      classList.remove('pulse');
      setTimeout(classList.add.bind(classList, 'pulse'), 0);
      event.preventDefault();
    }
  };

  /** @private */
  BaseDialog.prototype.onOkClick_ = function(event) {
    this.hide();
    if (this.onOk_) {
      this.onOk_();
    }
  };

  /** @private */
  BaseDialog.prototype.onCancelClick_ = function(event) {
    this.hide();
    if (this.onCancel_) {
      this.onCancel_();
    }
  };

  /** @param {string} label */
  BaseDialog.prototype.setOkLabel = function(label) {
    this.okButton_.textContent = label;
  };

  /** @param {string} label */
  BaseDialog.prototype.setCancelLabel = function(label) {
    this.cancelButton_.textContent = label;
  };

  BaseDialog.prototype.setInitialFocusOnCancel = function() {
    this.initialFocusElement_ = this.cancelButton_;
  };

  /**
   * @param {string} message
   * @param {Function=} opt_onOk
   * @param {Function=} opt_onCancel
   * @param {Function=} opt_onShow
   */
  BaseDialog.prototype.show = function(
      message, opt_onOk, opt_onCancel, opt_onShow) {
    this.showWithTitle('', message, opt_onOk, opt_onCancel, opt_onShow);
  };

  /**
   * @param {string} title
   * @param {string} message
   * @param {Function=} opt_onOk
   * @param {Function=} opt_onCancel
   * @param {Function=} opt_onShow
   */
  BaseDialog.prototype.showHtml = function(
      title, message, opt_onOk, opt_onCancel, opt_onShow) {
    this.text_.innerHTML = message;
    this.show_(title, opt_onOk, opt_onCancel, opt_onShow);
  };

  /** @private */
  BaseDialog.prototype.findFocusableElements_ = function(doc) {
    let elements =
        Array.prototype.filter.call(doc.querySelectorAll('*'), function(n) {
          return n.tabIndex >= 0;
        });

    const iframes = doc.querySelectorAll('iframe');
    for (let i = 0; i < iframes.length; i++) {
      // Some iframes have an undefined contentDocument for security reasons,
      // such as chrome://terms (which is used in the chromeos OOBE screens).
      const iframe = iframes[i];
      let contentDoc;
      try {
        contentDoc = iframe.contentDocument;
      } catch (e) {
      }  // ignore SecurityError
      if (contentDoc) {
        elements = elements.concat(this.findFocusableElements_(contentDoc));
      }
    }
    return elements;
  };

  /**
   * @param {string} title
   * @param {string} message
   * @param {Function=} opt_onOk
   * @param {Function=} opt_onCancel
   * @param {Function=} opt_onShow
   */
  BaseDialog.prototype.showWithTitle = function(
      title, message, opt_onOk, opt_onCancel, opt_onShow) {
    this.text_.textContent = message;
    this.show_(title, opt_onOk, opt_onCancel, opt_onShow);
  };

  /**
   * @param {string} title
   * @param {Function=} opt_onOk
   * @param {Function=} opt_onCancel
   * @param {Function=} opt_onShow
   * @private
   */
  BaseDialog.prototype.show_ = function(
      title, opt_onOk, opt_onCancel, opt_onShow) {
    this.showing_ = true;
    // Make all outside nodes unfocusable while the dialog is active.
    this.deactivatedNodes_ = this.findFocusableElements_(this.document_);
    this.tabIndexes_ = this.deactivatedNodes_.map(function(n) {
      return n.getAttribute('tabindex');
    });
    this.deactivatedNodes_.forEach(function(n) {
      n.tabIndex = -1;
    });

    this.previousActiveElement_ = this.document_.activeElement;
    this.parentNode_.appendChild(this.container_);

    this.onOk_ = opt_onOk;
    this.onCancel_ = opt_onCancel;

    if (title) {
      this.title_.textContent = title;
      this.title_.hidden = false;
      this.frame_.setAttribute('aria-label', title);
    } else {
      this.title_.textContent = '';
      this.title_.hidden = true;
      this.frame_.removeAttribute('aria-label');
    }

    const self = this;
    setTimeout(function() {
      // Check that hide() was not called in between.
      if (self.showing_) {
        self.container_.classList.add('shown');
        self.initialFocusElement_.focus();
      }
      setTimeout(function() {
        if (opt_onShow) {
          opt_onShow();
        }
      }, BaseDialog.ANIMATE_STABLE_DURATION);
    }, 0);
  };

  /** @param {Function=} opt_onHide */
  BaseDialog.prototype.hide = function(opt_onHide) {
    this.showing_ = false;
    // Restore focusability.
    for (let i = 0; i < this.deactivatedNodes_.length; i++) {
      const node = this.deactivatedNodes_[i];
      if (this.tabIndexes_[i] === null) {
        node.removeAttribute('tabindex');
      } else {
        node.setAttribute('tabindex', this.tabIndexes_[i]);
      }
    }
    this.deactivatedNodes_ = null;
    this.tabIndexes_ = null;

    this.container_.classList.remove('shown');

    if (this.previousActiveElement_) {
      this.previousActiveElement_.focus();
    } else {
      this.document_.body.focus();
    }
    this.frame_.classList.remove('pulse');

    const self = this;
    setTimeout(function() {
      // Wait until the transition is done before removing the dialog.
      // Check show() was not called in between.
      // It is also possible to show/hide/show/hide and have hide called twice
      // and container_ already removed from parentNode_.
      if (!self.showing_ && self.parentNode_ === self.container_.parentNode) {
        self.parentNode_.removeChild(self.container_);
      }
      if (opt_onHide) {
        opt_onHide();
      }
    }, BaseDialog.ANIMATE_STABLE_DURATION);
  };

  /**
   * AlertDialog contains just a message and an ok button.
   * @constructor
   * @extends {cr.ui.dialogs.BaseDialog}
   */
  function AlertDialog(parentNode) {
    BaseDialog.call(this, parentNode);
    this.cancelButton_.style.display = 'none';
  }

  AlertDialog.prototype = {__proto__: BaseDialog.prototype};

  /**
   * @param {Function=} opt_onOk
   * @param {Function=} opt_onShow
   * @override
   */
  AlertDialog.prototype.show = function(message, opt_onOk, opt_onShow) {
    return BaseDialog.prototype.show.call(
        this, message, opt_onOk, opt_onOk, opt_onShow);
  };

  /**
   * ConfirmDialog contains a message, an ok button, and a cancel button.
   * @constructor
   * @extends {cr.ui.dialogs.BaseDialog}
   */
  function ConfirmDialog(parentNode) {
    BaseDialog.call(this, parentNode);
  }

  ConfirmDialog.prototype = {__proto__: BaseDialog.prototype};

  /**
   * PromptDialog contains a message, a text input, an ok button, and a
   * cancel button.
   * @constructor
   * @extends {cr.ui.dialogs.BaseDialog}
   */
  function PromptDialog(parentNode) {
    BaseDialog.call(this, parentNode);
    this.input_ = this.document_.createElement('input');
    this.input_.setAttribute('type', 'text');
    this.input_.addEventListener('focus', this.onInputFocus.bind(this));
    this.input_.addEventListener('keydown', this.onKeyDown_.bind(this));
    this.initialFocusElement_ = this.input_;
    this.frame_.insertBefore(this.input_, this.text_.nextSibling);
  }

  PromptDialog.prototype = {__proto__: BaseDialog.prototype};

  PromptDialog.prototype.onInputFocus = function(event) {
    this.input_.select();
  };

  /** @private */
  PromptDialog.prototype.onKeyDown_ = function(event) {
    if (event.keyCode == 13) {  // Enter
      this.onOkClick_(event);
      event.preventDefault();
    }
  };

  /**
   * @param {string} message
   * @param {?} defaultValue
   * @param {Function=} opt_onOk
   * @param {Function=} opt_onCancel
   * @param {Function=} opt_onShow
   * @suppress {checkTypes}
   * TODO(fukino): remove suppression if there is a better way to avoid warning
   * about overriding method with different signature.
   */
  PromptDialog.prototype.show = function(
      message, defaultValue, opt_onOk, opt_onCancel, opt_onShow) {
    this.input_.value = defaultValue || '';
    return BaseDialog.prototype.show.call(
        this, message, opt_onOk, opt_onCancel, opt_onShow);
  };

  PromptDialog.prototype.getValue = function() {
    return this.input_.value;
  };

  /** @private */
  PromptDialog.prototype.onOkClick_ = function(event) {
    this.hide();
    if (this.onOk_) {
      this.onOk_(this.getValue());
    }
  };

  return {
    BaseDialog: BaseDialog,
    AlertDialog: AlertDialog,
    ConfirmDialog: ConfirmDialog,
    PromptDialog: PromptDialog
  };
});
