// Copyright 2017 The Chromium Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

Polymer({
  is: 'print-preview-app',

  behaviors: [
    SettingsBehavior,
    WebUIListenerBehavior,
  ],

  properties: {
    /** @type {!print_preview_new.State} */
    state: {
      type: Number,
      observer: 'onStateChanged_',
    },

    /** @private {!cloudprint.CloudPrintInterface} */
    cloudPrintInterface_: Object,

    /** @private {boolean} */
    controlsManaged_: Boolean,

    /** @private {print_preview.Destination} */
    destination_: Object,

    /** @private {!print_preview.DestinationState} */
    destinationState_: {
      type: Number,
      observer: 'onDestinationStateChange_',
    },

    /** @private {print_preview.DocumentSettings} */
    documentSettings_: Object,

    /** @private {string} */
    errorMessage_: String,

    /** @private {print_preview.Margins} */
    margins_: Object,

    /** @private {!print_preview.Size} */
    pageSize_: Object,

    /** @private {!print_preview.PrintableArea} */
    printableArea_: Object,

    /** @private {?print_preview.MeasurementSystem} */
    measurementSystem_: {
      type: Object,
      value: null,
    },

    /** @private {!print_preview_new.PreviewAreaState} */
    previewState_: {
      type: String,
      observer: 'onPreviewAreaStateChanged_',
    },
  },

  listeners: {
    'cr-dialog-open': 'onCrDialogOpen_',
    'close': 'onCrDialogClose_',
  },

  /** @private {?print_preview.NativeLayer} */
  nativeLayer_: null,

  /** @private {!EventTracker} */
  tracker_: new EventTracker(),

  /** @private {boolean} */
  cancelled_: false,

  /** @private {boolean} */
  printRequested_: false,

  /** @private {boolean} */
  startPreviewWhenReady_: false,

  /** @private {boolean} */
  showSystemDialogBeforePrint_: false,

  /** @private {boolean} */
  openPdfInPreview_: false,

  /** @private {boolean} */
  isInKioskAutoPrintMode_: false,

  /** @private {?Promise} */
  whenReady_: null,

  /** @private {!Array<!CrDialogElement>} */
  openDialogs_: [],

  /** @override */
  ready: function() {
    cr.ui.FocusOutlineManager.forDocument(document);
  },

  /** @override */
  attached: function() {
    document.documentElement.classList.remove('loading');
    this.nativeLayer_ = print_preview.NativeLayer.getInstance();
    this.addWebUIListener(
        'use-cloud-print', this.onCloudPrintEnable_.bind(this));
    this.addWebUIListener('print-failed', this.onPrintFailed_.bind(this));
    this.addWebUIListener(
        'print-preset-options', this.onPrintPresetOptions_.bind(this));
    this.tracker_.add(window, 'keydown', this.onKeyDown_.bind(this));
    this.$.previewArea.setPluginKeyEventCallback(this.onKeyDown_.bind(this));
    this.whenReady_ = print_preview.Model.whenReady();
    this.nativeLayer_.getInitialSettings().then(
        this.onInitialSettingsSet_.bind(this));
  },

  /** @override */
  detached: function() {
    this.tracker_.removeAll();
    this.whenReady_ = null;
  },

  /**
   * Consume escape and enter key presses and ctrl + shift + p. Delegate
   * everything else to the preview area.
   * @param {!KeyboardEvent} e The keyboard event.
   * @private
   */
  onKeyDown_: function(e) {
    // Escape key closes the topmost dialog that is currently open within
    // Print Preview. If no such dialog exists, then the Print Preview dialog
    // itself is closed.
    if (e.code == 'Escape' && !hasKeyModifiers(e)) {
      // Don't close the Print Preview dialog if there is a child dialog open.
      if (this.openDialogs_.length != 0) {
        // Manually cancel the dialog, since we call preventDefault() to prevent
        // views from closing the Print Preview dialog.
        const dialogToClose = this.openDialogs_[this.openDialogs_.length - 1];
        dialogToClose.cancel();
        e.preventDefault();
        return;
      }

      // On non-mac with toolkit-views, ESC key is handled by C++-side instead
      // of JS-side.
      if (cr.isMac) {
        this.close_();
        e.preventDefault();
      }
      return;
    }

    // On Mac, Cmd+Period should close the print dialog.
    if (cr.isMac && e.code == 'Period' && e.metaKey) {
      this.close_();
      e.preventDefault();
      return;
    }

    // Ctrl + Shift + p / Mac equivalent.
    if (e.code == 'KeyP') {
      if ((cr.isMac && e.metaKey && e.altKey && !e.shiftKey && !e.ctrlKey) ||
          (!cr.isMac && e.shiftKey && e.ctrlKey && !e.altKey && !e.metaKey)) {
        // Don't use system dialog if the link isn't available.
        if (!this.$.sidebar.systemDialogLinkAvailable()) {
          return;
        }

        // Don't try to print with system dialog on Windows if the document is
        // not ready, because we send the preview document to the printer on
        // Windows.
        if (!cr.isWindows || this.state == print_preview_new.State.READY) {
          this.onPrintWithSystemDialog_();
        }
        e.preventDefault();
        return;
      }
    }

    if ((e.code === 'Enter' || e.code === 'NumpadEnter') &&
        this.state === print_preview_new.State.READY &&
        this.openDialogs_.length === 0) {
      const activeElementTag = e.path[0].tagName;
      if (['PAPER-BUTTON', 'BUTTON', 'SELECT', 'A', 'CR-CHECKBOX'].includes(
              activeElementTag)) {
        return;
      }

      this.onPrintRequested_();
      e.preventDefault();
      return;
    }

    // Pass certain directional keyboard events to the PDF viewer.
    this.$.previewArea.handleDirectionalKeyEvent(e);
  },

  /**
   * @param {!Event} e The cr-dialog-open event.
   * @private
   */
  onCrDialogOpen_: function(e) {
    this.openDialogs_.push(
        /** @type {!CrDialogElement} */ (e.composedPath()[0]));
  },

  /**
   * @param {!Event} e The close event.
   * @private
   */
  onCrDialogClose_: function(e) {
    // Note: due to event re-firing in cr_dialog.js, this event will always
    // appear to be coming from the outermost child dialog.
    // TODO(rbpotter): Fix event re-firing so that the event comes from the
    // dialog that has been closed, and add an assertion that the removed
    // dialog matches e.composedPath()[0].
    if (e.composedPath()[0].nodeName == 'CR-DIALOG') {
      this.openDialogs_.pop();
    }
  },

  /**
   * @param {!print_preview.NativeInitialSettings} settings
   * @private
   */
  onInitialSettingsSet_: function(settings) {
    if (!this.whenReady_) {
      // This element and its corresponding model were detached while waiting
      // for the callback. This can happen in tests; return early.
      return;
    }
    this.whenReady_.then(() => {
      this.$.documentInfo.init(
          settings.previewModifiable, settings.documentTitle,
          settings.documentHasSelection);
      this.$.model.setStickySettings(settings.serializedAppStateStr);
      this.$.model.setPolicySettings(
          settings.headerFooter, settings.isHeaderFooterManaged);
      this.measurementSystem_ = new print_preview.MeasurementSystem(
          settings.thousandsDelimeter, settings.decimalDelimeter,
          settings.unitType);
      this.setSetting('selectionOnly', settings.shouldPrintSelectionOnly);
      this.$.sidebar.init(
          settings.isInAppKioskMode, settings.printerName,
          settings.serializedDefaultDestinationSelectionRulesStr);
      this.isInKioskAutoPrintMode_ = settings.isInKioskAutoPrintMode;

      // This is only visible in the task manager.
      let title = document.head.querySelector('title');
      if (!title) {
        title = document.createElement('title');
        document.head.appendChild(title);
      }
      title.textContent = settings.documentTitle;
    });
  },

  /**
   * Called when Google Cloud Print integration is enabled by the
   * PrintPreviewHandler.
   * Fetches the user's cloud printers.
   * @param {string} cloudPrintUrl The URL to use for cloud print servers.
   * @param {boolean} appKioskMode Whether to print automatically for kiosk
   *     mode.
   * @private
   */
  onCloudPrintEnable_: function(cloudPrintUrl, appKioskMode) {
    if (!this.whenReady_) {
      // This element and its corresponding model were detached while waiting
      // for the callback. This can happen in tests; return early.
      return;
    }
    this.whenReady_.then(() => {
      assert(!this.cloudPrintInterface_);
      this.cloudPrintInterface_ = cloudprint.getCloudPrintInterface(
          cloudPrintUrl, assert(this.nativeLayer_), appKioskMode);
      this.tracker_.add(
          assert(this.cloudPrintInterface_).getEventTarget(),
          cloudprint.CloudPrintInterfaceEventType.SUBMIT_DONE,
          this.close_.bind(this));
      this.tracker_.add(
          assert(this.cloudPrintInterface_).getEventTarget(),
          cloudprint.CloudPrintInterfaceEventType.SUBMIT_FAILED,
          this.onCloudPrintError_.bind(this, appKioskMode));
    });
  },

  /** @private */
  onDestinationStateChange_: function() {
    switch (this.destinationState_) {
      case print_preview.DestinationState.SELECTED:
        // If the plugin does not exist do not attempt to load the preview.
        if (this.state !== print_preview_new.State.FATAL_ERROR) {
          this.$.state.transitTo(print_preview_new.State.NOT_READY);
        }
        break;
      case print_preview.DestinationState.UPDATED:
        if (!this.$.model.initialized()) {
          this.$.model.applyStickySettings();
        }

        // <if expr="chromeos">
        this.$.model.applyDestinationSpecificPolicies();
        // </if>

        this.startPreviewWhenReady_ = true;
        if (this.state == print_preview_new.State.NOT_READY ||
            this.state == print_preview_new.State.INVALID_PRINTER) {
          this.$.state.transitTo(print_preview_new.State.READY);
          if (this.isInKioskAutoPrintMode_ || this.printRequested_) {
            this.onPrintRequested_();
            // Reset in case printing fails.
            this.printRequested_ = false;
          }
        }
        break;
      case print_preview.DestinationState.INVALID:
      case print_preview.DestinationState.UNSUPPORTED:
      // <if expr="chromeos">
      case print_preview.DestinationState.NO_DESTINATIONS:
        // </if>
        this.$.state.transitTo(print_preview_new.State.INVALID_PRINTER);
        break;
      default:
        break;
    }
  },

  /**
   * @param {!CustomEvent<string>} e Event containing the new sticky settings.
   * @private
   */
  onStickySettingChanged_: function(e) {
    this.nativeLayer_.saveAppState(e.detail);
  },

  /** @private */
  onPreviewSettingChanged_: function() {
    if (this.state === print_preview_new.State.READY) {
      this.$.previewArea.startPreview();
      this.startPreviewWhenReady_ = false;
    } else {
      this.startPreviewWhenReady_ = true;
    }
  },

  /** @private */
  onStateChanged_: function() {
    if (this.state == print_preview_new.State.READY) {
      if (this.startPreviewWhenReady_) {
        this.$.previewArea.startPreview();
        this.startPreviewWhenReady_ = false;
      }
    } else if (this.state == print_preview_new.State.CLOSING) {
      this.remove();
      this.nativeLayer_.dialogClose(this.cancelled_);
    } else if (this.state == print_preview_new.State.HIDDEN) {
      if (this.destination_.isLocal &&
          this.destination_.id !==
              print_preview.Destination.GooglePromotedId.SAVE_AS_PDF) {
        // Only hide the preview for local, non PDF destinations.
        this.nativeLayer_.hidePreview();
      }
    } else if (this.state == print_preview_new.State.PRINTING) {
      const destination = assert(this.destination_);
      const whenPrintDone =
          this.nativeLayer_.print(this.$.model.createPrintTicket(
              destination, this.openPdfInPreview_,
              this.showSystemDialogBeforePrint_));
      if (destination.isLocal) {
        const onError = destination.id ==
                print_preview.Destination.GooglePromotedId.SAVE_AS_PDF ?
            this.onFileSelectionCancel_.bind(this) :
            this.onPrintFailed_.bind(this);
        whenPrintDone.then(this.close_.bind(this), onError);
      } else {
        // Cloud print resolves when print data is returned to submit to cloud
        // print, or if print ticket cannot be read, no PDF data is found, or
        // PDF is oversized.
        whenPrintDone.then(
            this.onPrintToCloud_.bind(this), this.onPrintFailed_.bind(this));
      }
    }
  },

  /** @private */
  onPrintRequested_: function() {
    if (this.state === print_preview_new.State.NOT_READY) {
      this.printRequested_ = true;
      return;
    }
    this.$.state.transitTo(
        this.$.previewArea.previewLoaded() ? print_preview_new.State.PRINTING :
                                             print_preview_new.State.HIDDEN);
  },

  /** @private */
  onCancelRequested_: function() {
    this.cancelled_ = true;
    this.$.state.transitTo(print_preview_new.State.CLOSING);
  },

  /**
   * @param {!CustomEvent<boolean>} e The event containing the new validity.
   * @private
   */
  onSettingValidChanged_: function(e) {
    this.$.state.transitTo(
        e.detail ? print_preview_new.State.READY :
                   print_preview_new.State.INVALID_TICKET);
  },

  /** @private */
  onFileSelectionCancel_: function() {
    this.$.state.transitTo(print_preview_new.State.READY);
  },

  /**
   * Called when the native layer has retrieved the data to print to Google
   * Cloud Print.
   * @param {string} data The body to send in the HTTP request.
   * @private
   */
  onPrintToCloud_: function(data) {
    assert(
        this.cloudPrintInterface_ != null, 'Google Cloud Print is not enabled');
    const destination = assert(this.destination_);
    this.cloudPrintInterface_.submit(
        destination, this.$.model.createCloudJobTicket(destination),
        this.documentSettings_.title, data);
  },

  // <if expr="not chromeos">
  /** @private */
  onPrintWithSystemDialog_: function() {
    // <if expr="is_win">
    this.showSystemDialogBeforePrint_ = true;
    this.onPrintRequested_();
    // </if>
    // <if expr="not is_win">
    this.nativeLayer_.showSystemDialog();
    this.$.state.transitTo(print_preview_new.State.SYSTEM_DIALOG);
    // </if>
  },
  // </if>

  // <if expr="is_macosx">
  /** @private */
  onOpenPdfInPreview_: function() {
    this.openPdfInPreview_ = true;
    this.$.previewArea.setOpeningPdfInPreview();
    this.onPrintRequested_();
  },
  // </if>

  /**
   * Called when printing to a privet, cloud, or extension printer fails.
   * @param {*} httpError The HTTP error code, or -1 or a string describing
   *     the error, if not an HTTP error.
   * @private
   */
  onPrintFailed_: function(httpError) {
    console.error('Printing failed with error code ' + httpError);
    this.$.state.transitTo(print_preview_new.State.FATAL_ERROR);
    this.errorMessage_ = loadTimeData.getString('couldNotPrint');
  },

  /** @private */
  onPreviewAreaStateChanged_: function() {
    switch (this.previewState_) {
      case print_preview_new.PreviewAreaState.PREVIEW_FAILED:
      case print_preview_new.PreviewAreaState.NO_PLUGIN:
        this.$.state.transitTo(print_preview_new.State.FATAL_ERROR);
        break;
      case print_preview_new.PreviewAreaState.INVALID_SETTINGS:
        if (this.state != print_preview_new.State.INVALID_PRINTER) {
          this.$.state.transitTo(print_preview_new.State.INVALID_PRINTER);
        }
        break;
      case print_preview_new.PreviewAreaState.DISPLAY_PREVIEW:
      case print_preview_new.PreviewAreaState.OPEN_IN_PREVIEW_LOADED:
        if (this.state == print_preview_new.State.HIDDEN) {
          this.$.state.transitTo(print_preview_new.State.PRINTING);
        }
        break;
      default:
        break;
    }
  },

  /**
   * Called when there was an error communicating with Google Cloud print.
   * Displays an error message in the print header.
   * @param {boolean} appKioskMode
   * @param {!CustomEvent<!cloudprint.CloudPrintInterfaceErrorEventDetail>}
   *     event Contains the error message.
   * @private
   */
  onCloudPrintError_: function(appKioskMode, event) {
    if (event.detail.status == 0 ||
        (event.detail.status == 403 && !appKioskMode)) {
      return;  // No internet connectivity or not signed in.
    }
    this.$.state.transitTo(print_preview_new.State.FATAL_ERROR);
    this.errorMessage_ = event.detail.message;
    if (event.detail.status == 200) {
      console.error(
          'Google Cloud Print Error: ' +
          `(${event.detail.errorCode}) ${event.detail.message}`);
    } else {
      console.error(
          'Google Cloud Print Error: ' +
          `HTTP status ${event.detail.status}`);
    }
  },

  /**
   * Updates printing options according to source document presets.
   * @param {boolean} disableScaling Whether the document disables scaling.
   * @param {number} copies The default number of copies from the document.
   * @param {!print_preview_new.DuplexMode} duplex The default duplex setting
   *     from the document.
   * @private
   */
  onPrintPresetOptions_: function(disableScaling, copies, duplex) {
    if (disableScaling) {
      this.$.documentInfo.updateIsScalingDisabled(true);
    }

    if (copies > 0 && this.getSetting('copies').available) {
      this.setSetting('copies', copies);
    }

    if (duplex !== print_preview_new.DuplexMode.UNKNOWN_DUPLEX_MODE &&
        this.getSetting('duplex').available) {
      this.setSetting(
          'duplex',
          duplex === print_preview_new.DuplexMode.LONG_EDGE ||
              duplex === print_preview_new.DuplexMode.SHORT_EDGE);
    }
    if (duplex !== print_preview_new.DuplexMode.UNKNOWN_DUPLEX_MODE &&
        duplex !== print_preview_new.DuplexMode.SIMPLEX &&
        this.getSetting('duplexShortEdge').available) {
      this.setSetting(
          'duplexShortEdge',
          duplex === print_preview_new.DuplexMode.SHORT_EDGE);
    }
  },

  /**
   * @param {!CustomEvent<number>} e Contains the new preview request ID.
   * @private
   */
  onPreviewStart_: function(e) {
    this.$.documentInfo.inFlightRequestId = e.detail;
  },

  /** @private */
  close_: function() {
    this.$.state.transitTo(print_preview_new.State.CLOSING);
  },
});
