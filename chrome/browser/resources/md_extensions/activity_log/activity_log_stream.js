// Copyright 2019 The Chromium Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

cr.define('extensions', function() {
  'use strict';

  /** @interface */
  class ActivityLogEventDelegate {
    /** @return {!ChromeEvent} */
    getOnExtensionActivity() {}
  }

  /**
   * Process activity for the stream. In the case of content scripts, we split
   * the activity for every script invoked.
   * @param {!chrome.activityLogPrivate.ExtensionActivity}
   *     activity
   * @return {!Array<!extensions.StreamItem>}
   */
  function processActivityForStream(activity) {
    const activityType = activity.activityType;
    const timestamp = activity.time;
    const isContentScript = activityType ===
        chrome.activityLogPrivate.ExtensionActivityType.CONTENT_SCRIPT;

    const args = isContentScript ? JSON.stringify([]) : activity.args;

    let streamItemNames = [activity.apiCall];

    // TODO(kelvinjiang): Reuse logic from activity_log_history and refactor
    // some of the processing code into a separate file in a follow up CL.
    if (isContentScript) {
      streamItemNames = activity.args ? JSON.parse(activity.args) : [];
      assert(Array.isArray(streamItemNames), 'Invalid data for script names.');
    }

    const other = activity.other;
    const webRequestInfo = other && other.webRequest;

    return streamItemNames.map(name => ({
                                 args,
                                 argUrl: activity.argUrl,
                                 activityType,
                                 name,
                                 pageUrl: activity.pageUrl,
                                 timestamp,
                                 webRequestInfo,
                                 expanded: false,
                               }));
  }

  const ActivityLogStream = Polymer({
    is: 'activity-log-stream',

    properties: {
      /** @type {string} */
      extensionId: String,

      /** @type {!extensions.ActivityLogEventDelegate} */
      delegate: Object,

      /** @private */
      isStreamOn_: {
        type: Boolean,
        value: false,
      },

      /** @private {!Array<!chrome.activityLogPrivate.ExtensionActivity>} */
      activityStream_: {
        type: Array,
        value: () => [],
      },
    },

    listeners: {
      'resize-stream': 'onResizeStream_',
    },

    /**
     * Instance of |extensionActivityListener_| bound to |this|.
     * @private {!Function}
     */
    listenerInstance_: () => {},

    /** @override */
    attached: function() {
      this.listenerInstance_ = this.extensionActivityListener_.bind(this);
      this.startStream_();
    },

    /** @override */
    detached: function() {
      this.pauseStream_();
      this.clearStream_();
    },

    /** @private */
    onResizeStream_: function(e) {
      this.$$('iron-list').notifyResize();
    },

    /** @private */
    clearStream_: function() {
      this.splice('activityStream_', 0, this.activityStream_.length);
    },

    /** @private */
    startStream_: function() {
      if (this.isStreamOn_) {
        return;
      }

      this.isStreamOn_ = true;
      this.delegate.getOnExtensionActivity().addListener(
          this.listenerInstance_);
    },

    /** @private */
    pauseStream_: function() {
      if (!this.isStreamOn_) {
        return;
      }

      this.delegate.getOnExtensionActivity().removeListener(
          this.listenerInstance_);
      this.isStreamOn_ = false;
    },

    /** @private */
    onToggleButtonClick_: function() {
      if (this.isStreamOn_) {
        this.pauseStream_();
      } else {
        this.startStream_();
      }
    },

    /**
     * @private
     * @return {boolean}
     */
    isStreamEmpty_: function() {
      return this.activityStream_.length == 0;
    },

    /**
     * @private
     * @param {!chrome.activityLogPrivate.ExtensionActivity} activity
     */
    extensionActivityListener_: function(activity) {
      if (activity.extensionId != this.extensionId) {
        return;
      }

      this.splice(
          'activityStream_', this.activityStream_.length, 0,
          ...processActivityForStream(activity));

      // Used to update the scrollbar.
      this.$$('iron-list').notifyResize();
    },
  });

  return {
    ActivityLogStream: ActivityLogStream,
    ActivityLogEventDelegate: ActivityLogEventDelegate,
  };
});
